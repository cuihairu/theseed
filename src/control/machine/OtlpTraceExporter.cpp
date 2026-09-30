#include "theseed/control/machine/OtlpTraceExporter.h"

#include "theseed/foundation/Logger.h"
#include "theseed/runtime/TcpConnection.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>

namespace theseed::control::machine {

namespace {

// 启动日志与导出面文档同源的数据范围声明（硬要求①）：开启时向运维
// 明示本导出器外发的 span/trace 字段与不外发的遥测面。
constexpr std::string_view kDataScope =
    "exported: finished span name, traceId, spanId, parentSpanId, "
    "startTimeUnixNano, endTimeUnixNano, attributes (key + typed value), "
    "resource service.name, scope theseed.control.tracing; "
    "not exported: structured logs, metrics, request/audit payloads";

constexpr std::string_view kUrlScheme = "http://";
constexpr std::string_view kScopeName = "theseed.control.tracing";
constexpr std::uint16_t kDefaultOtlpPort = 4318;  // OTLP/HTTP 约定端口

// 严格 IPv4 字面量（与 TcpConnection 的 inet_pton 同口径）：四段、
// 每段 ≤3 位、值 ≤255、拒绝前导零——防「解析失败静默回退 0.0.0.0
// 意外连本机」的地址语义漂移。
bool isIpv4Literal(std::string_view text) {
    int octets = 0;
    std::size_t i = 0;
    while (i < text.size()) {
        int value = 0;
        int digits = 0;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
            if (digits > 3) {
                return false;  // 段长超 3 位（也封住 value 溢出路径）
            }
            if (digits == 1 && value == 0) {
                return false;  // 前导零（"04" 形态；"0" 单段另由段数拒绝）
            }
            value = value * 10 + (text[i] - '0');
            ++digits;
            ++i;
        }
        if (digits == 0) {
            return false;  // 空段 / 非数字段（含 "::"、"a.b.c.d"）
        }
        if (value > 255) {
            return false;
        }
        ++octets;
        if (octets > 4) {
            return false;  // 段数超 4
        }
        if (i < text.size()) {
            if (text[i] != '.') {
                return false;  // 分隔符非点（含冒号残留的畸形端口段）
            }
            ++i;
            if (i == text.size()) {
                return false;  // 尾点（"1.2.3.4." 形态）
            }
        }
    }
    return octets == 4;
}

void appendJsonString(std::string& out, std::string_view value) {
    out.push_back('"');
    for (const char c : value) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                static constexpr char kHex[] = "0123456789abcdef";
                out += "\\u00";
                out += kHex[(static_cast<unsigned char>(c) >> 4) & 0xF];
                out += kHex[static_cast<unsigned char>(c) & 0xF];
            } else {
                out.push_back(c);
            }
        }
    }
    out.push_back('"');
}

// 单条 span 属性 → OTLP JSON attribute。intValue 按 OTLP/JSON 规范走
// 字符串编码（int64 超 double 安全整数）；非有限 double 无合法 JSON
// 数字表示，按文本 stringValue 导出（"nan"/"inf"）。
void appendAttribute(std::string& out, const foundation::LogAttribute& attr) {
    out += "{\"key\":";
    appendJsonString(out, attr.key);
    out += ",\"value\":";
    std::visit(
        [&out](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::string>) {
                out += "{\"stringValue\":";
                appendJsonString(out, v);
                out += "}";
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                out += "{\"intValue\":\"";
                out += std::to_string(v);
                out += "\"}";
            } else if constexpr (std::is_same_v<T, bool>) {
                out += v ? "{\"boolValue\":true}" : "{\"boolValue\":false}";
            } else {
                std::ostringstream text;
                text << v;
                if (std::isfinite(v)) {
                    out += "{\"doubleValue\":";
                    out += text.str();
                    out += "}";
                } else {
                    out += "{\"stringValue\":\"";
                    out += text.str();
                    out += "\"}";
                }
            }
        },
        attr.value);
    out += "}";
}

std::string unixNano(std::chrono::system_clock::time_point tp) {
    const auto nanos =
        std::chrono::duration_cast<std::chrono::nanoseconds>(tp.time_since_epoch())
            .count();
    return std::to_string(nanos);
}

