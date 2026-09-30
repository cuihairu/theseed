// OtlpMetricsExporter 测试：默认关闭口径、启用启动日志的 data_scope 声
// 明、OTLP/HTTP JSON 编码（counter→累积 sum / gauge / 累积直方图、非有
// 限 sum 省略、空批次）、回环收集端 e2e（200/500/静默超时/先关/垃圾状
// 态行/分片响应/拒绝连接）、interval 到期门、失败不重试、传输接缝。
// 回环 TCP 测试同仓内惯例门控 Linux（见 tests/runtime/CMakeLists）。
#include "theseed/control/machine/OtlpMetricsExporter.h"

#include "theseed/foundation/Logger.h"
#include "theseed/runtime/TcpConnection.h"
#include "theseed/runtime/TcpListener.h"

#include <atomic>
#include <chrono>
#include <cmath>
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
using machine::OtlpMetricsExporter;
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

// 捕获日志假件（单线程：导出与断言同线程）。
class CapturingLoggerImpl final : public foundation::ILogger {
public:
    void log(foundation::LogRecord record) override {
        records_.push_back(std::move(record));
    }
    void setLevel(foundation::LogLevel level) override { level_ = level; }
    foundation::LogLevel level() const override { return level_; }

    std::vector<foundation::LogRecord> drain() { return std::move(records_); }

private:
    foundation::LogLevel level_ = foundation::LogLevel::Debug;
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

// 注册表是进程级单例：注册用例的前后都清空，互不串味。
struct RegistryReset {
    RegistryReset() { foundation::MetricsRegistry::instance().reset(); }
    ~RegistryReset() { foundation::MetricsRegistry::instance().reset(); }
};

static bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

static std::size_t countOf(std::string_view haystack, std::string_view needle) {
    std::size_t count = 0;
    for (auto pos = haystack.find(needle); pos != std::string_view::npos;
         pos = haystack.find(needle, pos + needle.size())) {
        ++count;
    }
    return count;
}

static OtlpMetricsExporter::Config endpointConfig(
    std::uint16_t port, std::chrono::milliseconds timeout) {
    OtlpMetricsExporter::Config config;
    config.enabled = true;
    config.endpoint =
        "http://127.0.0.1:" + std::to_string(port) + "/v1/metrics";
    config.timeout = timeout;
    return config;
}

// 回环 OTLP 收集端：独立线程泵循环（客户端在导出调用里同步阻塞 POST，
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
    // 每连接状态：rx 缓冲与已记录/已回应旗标（多笔导出 = 多条连接）。
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

static void test_default_config_stays_disabled() {
    TEST("default config stays disabled and touches nothing");
    LoggerReset loggerReset;
    RegistryReset registryReset;

    OtlpMetricsExporter exporter(OtlpMetricsExporter::Config{});
    if (exporter.install()) FAIL("default config must not install");
    if (exporter.installed()) FAIL("failed install must not mark installed");
    if (exporter.exportOnce()) FAIL("disabled exporter must not export");
    if (exporter.exportIfDue(std::chrono::steady_clock::now()))
        FAIL("disabled exporter must not export on tick");
    if (exporter.exportedOk() != 0 || exporter.exportFailed() != 0)
        FAIL("disabled exporter must not count anything");
    for (const auto& record : loggerReset.captured->drain()) {
        if (record.message.rfind("otlp.", 0) == 0)
            FAIL("disabled exporter must not log");
    }
    PASS();
}

static void test_invalid_endpoint_warns_and_refuses() {
    TEST("enabled with invalid endpoint warns and refuses to install");
    LoggerReset loggerReset;

    OtlpMetricsExporter::Config config;
    config.enabled = true;
    config.endpoint = "https://1.2.3.4/v1/metrics";
    OtlpMetricsExporter exporter(config);
    if (exporter.install()) FAIL("https endpoint must not install");
    if (exporter.installed()) FAIL("failed install must not mark installed");
    if (exporter.exportOnce()) FAIL("failed install must not export");

    bool sawWarn = false;
    for (const auto& record : loggerReset.captured->drain()) {
        if (record.message == "otlp.metrics.export.invalid_endpoint") {
            sawWarn = true;
        }
    }
    if (!sawWarn) FAIL("invalid endpoint must warn");
    PASS();
}

static void test_enabled_install_logs_data_scope_idempotently() {
    TEST("enabled install logs data_scope once and stays idempotent");
    LoggerReset loggerReset;

    OtlpMetricsExporter::Config config;
    config.enabled = true;
    config.endpoint = "http://10.0.0.2:4318/v1/metrics";
    config.interval = std::chrono::milliseconds{1234};
    OtlpMetricsExporter exporter(config);
    if (!exporter.install()) FAIL("install");
    if (!exporter.install()) FAIL("double install must stay true");

    bool sawEnabled = false;
    int enabledLogs = 0;
    std::string scope;
    bool sawInterval = false;
    for (const auto& record : loggerReset.captured->drain()) {
        // drain() 按值返回：断言所需字段必须在循环内就地取出。
        if (record.message != "otlp.metrics.export.enabled")
            continue;
        ++enabledLogs;
        sawEnabled = true;
        for (const auto& attr : record.attrs) {
            if (attr.key == "data_scope")
                scope = std::get<std::string>(attr.value);
            if (attr.key == "interval_ms" &&
                std::get<std::int64_t>(attr.value) == 1234)
                sawInterval = true;
        }
    }
    if (!sawEnabled) FAIL("install must log otlp.metrics.export.enabled");
    if (enabledLogs != 1) FAIL("idempotent install must log exactly once");
    if (!sawInterval) FAIL("enabled log must carry the interval");
    if (scope.find("counter/gauge values") == std::string::npos ||
        scope.find("bucketCounts") == std::string::npos ||
        scope.find("explicitBounds") == std::string::npos ||
        scope.find("spans/traces") == std::string::npos ||
        scope.find("not exported") == std::string::npos)
        FAIL("enabled log must state the exported data scope");
    PASS();
}

static void test_collector_receives_otlp_metrics_end_to_end() {
    TEST("collector receives OTLP metrics batch end to end");
    LoggerReset loggerReset;
    RegistryReset registryReset;
    MockOtlpCollector collector;
    if (!collector.start(MockOtlpCollector::Mode::Reply200)) FAIL("collector start");

    OtlpMetricsExporter exporter(
        endpointConfig(collector.port(), std::chrono::milliseconds{2000}));
    if (!exporter.install()) FAIL("install");

    foundation::MetricsRegistry::instance()
        .counter("otlp_mx_c", "test counter")
        .increment(7);
    foundation::MetricsRegistry::instance()
        .gauge("otlp_mx_g", "test gauge")
        .set(-3);
    foundation::MetricsRegistry::instance()
        .histogram("otlp_mx_h", {1.0, 2.0}, "test histogram")
        .observe(0.5);
    foundation::MetricsRegistry::instance()
        .histogram("otlp_mx_h", {1.0, 2.0})
        .observe(1.5);
    foundation::MetricsRegistry::instance()
        .histogram("otlp_mx_h", {1.0, 2.0})
        .observe(3.0);

    // 进程级注册表还含 transport 等基础设施指标：笔数以 collect() 为准。
    const auto expectedSamples =
        foundation::MetricsRegistry::instance().collect().size();
    if (expectedSamples < 3) FAIL("registry must hold at least the three test metrics");

    if (!exporter.exportOnce()) FAIL("exportOnce must attempt");
    const auto requests = collector.requests();
    if (requests.size() != 1) FAIL("expected exactly one POST");
    const std::string& request = requests.front();
    const auto bodyBegin = request.find("\r\n\r\n");
    if (bodyBegin == std::string::npos) FAIL("request must have header terminator");
    const std::string body = request.substr(bodyBegin + 4);

    if (!contains(request, "POST /v1/metrics HTTP/1.1\r\n")) FAIL("request line");
    if (!contains(request,
                  "Host: 127.0.0.1:" + std::to_string(collector.port()) + "\r\n")) {
        FAIL("Host header");
    }
    if (!contains(request, "Content-Type: application/json\r\n")) FAIL("content type");
    const auto lengthKey = request.find("Content-Length: ");
    if (lengthKey == std::string::npos) FAIL("content length header");
    const auto length = std::strtoul(request.c_str() + lengthKey + 16, nullptr, 10);
    if (length != body.size()) FAIL("content length must match body size");

    if (!contains(body,
                  "\"key\":\"service.name\",\"value\":{\"stringValue\":\"theseed\"}")) {
        FAIL("resource service.name");
    }
    if (!contains(body, "\"scopeMetrics\":[{\"scope\":{\"name\":"
                        "\"theseed.control.metrics\"}")) FAIL("scopeMetrics scope");
    // 每笔指标共享同一采集时刻（timeUnixNano 出现数 = 快照样本数）。
    if (countOf(body, "\"timeUnixNano\":\"") != expectedSamples)
        FAIL("one timeUnixNano per sample");
    if (!contains(body, "\"name\":\"otlp_mx_c\",\"description\":\"test counter\","
                        "\"sum\":{\"aggregationTemporality\":2,\"isMonotonic\":true,"
                        "\"dataPoints\":[{\"timeUnixNano\":\""))
        FAIL("counter as cumulative monotonic sum");
    if (!contains(body, "\"asInt\":\"7\"")) FAIL("counter value asInt");
    if (!contains(body, "\"name\":\"otlp_mx_g\",\"description\":\"test gauge\","
                        "\"gauge\":{\"dataPoints\":[{\"timeUnixNano\":\""))
        FAIL("gauge as gauge");
    if (!contains(body, "\"asInt\":\"-3\"")) FAIL("negative gauge value asInt");
    if (!contains(body, "\"name\":\"otlp_mx_h\",\"description\":\"test histogram\","
                        "\"histogram\":{\"aggregationTemporality\":2"))
        FAIL("histogram as cumulative histogram");
    if (!contains(body, "\"count\":\"3\"")) FAIL("histogram count");
    if (!contains(body, "\"sum\":5")) FAIL("histogram finite sum");
    // 累积语义：≤1.0 一笔、≤2.0 两笔、+Inf 三笔。
    if (!contains(body, "\"bucketCounts\":[\"1\",\"2\",\"3\"]"))
        FAIL("cumulative bucket counts");
    if (!contains(body, "\"explicitBounds\":[1,2]")) FAIL("explicit bounds");

    if (exporter.exportedOk() != 1) FAIL("exportedOk must be 1");
    if (exporter.exportFailed() != 0) FAIL("no failures expected");
    for (const auto& record : loggerReset.captured->drain()) {
        // install 时的 otlp.metrics.export.enabled 属启动声明（硬要求①），
        // 此处只禁成功导出路径上的失败/告警记录。
        if (record.message.rfind("otlp.", 0) == 0 &&
            record.message != "otlp.metrics.export.enabled") {
            FAIL("successful export must not log");
        }
    }
    exporter.uninstall();
    PASS();
}

static void test_uninstall_stops_exporting() {
    TEST("uninstall stops exporting and no-ops before install");
    LoggerReset loggerReset;
    RegistryReset registryReset;
    MockOtlpCollector collector;
    if (!collector.start(MockOtlpCollector::Mode::Reply200)) FAIL("collector start");

    OtlpMetricsExporter fresh(OtlpMetricsExporter::Config{});
    fresh.uninstall();  // 未安装形态：必须无副作用空转
    if (fresh.installed()) FAIL("uninstall before install must not install");

    OtlpMetricsExporter exporter(
        endpointConfig(collector.port(), std::chrono::milliseconds{2000}));
    if (!exporter.install()) FAIL("install");
    if (!exporter.exportOnce()) FAIL("export");
    if (exporter.exportedOk() != 1) FAIL("export must succeed");
    exporter.uninstall();
    if (exporter.installed()) FAIL("uninstall must clear the flag");
    if (exporter.exportOnce()) FAIL("uninstalled exporter must not export");
    if (exporter.exportIfDue(std::chrono::steady_clock::now()))
        FAIL("uninstalled exporter must not export on tick");
    if (collector.requests().size() != 1) FAIL("uninstall must stop the wire");
    PASS();
}

static void test_destructor_uninstalls() {
    TEST("destructor uninstalls");
    RegistryReset registryReset;
    MockOtlpCollector collector;
    if (!collector.start(MockOtlpCollector::Mode::Reply200)) FAIL("collector start");
    {
        OtlpMetricsExporter exporter(
            endpointConfig(collector.port(), std::chrono::milliseconds{2000}));
        if (!exporter.install()) FAIL("install");
        if (!exporter.exportOnce()) FAIL("export");
    }
    if (collector.requests().size() != 1) FAIL("export before dtor");

    OtlpMetricsExporter::Config after;
    OtlpMetricsExporter afterExporter(after);
    if (afterExporter.exportOnce()) FAIL("dtor must have uninstalled");
    PASS();
}

static void test_interval_gate_and_zero_interval() {
    TEST("interval gates exportIfDue; interval zero exports every call");
    RegistryReset registryReset;
    MockOtlpCollector collector;
    if (!collector.start(MockOtlpCollector::Mode::Reply200)) FAIL("collector start");

    OtlpMetricsExporter::Config config =
        endpointConfig(collector.port(), std::chrono::milliseconds{2000});
    config.interval = std::chrono::milliseconds{100};
    OtlpMetricsExporter exporter(config);
    if (!exporter.install()) FAIL("install");

    const auto t0 = std::chrono::steady_clock::now();
    if (!exporter.exportIfDue(t0)) FAIL("first call must be due");
    if (exporter.exportIfDue(t0 + std::chrono::milliseconds{50}))
        FAIL("call within interval must be gated");
    if (collector.requests().size() != 1) FAIL("gated call must not post");
    if (!exporter.exportIfDue(t0 + std::chrono::milliseconds{101}))
        FAIL("call past interval must export");
    if (collector.requests().size() != 2) FAIL("due call must post");

    // interval ≤ 0 = 每次调用都导出（配置口径的显式 degenerate 形态）。
    OtlpMetricsExporter::Config eager =
        endpointConfig(collector.port(), std::chrono::milliseconds{2000});
    eager.interval = std::chrono::milliseconds{0};
    OtlpMetricsExporter eagerExporter(eager);
    if (!eagerExporter.install()) FAIL("eager install");
    if (!eagerExporter.exportIfDue(t0) || !eagerExporter.exportIfDue(t0))
        FAIL("zero interval must export on every call");
    if (collector.requests().size() != 4) FAIL("eager calls must post");
    PASS();
}

static void test_500_counts_failed_with_status() {
    TEST("500 response counts as failed and logs warn with status");
    LoggerReset loggerReset;
    RegistryReset registryReset;
    MockOtlpCollector collector;
    if (!collector.start(MockOtlpCollector::Mode::Reply500)) FAIL("collector start");

    OtlpMetricsExporter exporter(
        endpointConfig(collector.port(), std::chrono::milliseconds{2000}));
    if (!exporter.install()) FAIL("install");
    if (!exporter.exportOnce()) FAIL("exportOnce must attempt");
    if (exporter.exportedOk() != 0) FAIL("500 must not count ok");
    if (exporter.exportFailed() != 1) FAIL("500 must count failed");

    bool sawStatus = false;
    for (const auto& record : loggerReset.captured->drain()) {
        if (record.message != "otlp.metrics.export.failed")
            continue;
        for (const auto& attr : record.attrs) {
            if (attr.key == "http_status" &&
                std::get<std::int64_t>(attr.value) == 500)
                sawStatus = true;
        }
    }
    if (!sawStatus) FAIL("warn must carry the http status");
    PASS();
}

static void test_refused_endpoint_fails_fast() {
    TEST("connection refused fails fast without waiting out the timeout");
    RegistryReset registryReset;

    theseed::runtime::TcpListener portHolder;
    if (!portHolder.listen("127.0.0.1", 0)) FAIL("port holder listen");
    const auto refusedPort = portHolder.localPort();
    portHolder.close();

    OtlpMetricsExporter::Config config =
        endpointConfig(refusedPort, std::chrono::milliseconds{2000});
    OtlpMetricsExporter exporter(config);
    if (!exporter.install()) FAIL("install");

    const auto begin = std::chrono::steady_clock::now();
    if (!exporter.exportOnce()) FAIL("exportOnce must attempt");
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - begin);
    if (exporter.exportFailed() != 1) FAIL("refused must count failed");
    if (elapsed >= std::chrono::milliseconds{2000})
        FAIL("refused must fail before the timeout");
    PASS();
}

