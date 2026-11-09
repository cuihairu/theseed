// MachineDaemon 的会话运维面（§6.1 受控命令 + §8 Phase 2 会话运维）。
// 会话入口（kick-session / list-sessions / kick-sessions / extend-sessions）
// 共用 SessionStore 二级索引与作用域选择器；节点生命周期命令（draining /
// shutdown）与踢人联动通知同归本面。来源白名单与角色门叠加，令牌原文
// 不出进程（审计与协议出口一律长度指纹）。

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

bool MachineDaemon::isNodeOpsTrusted(runtime::ComponentId source) const {
    const auto& trusted = config_.nodeOpsPolicy.trustedComponents;
    return std::find(trusted.begin(), trusted.end(), source) != trusted.end();
}

// §6.1 kick session（Operator 级）：吊销 SessionStore 里的会话令牌——
// 令牌是 redis 共享存储的真实会话面（LoginApp 写入，吊销后 load 不
// 复命中，全集群生效）。令牌原文不进审计（只留长度指纹）。
void MachineDaemon::handleKickSession(runtime::RuntimeInvocation& inv) {
    foundation::SpanScope span("machine.kick-session");
    span.setAttribute("source",
                      static_cast<std::int64_t>(inv.sourceComponent));

    AuditEntry entry;
    entry.timestamp = std::chrono::system_clock::now();
    entry.source = inv.sourceComponent;
    entry.requestId = inv.requestId;  // §6.2 关联 id 透传（0 = 未携带）
    entry.command = kKickCommand;

    const auto reject = [&](const std::string& reason) {
        entry.accepted = false;
        appendAudit(entry);
        rejectControlAction(entry, &span, kKickRejectedCount,
                            "machine.session.rejected", reason);
        const auto payload = toBytes(reason);
        sendResponse(inv.sourceComponent, MachineMethod::kError,
                     std::span<const std::byte>(payload));
    };

    // 空载荷的 data() 可能为空指针，先护栏再取字节（与 pid/句柄解析同纪律）。
    std::string token;
    if (!inv.payload.empty()) {
        token.assign(reinterpret_cast<const char*>(inv.payload.data()),
                     inv.payload.size());
    }
    entry.args = redactSessionToken(token);
    if (token.empty()) {
        reject("kick session rejected: malformed token payload");
        return;
    }

    if (!isNodeOpsTrusted(inv.sourceComponent)) {
        reject("kick session rejected: source component " +
               std::to_string(inv.sourceComponent) + " is not trusted");
        return;
    }

    // §6.1 角色门（叠加在来源白名单之上）：会话处置是 operate 面。
    if (!hasRole(inv.sourceComponent, AccessRole::Operator)) {
        reject("kick session rejected: source component " +
               std::to_string(inv.sourceComponent) +
               " requires Operator role");
        return;
    }

    // 吊销前先取会话行：联动通知需要 account/realm（指纹已有）。行取
    // 不到 = 未知令牌，与吊销失败同口径具名拒绝。
    const auto session = config_.sessionStore->load(token);
    if (!session) {
        reject("kick session rejected: unknown session token");
        return;
    }

    // 吊销即生效：revoke 对未知令牌返回 false（读取后、吊销前被并发
    // 摘除的窗口）——具名区分于"从未存在"，两臂都如实入审计。
    if (!config_.sessionStore->revoke(token)) {
        reject("kick session rejected: session vanished before revocation");
        return;
    }

    entry.accepted = true;
    entry.ok = true;
    appendAudit(entry);
    telemetryCounter(kKickAcceptedCount).increment();
    span.setAttribute("accepted", true);
    const auto sourceValue = static_cast<std::int64_t>(entry.source);
    const foundation::LogAttribute sourceAttr = {"source", sourceValue};
    const foundation::LogAttribute sessionAttr = {"session", entry.args};
    foundation::logInfo("machine.kick-session", {sourceAttr, sessionAttr});
    const std::byte result = std::byte{0x01};
    sendResponse(inv.sourceComponent, MachineMethod::kKickSessionOk,
                 std::span<const std::byte>(&result, 1));
    notifySessionRevoked(session->accountId, session->realmId, entry.args,
                         "operator.kick");
}

