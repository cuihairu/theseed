// OtlpTraceExporter 测试：endpoint 解析、OTLP/HTTP JSON 编码（四类型属性
// + 转义 + 非有限 double）、异步批量外发（满批/周期取批、有界重试、队列
// 溢出丢弃、停机排水、重装）、回环收集端 e2e（200/500/静默超时/先关/垃圾
// 状态行/分片响应）、链式发射器与卸载语义、默认关闭口径。
// 回环 TCP 测试同仓内惯例门控 Linux（见 tests/runtime/CMakeLists）。
#include "theseed/control/machine/OtlpTraceExporter.h"

#include "theseed/foundation/Logger.h"
#include "theseed/foundation/Tracing.h"
#include "theseed/runtime/TcpConnection.h"
#include "theseed/runtime/TcpListener.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace foundation = theseed::foundation;
namespace machine = theseed::control::machine;
using machine::OtlpTraceExporter;
using theseed::runtime::TcpConnection;
using theseed::runtime::TcpListener;

static int testsPassed = 0;
static int testsFailed = 0;

#define TEST(name)                              \
    do {                                        \
        std::cout << "  " << (name) << "... " << std::flush; \
    } while (0)

#define PASS()                                                  \
    do {                                                        \
        std::cout << "OK\n";                                    \
        ++testsPassed;                                          \
    } while (0)

#define FAIL(msg)                                               \
    do {                                                        \
        std::cout << "FAILED: " << (msg) << "\n";               \
        ++testsFailed;                                          \
        return;                                                 \
    } while (0)

// 测试后置守卫：任何 FAIL 早退也不污染全局发射器/日志器槽位。
struct EmitterReset {
    ~EmitterReset() { foundation::setSpanEmitter(nullptr); }
};

// 捕获日志假件（worker 线程的失败 warn 与断言线程的 drain 并发，加锁）。
class CapturingLoggerImpl final : public foundation::ILogger {
public:
    void log(foundation::LogRecord record) override {
        const std::lock_guard lock(mutex_);
        records_.push_back(std::move(record));
    }
    void setLevel(foundation::LogLevel level) override { level_ = level; }
    foundation::LogLevel level() const override { return level_; }

    std::vector<foundation::LogRecord> drain() {
        const std::lock_guard lock(mutex_);
        return std::move(records_);
    }

private:
    foundation::LogLevel level_ = foundation::LogLevel::Debug;
    std::mutex mutex_;
    std::vector<foundation::LogRecord> records_;
};

// 测试期间接管全局日志器，析构还原（CapturingLoggerImpl 捕获输出）。
struct LoggerReset {
    LoggerReset()
        : previous(foundation::takeGlobalLogger()),
          captured(std::make_shared<CapturingLoggerImpl>()) {
        foundation::setGlobalLogger(captured);
    }
    ~LoggerReset() { foundation::setGlobalLogger(std::move(previous)); }

    std::shared_ptr<foundation::ILogger> previous;
    std::shared_ptr<CapturingLoggerImpl> captured;
};

// 回环 OTLP 收集端：独立线程泵循环（客户端在发射钩子里同步阻塞 POST，
// 服务端必须并发泵）。按模式回应/沉默/先关/垃圾状态行/分片响应。
class MockOtlpCollector {
public:
    enum class Mode {
        Reply200,
        Reply500,
        Silent,
        CloseOnAccept,
        GarbageThenClose,
        Fragmented200,
    };

    ~MockOtlpCollector() { stop(); }

    bool start(Mode mode) {
        mode_ = mode;
        if (!listener_.listen("127.0.0.1", 0)) return false;
        thread_ = std::thread([this] { run(); });
        return true;
    }

    void stop() {
        stop_.store(true);
        if (thread_.joinable()) thread_.join();
        listener_.close();
    }

    std::uint16_t port() const { return listener_.localPort(); }

    std::vector<std::string> requests() const {
        std::lock_guard lock(mutex_);
        return requests_;
    }

private:
    // 每连接状态：rx 缓冲与已记录/已回应旗标（多笔 span = 多条连接）。
    // rx 用 shared_ptr 持有：接收回调按值捕获同一份，ConnState 被
    // move 进容器后回调仍指向有效缓冲（裸指针会悬垂）。
    struct ConnState {
        std::shared_ptr<TcpConnection> conn;
        std::shared_ptr<std::string> rx = std::make_shared<std::string>();
        bool recorded = false;
        bool responded = false;
    };