static void test_silent_collector_fails_via_timeout() {
    TEST("silent collector fails via timeout");
    RegistryReset registryReset;
    MockOtlpCollector collector;
    if (!collector.start(MockOtlpCollector::Mode::Silent)) FAIL("collector start");

    OtlpMetricsExporter exporter(
        endpointConfig(collector.port(), std::chrono::milliseconds{100}));
    if (!exporter.install()) FAIL("install");

    const auto begin = std::chrono::steady_clock::now();
    if (!exporter.exportOnce()) FAIL("exportOnce must attempt");
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - begin);
    if (exporter.exportFailed() != 1) FAIL("timeout must count failed");
    if (elapsed < std::chrono::milliseconds{90})
        FAIL("timeout arm must wait out the timeout");
    PASS();
}

static void test_collector_closing_without_usable_response_fails() {
    TEST("collector closing without a usable response fails");
    RegistryReset registryReset;
    MockOtlpCollector collector;
    if (!collector.start(MockOtlpCollector::Mode::CloseOnAccept))
        FAIL("collector start");

    OtlpMetricsExporter exporter(
        endpointConfig(collector.port(), std::chrono::milliseconds{2000}));
    if (!exporter.install()) FAIL("install");
    if (!exporter.exportOnce()) FAIL("exportOnce must attempt");
    if (exporter.exportFailed() != 1) FAIL("EOF must count failed");
    PASS();
}