// §8 Phase 2 会话枚举（inspect 面，≥ReadOnly）：经 SessionStore 二级
// 索引列出活会话。令牌原文不出进程——协议行只带长度指纹 + 运维可见
// 属性（账号/领域/user id），metadata 是客户端态不进运维面。只读动作
// 不进 trace（与清单/审计查询同口径：无 span），接受照记审计（含
// count，供中心侧对账）。
void MachineDaemon::handleListSessions(runtime::RuntimeInvocation& inv) {
    AuditEntry entry;
    entry.timestamp = std::chrono::system_clock::now();
    entry.source = inv.sourceComponent;
    entry.requestId = inv.requestId;  // §6.2 关联 id 透传（0 = 未携带）
    entry.command = kListSessionsCommand;

    const auto reject = [&](const std::string& reason) {
        entry.accepted = false;
        appendAudit(entry);
        rejectControlAction(entry, nullptr, kListSessionsRejectedCount,
                            "machine.session.rejected", reason);
        const auto payload = toBytes(reason);
        sendResponse(inv.sourceComponent, MachineMethod::kError,
                     std::span<const std::byte>(payload));
    };

    if (!isNodeOpsTrusted(inv.sourceComponent)) {
        reject("session listing rejected: source component " +
               std::to_string(inv.sourceComponent) + " is not trusted");
        return;
    }

    // §6.1 角色门（叠加在来源白名单之上）：枚举是 inspect 面。
    if (!hasRole(inv.sourceComponent, AccessRole::ReadOnly)) {
        reject("session listing rejected: source component " +
               std::to_string(inv.sourceComponent) +
               " requires ReadOnly role");
        return;
    }

    const auto views = config_.sessionStore->listSessions();
    std::ostringstream out;
    out << "[";
    for (std::size_t i = 0; i < views.size(); ++i) {
        if (i != 0) {
            out << ',';
        }
        // 先拼字段再入流：多行 << 链会让 gcc 把覆盖计数错误归因到续行
        const std::string sessionField =
            "\"session\":\"" + redactSessionToken(views[i].token) + "\"";
        const std::string accountField =
            "\"account\":\"" + escapeJsonString(views[i].accountId) + "\"";
        const std::string realmField =
            "\"realm\":\"" + escapeJsonString(views[i].realmId) + "\"";
        const std::string userField =
            "\"user_id\":" + std::to_string(views[i].userId);
        out << "{" << sessionField << "," << accountField << ","
            << realmField << "," << userField << "}";
    }
    out << "]";

    entry.accepted = true;
    entry.ok = true;
    entry.args = "count=" + std::to_string(views.size());
    appendAudit(entry);
    telemetryCounter(kListSessionsAcceptedCount).increment();
    const foundation::LogAttribute countAttr = {"count", entry.args};
    foundation::logInfo("machine.list-sessions", {countAttr});
    const auto json = toBytes(out.str());
    sendResponse(inv.sourceComponent, MachineMethod::kListSessionsOk,
                 std::span<const std::byte>(json));
}