// 单个 span 对象（含自身花括号）：批量编码的公共单元。收尾 "] }" =
// attributes 数组闭合 + span 对象闭合，其后的批次收尾由调用方补。
void appendSpan(std::string& out, const foundation::Span& span) {
    out += "{\"traceId\":";
    appendJsonString(out, span.context.traceId);
    out += ",\"spanId\":";
    appendJsonString(out, span.context.spanId);
    if (!span.context.parentSpanId.empty()) {
        out += ",\"parentSpanId\":";
        appendJsonString(out, span.context.parentSpanId);
    }
    out += ",\"name\":";
    appendJsonString(out, span.name);
    out += ",\"kind\":1,\"startTimeUnixNano\":\"";
    out += unixNano(span.startTime);
    out += "\",\"endTimeUnixNano\":\"";
    out += unixNano(span.endTime);
    out += "\",\"attributes\":[";
    for (std::size_t i = 0; i < span.attrs.size(); ++i) {
        if (i > 0) {
            out.push_back(',');
        }
        appendAttribute(out, span.attrs[i]);
    }
    out += "]}";
}

std::string buildRequest(const OtlpTraceExporter::Target& target,
                         const std::string& body) {
    std::string request;
    request += "POST ";
    request += target.path;
    request += " HTTP/1.1\r\nHost: ";
    request += target.host;
    request += ':';
    request += std::to_string(target.port);
    request += "\r\nContent-Type: application/json\r\nContent-Length: ";
    request += std::to_string(body.size());
    request += "\r\nConnection: close\r\n\r\n";
    request += body;
    return request;
}

}  // namespace

OtlpTraceExporter::OtlpTraceExporter(Config config, HttpPost post)
    : config_(std::move(config)),
      post_(post ? std::move(post)
                 : HttpPost(&OtlpTraceExporter::postViaTcp)) {}

OtlpTraceExporter::~OtlpTraceExporter() { uninstall(); }

bool OtlpTraceExporter::install() {
    if (installed_) {
        return true;  // 幂等：重复 install 不重复挂发射器
    }
    if (!config_.enabled) {
        return false;  // 硬要求①：默认关闭，未显式开启不外发
    }
    const auto target = resolveEndpoint(config_.endpoint);
    if (!target) {
        const foundation::LogAttribute endpointAttr = {"endpoint", config_.endpoint};
        foundation::logWarn("otlp.traces.export.invalid_endpoint", {endpointAttr});
        return false;
    }
    target_ = *target;
    // 退化配置钳制（0 是无意义形态）：batchSize=0 会使取批恒空进而空
    // POST 忙循环；maxAttempts=0 是不发包却计整批失败；maxQueue=0 是
    // 入队恒丢。flushInterval ≤0 不钳（语义 = 有 span 即发，不忙等）。
    if (config_.batchSize == 0) {
        config_.batchSize = 1;
    }
    if (config_.maxAttempts == 0) {
        config_.maxAttempts = 1;
    }
    if (config_.maxQueue == 0) {
        config_.maxQueue = 1;
    }
    previous_ = foundation::takeSpanEmitter();
    foundation::setSpanEmitter(
        [this](const foundation::Span& span) { onSpan(span); });
    installed_ = true;
    // 重装复位（uninstall 置位的停机状态）；取批时钟自安装起算——首个
    // 兜底周期从这里数。worker 在挂发射器之后起：其间入队的 span 由
    // 谓词等待保证不丢唤醒。
    stopping_ = false;
    lastFlushAt_ = std::chrono::steady_clock::now();
    worker_ = std::thread(&OtlpTraceExporter::workerLoop, this);
    // 硬要求①：开启即在启动日志明示导出的数据范围（span/trace 内容）。
    const foundation::LogAttribute endpointAttr = {"endpoint", config_.endpoint};
    const foundation::LogAttribute formatAttr = {"format", "otlp-http/json"};
    const foundation::LogAttribute scopeAttr = {"data_scope", std::string(kDataScope)};
    const foundation::LogAttribute batchAttr = {"batch_size", static_cast<std::int64_t>(config_.batchSize)};
    const foundation::LogAttribute intervalAttr = {"flush_interval_ms", static_cast<std::int64_t>(config_.flushInterval.count())};
    const foundation::LogAttribute attemptsAttr = {"max_attempts", static_cast<std::int64_t>(config_.maxAttempts)};
    foundation::logInfo("otlp.traces.export.enabled",
                        {endpointAttr, formatAttr, scopeAttr, batchAttr, intervalAttr, attemptsAttr});
    return true;
}