    void run() {
        std::vector<ConnState> conns;
        while (!stop_.load()) {
            while (auto accepted = listener_.accept()) {
                if (mode_ == Mode::CloseOnAccept) {
                    accepted->close();  // 不收不发：客户端走 EOF 失败臂
                    continue;
                }
                ConnState state;
                state.conn = accepted;
                const auto rx = state.rx;  // shared_ptr 按值进回调
                state.conn->setOnReceived(
                    [rx](std::span<const std::byte> data) {
                        rx->append(reinterpret_cast<const char*>(data.data()),
                                   data.size());
                    });
                conns.push_back(std::move(state));
            }
            for (auto& state : conns) {
                if (state.conn && state.conn->isConnected()) {
                    state.conn->pump();
                    if (!state.recorded && requestComplete(*state.rx)) {
                        std::lock_guard lock(mutex_);
                        requests_.push_back(*state.rx);
                        state.recorded = true;
                    }
                    if (state.recorded && !state.responded) {
                        respond(*state.conn);
                        state.responded = true;
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        for (auto& state : conns) {
            if (state.conn) state.conn->close();
        }
    }

    void respond(TcpConnection& conn) {
        switch (mode_) {
        case Mode::Reply200:
            writeAll(conn, "HTTP/1.1 200 OK\r\nContent-Type: application/json"
                           "\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}");
            break;
        case Mode::Reply500:
            writeAll(conn, "HTTP/1.1 500 Internal Server Error\r\n"
                           "Content-Length: 0\r\nConnection: close\r\n\r\n");
            break;
        case Mode::GarbageThenClose:
            writeAll(conn, "NOT-HTTP\r\n\r\n");
            conn.close();
            break;
        case Mode::Fragmented200:
            // 状态行拆包：迫使客户端解码走「状态行未收全」的续等迭代。
            writeAll(conn, "HTTP/1.1 20");
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            writeAll(conn, "0 OK\r\n");
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            writeAll(conn, "Content-Length: 2\r\nConnection: close\r\n\r\n{}");
            break;
        case Mode::Silent:
        case Mode::CloseOnAccept:
            break;  // 不回应：客户端走超时 / EOF 臂
        }
    }

    static void writeAll(TcpConnection& conn, std::string_view text) {
        static_cast<void>(conn.write(std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(text.data()), text.size())));
    }

    static bool requestComplete(const std::string& rx) {
        const auto headersEnd = rx.find("\r\n\r\n");
        if (headersEnd == std::string::npos) return false;
        const auto key = rx.find("Content-Length: ");
        if (key == std::string::npos || key > headersEnd) return false;
        const auto valueBegin = key + std::strlen("Content-Length: ");
        const auto valueEnd = rx.find("\r\n", valueBegin);
        if (valueEnd == std::string::npos) return false;
        const auto length = std::strtoul(rx.c_str() + valueBegin, nullptr, 10);
        return rx.size() >= headersEnd + 4 + length;
    }

    Mode mode_ = Mode::Reply200;
    TcpListener listener_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    mutable std::mutex mutex_;
    std::vector<std::string> requests_;
};

// 捕获发射器：计数并留档 span（断言链式转发用）。
struct SpanCapture {
    std::vector<foundation::Span> spans;
    void install() {
        const auto self = this;
        foundation::setSpanEmitter([self](const foundation::Span& span) {
            self->spans.push_back(span);
        });
    }
};

static OtlpTraceExporter::Config endpointConfig(std::uint16_t port,
                                                std::chrono::milliseconds timeout) {
    OtlpTraceExporter::Config config;
    config.enabled = true;
    config.endpoint =
        "http://127.0.0.1:" + std::to_string(port) + "/v1/traces";
    config.timeout = timeout;
    // 既有单 span 用例保持「每 span 一 POST」的旧口径（batchSize=1 满批
    // 即发）；兜底周期压到 10ms、退避压到 10ms——异步化后不拖慢测试，
    // 失败用例的有界重试（缺省 3 次）仍全程在旧断言的时间上界内。
    config.batchSize = 1;
    config.flushInterval = std::chrono::milliseconds{10};
    config.retryBackoff = std::chrono::milliseconds{10};
    return config;
}

static bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

// 异步外发的轮询等待：worker 线程先落日志再落计数（sendBatch 语义），
// 观测者等到计数即可安全断言此前的日志与请求数。
template <class Pred>
static bool waitUntil(Pred&& pred,
                      std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!pred()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return pred();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return true;
}

static int countOf(std::string_view haystack, std::string_view needle) {
    int count = 0;
    for (std::size_t pos = haystack.find(needle); pos != std::string_view::npos;
         pos = haystack.find(needle, pos + needle.size())) {
        ++count;
    }
    return count;
}

// 花括号配平自检（批量报文结构 sanity：不回负且收口为 0）。
static bool bracesBalanced(std::string_view text) {
    int depth = 0;
    for (const char c : text) {
        if (c == '{') {
            ++depth;
        } else if (c == '}') {
            if (--depth < 0) {
                return false;
            }
        }
    }
    return depth == 0;
}

static void test_resolveEndpoint_parses_scheme_host_port_path() {
    TEST("resolveEndpoint parses scheme/host/port/path");
    const auto full = OtlpTraceExporter::resolveEndpoint("http://10.0.0.1:9999/v1/traces");
    if (!full) FAIL("full form should parse");
    if (full->host != "10.0.0.1" || full->port != 9999 ||
        full->path != "/v1/traces") {
        FAIL("full form fields mismatch");
    }

    const auto defaults = OtlpTraceExporter::resolveEndpoint("http://10.0.0.1");
    if (!defaults) FAIL("bare host should parse");
    if (defaults->port != 4318 || defaults->path != "/") {
        FAIL("default port/path mismatch");
    }

    const auto defaultPort = OtlpTraceExporter::resolveEndpoint("http://10.0.0.1/v1/traces");
    if (!defaultPort || defaultPort->port != 4318 ||
        defaultPort->path != "/v1/traces") {
        FAIL("default port with path mismatch");
    }

    const auto maxPort = OtlpTraceExporter::resolveEndpoint("http://1.2.3.4:65535");
    if (!maxPort || maxPort->port != 65535) FAIL("port 65535 should parse");
    PASS();
}

static void test_resolveEndpoint_rejects_invalid_urls() {
    TEST("resolveEndpoint rejects invalid urls");
    static const char* const kInvalid[] = {
        "",
        "https://1.2.3.4/v1/traces",   // 无 TLS 支持口径
        "ftp://1.2.3.4/x",
        "1.2.3.4:4318/v1",             // 缺 scheme
        "http://",                     // 空 host
        "http:///v1",                  // 空 host + path
        "http://collector:4318/v1",    // 域名（无 DNS 口径）
        "http://[::1]:4318/v1",        // IPv6
        "http://1.2.3.4:/v1",          // 空端口
        "http://1.2.3.4:abc/v1",       // 非数字端口
        "http://1.2.3.4:70000/v1",     // 越界端口
        "http://1.2.3.4:0/v1",         // 0 端口
        "http://1.2.3.4:65536/v1",     // 越界端口
        "http://999.2.3.4:4318/v1",    // 八位组越界
        "http://1.2.3.4444/v1",        // 八位组 4 位（由 >255 臂拒绝）
        "http://1.2.3.44444/v1",       // 八位组超 3 位（第 5 位触发防溢出臂）
        "http://1234.1.1.1:4318/v1",   // 首段超 3 位
        "http://1.2.3:4318/v1",        // 三段
        "http://1.2.3.4.5/v1",         // 五段
        "http://1.2.3.04/v1",          // 前导零（inet_pton 拒绝形态）
        "http://1.2.3.4.:4318/v1",     // 尾点
        "http://1.2.3.4:4318:9/v1",    // 冒号残留进 host
    };
    for (const char* const url : kInvalid) {
        if (OtlpTraceExporter::resolveEndpoint(url)) {
            FAIL(std::string("should reject: ") + url);
            return;
        }
    }
    PASS();
}

static void test_decodeHttpStatus_variants() {
    TEST("decodeHttpStatus variants");
    if (OtlpTraceExporter::decodeHttpStatus("") != 0) FAIL("empty -> 0");
    if (OtlpTraceExporter::decodeHttpStatus("HTTP/1.1 20") != 0) {
        FAIL("incomplete status line -> 0");
    }
    if (OtlpTraceExporter::decodeHttpStatus("GARBAGE\r\n\r\n") != 0) {
        FAIL("non-status line -> 0");
    }
    if (OtlpTraceExporter::decodeHttpStatus("HTTP/1.1 \r\n") != 0) {
        FAIL("no code after space -> 0");
    }
    if (OtlpTraceExporter::decodeHttpStatus(
            "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}") != 200) {
        FAIL("200 not decoded");
    }
    if (OtlpTraceExporter::decodeHttpStatus(
            "HTTP/1.1 500 Internal Server Error\r\n") != 500) {
        FAIL("500 not decoded");
    }
    PASS();
}

static void test_disabled_by_default() {
    TEST("default config stays disabled and touches no emitter");
    EmitterReset reset;
    SpanCapture capture;
    capture.install();

    OtlpTraceExporter exporter(OtlpTraceExporter::Config{});
    if (exporter.install()) FAIL("disabled install must not succeed");
    if (exporter.installed()) FAIL("disabled install must not mark installed");

    { foundation::SpanScope span("unit.disabled"); static_cast<void>(span); }
    if (capture.spans.size() != 1) FAIL("emitter slot must stay untouched");
    if (exporter.exportedOk() != 0 || exporter.exportFailed() != 0) {
        FAIL("disabled exporter must not count exports");
    }
    PASS();
}

static void test_invalid_endpoint_warns_and_fails_install() {
    TEST("enabled with invalid endpoint warns and refuses to install");
    EmitterReset emitterReset;
    LoggerReset loggerReset;
    SpanCapture capture;
    capture.install();

    OtlpTraceExporter::Config config;
    config.enabled = true;
    config.endpoint = "https://1.2.3.4/v1/traces";
    OtlpTraceExporter exporter(config);
    if (exporter.install()) FAIL("https endpoint must not install");
    if (exporter.installed()) FAIL("failed install must not mark installed");

    bool sawWarn = false;
    for (const auto& record : loggerReset.captured->drain()) {
        if (record.message == "otlp.traces.export.invalid_endpoint") {
            sawWarn = true;
        }
    }
    if (!sawWarn) FAIL("invalid endpoint must warn");

    { foundation::SpanScope span("unit.invalid"); static_cast<void>(span); }
    if (capture.spans.size() != 1) FAIL("emitter slot must stay untouched");
    if (exporter.exportFailed() != 0) FAIL("not installed must not count failures");
    PASS();
}

static void test_unroutable_endpoint_fails_at_connect() {
    TEST("unroutable endpoint fails at connect without waiting out timeout");
    EmitterReset emitterReset;
    LoggerReset loggerReset;

    // 255.255.255.255 是合法 IPv4 字面量（解析通过），但无 SO_BROADCAST
    // 的 ::connect 对广播地址立即 EACCES——TcpConnection 同步建连失败臂，
    // 走「未收到响应」的 PostResult{} 形态。
    OtlpTraceExporter::Config config;
    config.enabled = true;
    config.endpoint = "http://255.255.255.255/v1/traces";
    config.timeout = std::chrono::milliseconds{200};
    config.batchSize = 1;
    config.flushInterval = std::chrono::milliseconds{10};
    config.retryBackoff = std::chrono::milliseconds{10};
    OtlpTraceExporter exporter(config);
    if (!exporter.install()) FAIL("install");

    const auto begin = std::chrono::steady_clock::now();
    { foundation::SpanScope span("unit.unroutable"); static_cast<void>(span); }
    if (!waitUntil([&] { return exporter.exportFailed() == 1; })) {
        FAIL("connect failure must be counted");
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - begin);
    if (exporter.exportFailed() != 1) FAIL("connect failure must count failed");
    if (exporter.exportedOk() != 0) FAIL("nothing can succeed");
    if (elapsed >= std::chrono::milliseconds{200})
        FAIL("connect failure must return before the timeout");
    bool sawWarn = false;
    for (const auto& record : loggerReset.captured->drain()) {
        if (record.message == "otlp.traces.export.failed")
            sawWarn = true;
    }
    if (!sawWarn) FAIL("connect failure must warn");
    PASS();
}

static void test_collector_receives_otlp_json_span() {
    TEST("collector receives OTLP/HTTP JSON span end to end");
    EmitterReset emitterReset;
    LoggerReset loggerReset;
    MockOtlpCollector collector;
    if (!collector.start(MockOtlpCollector::Mode::Reply200)) FAIL("collector start");

    OtlpTraceExporter exporter(endpointConfig(collector.port(), std::chrono::milliseconds{2000}));
    if (!exporter.install()) FAIL("install");

    std::string traceId;
    std::string spanId;
    {
        foundation::SpanScope span("unit.span");
        traceId = span.context().traceId;
        spanId = span.context().spanId;
        span.setAttribute("str_key", std::string("unit"));
        span.setAttribute("n", std::int64_t{42});
        span.setAttribute("d", 0.5);
        span.setAttribute("bt", true);
        span.setAttribute("bf", false);
    }
    if (!waitUntil([&] { return exporter.exportedOk() == 1; })) {
        FAIL("async export must complete");
    }

    const auto requests = collector.requests();
    if (requests.size() != 1) FAIL("expected exactly one POST");
    const std::string& request = requests.front();
    const auto bodyBegin = request.find("\r\n\r\n");
    if (bodyBegin == std::string::npos) FAIL("request must have header terminator");
    const std::string body = request.substr(bodyBegin + 4);

    if (!contains(request, "POST /v1/traces HTTP/1.1\r\n")) FAIL("request line");
    if (!contains(request, "Host: 127.0.0.1:" + std::to_string(collector.port()) + "\r\n")) {
        FAIL("Host header");
    }
    if (!contains(request, "Content-Type: application/json\r\n")) FAIL("content type");
    const auto lengthKey = request.find("Content-Length: ");
    if (lengthKey == std::string::npos) FAIL("content length header");
    const auto length = std::strtoul(request.c_str() + lengthKey + 16, nullptr, 10);
    if (length != body.size()) FAIL("content length must match body size");

    if (!contains(body, "\"traceId\":\"" + traceId + "\"")) FAIL("traceId");
    if (!contains(body, "\"spanId\":\"" + spanId + "\"")) FAIL("spanId");
    if (contains(body, "parentSpanId")) FAIL("root span must omit parentSpanId");
    if (!contains(body, "\"name\":\"unit.span\"")) FAIL("span name");
    if (!contains(body, "\"kind\":1")) FAIL("internal span kind");
    if (!contains(body, "\"startTimeUnixNano\":\"")) FAIL("start nano");
    if (!contains(body, "\"endTimeUnixNano\":\"")) FAIL("end nano");
    if (!contains(body, "\"key\":\"service.name\",\"value\":{\"stringValue\":\"theseed\"}")) {
        FAIL("resource service.name");
    }
    if (!contains(body, "\"scope\":{\"name\":\"theseed.control.tracing\"}")) FAIL("scope name");
    if (!contains(body, "\"key\":\"str_key\",\"value\":{\"stringValue\":\"unit\"}")) FAIL("string attr");
    if (!contains(body, "\"key\":\"n\",\"value\":{\"intValue\":\"42\"}")) FAIL("int64 attr");
    if (!contains(body, "\"key\":\"d\",\"value\":{\"doubleValue\":0.5}")) FAIL("double attr");
    if (!contains(body, "\"key\":\"bt\",\"value\":{\"boolValue\":true}")) FAIL("bool true attr");
    if (!contains(body, "\"key\":\"bf\",\"value\":{\"boolValue\":false}")) FAIL("bool false attr");

    if (exporter.exportedOk() != 1) FAIL("exportedOk must be 1");
    if (exporter.exportFailed() != 0) FAIL("no failures expected");
    for (const auto& record : loggerReset.captured->drain()) {
        // install 时的 otlp.traces.export.enabled 属启动声明（硬要求①），
        // 此处只禁成功导出路径上的失败/告警记录。
        if (record.message.rfind("otlp.", 0) == 0 &&
            record.message != "otlp.traces.export.enabled") {
            FAIL("successful export must not log");
        }
    }
    exporter.uninstall();
    PASS();
}

static void test_child_span_carries_parent_span_id() {
    TEST("child span carries parentSpanId, parent omits it");
    EmitterReset emitterReset;
    MockOtlpCollector collector;
    if (!collector.start(MockOtlpCollector::Mode::Reply200)) FAIL("collector start");

    OtlpTraceExporter exporter(endpointConfig(collector.port(), std::chrono::milliseconds{2000}));
    if (!exporter.install()) FAIL("install");

    std::string parentSpanId;
    std::string childParentSpanId;
    {
        foundation::SpanScope parent("unit.parent");
        parentSpanId = parent.context().spanId;
        {
            foundation::SpanScope child("unit.child");
            childParentSpanId = child.context().parentSpanId;
        }
    }
    if (!waitUntil([&] { return exporter.exportedOk() == 2; })) {
        FAIL("both spans must export");
    }

    const auto requests = collector.requests();
    if (requests.size() != 2) FAIL("expected two POSTs (child then parent)");
    const auto childBodyBegin = requests[0].find("\r\n\r\n");
    const auto parentBodyBegin = requests[1].find("\r\n\r\n");
    if (childBodyBegin == std::string::npos || parentBodyBegin == std::string::npos) {
        FAIL("both requests must be complete");
    }
    const std::string childBody = requests[0].substr(childBodyBegin + 4);
    const std::string parentBody = requests[1].substr(parentBodyBegin + 4);
    if (!contains(childBody, "\"parentSpanId\":\"" + parentSpanId + "\"")) {
        FAIL("child must carry parent span id");
    }
    if (!contains(childBody, "\"name\":\"unit.child\"")) FAIL("child name");
    if (contains(parentBody, "parentSpanId")) FAIL("parent span is root");
    if (!contains(parentBody, "\"name\":\"unit.parent\"")) FAIL("parent name");
    if (exporter.exportedOk() != 2) FAIL("two successful exports");
    PASS();
}

static void test_json_string_escaping() {
    TEST("span name and string attr escape JSON specials");
    EmitterReset emitterReset;
    MockOtlpCollector collector;
    if (!collector.start(MockOtlpCollector::Mode::Reply200)) FAIL("collector start");

    OtlpTraceExporter exporter(endpointConfig(collector.port(), std::chrono::milliseconds{2000}));
    if (!exporter.install()) FAIL("install");

    {
        foundation::SpanScope span("a\"b\\c\nd\te\rf\bg\x01h\fv");
        span.setAttribute("esc", std::string("x\"y\\z\n"));
    }
    if (!waitUntil([&] { return exporter.exportedOk() == 1; })) {
        FAIL("escaped span must export");
    }

    const auto requests = collector.requests();
    if (requests.size() != 1) FAIL("expected one POST");
    const auto bodyBegin = requests.front().find("\r\n\r\n");
    if (bodyBegin == std::string::npos) FAIL("request must be complete");
    const std::string body = requests.front().substr(bodyBegin + 4);

    if (!contains(body, R"(\")")) FAIL("escaped quote");
    if (!contains(body, R"(\\)")) FAIL("escaped backslash");
    if (!contains(body, R"(\n)")) FAIL("escaped newline");
    if (!contains(body, R"(\t)")) FAIL("escaped tab");
    if (!contains(body, R"(\r)")) FAIL("escaped carriage return");
    if (!contains(body, R"(\b)")) FAIL("escaped backspace");
    if (!contains(body, R"(\f)")) FAIL("escaped form feed");
    if (!contains(body, R"(\u0001)")) FAIL("escaped control char");
    if (exporter.exportedOk() != 1) FAIL("export must still succeed");
    PASS();
}

static void test_non_2xx_response_counts_failed_and_warns() {
    TEST("500 response counts as failed and logs warn");
    EmitterReset emitterReset;
    LoggerReset loggerReset;
    MockOtlpCollector collector;
    if (!collector.start(MockOtlpCollector::Mode::Reply500)) FAIL("collector start");

    OtlpTraceExporter exporter(endpointConfig(collector.port(), std::chrono::milliseconds{2000}));
    if (!exporter.install()) FAIL("install");
    { foundation::SpanScope span("unit.reject"); static_cast<void>(span); }
    if (!waitUntil([&] { return exporter.exportFailed() == 1; })) {
        FAIL("retried 500 must count as failed");
    }

    if (exporter.exportedOk() != 0) FAIL("500 must not count as exported");
    if (exporter.exportFailed() != 1) FAIL("500 must count as failed");

    bool sawWarn = false;
    std::int64_t status = 0;
    for (const auto& record : loggerReset.captured->drain()) {
        if (record.message == "otlp.traces.export.failed") {
            sawWarn = true;
            for (const auto& attr : record.attrs) {
                if (attr.key == "http_status") status = std::get<std::int64_t>(attr.value);
            }
        }
    }
    if (!sawWarn) FAIL("failed export must warn");
    if (status != 500) FAIL("warn must carry the http status");
    PASS();
}

static void test_connection_refused_fails_fast() {
    TEST("connection refused fails fast without waiting out the timeout");
    EmitterReset emitterReset;
    LoggerReset loggerReset;

    TcpListener portHolder;
    if (!portHolder.listen("127.0.0.1", 0)) FAIL("port holder listen");
    const auto refusedPort = portHolder.localPort();
    portHolder.close();  // 端口放掉：连接必然被拒

    OtlpTraceExporter exporter(
        endpointConfig(refusedPort, std::chrono::milliseconds{1000}));
    if (!exporter.install()) FAIL("install");
    const auto began = std::chrono::steady_clock::now();
    { foundation::SpanScope span("unit.refused"); static_cast<void>(span); }
    if (!waitUntil([&] { return exporter.exportFailed() == 1; })) {
        FAIL("refused must count as failed");
    }
    const auto elapsed = std::chrono::steady_clock::now() - began;

    if (exporter.exportFailed() != 1) FAIL("refused must count as failed");
    if (elapsed >= std::chrono::milliseconds{1000}) {
        FAIL("refused must fail fast, not wait out the timeout");
    }
    PASS();
}

static void test_silent_collector_times_out() {
    TEST("silent collector fails via timeout");
    EmitterReset emitterReset;
    LoggerReset loggerReset;
    MockOtlpCollector collector;
    if (!collector.start(MockOtlpCollector::Mode::Silent)) FAIL("collector start");

    OtlpTraceExporter exporter(endpointConfig(collector.port(), std::chrono::milliseconds{100}));
    if (!exporter.install()) FAIL("install");
    const auto began = std::chrono::steady_clock::now();
    { foundation::SpanScope span("unit.timeout"); static_cast<void>(span); }
    if (!waitUntil([&] { return exporter.exportFailed() == 1; })) {
        FAIL("timeout must count as failed");
    }
    const auto elapsed = std::chrono::steady_clock::now() - began;

    if (exporter.exportFailed() != 1) FAIL("timeout must count as failed");
    if (elapsed < std::chrono::milliseconds{90}) {
        FAIL("timeout path must respect the deadline");
    }
    if (elapsed > std::chrono::seconds{5}) {
        FAIL("timeout path must not hang far beyond the deadline");
    }
    PASS();
}

static void test_collector_close_and_garbage_fail() {
    TEST("collector closing without a usable response fails");
    EmitterReset emitterReset;
    LoggerReset loggerReset;
    {
        MockOtlpCollector collector;
        if (!collector.start(MockOtlpCollector::Mode::CloseOnAccept)) FAIL("collector start");
        OtlpTraceExporter closer(endpointConfig(collector.port(), std::chrono::milliseconds{2000}));
        if (!closer.install()) FAIL("install");
        { foundation::SpanScope span("unit.closed"); static_cast<void>(span); }
        if (!waitUntil([&] { return closer.exportFailed() == 1; })) {
            FAIL("eof must count as failed");
        }
        closer.uninstall();
        collector.stop();
    }
    {
        MockOtlpCollector collector;
        if (!collector.start(MockOtlpCollector::Mode::GarbageThenClose)) FAIL("collector start");
        OtlpTraceExporter garbage(endpointConfig(collector.port(), std::chrono::milliseconds{2000}));
        if (!garbage.install()) FAIL("install");
        { foundation::SpanScope span("unit.garbage"); static_cast<void>(span); }
        if (!waitUntil([&] { return garbage.exportFailed() == 1; })) {
            FAIL("garbage status must count as failed");
        }
        garbage.uninstall();
        collector.stop();
    }
    PASS();
}

static void test_fragmented_response_still_parses() {
    TEST("fragmented status line still parses to 200");
    EmitterReset emitterReset;
    MockOtlpCollector collector;
    if (!collector.start(MockOtlpCollector::Mode::Fragmented200)) FAIL("collector start");

    OtlpTraceExporter exporter(endpointConfig(collector.port(), std::chrono::milliseconds{2000}));
    if (!exporter.install()) FAIL("install");
    { foundation::SpanScope span("unit.fragmented"); static_cast<void>(span); }
    if (!waitUntil([&] { return exporter.exportedOk() == 1; })) {
        FAIL("fragmented 200 must export");
    }
    if (exporter.exportedOk() != 1) FAIL("fragmented 200 must export");
    PASS();
}

static void test_chained_emitter_and_uninstall_restore() {
    TEST("previous emitter is chained on export and restored on uninstall");
    EmitterReset emitterReset;
    MockOtlpCollector collector;
    if (!collector.start(MockOtlpCollector::Mode::Reply200)) FAIL("collector start");

    SpanCapture capture;
    capture.install();
    OtlpTraceExporter exporter(endpointConfig(collector.port(), std::chrono::milliseconds{2000}));
    if (!exporter.install()) FAIL("install");

    { foundation::SpanScope span("unit.chained"); static_cast<void>(span); }
    if (capture.spans.size() != 1) FAIL("chained emitter must still see spans");
    if (!waitUntil([&] { return exporter.exportedOk() == 1; })) {
        FAIL("exporter must export");
    }
    if (collector.requests().size() != 1) FAIL("exporter must export");

    exporter.uninstall();
    if (exporter.installed()) FAIL("uninstall must clear installed");
    { foundation::SpanScope span("unit.restored"); static_cast<void>(span); }
    if (capture.spans.size() != 2) FAIL("restored emitter must see spans");
    if (collector.requests().size() != 1) FAIL("uninstalled exporter must not export");
    PASS();
}

static void test_double_install_is_idempotent() {
    TEST("double install keeps a single emitter hop");
    EmitterReset emitterReset;
    MockOtlpCollector collector;
    if (!collector.start(MockOtlpCollector::Mode::Reply200)) FAIL("collector start");

    OtlpTraceExporter exporter(endpointConfig(collector.port(), std::chrono::milliseconds{2000}));
    if (!exporter.install()) FAIL("first install");
    if (!exporter.install()) FAIL("second install");
    { foundation::SpanScope span("unit.twice"); static_cast<void>(span); }
    if (!waitUntil([&] { return exporter.exportedOk() == 1; })) {
        FAIL("idempotent install must export once");
    }
    if (collector.requests().size() != 1) FAIL("idempotent install must export once");
    if (exporter.exportedOk() != 1) FAIL("single export count");
    PASS();
}

static void test_destructor_uninstalls() {
    TEST("destructor uninstalls and restores the previous emitter");
    EmitterReset emitterReset;
    MockOtlpCollector collector;
    if (!collector.start(MockOtlpCollector::Mode::Reply200)) FAIL("collector start");

    SpanCapture capture;
    capture.install();
    {
        OtlpTraceExporter exporter(endpointConfig(collector.port(), std::chrono::milliseconds{2000}));
        if (!exporter.install()) FAIL("install");
        { foundation::SpanScope span("unit.dtor"); static_cast<void>(span); }
    }
    { foundation::SpanScope span("unit.after.dtor"); static_cast<void>(span); }

    if (collector.requests().size() != 1) FAIL("exporter must stop after dtor");
    if (capture.spans.size() != 2) FAIL("dtor must restore the previous emitter");
    PASS();
}

static void test_injected_transport_receives_target_and_body() {
    TEST("injected transport receives resolved target and encoded body");
    EmitterReset emitterReset;

    OtlpTraceExporter::Target gotTarget;
    std::string gotBody;
    std::atomic<int> calls{0};
    OtlpTraceExporter::Config config;
    config.enabled = true;
    config.endpoint = "http://10.0.0.2:9443/x/y";
    config.timeout = std::chrono::milliseconds{7};
    config.batchSize = 1;
    config.flushInterval = std::chrono::milliseconds{10};
    OtlpTraceExporter exporter(
        config,
        [&](const OtlpTraceExporter::Target& target, const std::string& body,
            std::chrono::milliseconds /*timeout*/) {
            calls.fetch_add(1);
            gotTarget = target;
            gotBody = body;
            return OtlpTraceExporter::PostResult{true, 200};
        });
    if (!exporter.install()) FAIL("install");

    std::string spanId;
    {
        foundation::SpanScope span("unit.seam");
        spanId = span.context().spanId;
        span.setAttribute("seam", std::string("ok"));
    }
    if (!waitUntil([&] { return exporter.exportedOk() == 1; })) {
        FAIL("injected transport must be called");
    }

    if (calls.load() != 1) FAIL("fake transport must be called once");
    if (gotTarget.host != "10.0.0.2" || gotTarget.port != 9443 ||
        gotTarget.path != "/x/y") {
        FAIL("resolved target mismatch");
    }
    if (!contains(gotBody, "\"spanId\":\"" + spanId + "\"")) FAIL("body spanId");
    if (!contains(gotBody, "\"key\":\"seam\"")) FAIL("body attr");
    if (exporter.exportedOk() != 1) FAIL("injected 200 counts as exported");
    PASS();
}

static void test_encodeTraces_nonfinite_double_as_text() {
    TEST("non-finite doubles export as text, not JSON numbers");
    foundation::Span span;
    span.name = "unit.nan";
    span.context.traceId = std::string(32, 'a');
    span.context.spanId = std::string(16, 'b');
    span.startTime = std::chrono::system_clock::now();
    span.endTime = span.startTime;
    span.attrs.push_back({"nan", std::numeric_limits<double>::quiet_NaN()});
    span.attrs.push_back({"inf", std::numeric_limits<double>::infinity()});

    const std::string body = OtlpTraceExporter::encodeTraces(span, "svc");
    if (contains(body, "doubleValue")) FAIL("non-finite must not be a JSON number");
    if (!contains(body, "\"key\":\"nan\",\"value\":{\"stringValue\":\"nan")) {
        FAIL("nan must export as text");
    }
    if (!contains(body, "\"key\":\"inf\",\"value\":{\"stringValue\":\"inf")) {
        FAIL("inf must export as text");
    }
    if (!contains(body, "\"traceId\":\"" + std::string(32, 'a') + "\"")) {
        FAIL("hand-built span must encode");
    }
    PASS();
}

// --- 异步批量（todo「批量、重试、异步队列后置登记」落地批） ---------------

static void test_batch_accumulates_until_size() {
    TEST("batching: spans accumulate to batchSize in a single POST");
    EmitterReset emitterReset;
    MockOtlpCollector collector;
    if (!collector.start(MockOtlpCollector::Mode::Reply200)) FAIL("collector start");

    OtlpTraceExporter::Config config =
        endpointConfig(collector.port(), std::chrono::milliseconds{2000});
    config.batchSize = 3;
    config.flushInterval = std::chrono::seconds{60};  // 周期不可能先到
    OtlpTraceExporter exporter(config);
    if (!exporter.install()) FAIL("install");

    {
        foundation::SpanScope a("unit.batch.a");
        static_cast<void>(a);
    }
    {
        foundation::SpanScope b("unit.batch.b");
        static_cast<void>(b);
    }
    {
        foundation::SpanScope c("unit.batch.c");
        static_cast<void>(c);
    }
    if (!waitUntil([&] { return exporter.exportedOk() == 3; })) {
        FAIL("three spans must export");
    }

    const auto requests = collector.requests();
    if (requests.size() != 1) FAIL("batch must share a single POST");
    const auto bodyBegin = requests.front().find("\r\n\r\n");
    if (bodyBegin == std::string::npos) FAIL("request must be complete");
    const std::string body = requests.front().substr(bodyBegin + 4);
    if (countOf(body, "\"name\":\"unit.batch.") != 3) FAIL("three span names");
    if (countOf(body, "\"traceId\":") != 3) FAIL("three spans in payload");
    if (countOf(body, "\"resource\":") != 1) FAIL("resource emitted once");
    if (!bracesBalanced(body)) FAIL("batch payload must balance braces");
    PASS();
}

static void test_flush_interval_exports_partial_batch() {
    TEST("flush interval exports a partial batch and idles without POSTs");
    EmitterReset emitterReset;
    MockOtlpCollector collector;
    if (!collector.start(MockOtlpCollector::Mode::Reply200)) FAIL("collector start");

    OtlpTraceExporter::Config config =
        endpointConfig(collector.port(), std::chrono::milliseconds{2000});
    config.batchSize = 32;  // 单 span 永远满不了批 → 只能由兜底周期触发
    config.flushInterval = std::chrono::milliseconds{50};
    OtlpTraceExporter exporter(config);
    if (!exporter.install()) FAIL("install");

    { foundation::SpanScope span("unit.partial"); static_cast<void>(span); }
    if (!waitUntil([&] { return exporter.exportedOk() == 1; })) {
        FAIL("periodic flush must export the lone span");
    }
    if (collector.requests().size() != 1) FAIL("one POST for one span");

    // 空队列静默：周期再过几轮也不得凭空发包（无 span 零请求）。
    std::this_thread::sleep_for(std::chrono::milliseconds{150});
    if (collector.requests().size() != 1) FAIL("idle exporter must not POST");
    PASS();
}

static void test_retry_recovers_after_transient_failures() {
    TEST("retry: transient failures recover within maxAttempts, zero warn");
    EmitterReset emitterReset;
    LoggerReset loggerReset;

    std::atomic<int> calls{0};
    OtlpTraceExporter::Config config;
    config.enabled = true;
    config.endpoint = "http://10.0.0.2:9443/x/y";
    config.batchSize = 1;
    config.flushInterval = std::chrono::milliseconds{10};
    config.maxAttempts = 3;
    config.retryBackoff = std::chrono::milliseconds{1};
    OtlpTraceExporter exporter(
        config,
        [&](const OtlpTraceExporter::Target&, const std::string&,
            std::chrono::milliseconds) {
            const int attempt = calls.fetch_add(1);
            return attempt < 2 ? OtlpTraceExporter::PostResult{false, 500}
                               : OtlpTraceExporter::PostResult{true, 200};
        });
    if (!exporter.install()) FAIL("install");

    { foundation::SpanScope span("unit.retry.ok"); static_cast<void>(span); }
    if (!waitUntil([&] { return exporter.exportedOk() == 1; })) {
        FAIL("third attempt must succeed");
    }
    if (calls.load() != 3) FAIL("two failures plus one success = three attempts");
    if (exporter.exportFailed() != 0) FAIL("recovered batch must not count failed");
    for (const auto& record : loggerReset.captured->drain()) {
        if (record.message == "otlp.traces.export.failed") {
            FAIL("recovered export must not warn");
        }
    }
    PASS();
}

static void test_retry_exhaustion_warns_once_for_the_batch() {
    TEST("retry: exhaustion drops the batch once with a single warn");
    EmitterReset emitterReset;
    LoggerReset loggerReset;

    std::atomic<int> calls{0};
    OtlpTraceExporter::Config config;
    config.enabled = true;
    config.endpoint = "http://10.0.0.2:9443/x/y";
    config.batchSize = 1;
    config.flushInterval = std::chrono::milliseconds{10};
    config.maxAttempts = 3;
    config.retryBackoff = std::chrono::milliseconds{1};
    OtlpTraceExporter exporter(
        config,
        [&](const OtlpTraceExporter::Target&, const std::string&,
            std::chrono::milliseconds) {
            calls.fetch_add(1);
            return OtlpTraceExporter::PostResult{false, 503};
        });
    if (!exporter.install()) FAIL("install");

    { foundation::SpanScope span("unit.retry.fail"); static_cast<void>(span); }
    if (!waitUntil([&] { return exporter.exportFailed() == 1; })) {
        FAIL("exhausted retry must count the batch");
    }
    if (calls.load() != 3) FAIL("exactly maxAttempts attempts");
    if (exporter.exportedOk() != 0) FAIL("nothing succeeded");

    int warnCount = 0;
    std::int64_t status = 0;
    for (const auto& record : loggerReset.captured->drain()) {
        if (record.message != "otlp.traces.export.failed") continue;
        ++warnCount;
        for (const auto& attr : record.attrs) {
            if (attr.key == "http_status") status = std::get<std::int64_t>(attr.value);
        }
    }
    if (warnCount != 1) FAIL("exhaustion must warn exactly once");
    if (status != 503) FAIL("warn carries the last attempt status");
    PASS();
}

static void test_queue_overflow_drops_new_spans() {
    TEST("queue overflow drops new spans and counts them as failed");
    EmitterReset emitterReset;

    // 闸门：seam 进入后阻塞到 release——确保 worker 拿走首 span 后停在
    // 传输里，期间灌满队列再溢出。wait_for 自带 2s 上界，FAIL 早退不
    // 会在析构 join 上卡死。
    std::mutex gateMutex;
    std::condition_variable gateCv;
    bool released = false;
    int entered = 0;

    OtlpTraceExporter::Config config;
    config.enabled = true;
    config.endpoint = "http://10.0.0.2:9443/x/y";
    config.batchSize = 1;
    config.flushInterval = std::chrono::milliseconds{10};
    config.maxAttempts = 1;
    config.maxQueue = 2;
    OtlpTraceExporter exporter(
        config,
        [&](const OtlpTraceExporter::Target&, const std::string&,
            std::chrono::milliseconds) {
            std::unique_lock lock(gateMutex);
            ++entered;
            gateCv.notify_all();
            static_cast<void>(gateCv.wait_for(lock, std::chrono::seconds{2},
                                              [&] { return released; }));
            return OtlpTraceExporter::PostResult{true, 200};
        });
    if (!exporter.install()) FAIL("install");

    { foundation::SpanScope span("unit.queue.1"); static_cast<void>(span); }
    if (!waitUntil([&] {
            const std::lock_guard lock(gateMutex);
            return entered >= 1;
        })) {
        {
            const std::lock_guard lock(gateMutex);
            released = true;
        }
        gateCv.notify_all();
        FAIL("worker must enter transport for the first span");
    }
    // worker 已停在传输（队列空）：s2/s3 占满上限 2，s4 溢出被丢。
    { foundation::SpanScope span("unit.queue.2"); static_cast<void>(span); }
    { foundation::SpanScope span("unit.queue.3"); static_cast<void>(span); }
    { foundation::SpanScope span("unit.queue.4"); static_cast<void>(span); }
    {
        const std::lock_guard lock(gateMutex);
        released = true;
    }
    gateCv.notify_all();

    if (!waitUntil([&] {
            return exporter.exportedOk() == 3 && exporter.exportFailed() == 1;
        })) {
        FAIL("three spans export and one overflows");
    }
    PASS();
}

static void test_uninstall_drains_pending_spans() {
    TEST("uninstall drains the pending batch before joining");
    EmitterReset emitterReset;
    MockOtlpCollector collector;
    if (!collector.start(MockOtlpCollector::Mode::Reply200)) FAIL("collector start");

    OtlpTraceExporter::Config config =
        endpointConfig(collector.port(), std::chrono::milliseconds{2000});
    config.batchSize = 10;                          // 满不了批
    config.flushInterval = std::chrono::seconds{60};  // 周期不可能先到
    OtlpTraceExporter exporter(config);
    if (!exporter.install()) FAIL("install");

    { foundation::SpanScope span("unit.drain"); static_cast<void>(span); }
    // 此刻 worker 必然还在等（batch 未满、周期未到）——卸载必须排空，
    // 且 join 返回即同步收口（后续断言无需轮询）。
    exporter.uninstall();
    if (collector.requests().size() != 1) FAIL("pending span must flush on uninstall");
    if (exporter.exportedOk() != 1) FAIL("drained span counts as exported");
    if (exporter.installed()) FAIL("uninstall must clear installed");
    PASS();
}

static void test_stopping_drain_skips_retries() {
    TEST("stopping drain posts once without retry");
    EmitterReset emitterReset;
    LoggerReset loggerReset;

    std::atomic<int> calls{0};
    OtlpTraceExporter::Config config;
    config.enabled = true;
    config.endpoint = "http://10.0.0.2:9443/x/y";
    config.batchSize = 10;
    config.flushInterval = std::chrono::seconds{60};
    config.maxAttempts = 3;
    config.retryBackoff = std::chrono::milliseconds{1};
    OtlpTraceExporter exporter(
        config,
        [&](const OtlpTraceExporter::Target&, const std::string&,
            std::chrono::milliseconds) {
            calls.fetch_add(1);
            return OtlpTraceExporter::PostResult{false, 503};
        });
    if (!exporter.install()) FAIL("install");

    { foundation::SpanScope span("unit.stop.drain"); static_cast<void>(span); }
    exporter.uninstall();
    if (calls.load() != 1) FAIL("drain must not retry (exit first)");
    if (exporter.exportFailed() != 1) FAIL("drained batch counts as failed");
    PASS();
}

static void test_reinstall_after_uninstall_resumes() {
    TEST("reinstall after uninstall resets shutdown and exports again");
    EmitterReset emitterReset;
    MockOtlpCollector collector;
    if (!collector.start(MockOtlpCollector::Mode::Reply200)) FAIL("collector start");

    OtlpTraceExporter exporter(
        endpointConfig(collector.port(), std::chrono::milliseconds{2000}));
    if (!exporter.install()) FAIL("first install");
    { foundation::SpanScope span("unit.reinstall.a"); static_cast<void>(span); }
    if (!waitUntil([&] { return exporter.exportedOk() == 1; })) {
        FAIL("first export must complete");
    }
    exporter.uninstall();

    if (!exporter.install()) FAIL("second install");
    { foundation::SpanScope span("unit.reinstall.b"); static_cast<void>(span); }
    if (!waitUntil([&] { return exporter.exportedOk() == 2; })) {
        FAIL("second export must complete after reinstall");
    }
    exporter.uninstall();
    if (collector.requests().size() != 2) FAIL("two rounds = two POSTs");
    PASS();
}

static void test_degenerate_config_clamps_and_logs() {
    TEST("zeroed batch/attempts/queue config clamps; interval zero allowed");
    EmitterReset emitterReset;
    LoggerReset loggerReset;
    MockOtlpCollector collector;
    if (!collector.start(MockOtlpCollector::Mode::Reply200)) FAIL("collector start");

    OtlpTraceExporter::Config config =
        endpointConfig(collector.port(), std::chrono::milliseconds{2000});
    config.batchSize = 0;
    config.flushInterval = std::chrono::milliseconds{0};
    config.maxAttempts = 0;
    config.maxQueue = 0;
    OtlpTraceExporter exporter(config);
    if (!exporter.install()) FAIL("install");

    bool sawLog = false;
    std::int64_t loggedBatch = 0;
    std::int64_t loggedAttempts = 0;
    std::int64_t loggedInterval = -1;
    for (const auto& record : loggerReset.captured->drain()) {
        if (record.message != "otlp.traces.export.enabled") continue;
        sawLog = true;
        for (const auto& attr : record.attrs) {
            if (attr.key == "batch_size") loggedBatch = std::get<std::int64_t>(attr.value);
            if (attr.key == "max_attempts") loggedAttempts = std::get<std::int64_t>(attr.value);
            if (attr.key == "flush_interval_ms") loggedInterval = std::get<std::int64_t>(attr.value);
        }
    }
    if (!sawLog) FAIL("enabled log must declare the batch face");
    if (loggedBatch != 1) FAIL("zero batchSize must clamp to 1");
    if (loggedAttempts != 1) FAIL("zero maxAttempts must clamp to 1");
    if (loggedInterval != 0) FAIL("flush interval zero stays (immediate)");

    { foundation::SpanScope span("unit.degenerate"); static_cast<void>(span); }
    if (!waitUntil([&] { return exporter.exportedOk() == 1; })) {
        FAIL("clamped config must still export");
    }
    if (collector.requests().size() != 1) FAIL("one POST");
    PASS();
}

int main() {
    TcpConnection::globalInit();

    test_resolveEndpoint_parses_scheme_host_port_path();
    test_resolveEndpoint_rejects_invalid_urls();
    test_decodeHttpStatus_variants();
    test_disabled_by_default();
    test_invalid_endpoint_warns_and_fails_install();
    test_unroutable_endpoint_fails_at_connect();
    test_collector_receives_otlp_json_span();
    test_child_span_carries_parent_span_id();
    test_json_string_escaping();
    test_non_2xx_response_counts_failed_and_warns();
    test_connection_refused_fails_fast();
    test_silent_collector_times_out();
    test_collector_close_and_garbage_fail();
    test_fragmented_response_still_parses();
    test_chained_emitter_and_uninstall_restore();
    test_double_install_is_idempotent();
    test_destructor_uninstalls();
    test_injected_transport_receives_target_and_body();
    test_encodeTraces_nonfinite_double_as_text();
    test_batch_accumulates_until_size();
    test_flush_interval_exports_partial_batch();
    test_retry_recovers_after_transient_failures();
    test_retry_exhaustion_warns_once_for_the_batch();
    test_queue_overflow_drops_new_spans();
    test_uninstall_drains_pending_spans();
    test_stopping_drain_skips_retries();
    test_reinstall_after_uninstall_resumes();
    test_degenerate_config_clamps_and_logs();

    std::cout << "  passed=" << testsPassed << " failed=" << testsFailed << "\n";
    return testsFailed == 0 ? 0 : 1;
}