// §8 Phase 2 批量 kick（operate 面，≥Operator）：载荷 = 作用域选择器
// （all | account=<id> | realm=<id>），逐个吊销、成功与失败分列回给
// 调用方。部分失败（枚举后、吊销前被并发摘除——跨进程竞争的真实臂）
// 仍算动作接受：选择器合法、动作已执行，missing 明细就是给运维的对账
// 凭证。审计只记 scope 与计数，令牌原文与指纹序列都不进审计环。
void MachineDaemon::handleKickSessions(runtime::RuntimeInvocation& inv) {
    foundation::SpanScope span("machine.kick-sessions");
    span.setAttribute("source",
                      static_cast<std::int64_t>(inv.sourceComponent));

    AuditEntry entry;
    entry.timestamp = std::chrono::system_clock::now();
    entry.source = inv.sourceComponent;
    entry.requestId = inv.requestId;  // §6.2 关联 id 透传（0 = 未携带）
    entry.command = kKickSessionsCommand;

    const auto reject = [&](const std::string& reason) {
        entry.accepted = false;
        appendAudit(entry);
        rejectControlAction(entry, &span, kKickSessionsRejectedCount,
                            "machine.session.rejected", reason);
        const auto payload = toBytes(reason);
        sendResponse(inv.sourceComponent, MachineMethod::kError,
                     std::span<const std::byte>(payload));
    };

    // 空载荷的 data() 可能为空指针，先护栏再取字节（与单令牌 kick 同纪律）。
    std::string selector;
    if (!inv.payload.empty()) {
        selector.assign(reinterpret_cast<const char*>(inv.payload.data()),
                        inv.payload.size());
    }
    const SessionScope scope = parseSessionScope(selector);
    if (scope.valid) {
        // args 走 key=value 原文（序列化时由 auditEntryJson 统一转义）
        entry.args = "scope=" + selector;
    }
    if (!scope.valid) {
        reject("kick sessions rejected: malformed scope selector "
               "(expected all | account=<id> | realm=<id>)");
        return;
    }

    if (!isNodeOpsTrusted(inv.sourceComponent)) {
        reject("kick sessions rejected: source component " +
               std::to_string(inv.sourceComponent) + " is not trusted");
        return;
    }

    // §6.1 角色门（叠加在来源白名单之上）：批量处置是 operate 面。
    if (!hasRole(inv.sourceComponent, AccessRole::Operator)) {
        reject("kick sessions rejected: source component " +
               std::to_string(inv.sourceComponent) +
               " requires Operator role");
        return;
    }

    const auto views = config_.sessionStore->listSessions();
    std::vector<std::string> revoked;
    std::vector<std::string> missing;
    std::size_t requested = 0;
    for (const auto& view : views) {
        const bool matches = scope.all ||
                             (scope.byAccount ? view.accountId == scope.value
                                              : view.realmId == scope.value);
        if (!matches) {
            continue;
        }
        ++requested;
        if (config_.sessionStore->revoke(view.token)) {
            const std::string fingerprint = redactSessionToken(view.token);
            revoked.push_back(fingerprint);
            // 联动通知：批量臂逐令牌推送（各行自带 account/realm，接收方
            // 按行定位本地会话；best-effort，不因送达失败回滚吊销）。
            notifySessionRevoked(view.accountId, view.realmId, fingerprint,
                                 "operator.kick.batch");
        } else {
            // 枚举与吊销之间被并发摘除：按失败列报，指纹同款脱敏。
            missing.push_back(redactSessionToken(view.token));
        }
    }

    // 应答 JSON：requested/revoked/missing 分列（指纹序列只进应答体，
    // 审计 args 保持 scope + 计数摘要）。
    std::ostringstream out;
    out << "{\"requested\":" << requested << ",\"revoked\":[";
    for (std::size_t i = 0; i < revoked.size(); ++i) {
        if (i != 0) {
            out << ',';
        }
        out << '"' << revoked[i] << '"';
    }
    out << "],\"missing\":[";
    for (std::size_t i = 0; i < missing.size(); ++i) {
        if (i != 0) {
            out << ',';
        }
        out << '"' << missing[i] << '"';
    }
    out << "]}";

    entry.accepted = true;
    entry.ok = true;
    entry.args = "scope=" + selector +
                 " revoked=" + std::to_string(revoked.size()) +
                 " missing=" + std::to_string(missing.size());
    appendAudit(entry);
    telemetryCounter(kKickSessionsAcceptedCount).increment();
    span.setAttribute("accepted", true);
    span.setAttribute("requested", static_cast<std::int64_t>(requested));
    const auto sourceValue = static_cast<std::int64_t>(entry.source);
    const foundation::LogAttribute sourceAttr = {"source", sourceValue};
    const foundation::LogAttribute scopeAttr = {"scope", selector};
    foundation::logInfo("machine.kick-sessions", {sourceAttr, scopeAttr});
    const auto json = toBytes(out.str());
    sendResponse(inv.sourceComponent, MachineMethod::kKickSessionsOk,
                 std::span<const std::byte>(json));
}