void OtlpTraceExporter::uninstall() {
    if (!installed_) {
        return;  // 未安装（含 disabled/非法 endpoint 的 install 失败形态）
    }
    // 先还原槽位（新 span 不再入队），再停机排水并 join——卸载即同步
    // 收口点：join 返回后 worker 不再碰本对象。发射与卸载并发的极窄
    // 窗口里已过钩子分发的 span 可能残留队列（best-effort，析构释放，
    // 不做 epoch 确认）。
    foundation::setSpanEmitter(previous_);
    previous_ = nullptr;
    installed_ = false;
    {
        const std::lock_guard lock(queueMutex_);
        stopping_ = true;
    }
    flushCv_.notify_all();
    worker_.join();
}

bool OtlpTraceExporter::installed() const noexcept { return installed_; }

std::uint64_t OtlpTraceExporter::exportedOk() const noexcept {
    return exportedOk_.load(std::memory_order_relaxed);
}

std::uint64_t OtlpTraceExporter::exportFailed() const noexcept {
    return exportFailed_.load(std::memory_order_relaxed);
}

std::optional<OtlpTraceExporter::Target> OtlpTraceExporter::resolveEndpoint(
    std::string_view url) {
    if (!url.starts_with(kUrlScheme)) {
        return std::nullopt;  // 缺 scheme 或 https（无 TLS 支持口径）
    }
    std::string_view rest = url.substr(kUrlScheme.size());

    // path 从第一个 '/' 起（先切 path 再切端口，path 内 ':' 不参与端口）；
    // 缺省 "/"。
    std::string_view path = "/";
    if (const auto pathBegin = rest.find('/'); pathBegin != std::string_view::npos) {
        path = rest.substr(pathBegin);
        rest = rest.substr(0, pathBegin);
    }

    std::string_view host = rest;
    std::uint16_t port = kDefaultOtlpPort;
    if (const auto colon = rest.rfind(':'); colon != std::string_view::npos) {
        host = rest.substr(0, colon);
        const auto portText = rest.substr(colon + 1);
        std::uint32_t parsed = 0;
        const auto converted = std::from_chars(portText.data(),
                                               portText.data() + portText.size(),
                                               parsed);
        if (converted.ec != std::errc{} ||
            converted.ptr != portText.data() + portText.size()) {
            return std::nullopt;  // 空/非数字/越界（>65535）端口
        }
        if (parsed == 0 || parsed > 65535) {
            return std::nullopt;
        }
        port = static_cast<std::uint16_t>(parsed);
    }

    if (!isIpv4Literal(host)) {
        return std::nullopt;  // 空 host / 域名（无 DNS 口径）/ IPv6 / 畸形
    }

    Target target;
    target.host = std::string(host);
    target.port = port;
    target.path = std::string(path);
    return target;
}

std::string OtlpTraceExporter::encodeTraces(const foundation::Span& span,
                                            std::string_view serviceName) {
    return encodeTraces(std::vector<foundation::Span>{span}, serviceName);
}

std::string OtlpTraceExporter::encodeTraces(
    const std::vector<foundation::Span>& spans, std::string_view serviceName) {
    std::string out;
    out += "{\"resourceSpans\":[{\"resource\":{\"attributes\":[{\"key\":";
    out += "\"service.name\",\"value\":{\"stringValue\":";
    appendJsonString(out, serviceName);
    out += "}}]},\"scopeSpans\":[{\"scope\":{\"name\":";
    appendJsonString(out, kScopeName);
    out += "},\"spans\":[";
    for (std::size_t i = 0; i < spans.size(); ++i) {
        if (i > 0) {
            out.push_back(',');
        }
        appendSpan(out, spans[i]);
    }
    // 批次收尾（三对）：spans] scope元素} | scopeSpans] resSpans元素} |
    // resourceSpans] 根}。
    out += "]}" "]}" "]}";
    return out;
}

int OtlpTraceExporter::decodeHttpStatus(std::string_view response) {
    const auto lineEnd = response.find("\r\n");
    if (lineEnd == std::string_view::npos) {
        return 0;  // 状态行未收全
    }
    const auto line = response.substr(0, lineEnd);
    const auto space = line.find(' ');
    if (space == std::string_view::npos) {
        return 0;  // 非状态行形态（无状态码位）
    }
    int code = 0;
    bool hasDigit = false;
    for (std::size_t i = space + 1; i < line.size(); ++i) {
        const char c = line[i];
        if (c < '0' || c > '9') {
            break;
        }
        code = code * 10 + (c - '0');
        hasDigit = true;
    }
    if (!hasDigit) {
        return 0;  // 空格后无状态码（"HTTP/1.1 \r\n"）
    }
    return code;
}