static void test_garbage_response_fails_with_status_zero() {
    TEST("garbage status line fails with status 0");
    LoggerReset loggerReset;
    RegistryReset registryReset;
    MockOtlpCollector collector;
    if (!collector.start(MockOtlpCollector::Mode::GarbageThenClose))
        FAIL("collector start");

    OtlpMetricsExporter exporter(
        endpointConfig(collector.port(), std::chrono::milliseconds{2000}));
    if (!exporter.install()) FAIL("install");
    if (!exporter.exportOnce()) FAIL("exportOnce must attempt");
    if (exporter.exportFailed() != 1) FAIL("garbage must count failed");

    bool sawZeroStatus = false;
    for (const auto& record : loggerReset.captured->drain()) {
        if (record.message != "otlp.metrics.export.failed")
            continue;
        for (const auto& attr : record.attrs) {
            if (attr.key == "http_status" &&
                std::get<std::int64_t>(attr.value) == 0)
                sawZeroStatus = true;
        }
    }
    if (!sawZeroStatus) FAIL("garbage must warn with status 0");
    PASS();
}

static void test_fragmented_status_line_still_parses() {
    TEST("fragmented status line still parses to 200");
    RegistryReset registryReset;
    MockOtlpCollector collector;
    if (!collector.start(MockOtlpCollector::Mode::Fragmented200))
        FAIL("collector start");

    OtlpMetricsExporter exporter(
        endpointConfig(collector.port(), std::chrono::milliseconds{2000}));
    if (!exporter.install()) FAIL("install");
    if (!exporter.exportOnce()) FAIL("exportOnce must attempt");
    if (exporter.exportedOk() != 1) FAIL("fragmented 200 must export");
    PASS();
}