// §8 Phase 2 会话续期（operate 面，≥Operator，≙ 续期策略）：把选中
// 会话 refresh 到续期策略 TTL（Config.sessionRenewalTtl——策略值非
// 调用方临时指定，扩策略须改配置评审，与 §6.3 白名单同纪律）。用途：
// 维护窗口前批量滑动过期，防无关批量掉线。选择器与应答口径同
// kick-sessions（extended/missing 分列，指纹脱敏）。
void MachineDaemon::handleExtendSessions(runtime::RuntimeInvocation& inv) {
    foundation::SpanScope span("machine.extend-sessions");
    span.setAttribute("source",
                      static_cast<std::int64_t>(inv.sourceComponent));

    AuditEntry entry;
    entry.timestamp = std::chrono::system_clock::now();
    entry.source = inv.sourceComponent;
    entry.requestId = inv.requestId;  // §6.2 关联 id 透传（0 = 未携带）
    entry.command = kExtendSessionsCommand;

    const auto reject = [&](const std::string& reason) {
        entry.accepted = false;
        appendAudit(entry);
        rejectControlAction(entry, &span, kExtendSessionsRejectedCount,
                            "machine.session.rejected", reason);
        const auto payload = toBytes(reason);
        sendResponse(inv.sourceComponent, MachineMethod::kError,
                     std::span<const std::byte>(payload));
    };

    // 空载荷的 data() 可能为空指针，先护栏再取字节（与 kick 族同纪律）。
    std::string selector;
    if (!inv.payload.empty()) {
        selector.assign(reinterpret_cast<const char*>(inv.payload.data()),
                        inv.payload.size());
    }
    const SessionScope scope = parseSessionScope(selector);
    if (scope.valid) {
        // args 走 key=value 原文（序列化时由 auditEntryJson 统一转义）
        entry.args = "scope=" + selector;
    }
    if (!scope.valid) {
        reject("extend sessions rejected: malformed scope selector "
               "(expected all | account=<id> | realm=<id>)");
        return;
    }

    if (!isNodeOpsTrusted(inv.sourceComponent)) {
        reject("extend sessions rejected: source component " +
               std::to_string(inv.sourceComponent) + " is not trusted");
        return;
    }

    // §6.1 角色门（叠加在来源白名单之上）：会话续期是 operate 面。
    if (!hasRole(inv.sourceComponent, AccessRole::Operator)) {
        reject("extend sessions rejected: source component " +
               std::to_string(inv.sourceComponent) +
               " requires Operator role");
        return;
    }

    const auto views = config_.sessionStore->listSessions();
    std::vector<std::string> extended;
    std::vector<std::string> missing;
    std::size_t requested = 0;
    for (const auto& view : views) {
        const bool matches = scope.all ||
                             (scope.byAccount ? view.accountId == scope.value
                                              : view.realmId == scope.value);
        if (!matches) {
            continue;
        }
        ++requested;
        if (config_.sessionStore->refresh(view.token,
                                          config_.sessionRenewalTtl)) {
            extended.push_back(redactSessionToken(view.token));
        } else {
            // 枚举与续期之间被并发摘除/过期：按失败列报，指纹同款脱敏。
            missing.push_back(redactSessionToken(view.token));
        }
    }

    // 应答 JSON：与 kick-sessions 同形（extended 列替换 revoked 列）。
    std::ostringstream out;
    out << "{\"requested\":" << requested << ",\"extended\":[";
    for (std::size_t i = 0; i < extended.size(); ++i) {
        if (i != 0) {
            out << ',';
        }
        out << '"' << extended[i] << '"';
    }
    out << "],\"missing\":[";
    for (std::size_t i = 0; i < missing.size(); ++i) {
        if (i != 0) {
            out << ',';
        }
        out << '"' << missing[i] << '"';
    }
    out << "]}";

    entry.accepted = true;
    entry.ok = true;
    entry.args = "scope=" + selector +
                 " extended=" + std::to_string(extended.size()) +
                 " missing=" + std::to_string(missing.size());
    appendAudit(entry);
    telemetryCounter(kExtendSessionsAcceptedCount).increment();
    span.setAttribute("accepted", true);
    span.setAttribute("requested", static_cast<std::int64_t>(requested));
    const auto sourceValue = static_cast<std::int64_t>(entry.source);
    const foundation::LogAttribute sourceAttr = {"source", sourceValue};
    const foundation::LogAttribute scopeAttr = {"scope", selector};
    foundation::logInfo("machine.extend-sessions", {sourceAttr, scopeAttr});
    const auto json = toBytes(out.str());
    sendResponse(inv.sourceComponent, MachineMethod::kExtendSessionsOk,
                 std::span<const std::byte>(json));
}