OtlpTraceExporter::PostResult OtlpTraceExporter::postViaTcp(
    const Target& target, const std::string& body,
    std::chrono::milliseconds timeout) {
    auto conn = runtime::TcpConnection::create();
    if (!conn->connect(target.host, target.port)) {
        return PostResult{};  // 同步建连失败（不可路由地址等）
    }

    std::string response;
    conn->setOnReceived([&response](std::span<const std::byte> data) {
        response.append(reinterpret_cast<const char*>(data.data()), data.size());
    });
    const std::string request = buildRequest(target, body);
    static_cast<void>(conn->write(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(request.data()), request.size())));

    PostResult result;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        conn->pump();
        const int status = decodeHttpStatus(response);
        if (status > 0) {
            result.ok = status >= 200 && status < 300;
            result.status = status;
            break;
        }
        if (!conn->isConnected()) {
            break;  // 对端先关（拒绝/Reset/EOF）：响应缺席
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            break;  // 超时：响应缺席
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    conn->close();
    return result;
}

void OtlpTraceExporter::onSpan(const foundation::Span& span) {
    {
        const std::lock_guard lock(queueMutex_);
        if (queue_.size() >= config_.maxQueue) {
            // 本地过载：丢新 span 只计数不刷屏（传输失败才有 warn）。
            ++exportFailed_;
        } else {
            queue_.push_back(span);
        }
    }
    flushCv_.notify_one();
    if (previous_) {
        previous_(span);  // 链式：宿主既有发射器在发射线程同步收到
    }
}

void OtlpTraceExporter::workerLoop() {
    std::unique_lock lock(queueMutex_);
    for (;;) {
        // 满批或兜底周期到点即发；空队列无限等（无 span 零请求、不忙
        // 等，谓词防丢唤醒）；停机跳过等待直接排水。
        const bool flushDue =
            !queue_.empty() &&
            (queue_.size() >= config_.batchSize ||
             std::chrono::steady_clock::now() >=
                 lastFlushAt_ + config_.flushInterval);
        if (!stopping_ && !flushDue) {
            if (queue_.empty()) {
                flushCv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            } else {
                flushCv_.wait_until(lock, lastFlushAt_ + config_.flushInterval);
            }
            continue;  // 醒来重算到期（新 span / 周期到点 / 停机）
        }
        if (stopping_ && queue_.empty()) {
            break;  // 排空且停机：退出（uninstall 在此 join）
        }
        // 取一批（≤batchSize）。draining = 停机排水，sendBatch 只试一次。
        const std::size_t takeCount = std::min(queue_.size(), config_.batchSize);
        std::vector<foundation::Span> batch;
        batch.reserve(takeCount);
        for (std::size_t i = 0; i < takeCount; ++i) {
            batch.push_back(std::move(queue_[i]));
        }
        queue_.erase(queue_.begin(),
                     queue_.begin() + static_cast<std::ptrdiff_t>(takeCount));
        lastFlushAt_ = std::chrono::steady_clock::now();
        const bool draining = stopping_;
        lock.unlock();
        sendBatch(batch, draining);
        lock.lock();
    }
}

void OtlpTraceExporter::sendBatch(const std::vector<foundation::Span>& batch,
                                  bool draining) {
    const std::string body = encodeTraces(batch, config_.serviceName);
    // 有界重试：停机排水只试一次（退出优先），否则共 maxAttempts 次
    // 尝试、相邻间隔 retryBackoff；不按状态码分流（有界尝试 + 控制面
    // 低频，避免 4xx 分流的分支面）。
    const std::uint32_t attempts = draining ? 1u : config_.maxAttempts;
    PostResult result;
    for (std::uint32_t attempt = 0; attempt < attempts; ++attempt) {
        if (attempt > 0) {
            std::this_thread::sleep_for(config_.retryBackoff);
        }
        result = post_(target_, body, config_.timeout);
        if (result.ok) {
            break;
        }
    }
    if (result.ok) {
        exportedOk_ += static_cast<std::uint64_t>(batch.size());
        return;
    }
    // 失败语义：整批丢弃 + 计数（按批内 span 数）+ 一次 warn（末次尝试
    // 的 http_status，0 = 无响应）。warn 先于计数：异步下观测者先见日志
    // 再见计数。
    const foundation::LogAttribute endpointAttr = {"endpoint", config_.endpoint};
    const foundation::LogAttribute statusAttr =
        {"http_status", static_cast<std::int64_t>(result.status)};
    foundation::logWarn("otlp.traces.export.failed", {endpointAttr, statusAttr});
    exportFailed_ += static_cast<std::uint64_t>(batch.size());
}

}  // namespace theseed::control::machine
