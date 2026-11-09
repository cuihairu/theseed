// MachineDaemon 各 TU 共用的辅助件定义（声明见 MachineDaemonDetail.h）。
// 单一定义点：头文件只放声明，避免内联函数在多 TU 间造成重复弱符号与
// 覆盖率归因漂移。

#include "MachineDaemonDetail.h"

#include "theseed/control/machine/MachineSnapshotCodec.h"
#include "theseed/foundation/Logger.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstring>
#include <sstream>
#include <string_view>

namespace theseed::control::machine::detail {

std::vector<std::byte> toBytes(const std::string& text) {
    std::vector<std::byte> bytes(text.size());
    if (!text.empty()) {
        std::memcpy(bytes.data(), text.data(), text.size());
    }
    return bytes;
}

// 审计条目 → JSON 对象（字段顺序固定，时间 epoch 毫秒）。
std::string auditEntryJson(const AuditEntry& entry) {
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                            entry.timestamp.time_since_epoch())
                            .count();
    // 先拼字段再入流：多行 << 链会让 gcc 把覆盖计数错误归因到续行
    const std::string commandField =
        "\"command\":\"" + escapeJsonString(entry.command) + "\"";
    const std::string argsField = "\"args\":\"" + escapeJsonString(entry.args) + "\"";
    const std::string acceptedField =
        std::string("\"accepted\":") + (entry.accepted ? "true" : "false");
    const std::string okField = std::string("\"ok\":") + (entry.ok ? "true" : "false");
    const std::string requestField =
        "\"request_id\":" + std::to_string(entry.requestId);

    std::ostringstream out;
    out << "{\"ts\":" << millis << ",\"source\":" << entry.source << ","
        << requestField << "," << commandField << "," << argsField << ","
        << acceptedField << "," << okField << "}";
    return out.str();
}

foundation::Counter& telemetryCounter(const char* name) {
    return foundation::MetricsRegistry::instance().counter(name);
}

// 控制面动作拒绝（治理/诊断采样共用）：计数 + span 标记 + 结构化日志。
// span 可空（只读动作不进 trace）；审计与错误响应由调用方处理。
void rejectControlAction(const AuditEntry& entry,
                         foundation::SpanScope* span,
                         const char* counterName,
                         const std::string& logMessage,
                         const std::string& reason) {
    telemetryCounter(counterName).increment();
    if (span != nullptr) {
        span->setAttribute("accepted", false);
        span->setAttribute("reason", reason);
    }
    const auto sourceValue = static_cast<std::int64_t>(entry.source);
    const foundation::LogAttribute sourceAttr = {"source", sourceValue};
    const foundation::LogAttribute commandAttr = {"command", entry.command};
    const foundation::LogAttribute reasonAttr = {"reason", reason};
    foundation::logWarn(logMessage, {sourceAttr, commandAttr, reasonAttr});
}

ParsedPid parsePidPayload(const std::vector<std::byte>& payload) {
    if (payload.empty()) {
        return {};
    }
    const std::string text(reinterpret_cast<const char*>(payload.data()),
                           payload.size());
    std::uint32_t pid = 0;
    const auto* begin = text.data();
    const auto* end = begin + text.size();
    const auto [ptr, ec] = std::from_chars(begin, end, pid);
    if (ec != std::errc{} || ptr != end || pid == 0) {
        return {};
    }
    return {true, pid};
}

ParsedHandle parseHandlePayload(const std::vector<std::byte>& payload) {
    if (payload.empty()) {
        return {};
    }
    const std::string text(reinterpret_cast<const char*>(payload.data()),
                           payload.size());
    std::uint64_t handle = 0;
    const auto* begin = text.data();
    const auto* end = begin + text.size();
    const auto [ptr, ec] = std::from_chars(begin, end, handle);
    if (ec != std::errc{} || ptr != end || handle == 0) {
        return {};
    }
    return {true, handle};
}

ParsedConfigPayload parseConfigPayload(const std::vector<std::byte>& payload) {
    const auto separator =
        std::find(payload.begin(), payload.end(), std::byte{0});
    if (separator == payload.end()) {
        return {};
    }
    ParsedConfigPayload parsed;
    const auto* begin = reinterpret_cast<const char*>(payload.data());
    const auto keyLength = static_cast<std::size_t>(separator - payload.begin());
    parsed.key.assign(begin, keyLength);
    parsed.value.assign(begin + keyLength + 1,
                        payload.size() - keyLength - 1);
    parsed.ok = !parsed.key.empty();
    return parsed;
}

ParsedMillis parseMillisValue(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    std::uint64_t millis = 0;
    const auto* begin = text.data();
    const auto* end = begin + text.size();
    const auto [ptr, ec] = std::from_chars(begin, end, millis);
    if (ec != std::errc{} || ptr != end) {
        return {};
    }
    return {true, millis};
}

std::string redactSessionToken(const std::string& token) {
    return "session(len=" + std::to_string(token.size()) + ")";
}

SessionScope parseSessionScope(const std::string& text) {
    constexpr std::string_view kAll = "all";
    constexpr std::string_view kAccountPrefix = "account=";
    constexpr std::string_view kRealmPrefix = "realm=";

    if (text == kAll) {
        SessionScope scope;
        scope.valid = true;
        scope.all = true;
        return scope;
    }
    if (text.size() > kAccountPrefix.size() &&
        text.compare(0, kAccountPrefix.size(), kAccountPrefix) == 0) {
        SessionScope scope;
        scope.valid = true;
        scope.byAccount = true;
        scope.value = text.substr(kAccountPrefix.size());
        return scope;
    }
    if (text.size() > kRealmPrefix.size() &&
        text.compare(0, kRealmPrefix.size(), kRealmPrefix) == 0) {
        SessionScope scope;
        scope.valid = true;
        scope.value = text.substr(kRealmPrefix.size());
        return scope;
    }
    return {};
}

}  // namespace theseed::control::machine::detail
