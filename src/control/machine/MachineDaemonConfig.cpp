// MachineDaemon 的运行时配置热改面（04 §6.3，machine.config.apply）。
// 白名单是热改的唯一通道——禁改四类（协议定义/持久化 schema/entity
// property flags/迁移语义）命中即拒绝且指认类别，白名单外任意键一律
// 拒绝（扩面须改码评审）；白名单内解析合法即就地生效。

#include "theseed/control/machine/MachineDaemon.h"
#include "theseed/control/machine/MachineSnapshotCodec.h"
#include "theseed/foundation/Logger.h"
#include "theseed/foundation/Metrics.h"
#include "theseed/foundation/Tracing.h"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <string>
#include <vector>

#include "MachineDaemonDetail.h"

namespace theseed::control::machine {

using namespace detail;

// 运行时配置热改（04 §6.3，Admin 级）：白名单是热改的唯一通道——
// 禁改四类（协议定义/持久化 schema/entity property flags/迁移语义）
// 命中即拒绝且指认类别，白名单外任意键一律拒绝（扩面须改码评审），
// 白名单内解析合法即就地生效。效果立即可观测（可调项即时改写
// config_，下一轮 tick 即按新值运转）。
void MachineDaemon::handleConfigApply(runtime::RuntimeInvocation& inv) {
    // 配置变更是关键受控动作：全程 span（05 §3.1），拒绝也入 span。
    foundation::SpanScope span("machine.config.apply");
    span.setAttribute("source",
                      static_cast<std::int64_t>(inv.sourceComponent));

    AuditEntry entry;
    entry.timestamp = std::chrono::system_clock::now();
    entry.source = inv.sourceComponent;
    entry.requestId = inv.requestId;  // §6.2 关联 id 透传（0 = 未携带）
    entry.command = kConfigApplyCommand;

    // 拒绝臂共用出口：审计先行，再计数/span/日志与错误响应。
    const auto reject = [&](const std::string& reason) {
        entry.accepted = false;
        appendAudit(entry);
        rejectControlAction(entry, &span, kConfigApplyRejectedCount,
                            "machine.config.rejected", reason);
        const auto payload = toBytes(reason);
        sendResponse(inv.sourceComponent, MachineMethod::kError,
                     std::span<const std::byte>(payload));
    };

    const auto parsed = parseConfigPayload(inv.payload);
    entry.args = parsed.ok ? (parsed.key + "=" + parsed.value) : "";
    if (!parsed.ok) {
        reject("config apply rejected: malformed payload "
               "(expected key NUL value)");
        return;
    }
    span.setAttribute("key", parsed.key);

    // §6.1 角色门：配置生效 ≙ §6.1 的 apply runtime config，需 Admin。
    if (!hasRole(inv.sourceComponent, AccessRole::Admin)) {
        reject("config apply rejected: source component " +
               std::to_string(inv.sourceComponent) + " requires Admin role");
        return;
    }

    // 禁改四类先行指认：比"白名单外"更具体的拒绝原因（审计与指标里
    // 可直接看出命中的保护类别）。
    for (const auto& guarded : kProtectedConfigClasses) {
        if (parsed.key.starts_with(guarded.prefix)) {
            reject("config apply rejected: '" + parsed.key +
                   "' modifies the " + guarded.className +
                   " (online changes are forbidden)");
            return;
        }
    }

    // 白名单：目前唯一可调项是上报周期（毫秒；0 = 关闭周期上报）。
    if (parsed.key == kConfigReportIntervalKey) {
        const auto millis = parseMillisValue(parsed.value);
        if (!millis.ok) {
            reject("config apply rejected: invalid value for '" + parsed.key +
                   "' (expected non-negative integer milliseconds)");
            return;
        }
        config_.reportInterval = std::chrono::milliseconds{millis.value};
    } else {
        reject("config apply rejected: key '" + parsed.key +
               "' is not in the change whitelist");
        return;
    }

    entry.accepted = true;
    entry.ok = true;
    appendAudit(entry);
    telemetryCounter(kConfigApplyAcceptedCount).increment();
    span.setAttribute("accepted", true);
    span.setAttribute("value", parsed.value);
    const auto sourceValue = static_cast<std::int64_t>(entry.source);
    const foundation::LogAttribute sourceAttr = {"source", sourceValue};
    const foundation::LogAttribute keyAttr = {"key", parsed.key};
    const foundation::LogAttribute valueAttr = {"value", parsed.value};
    foundation::logInfo("machine.config.applied",
                        {sourceAttr, keyAttr, valueAttr});
    const std::byte result = std::byte{0x01};
    sendResponse(inv.sourceComponent, MachineMethod::kConfigApplyOk,
                 std::span<const std::byte>(&result, 1));
}

}  // namespace theseed::control::machine