static void test_failed_export_does_not_retry() {
    TEST("failed export does not retry within a call");
    RegistryReset registryReset;
    MockOtlpCollector collector;
    if (!collector.start(MockOtlpCollector::Mode::Reply500)) FAIL("collector start");

    OtlpMetricsExporter exporter(
        endpointConfig(collector.port(), std::chrono::milliseconds{2000}));
    if (!exporter.install()) FAIL("install");
    if (!exporter.exportOnce()) FAIL("exportOnce must attempt");
    if (exporter.exportFailed() != 1) FAIL("first failure");
    if (collector.requests().size() != 1)
        FAIL("failure must not retry inside the export call");
    PASS();
}

static void test_injected_transport_receives_target_and_body() {
    TEST("injected transport receives resolved target and encoded body");
    RegistryReset registryReset;

    OtlpTraceExporter::Target gotTarget;
    std::string gotBody;
    int calls = 0;
    OtlpMetricsExporter::Config config;
    config.enabled = true;
    config.endpoint = "http://10.0.0.2:9443/x/y";
    config.timeout = std::chrono::milliseconds{7};
    OtlpMetricsExporter exporter(
        config,
        [&](const OtlpTraceExporter::Target& target, const std::string& body,
            std::chrono::milliseconds /*timeout*/) {
            ++calls;
            gotTarget = target;
            gotBody = body;
            return OtlpTraceExporter::PostResult{true, 200};
        });
    if (!exporter.install()) FAIL("install");
    if (!exporter.exportOnce()) FAIL("exportOnce must attempt");

    if (calls != 1) FAIL("fake transport must be called once");
    if (gotTarget.host != "10.0.0.2" || gotTarget.port != 9443 ||
        gotTarget.path != "/x/y") {
        FAIL("resolved target mismatch");
    }
    if (!contains(gotBody, "\"scopeMetrics\":[{\"scope\":{\"name\":"
                           "\"theseed.control.metrics\"}")) FAIL("body scope");
    if (exporter.exportedOk() != 1) FAIL("injected 200 counts as exported");
    PASS();
}