// §6.1 set draining（Operator 级）：翻转节点排水位。agent 是节点状态
// 持有方——置位后快照 RPC（inspect 读侧）与周期上报（中心聚合）同一
// 数据源透出，中心侧 latest/snapshotNodes 即见。
void MachineDaemon::handleSetDraining(runtime::RuntimeInvocation& inv) {
    foundation::SpanScope span("machine.set-draining");
    span.setAttribute("source",
                      static_cast<std::int64_t>(inv.sourceComponent));

    AuditEntry entry;
    entry.timestamp = std::chrono::system_clock::now();
    entry.source = inv.sourceComponent;
    entry.requestId = inv.requestId;  // §6.2 关联 id 透传（0 = 未携带）
    entry.command = kDrainCommand;

    const auto reject = [&](const std::string& reason) {
        entry.accepted = false;
        appendAudit(entry);
        rejectControlAction(entry, &span, kDrainRejectedCount,
                            "machine.node.rejected", reason);
        const auto payload = toBytes(reason);
        sendResponse(inv.sourceComponent, MachineMethod::kError,
                     std::span<const std::byte>(payload));
    };

    // 载荷 = 1 字节 0x01/0x00（与 execute/terminate 应答字节同约定）。
    const bool parsed =
        inv.payload.size() == 1 &&
        (inv.payload[0] == std::byte{0x01} || inv.payload[0] == std::byte{0x00});
    const bool draining = parsed && inv.payload[0] == std::byte{0x01};
    entry.args = parsed ? (std::string("draining=") + (draining ? "true" : "false"))
                        : "";
    if (!parsed) {
        reject("set draining rejected: malformed payload "
               "(expected 1 byte 0x01/0x00)");
        return;
    }

    if (!isNodeOpsTrusted(inv.sourceComponent)) {
        reject("set draining rejected: source component " +
               std::to_string(inv.sourceComponent) + " is not trusted");
        return;
    }

    // §6.1 角色门（叠加在来源白名单之上）：排水决策是 operate 面。
    if (!hasRole(inv.sourceComponent, AccessRole::Operator)) {
        reject("set draining rejected: source component " +
               std::to_string(inv.sourceComponent) +
               " requires Operator role");
        return;
    }

    agent_.setDraining(draining);
    entry.accepted = true;
    entry.ok = true;
    appendAudit(entry);
    telemetryCounter(kDrainAcceptedCount).increment();
    span.setAttribute("accepted", true);
    span.setAttribute("draining", draining);
    const auto sourceValue = static_cast<std::int64_t>(entry.source);
    const foundation::LogAttribute sourceAttr = {"source", sourceValue};
    const foundation::LogAttribute drainingAttr = {"draining", draining};
    foundation::logInfo("machine.set-draining", {sourceAttr, drainingAttr});
    const std::byte result = std::byte{0x01};
    sendResponse(inv.sourceComponent, MachineMethod::kSetDrainingOk,
                 std::span<const std::byte>(&result, 1));
}

