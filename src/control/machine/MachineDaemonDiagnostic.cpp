// MachineDaemon 的诊断采样面（04 §7：machine.profile.trigger / .profiles /
// .profile）。触发（写侧，消耗主机性能预算）与访问（读侧，明细外泄面）
// 各过 DiagnosticsPolicy 的 canTrigger / canAccess 白名单，再叠加 §6.1
// 角色门。采样入口关闭（tickProfiler == nullptr）时这些方法不被分发
// （handleInvocation 视同未知方法），本面仅服务入口开启的部署。

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

bool MachineDaemon::isTriggerAuthorized(runtime::ComponentId source) const {
    const auto& allowed = config_.diagnosticsPolicy.canTrigger;
    return std::find(allowed.begin(), allowed.end(), source) != allowed.end();
}

bool MachineDaemon::isDiagnosticsAccessAuthorized(
    runtime::ComponentId source) const {
    const auto& allowed = config_.diagnosticsPolicy.canAccess;
    return std::find(allowed.begin(), allowed.end(), source) != allowed.end();
}

void MachineDaemon::handleProfileTrigger(runtime::RuntimeInvocation& inv) {
    // 触发是关键受控动作：消耗主机性能预算，全程 span（05 §3.1），
    // 限流拒绝也入 span。
    foundation::SpanScope span("machine.profile.trigger");
    span.setAttribute("source",
                      static_cast<std::int64_t>(inv.sourceComponent));

    AuditEntry entry;
    entry.timestamp = std::chrono::system_clock::now();
    entry.source = inv.sourceComponent;
    entry.requestId = inv.requestId;  // §6.2 关联 id 透传（0 = 未携带）
    entry.command = kProfileTriggerCommand;

    // 拒绝臂共用出口：审计先行，再计数/span/日志与错误响应。
    const auto reject = [&](const std::string& reason) {
        entry.accepted = false;
        appendAudit(entry);
        rejectControlAction(entry, &span, kProfileTriggerRejectedCount,
                            "machine.profile.rejected", reason);
        const auto payload = toBytes(reason);
        sendResponse(inv.sourceComponent, MachineMethod::kError,
                     std::span<const std::byte>(payload));
    };

    if (!isTriggerAuthorized(inv.sourceComponent)) {
        reject("profile trigger rejected: source component " +
               std::to_string(inv.sourceComponent) + " is not authorized");
        return;
    }

    // §6.1 角色门（叠加在 canTrigger 之上）：触发消耗主机性能预算，
    // operate 面需 Operator 及以上。
    if (!hasRole(inv.sourceComponent, AccessRole::Operator)) {
        reject("profile trigger rejected: source component " +
               std::to_string(inv.sourceComponent) +
               " requires Operator role");
        return;
    }

    const auto handle = config_.tickProfiler->trigger();
    if (handle == 0) {
        reject("profile trigger rejected: sampling window unavailable "
               "(active window or invalid window config)");
        return;
    }

    entry.accepted = true;
    entry.ok = true;
    entry.args = std::to_string(handle);
    appendAudit(entry);
    telemetryCounter(kProfileTriggerAcceptedCount).increment();
    span.setAttribute("accepted", true);
    span.setAttribute("handle", static_cast<std::int64_t>(handle));
    const auto sourceValue = static_cast<std::int64_t>(entry.source);
    const auto handleValue = static_cast<std::int64_t>(handle);
    const foundation::LogAttribute sourceAttr = {"source", sourceValue};
    const foundation::LogAttribute handleAttr = {"handle", handleValue};
    foundation::logInfo("machine.profile.trigger", {sourceAttr, handleAttr});
    const auto payload = toBytes(std::to_string(handle));
    sendResponse(inv.sourceComponent, MachineMethod::kProfileTriggerOk,
                 std::span<const std::byte>(payload));
}

