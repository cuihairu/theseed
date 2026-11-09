// MachineDaemon 的主机进程治理面（machine.processes / machine.terminate）。
// 与受管进程编排（execute）刻意分离：治理是对主机上"非本代理管理"进程的
// 越权面，来源白名单与目标 comm 名单独立于 ExecPolicy，审计口径同族。

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

bool MachineDaemon::isGovernTrustedSource(runtime::ComponentId source) const {
    const auto& trusted = config_.processGovernPolicy.trustedComponents;
    return std::find(trusted.begin(), trusted.end(), source) != trusted.end();
}

bool MachineDaemon::isKillableName(const std::string& name) const {
    const auto& killable = config_.processGovernPolicy.killableNames;
    return std::find(killable.begin(), killable.end(), name) != killable.end();
}

void MachineDaemon::handleProcesses(runtime::RuntimeInvocation& inv) {
    AuditEntry entry;
    entry.timestamp = std::chrono::system_clock::now();
    entry.source = inv.sourceComponent;
    entry.requestId = inv.requestId;  // §6.2 关联 id 透传（0 = 未携带）
    entry.command = kGovernListCommand;

    if (!isGovernTrustedSource(inv.sourceComponent)) {
        const std::string reason =
            "process listing rejected: source component " +
            std::to_string(inv.sourceComponent) + " is not trusted";
        entry.accepted = false;
        appendAudit(entry);
        rejectControlAction(entry, nullptr, kProcessListRejectedCount,
                            "machine.process.govern.rejected", reason);
        const auto payload = toBytes(reason);
        sendResponse(inv.sourceComponent, MachineMethod::kError,
                     std::span<const std::byte>(payload));
        return;
    }

    // §6.1 角色门：枚举是 inspect 面，需 ReadOnly 及以上（叠加在来源
    // 白名单之上）。
    if (!hasRole(inv.sourceComponent, AccessRole::ReadOnly)) {
        const std::string reason =
            "process listing rejected: source component " +
            std::to_string(inv.sourceComponent) + " requires ReadOnly role";
        entry.accepted = false;
        appendAudit(entry);
        rejectControlAction(entry, nullptr, kProcessListRejectedCount,
                            "machine.process.govern.rejected", reason);
        const auto payload = toBytes(reason);
        sendResponse(inv.sourceComponent, MachineMethod::kError,
                     std::span<const std::byte>(payload));
        return;
    }

    const auto hostProcesses = agent_.enumerateHostProcesses();
    // 非受控进程视图：治理面向主机上“非本代理管理”的进程；受管进程的
    // 编排走 execute（start/stop/restart），不在此重复暴露。
    std::ostringstream out;
    out << "[";
    std::size_t listed = 0;
    for (const auto& process : hostProcesses) {
        if (process.managed) {
            continue;
        }
        if (listed != 0) {
            out << ',';
        }
        const std::string pidField = "\"pid\":" + std::to_string(process.pid);
        const std::string nameField =
            "\"name\":\"" + escapeJsonString(process.name) + "\"";
        out << "{" << pidField << "," << nameField << "}";
        ++listed;
    }
    out << "]";

    entry.accepted = true;
    entry.ok = true;
    entry.args = std::to_string(listed);
    appendAudit(entry);
    telemetryCounter(kProcessListCount).increment();
    const auto json = toBytes(out.str());
    sendResponse(inv.sourceComponent, MachineMethod::kProcessesOk,
                 std::span<const std::byte>(json));
}

void MachineDaemon::handleTerminate(runtime::RuntimeInvocation& inv) {
    // 处置是关键受控动作：全程 span（05 §3.1），拒绝也入 span——“谁在
    // 何时被拒、为什么”与审计同视角。
    foundation::SpanScope span("machine.terminate");
    span.setAttribute("source",
                      static_cast<std::int64_t>(inv.sourceComponent));

    AuditEntry entry;
    entry.timestamp = std::chrono::system_clock::now();
    entry.source = inv.sourceComponent;
    entry.requestId = inv.requestId;  // §6.2 关联 id 透传（0 = 未携带）
    entry.command = kGovernKillCommand;

    // 拒绝臂共用出口：审计先行（与 execute 时序一致），再计数/span/日志
    // 与错误响应。
    const auto reject = [&](const std::string& reason) {
        entry.accepted = false;
        appendAudit(entry);
        rejectControlAction(entry, &span, kTerminateRejectedCount,
                            "machine.process.govern.rejected", reason);
        const auto payload = toBytes(reason);
        sendResponse(inv.sourceComponent, MachineMethod::kError,
                     std::span<const std::byte>(payload));
    };

    const auto parsed = parsePidPayload(inv.payload);
    entry.args = parsed.ok ? std::to_string(parsed.value) : "";
    if (!parsed.ok) {
        reject("terminate rejected: malformed pid payload");
        return;
    }
    span.setAttribute("pid", static_cast<std::int64_t>(parsed.value));

    if (!isGovernTrustedSource(inv.sourceComponent)) {
        reject("terminate rejected: source component " +
               std::to_string(inv.sourceComponent) + " is not trusted");
        return;
    }

    // §6.1 角色门：处置 ≙ §6.1 的 retire process，administer 面需 Admin。
    if (!hasRole(inv.sourceComponent, AccessRole::Admin)) {
        reject("terminate rejected: source component " +
               std::to_string(inv.sourceComponent) + " requires Admin role");
        return;
    }

    // 目标解析：治理目标必须是主机上真实存在、非自身、非受管的进程。
    // 枚举与处置之间固有竞态（进程可先退出），MVP 以 SIGTERM 语义接受：
    // kill 返回 ESRCH 时处置记失败（ok=false）而非拒绝。
    const auto hostProcesses = agent_.enumerateHostProcesses();
    const auto target = std::find_if(hostProcesses.begin(), hostProcesses.end(),
                                     [pid = parsed.value](
                                         const ProcessSummary& process) {
                                         return process.pid == pid;
                                     });
    if (target == hostProcesses.end()) {
        reject("terminate rejected: unknown pid " +
               std::to_string(parsed.value));
        return;
    }
    if (parsed.value == currentProcessId()) {
        reject("terminate rejected: target is the daemon process itself");
        return;
    }
    if (target->managed) {
        reject("terminate rejected: pid " + std::to_string(parsed.value) +
               " is a managed process (use stop/restart)");
        return;
    }
    if (!isKillableName(target->name)) {
        reject("terminate rejected: process name '" + target->name +
               "' is not in the kill list");
        return;
    }

    const bool ok = agent_.terminateHostProcess(parsed.value);
    entry.accepted = true;
    entry.ok = ok;
    appendAudit(entry);
    telemetryCounter(kTerminateAcceptedCount).increment();
    span.setAttribute("accepted", true);
    span.setAttribute("ok", ok);
    const auto sourceValue = static_cast<std::int64_t>(entry.source);
    const auto pidValue = static_cast<std::int64_t>(parsed.value);
    const foundation::LogAttribute sourceAttr = {"source", sourceValue};
    const foundation::LogAttribute pidAttr = {"pid", pidValue};
    const foundation::LogAttribute okAttr = {"ok", ok};
    foundation::logInfo("machine.terminate", {sourceAttr, pidAttr, okAttr});
    const std::byte result = ok ? std::byte{0x01} : std::byte{0x00};
    sendResponse(inv.sourceComponent, MachineMethod::kTerminateOk,
                 std::span<const std::byte>(&result, 1));
}

}  // namespace theseed::control::machine
