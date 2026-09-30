#include "theseed/control/machine/OtlpMetricsExporter.h"

#include "theseed/foundation/Logger.h"

#include <cmath>
#include <chrono>
#include <cstddef>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace theseed::control::machine {

namespace {

// 启动日志与导出面文档同源的数据范围声明（硬要求①）：开启时向运维
// 明示本导出器外发的指标字段与不外发的遥测面。
constexpr std::string_view kDataScope =
    "exported: metric name, description, counter/gauge values (asInt), "
    "histogram count/sum/bucketCounts/explicitBounds, collection "
    "timeUnixNano, resource service.name, scope theseed.control.metrics; "
    "not exported: spans/traces, structured logs, request/audit payloads";

constexpr std::string_view kScopeName = "theseed.control.metrics";

// 与 OtlpTraceExporter.cpp 的同族 JSON 转义（两导出器各持独立副本：
// 编码面冻结后零漂移风险，且互不引入交叉 include）。字符范围按
// RFC 8259：引号/反斜杠与 0x1F 以下控制字符必须转义。
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

std::string unixNano(std::chrono::system_clock::time_point tp) {
    const auto nanos =
        std::chrono::duration_cast<std::chrono::nanoseconds>(tp.time_since_epoch())
            .count();
    return std::to_string(nanos);
}

// 单个 Sample → OTLP JSON metric。三型 variant 直接分派（注册表 collect
// 恒产与 type 一致的载荷）：uint64 计数器 → 累积 sum；int64 仪表 →
// gauge；Histogram::Snapshot → 累积直方图。sum 非有限值无合法 JSON
// 数字表示，按 OTLP 可选字段口径省略（count/buckets 不受影响）。
void appendSample(std::string& out,
                  const foundation::MetricsRegistry::Sample& sample,
                  std::string_view timeUnixNano) {
    out += "{\"name\":";
    appendJsonString(out, sample.name);
    out += ",\"description\":";
    appendJsonString(out, sample.desc);
    std::visit(
        [&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, std::uint64_t>) {
                out += ",\"sum\":{\"aggregationTemporality\":2,"
                       "\"isMonotonic\":true,\"dataPoints\":[{\"timeUnixNano\":";
                appendJsonString(out, timeUnixNano);
                out += ",\"asInt\":\"";
                out += std::to_string(value);
                out += "\"}]}";
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                out += ",\"gauge\":{\"dataPoints\":[{\"timeUnixNano\":";
                appendJsonString(out, timeUnixNano);
                out += ",\"asInt\":\"";
                out += std::to_string(value);
                out += "\"}]}";
            } else {
                out += ",\"histogram\":{\"aggregationTemporality\":2,"
                       "\"dataPoints\":[{\"timeUnixNano\":";
                appendJsonString(out, timeUnixNano);
                out += ",\"count\":\"";
                out += std::to_string(value.count);
                out += "\"";
                if (std::isfinite(value.sum)) {
                    std::ostringstream sumText;
                    sumText << value.sum;
                    out += ",\"sum\":";
                    out += sumText.str();
                }
                out += ",\"bucketCounts\":[";
                for (std::size_t i = 0; i < value.bucketCounts.size(); ++i) {
                    if (i > 0) {
                        out.push_back(',');
                    }
                    out += "\"";
                    out += std::to_string(value.bucketCounts[i]);
                    out += "\"";
                }
                out += "],\"explicitBounds\":[";
                for (std::size_t i = 0; i < value.boundaries.size(); ++i) {
                    if (i > 0) {
                        out.push_back(',');
                    }
                    std::ostringstream boundText;
                    boundText << value.boundaries[i];
                    out += boundText.str();
                }
                out += "]}]}";
            }
        },
        sample.value);
}

}  // namespace

OtlpMetricsExporter::OtlpMetricsExporter(Config config,
                                         OtlpTraceExporter::HttpPost post)
    : config_(std::move(config)),
      post_(post ? std::move(post)
                 : OtlpTraceExporter::HttpPost(
                       &OtlpTraceExporter::postViaTcp)) {}

OtlpMetricsExporter::~OtlpMetricsExporter() { uninstall(); }