static void test_encode_counter_and_gauge_direct() {
    TEST("encodeMetrics: counter and gauge direct encoding");
    std::vector<foundation::MetricsRegistry::Sample> samples;
    foundation::MetricsRegistry::Sample counter;
    counter.type = foundation::MetricType::Counter;
    counter.name = "machine_c";
    counter.desc = "";
    counter.value = std::uint64_t{42};
    samples.push_back(counter);
    foundation::MetricsRegistry::Sample gauge;
    gauge.type = foundation::MetricType::Gauge;
    // torture 串打满转义全族："、\、退格、换页、CR、TAB、0x1F 以下控制
    // 字符与换行——appendJsonString 每个转义臂都有直测。
    gauge.name = "q\"\\b\bf\fr\r\t\x01z\n";
    gauge.desc = "d";
    gauge.value = std::int64_t{-9};
    samples.push_back(gauge);

    const auto now = std::chrono::system_clock::now();
    const std::string body =
        OtlpMetricsExporter::encodeMetrics(samples, "svc", now);

    if (!contains(body, "{\"resourceMetrics\":[{\"resource\":{\"attributes\":"
                        "[{\"key\":\"service.name\",\"value\":{\"stringValue\":"
                        "\"svc\"}}]}")) FAIL("resource envelope");
    if (!contains(body, "\"name\":\"machine_c\",\"description\":\"\""))
        FAIL("empty description passes through");
    if (!contains(body, "\"isMonotonic\":true,\"dataPoints\":[{\"timeUnixNano\":"))
        FAIL("counter sum shape");
    if (!contains(body, "\"asInt\":\"42\"")) FAIL("counter asInt");
    if (!contains(body, "\"name\":\"q\\\"\\\\b\\bf\\fr\\r\\t\\u0001z\\n\","
                        "\"description\":\"d\""))
        FAIL("name JSON escaping covers every arm");
    if (!contains(body, "\"gauge\":{\"dataPoints\":[{\"timeUnixNano\":"))
        FAIL("gauge shape");
    if (!contains(body, "\"asInt\":\"-9\"")) FAIL("negative gauge asInt");
    if (countOf(body, "\"timeUnixNano\":\"") != 2) FAIL("one time per sample");
    if (!contains(body, "\"metrics\":[{")) FAIL("metrics array");
    PASS();
}

