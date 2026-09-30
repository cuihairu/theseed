#include "theseed/control/machine/OtlpTraceExporter.h"

#include "theseed/foundation/Logger.h"
#include "theseed/runtime/TcpConnection.h"

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
    previous_ = foundation::takeSpanEmitter();
    foundation::setSpanEmitter(
        [this](const foundation::Span& span) { onSpan(span); });
    installed_ = true;
    // 硬要求①：开启即在启动日志明示导出的数据范围（span/trace 内容）。
    const foundation::LogAttribute endpointAttr = {"endpoint", config_.endpoint};
    const foundation::LogAttribute formatAttr = {"format", "otlp-http/json"};
    const foundation::LogAttribute scopeAttr = {"data_scope", std::string(kDataScope)};
    foundation::logInfo("otlp.traces.export.enabled",
                        {endpointAttr, formatAttr, scopeAttr});
    return true;
}

void OtlpTraceExporter::uninstall() {
    if (!installed_) {
        return;  // 未安装（含 disabled/非法 endpoint 的 install 失败形态）
    }
    foundation::setSpanEmitter(previous_);
    previous_ = nullptr;
    installed_ = false;
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
    std::string out;
    out += "{\"resourceSpans\":[{\"resource\":{\"attributes\":[{\"key\":";
    out += "\"service.name\",\"value\":{\"stringValue\":";
    appendJsonString(out, serviceName);
    out += "}}]},\"scopeSpans\":[{\"scope\":{\"name\":";
    appendJsonString(out, kScopeName);
    out += "},\"spans\":[{\"traceId\":";
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
    out += "]}]}]}]}";
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
    const std::string body = encodeTraces(span, config_.serviceName);
    const PostResult result = post_(target_, body, config_.timeout);
    if (result.ok) {
        ++exportedOk_;
    } else {
        ++exportFailed_;
        const foundation::LogAttribute endpointAttr = {"endpoint", config_.endpoint};
        const foundation::LogAttribute statusAttr =
            {"http_status", static_cast<std::int64_t>(result.status)};
        foundation::logWarn("otlp.traces.export.failed",
                            {endpointAttr, statusAttr});
    }
    if (previous_) {
        previous_(span);  // 链式：宿主既有发射器照常收到
    }
}

}  // namespace theseed::control::machine