bool OtlpMetricsExporter::install() {
    if (installed_) {
        return true;  // 幂等：重复 install 不重复打启用日志
    }
    if (!config_.enabled) {
        return false;  // 硬要求①：默认关闭，未显式开启不外发
    }
    const auto target = OtlpTraceExporter::resolveEndpoint(config_.endpoint);
    if (!target) {
        const foundation::LogAttribute endpointAttr = {"endpoint", config_.endpoint};
        foundation::logWarn("otlp.metrics.export.invalid_endpoint",
                            {endpointAttr});
        return false;
    }
    target_ = *target;
    installed_ = true;
    lastExportAt_ = {};  // 首个到期调用立即导出（reportIfDue 同口径）
    // 硬要求①：开启即在启动日志明示导出的数据范围（指标内容）。
    const foundation::LogAttribute endpointAttr = {"endpoint", config_.endpoint};
    const foundation::LogAttribute formatAttr = {"format", "otlp-http/json"};
    const foundation::LogAttribute intervalAttr = {
        "interval_ms", static_cast<std::int64_t>(config_.interval.count())};
    const foundation::LogAttribute scopeAttr = {"data_scope", std::string(kDataScope)};
    foundation::logInfo("otlp.metrics.export.enabled",
                        {endpointAttr, formatAttr, intervalAttr, scopeAttr});
    return true;
}

void OtlpMetricsExporter::uninstall() {
    if (!installed_) {
        return;  // 未安装（含 disabled/非法 endpoint 的 install 失败形态）
    }
    installed_ = false;
}

bool OtlpMetricsExporter::installed() const noexcept { return installed_; }

std::uint64_t OtlpMetricsExporter::exportedOk() const noexcept {
    return exportedOk_.load(std::memory_order_relaxed);
}

std::uint64_t OtlpMetricsExporter::exportFailed() const noexcept {
    return exportFailed_.load(std::memory_order_relaxed);
}

bool OtlpMetricsExporter::exportIfDue(
    std::chrono::steady_clock::time_point now) {
    if (!installed_) {
        return false;  // 未开启：零网络副作用
    }
    if (now - lastExportAt_ < config_.interval) {
        return false;  // 周期未满：空转（首个到期调用必然越过 epoch 门）
    }
    lastExportAt_ = now;
    return exportOnce();
}

bool OtlpMetricsExporter::exportOnce() {
    if (!installed_) {
        return false;
    }
    const auto samples = foundation::MetricsRegistry::instance().collect();
    onExport(
        encodeMetrics(samples, config_.serviceName, std::chrono::system_clock::now()));
    return true;
}

std::string OtlpMetricsExporter::encodeMetrics(
    const std::vector<foundation::MetricsRegistry::Sample>& samples,
    std::string_view serviceName,
    std::chrono::system_clock::time_point now) {
    const std::string timeUnixNano = unixNano(now);
    std::string out;
    out += "{\"resourceMetrics\":[{\"resource\":{\"attributes\":[{\"key\":";
    out += "\"service.name\",\"value\":{\"stringValue\":";
    appendJsonString(out, serviceName);
    out += "}}]},\"scopeMetrics\":[{\"scope\":{\"name\":";
    appendJsonString(out, kScopeName);
    out += "},\"metrics\":[";
    for (std::size_t i = 0; i < samples.size(); ++i) {
        if (i > 0) {
            out.push_back(',');
        }
        appendSample(out, samples[i], timeUnixNano);
    }
    out += "]}]}]}";
    return out;
}

void OtlpMetricsExporter::onExport(const std::string& body) {
    const OtlpTraceExporter::PostResult result =
        post_(target_, body, config_.timeout);
    if (result.ok) {
        ++exportedOk_;
        return;
    }
    // 失败语义：丢弃 + 计数 + warn，不重试（同 trace 半边）。
    ++exportFailed_;
    const foundation::LogAttribute endpointAttr = {"endpoint", config_.endpoint};
    const foundation::LogAttribute statusAttr = {
        "http_status", static_cast<std::int64_t>(result.status)};
    foundation::logWarn("otlp.metrics.export.failed", {endpointAttr, statusAttr});
}

}  // namespace theseed::control::machine