static void test_encode_histogram_direct() {
    TEST("encodeMetrics: histogram finite sum kept, non-finite omitted");
    foundation::Histogram::Snapshot finite;
    finite.count = 3;
    finite.sum = 2.5;
    finite.bucketCounts = {1, 0, 2};
    finite.boundaries = {1.0, 2.0};
    foundation::Histogram::Snapshot nonFinite;
    nonFinite.count = 1;
    nonFinite.sum = std::numeric_limits<double>::infinity();
    nonFinite.bucketCounts = {0, 1};
    nonFinite.boundaries = {1.0};

    std::vector<foundation::MetricsRegistry::Sample> samples;
    foundation::MetricsRegistry::Sample h1;
    h1.type = foundation::MetricType::Histogram;
    h1.name = "h_finite";
    h1.desc = "d1";
    h1.value = finite;
    samples.push_back(h1);
    foundation::MetricsRegistry::Sample h2;
    h2.type = foundation::MetricType::Histogram;
    h2.name = "h_nonfinite";  // 命名避开 "inf" 子串（断言需扫全负例）
    h2.desc = "d2";
    h2.value = nonFinite;
    samples.push_back(h2);

    const std::string body = OtlpMetricsExporter::encodeMetrics(
        samples, "svc", std::chrono::system_clock::now());

    if (!contains(body, "\"name\":\"h_finite\",\"description\":\"d1\","
                        "\"histogram\":{\"aggregationTemporality\":2"))
        FAIL("histogram shape");
    if (!contains(body, "\"count\":\"3\",\"sum\":2.5,\"bucketCounts\":"
                        "[\"1\",\"0\",\"2\"],\"explicitBounds\":[1,2]"))
        FAIL("finite histogram payload");
    if (!contains(body, "\"name\":\"h_nonfinite\"")) FAIL("non-finite sample present");
    if (!contains(body, "\"count\":\"1\",\"bucketCounts\":[\"0\",\"1\"],"
                        "\"explicitBounds\":[1]"))
        FAIL("non-finite sum must be omitted with rest intact");
    if (contains(body, "inf")) FAIL("non-finite sum must not be emitted");
    PASS();
}