void MachineDaemon::handleProfiles(runtime::RuntimeInvocation& inv) {
    AuditEntry entry;
    entry.timestamp = std::chrono::system_clock::now();
    entry.source = inv.sourceComponent;
    entry.requestId = inv.requestId;  // §6.2 关联 id 透传（0 = 未携带）
    entry.command = kProfileListCommand;

    if (!isDiagnosticsAccessAuthorized(inv.sourceComponent)) {
        const std::string reason =
            "profile listing rejected: source component " +
            std::to_string(inv.sourceComponent) + " is not authorized";
        entry.accepted = false;
        appendAudit(entry);
        rejectControlAction(entry, nullptr, kProfileAccessRejectedCount,
                            "machine.profile.rejected", reason);
        const auto payload = toBytes(reason);
        sendResponse(inv.sourceComponent, MachineMethod::kError,
                     std::span<const std::byte>(payload));
        return;
    }

    // §6.1 角色门（叠加在 canAccess 之上）：inspect 面需 ReadOnly 及以上。
    if (!hasRole(inv.sourceComponent, AccessRole::ReadOnly)) {
        const std::string reason =
            "profile listing rejected: source component " +
            std::to_string(inv.sourceComponent) + " requires ReadOnly role";
        entry.accepted = false;
        appendAudit(entry);
        rejectControlAction(entry, nullptr, kProfileAccessRejectedCount,
                            "machine.profile.rejected", reason);
        const auto payload = toBytes(reason);
        sendResponse(inv.sourceComponent, MachineMethod::kError,
                     std::span<const std::byte>(payload));
        return;
    }

    const auto artifacts = config_.tickProfiler->listArtifacts();
    std::ostringstream out;
    out << "[";
    for (std::size_t i = 0; i < artifacts.size(); ++i) {
        if (i != 0) {
            out << ',';
        }
        const std::string handleField =
            "\"handle\":" + std::to_string(artifacts[i].handle);
        const std::string tickField =
            "\"tick_count\":" + std::to_string(artifacts[i].tickCount);
        const std::string windowField =
            "\"window_ms\":" + std::to_string(artifacts[i].windowMs);
        out << "{" << handleField << "," << tickField << "," << windowField
            << "}";
    }
    out << "]";

    entry.accepted = true;
    entry.ok = true;
    entry.args = std::to_string(artifacts.size());
    appendAudit(entry);
    telemetryCounter(kProfileAccessAcceptedCount).increment();
    const auto json = toBytes(out.str());
    sendResponse(inv.sourceComponent, MachineMethod::kProfilesOk,
                 std::span<const std::byte>(json));
}

void MachineDaemon::handleProfileDownload(runtime::RuntimeInvocation& inv) {
    AuditEntry entry;
    entry.timestamp = std::chrono::system_clock::now();
    entry.source = inv.sourceComponent;
    entry.requestId = inv.requestId;  // §6.2 关联 id 透传（0 = 未携带）
    entry.command = kProfileDownloadCommand;

    const auto parsed = parseHandlePayload(inv.payload);
    entry.args = parsed.ok ? std::to_string(parsed.value) : "";

    const auto reject = [&](const std::string& reason) {
        entry.accepted = false;
        appendAudit(entry);
        rejectControlAction(entry, nullptr, kProfileAccessRejectedCount,
                            "machine.profile.rejected", reason);
        const auto payload = toBytes(reason);
        sendResponse(inv.sourceComponent, MachineMethod::kError,
                     std::span<const std::byte>(payload));
    };

    if (!parsed.ok) {
        reject("profile download rejected: malformed handle payload");
        return;
    }
    if (!isDiagnosticsAccessAuthorized(inv.sourceComponent)) {
        reject("profile download rejected: source component " +
               std::to_string(inv.sourceComponent) + " is not authorized");
        return;
    }

    // §6.1 角色门（叠加在 canAccess 之上）：inspect 面需 ReadOnly 及以上。
    if (!hasRole(inv.sourceComponent, AccessRole::ReadOnly)) {
        reject("profile download rejected: source component " +
               std::to_string(inv.sourceComponent) + " requires ReadOnly role");
        return;
    }

    std::string payloadText;
    if (!config_.tickProfiler->artifactPayload(parsed.value, payloadText)) {
        reject("profile download rejected: unknown handle " +
               std::to_string(parsed.value));
        return;
    }

    entry.accepted = true;
    entry.ok = true;
    appendAudit(entry);
    telemetryCounter(kProfileAccessAcceptedCount).increment();
    const auto payload = toBytes(payloadText);
    sendResponse(inv.sourceComponent, MachineMethod::kProfileOk,
                 std::span<const std::byte>(payload));
}

}  // namespace theseed::control::machine