// §6.1 controlled shutdown（Admin 级）：优雅停机——应答出站后置位，
// 本 tick 收尾时 stop()（注销中心 + 关听，见 tick()）。延迟到 tick 末
// 是因为 hub 不得在消息分发中途销毁；排队中的既有消息照常应答。停机
// 不波及受管子进程（编排仍走 execute stop 显式下达）也不触及主机上的
// 其他进程（那是治理面的守卫领域）——"受控"的边界就是本 daemon 的
// 生命周期自身。
void MachineDaemon::handleShutdown(runtime::RuntimeInvocation& inv) {
    foundation::SpanScope span("machine.shutdown");
    span.setAttribute("source",
                      static_cast<std::int64_t>(inv.sourceComponent));

    AuditEntry entry;
    entry.timestamp = std::chrono::system_clock::now();
    entry.source = inv.sourceComponent;
    entry.requestId = inv.requestId;  // §6.2 关联 id 透传（0 = 未携带）
    entry.command = kShutdownCommand;

    const auto reject = [&](const std::string& reason) {
        entry.accepted = false;
        appendAudit(entry);
        rejectControlAction(entry, &span, kShutdownRejectedCount,
                            "machine.node.rejected", reason);
        const auto payload = toBytes(reason);
        sendResponse(inv.sourceComponent, MachineMethod::kError,
                     std::span<const std::byte>(payload));
    };

    if (!isNodeOpsTrusted(inv.sourceComponent)) {
        reject("shutdown rejected: source component " +
               std::to_string(inv.sourceComponent) + " is not trusted");
        return;
    }

    // §6.1 角色门：受控停机是 administer 面，需 Admin。
    if (!hasRole(inv.sourceComponent, AccessRole::Admin)) {
        reject("shutdown rejected: source component " +
               std::to_string(inv.sourceComponent) + " requires Admin role");
        return;
    }

    shutdownRequested_ = true;
    entry.accepted = true;
    entry.ok = true;
    appendAudit(entry);
    telemetryCounter(kShutdownAcceptedCount).increment();
    span.setAttribute("accepted", true);
    // 单行具名语句：多行调用表达式的行计数会被 gcc 归因到首行（与
    // kick/set-draining 同款写法）。
    const foundation::LogAttribute sourceAttr = {
        "source", static_cast<std::int64_t>(entry.source)};
    foundation::logInfo("machine.shutdown", {sourceAttr});
    const std::byte result = std::byte{0x01};
    sendResponse(inv.sourceComponent, MachineMethod::kShutdownOk,
                 std::span<const std::byte>(&result, 1));
}

// 踢人联动通知（§8 Phase 2）：吊销成功后推给 sessionNotifyComponent。
// best-effort——目标未连接即丢弃并告警（无重试/确认：吊销事实以存储为
// 准，通知只是联动提示；重连后接收方可经 machine.list-sessions 对账）。
void MachineDaemon::notifySessionRevoked(const std::string& accountId,
                                         const std::string& realmId,
                                         const std::string& tokenFingerprint,
                                         const char* reason) {
    if (config_.sessionNotifyComponent == 0) {
        return;  // 未配置通知目标：推送关闭（不是错误，不告警）
    }
    // 先拼字段再入流：多行 << 链会让 gcc 把覆盖计数错误归因到续行
    const std::string accountField =
        "\"account\":\"" + escapeJsonString(accountId) + "\"";
    const std::string realmField =
        "\"realm\":\"" + escapeJsonString(realmId) + "\"";
    const std::string sessionField =
        "\"session\":\"" + escapeJsonString(tokenFingerprint) + "\"";
    const std::string reasonField =
        std::string("\"reason\":\"") + reason + "\"";
    std::ostringstream out;
    out << "{" << accountField << "," << realmField << "," << sessionField
        << "," << reasonField << "}";

    runtime::RuntimeInvocation notice;
    notice.sourceComponent = config_.componentId;
    notice.targetComponent = config_.sessionNotifyComponent;
    notice.entityId = 0;
    notice.method = MachineMethod::kSessionRevoked;
    notice.payload = toBytes(out.str());
    if (hub_->send(std::move(notice)) == runtime::SendResult::Accepted) {
        hub_->flush();
        telemetryCounter(kSessionNotifyCount).increment();
        return;
    }
    const auto targetValue =
        static_cast<std::int64_t>(config_.sessionNotifyComponent);
    const foundation::LogAttribute targetAttr = {"target", targetValue};
    foundation::logWarn("machine.session.notify.dropped", {targetAttr});
}

}  // namespace theseed::control::machine