static void test_encode_empty_batch() {
    TEST("encodeMetrics: empty batch is a legal payload");
    const std::string body = OtlpMetricsExporter::encodeMetrics(
        {}, "svc", std::chrono::system_clock::now());
    if (!contains(body, "\"metrics\":[]")) FAIL("empty metrics array");
    if (!contains(body, "\"resourceMetrics\":[{\"resource\":"))
        FAIL("envelope kept");
    PASS();
}

int main() {
    TcpConnection::globalInit();
    test_default_config_stays_disabled();
    test_invalid_endpoint_warns_and_refuses();
    test_enabled_install_logs_data_scope_idempotently();
    test_collector_receives_otlp_metrics_end_to_end();
    test_uninstall_stops_exporting();
    test_destructor_uninstalls();
    test_interval_gate_and_zero_interval();
    test_500_counts_failed_with_status();
    test_refused_endpoint_fails_fast();
    test_silent_collector_fails_via_timeout();
    test_collector_closing_without_usable_response_fails();
    test_garbage_response_fails_with_status_zero();
    test_fragmented_status_line_still_parses();
    test_failed_export_does_not_retry();
    test_injected_transport_receives_target_and_body();
    test_encode_counter_and_gauge_direct();
    test_encode_histogram_direct();
    test_encode_empty_batch();

    std::cout << "  passed=" << testsPassed << " failed=" << testsFailed
              << "\n";
    return testsFailed == 0 ? 0 : 1;
}
