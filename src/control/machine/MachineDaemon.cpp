// MachineDaemon 核心：生命周期（start/stop/tick）、TCP 分发与审计基建。
// 按面拆分的 handler 组（process / session / diagnostic / config）各居
// 独立 TU；共用的载荷解析、遥测命名与拒绝出口集中在
// MachineDaemonDetail.{h,cpp}（单一定义，见其头部注释）。

#include "theseed/control/machine/MachineDaemon.h"

#include "theseed/control/machine/MachineSnapshotCodec.h"
#include "theseed/foundation/Logger.h"
#include "theseed/foundation/Metrics.h"
#include "theseed/foundation/Tracing.h"
#include "theseed/runtime/NetworkTransport.h"
#include "theseed/runtime/TcpConnection.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

#include "MachineDaemonDetail.h"

namespace theseed::control::machine {

namespace {

// execute 分发耗时直方图（05-telemetry 控制面切片）：只被 handleInvocation
// 的 execute 臂使用，故留本 TU 匿名命名空间（不随 detail 共享）。
foundation::Histogram& executeDurationHistogram() {
    return foundation::MetricsRegistry::instance().histogram(
        detail::kExecuteDurationMs,
        foundation::Histogram::Boundaries{1.0, 5.0, 10.0, 25.0, 50.0, 100.0,
                                          250.0, 500.0, 1000.0, 2500.0},
        "machine.execute dispatch duration (ms)");
}

void logExecuteRejected(const AuditEntry& entry, const std::string& reason) {
    // 命名属性单行化：gcc 会把多行调用表达式的计数错归因到续行
    const auto sourceValue = static_cast<std::int64_t>(entry.source);
    const foundation::LogAttribute sourceAttr = {"source", sourceValue};
    const foundation::LogAttribute commandAttr = {"command", entry.command};
    const foundation::LogAttribute reasonAttr = {"reason", reason};
    foundation::logWarn("machine.execute.rejected",
                        {sourceAttr, commandAttr, reasonAttr});
}

}  // namespace

MachineDaemon::MachineDaemon(Config config, IMachineAgent& agent)
    : config_(std::move(config)), agent_(agent) {
    // 剖面元数据接线：本 daemon 是 IProfileMetaSource 的自然实现方
    // （持有采样出口指针），agent 组装 NodeReport 时向本机拉取清单。
    agent_.setProfileMetaSource(this);
}

MachineDaemon::~MachineDaemon() {  // LCOV_EXCL_LINE 私有继承 IProfileMetaSource 使析构隐式虚化：D0/D1/D2 三符号变体里基类子对象变体恒不被选中，签名行计数结构性为 0（D1 变体已在 100% 函数覆盖内）
    stop();
    // 摘除元数据接线，避免 agent 侧留悬垂指针（生命周期由调用方保证，
    // 但先于 agent 析构的 daemon 不应再被拉取）。
    agent_.setProfileMetaSource(nullptr);
}  // LCOV_EXCL_LINE 同上：闭合行计数随 D2 变体结构性为 0

bool MachineDaemon::start() {
    if (listener_.isListening()) {
        return true;  // 幂等：重复 start 不重建监听
    }

    hub_ = std::make_shared<runtime::TransportHub>(config_.componentId);
    if (!listener_.listen(config_.listenHost, config_.listenPort)) {
        hub_.reset();
        return false;
    }
    announceToCenter();
    // OTLP trace 外发（05-telemetry §2.1，默认关闭）：开启且 endpoint 合法
    // 才安装，安装成功即由导出器打数据范围启动日志；关闭/非法为无副作用
    // 空转（非法 endpoint 的 warn 由导出器记），不影响 daemon 上线。
    otlpExporter_ = std::make_unique<OtlpTraceExporter>(config_.otlpTrace);
    static_cast<void>(otlpExporter_->install());
    // OTLP metrics 外发（05-telemetry §2.2，默认关闭）：开启且 endpoint
    // 合法才安装；tick() 按 interval 驱动导出（首 tick 立即到期，同
    // reportIfDue 口径）。与 hub_ 同生命周期——tick 对其解引用不空。
    otlpMetricsExporter_ =
        std::make_unique<OtlpMetricsExporter>(config_.otlpMetrics);
    static_cast<void>(otlpMetricsExporter_->install());
    // 命名属性单行化（同上，规避续行归因）
    const auto listenPort = static_cast<std::int64_t>(listener_.localPort());
    const foundation::LogAttribute portAttr = {"listen_port", listenPort};
    const foundation::LogAttribute nodeIdAttr = {"node_id", nodeId_};
    foundation::logInfo("machine.daemon.started", {portAttr, nodeIdAttr});
    return true;
}

void MachineDaemon::announceToCenter() {
    // 身份口径 = 快照 hostname（06 §2.4 的 nodeId）。无中心出口不采样；
    // 空 hostname（异常探针）不入中心——中心侧同纪律丢弃无身份记录。
    if (config_.reportSink == nullptr && config_.auditSink == nullptr) {
        return;
    }
    nodeId_ = agent_.snapshot().host.hostname;
    // 注册只走 reportSink：auditSink-only 的部署只汇审计流，不上注册簿
    // （中心节点名册由注册+快照维持，与审计留存互不牵动）。
    if (config_.reportSink != nullptr && !nodeId_.empty()) {
        config_.reportSink->registerNode(nodeId_, std::chrono::system_clock::now());
    }
}

void MachineDaemon::stop() {
    // 优雅下线：先注销再关听——中心立即摘除，不等 pruneStale 的 TTL
    // 疑似掉线兜底。nodeId_ 已清空（二次 stop / 未 start）时跳过。
    if (config_.reportSink != nullptr && !nodeId_.empty()) {
        foundation::logInfo("machine.daemon.stopped", {{"node_id", nodeId_}});
        config_.reportSink->deregister(nodeId_);
    }
    nodeId_.clear();
    listener_.close();
    hub_.reset();
    otlpExporter_.reset();  // 导出器卸载还原全局发射器（dtor 内同样触发）
    otlpMetricsExporter_.reset();  // 卸载（install 无全局槽位，还原旗标）
}

void MachineDaemon::tick() {
    if (!hub_) {
        return;  // 未 start/已 stop：静默空转
    }

    acceptConnections();
    hub_->tick();
    processMessages();
    // §6.1 受控停机：请求应答已在分发中出站，此处收尾优雅停机（注销
    // 中心 + 关听）——先于周期上报，停机后的最后动作是干净下线而非
    // 再报一次状态。
    if (shutdownRequested_) {
        stop();
        return;
    }
    reportIfDue();
    // metrics 导出与周期上报同属周期性遥测出站：由本调用按 interval
    // 判到期（默认关闭时 install 空转，此处调用为无副作用空转）。
    static_cast<void>(otlpMetricsExporter_->exportIfDue());
    relayArtifacts();
}

void MachineDaemon::reportIfDue() {
    if (config_.reportInterval.count() <= 0 || config_.reportSink == nullptr) {
        return;  // 周期上报关闭
    }

    const auto now = std::chrono::steady_clock::now();
    // lastReportAt_ 默认 epoch：首个 tick 立即上报，保证中心侧新鲜度
    if (now - lastReportAt_ < config_.reportInterval) {
        return;
    }
    lastReportAt_ = now;
    agent_.report();
}

// 剖面回传（04 §7 中心持有副本）：发现新固化产物即推给 artifactSink。
// 推送（而非中心拉取）的选型理由：当前拓扑只有 agent→center 单向通道
// （daemon 是 TCP 服务端，中心不持有 agent 连接），拉取需要新的反向
// 传输腿，远超本批只读优先的范围；推送与审计/上报同向复用既有接缝。
void MachineDaemon::relayArtifacts() {
    if (config_.artifactSink == nullptr || config_.tickProfiler == nullptr ||
        nodeId_.empty()) {
        return;  // 回传未配置 / 采样入口关闭 / 无中心身份
    }

    const auto artifacts = config_.tickProfiler->listArtifacts();
    for (const auto& artifact : artifacts) {
        const bool relayed =
            std::find(relayedHandles_.begin(), relayedHandles_.end(),
                      artifact.handle) != relayedHandles_.end();
        if (relayed) {
            continue;
        }
        NodeProfileArtifact frame;
        frame.nodeId = nodeId_;
        frame.meta.handle = artifact.handle;
        frame.meta.tickCount = artifact.tickCount;
        frame.meta.windowMs = artifact.windowMs;
        // entity/entityType 维度无生产者：frame.meta 恒空（ProfileMeta
        // 的维度纪律），中心侧按这两维查询如实落空。
        if (!config_.tickProfiler->artifactPayload(artifact.handle,
                                                   frame.payload)) {
            continue;  // 清单与存储的窄窗竞态（环形刚逐出）：跳过
        }
        config_.artifactSink->publish(frame);
        relayedHandles_.push_back(artifact.handle);
        const foundation::LogAttribute nodeIdAttr = {"node_id", nodeId_};
        const foundation::LogAttribute handleAttr = {
            "handle", static_cast<std::int64_t>(artifact.handle)};
        const foundation::LogAttribute bytesAttr = {
            "bytes", static_cast<std::int64_t>(frame.payload.size())};
        foundation::logInfo("machine.profile.relayed",
                            {nodeIdAttr, handleAttr, bytesAttr});
    }

    // 已回传账本修剪：只留仍在产物环形里的句柄（环形逐出者不会复现，
    // 账本上界 = 产物环形容量）。
    relayedHandles_.erase(
        std::remove_if(relayedHandles_.begin(), relayedHandles_.end(),
                       [&artifacts](std::uint64_t handle) {
                           for (const auto& artifact : artifacts) {
                               if (artifact.handle == handle) {
                                   return false;
                               }
                           }
                           return true;
                       }),
        relayedHandles_.end());
}

std::vector<ProfileMeta> MachineDaemon::profileMetas() const {
    std::vector<ProfileMeta> metas;
    if (config_.tickProfiler == nullptr) {
        return metas;  // 采样入口关闭：诚实空清单
    }
    for (const auto& artifact : config_.tickProfiler->listArtifacts()) {
        ProfileMeta meta;
        meta.handle = artifact.handle;
        meta.tickCount = artifact.tickCount;
        meta.windowMs = artifact.windowMs;
        // entityId/entityType 恒空（无生产者，见 ProfileMeta 维度纪律）。
        metas.push_back(meta);
    }
    return metas;
}

bool MachineDaemon::isListening() const {
    return listener_.isListening();
}

std::uint16_t MachineDaemon::localPort() const {
    return listener_.localPort();
}

const std::vector<AuditEntry>& MachineDaemon::auditLog() const {
    return auditLog_;
}

bool MachineDaemon::isTrustedSource(runtime::ComponentId source) const {
    const auto& trusted = config_.execPolicy.trustedComponents;
    return std::find(trusted.begin(), trusted.end(), source) != trusted.end();
}

bool MachineDaemon::isCommandAllowed(const std::string& command) const {
    const auto& allowed = config_.execPolicy.allowedCommands;
    return std::find(allowed.begin(), allowed.end(), command) != allowed.end();
}

void MachineDaemon::appendAudit(const AuditEntry& entry) {
    if (config_.auditCapacity != 0) {
        if (auditLog_.size() == config_.auditCapacity) {
            auditLog_.erase(auditLog_.begin());  // 环形：满后丢最旧
        }
        auditLog_.push_back(entry);
    }
    // 本地环形容量只约束本地视图；中心聚合由 auditSink 独立开关（04 §8：
    // 拒绝与执行同权留痕，无审计盲区）。无身份（空 hostname）不归属，
    // 中心侧同纪律丢弃。
    if (config_.auditSink != nullptr && !nodeId_.empty()) {
        NodeAuditEntry record;
        record.nodeId = nodeId_;
        record.entry = entry;
        config_.auditSink->publish(record);
    }
}

void MachineDaemon::acceptConnections() {
    while (auto conn = listener_.accept()) {
        runtime::NetworkTransport::Config transportConfig;
        transportConfig.maxMessageSize = config_.transportMaxMessageSize;
        auto transport = std::make_shared<runtime::NetworkTransport>(
            conn, transportConfig);
        // 服务端模式注册：对端身份由其首条请求的 sourceComponent 自报
        // （与 DBApp 同机制），回复才能路由回对端。
        hub_->attachServerTransport(transport);
    }
}

void MachineDaemon::processMessages() {
    runtime::RuntimeInvocation inv;
    while (hub_->receive(config_.componentId, &inv, 1) > 0) {
        handleInvocation(inv);
    }
}

void MachineDaemon::handleInvocation(runtime::RuntimeInvocation& inv) {
    using namespace detail;
    if (inv.method == MachineMethod::kSnapshot) {
        // §6.1 角色门：inspect 面需 ReadOnly 及以上。接受的快照照旧不留
        // 审计（避免只读噪声）；拒绝是动作，照记审计与计数。
        if (!hasRole(inv.sourceComponent, AccessRole::ReadOnly)) {
            AuditEntry entry;
            entry.timestamp = std::chrono::system_clock::now();
            entry.source = inv.sourceComponent;
            entry.requestId = inv.requestId;  // §6.2 关联 id 透传（0 = 未携带）
            entry.command = kSnapshotCommand;
            entry.accepted = false;
            appendAudit(entry);
            const std::string reason =
                "snapshot rejected: source component " +
                std::to_string(inv.sourceComponent) + " requires ReadOnly role";
            telemetryCounter(kSnapshotRejectedCount).increment();
            const auto payload = toBytes(reason);
            sendResponse(inv.sourceComponent, MachineMethod::kError,
                         std::span<const std::byte>(payload));
            return;
        }
        telemetryCounter(kSnapshotCount).increment();
        const auto snapshotJson = toBytes(formatSnapshotJson(agent_.snapshot()));
        sendResponse(inv.sourceComponent, MachineMethod::kSnapshotOk,
                     std::span<const std::byte>(snapshotJson));
        return;
    }

    if (inv.method == MachineMethod::kAudit) {
        // 同 snapshot：inspect 面角色门，拒绝留痕、接受不留。
        if (!hasRole(inv.sourceComponent, AccessRole::ReadOnly)) {
            AuditEntry entry;
            entry.timestamp = std::chrono::system_clock::now();
            entry.source = inv.sourceComponent;
            entry.requestId = inv.requestId;  // §6.2 关联 id 透传（0 = 未携带）
            entry.command = kAuditQueryCommand;
            entry.accepted = false;
            appendAudit(entry);
            const std::string reason =
                "audit query rejected: source component " +
                std::to_string(inv.sourceComponent) + " requires ReadOnly role";
            telemetryCounter(kAuditRejectedCount).increment();
            const auto payload = toBytes(reason);
            sendResponse(inv.sourceComponent, MachineMethod::kError,
                         std::span<const std::byte>(payload));
            return;
        }
        telemetryCounter(kAuditCount).increment();
        handleAudit(inv);
        return;
    }

    if (inv.method == MachineMethod::kExecute) {
        // 控制面 trace：execute 是关键受控动作（05 §3.1），拒绝也进 span
        // ——"谁在何时被拒"与审计同视角；日志在 span 内自动带 trace 关联。
        foundation::SpanScope span("machine.execute");
        span.setAttribute("source",
                          static_cast<std::int64_t>(inv.sourceComponent));
        // payload = command '\0' args
        const auto separator =
            std::find(inv.payload.begin(), inv.payload.end(), std::byte{0});
        AuditEntry entry;
        entry.timestamp = std::chrono::system_clock::now();
        entry.source = inv.sourceComponent;
        entry.requestId = inv.requestId;  // §6.2 关联 id 透传（0 = 未携带）
        if (separator == inv.payload.end()) {
            entry.accepted = false;
            appendAudit(entry);
            telemetryCounter(kExecuteRejectedCount).increment();
            span.setAttribute("accepted", false);
            const std::string reason =
                "malformed execute payload: missing NUL separator";
            span.setAttribute("reason", reason);
            logExecuteRejected(entry, reason);
            const auto payload = toBytes(reason);
            sendResponse(inv.sourceComponent, MachineMethod::kError,
                         std::span<const std::byte>(payload));
            return;
        }

        const auto* payloadBegin = inv.payload.data();
        const auto commandLength =
            static_cast<std::size_t>(separator - inv.payload.begin());
        entry.command.assign(reinterpret_cast<const char*>(payloadBegin),
                             commandLength);
        entry.args.assign(reinterpret_cast<const char*>(payloadBegin + commandLength + 1),
                          inv.payload.size() - commandLength - 1);
        if (entry.command.empty()) {
            entry.accepted = false;
            appendAudit(entry);
            telemetryCounter(kExecuteRejectedCount).increment();
            span.setAttribute("accepted", false);
            span.setAttribute("reason", "empty command");
            logExecuteRejected(entry, "empty command");
            const auto payload = toBytes("empty command");
            sendResponse(inv.sourceComponent, MachineMethod::kError,
                         std::span<const std::byte>(payload));
            return;
        }

        // 权限边界（04 MVP “少量受控命令”）：来源与命令双白名单，安全
        // 缺省全拒；先于 agent 分发，拒绝照记审计（accepted=false）。
        if (!isTrustedSource(inv.sourceComponent)) {
            entry.accepted = false;
            appendAudit(entry);
            telemetryCounter(kExecuteRejectedCount).increment();
            span.setAttribute("accepted", false);
            const std::string reason = "execute rejected: source component " +
                                       std::to_string(inv.sourceComponent) +
                                       " is not trusted";
            span.setAttribute("reason", reason);
            logExecuteRejected(entry, reason);
            const auto payload = toBytes(reason);
            sendResponse(inv.sourceComponent, MachineMethod::kError,
                         std::span<const std::byte>(payload));
            return;
        }
        if (!isCommandAllowed(entry.command)) {
            entry.accepted = false;
            appendAudit(entry);
            telemetryCounter(kExecuteRejectedCount).increment();
            span.setAttribute("accepted", false);
            const std::string reason =
                "execute rejected: command not allowed: " + entry.command;
            span.setAttribute("reason", reason);
            logExecuteRejected(entry, reason);
            const auto payload = toBytes(reason);
            sendResponse(inv.sourceComponent, MachineMethod::kError,
                         std::span<const std::byte>(payload));
            return;
        }
        // §6.1 角色门（叠加在策略门之上）：execute 是 operate 面，需
        // Operator 及以上；拒绝同权留痕（审计 + 计数 + span 标记）。
        if (!hasRole(inv.sourceComponent, AccessRole::Operator)) {
            entry.accepted = false;
            appendAudit(entry);
            telemetryCounter(kExecuteRejectedCount).increment();
            span.setAttribute("accepted", false);
            const std::string reason =
                "execute rejected: source component " +
                std::to_string(inv.sourceComponent) +
                " requires Operator role";
            span.setAttribute("reason", reason);
            logExecuteRejected(entry, reason);
            const auto payload = toBytes(reason);
            sendResponse(inv.sourceComponent, MachineMethod::kError,
                         std::span<const std::byte>(payload));
            return;
        }

        entry.accepted = true;
        const auto execStart = std::chrono::steady_clock::now();
        entry.ok = agent_.execute(entry.command, entry.args);
        const std::chrono::duration<double, std::milli> execElapsed =
            std::chrono::steady_clock::now() - execStart;
        executeDurationHistogram().observe(execElapsed.count());
        telemetryCounter(kExecuteAcceptedCount).increment();
        span.setAttribute("accepted", true);
        span.setAttribute("ok", entry.ok);
        appendAudit(entry);
        const auto sourceValue = static_cast<std::int64_t>(entry.source);
        const foundation::LogAttribute sourceAttr = {"source", sourceValue};
        const foundation::LogAttribute commandAttr = {"command", entry.command};
        const foundation::LogAttribute okAttr = {"ok", entry.ok};
        foundation::logInfo("machine.execute",
                            {sourceAttr, commandAttr, okAttr});
        const std::byte result = entry.ok ? std::byte{0x01} : std::byte{0x00};
        sendResponse(inv.sourceComponent, MachineMethod::kExecuteOk,
                     std::span<const std::byte>(&result, 1));
        return;
    }

    // 采样能力出口为空 = 诊断入口关闭：machine.profile.* 视同未知方法，
    // 落到统一 unknown-method 错误臂（不进审计——入口未开，无动作发生）。
    const bool profileEntryOpen = config_.tickProfiler != nullptr;

    if (profileEntryOpen && inv.method == MachineMethod::kProfileTrigger) {
        handleProfileTrigger(inv);
        return;
    }

    if (profileEntryOpen && inv.method == MachineMethod::kProfiles) {
        handleProfiles(inv);
        return;
    }

    if (profileEntryOpen && inv.method == MachineMethod::kProfile) {
        handleProfileDownload(inv);
        return;
    }

    if (inv.method == MachineMethod::kProcesses) {
        handleProcesses(inv);
        return;
    }

    if (inv.method == MachineMethod::kTerminate) {
        handleTerminate(inv);
        return;
    }

    if (inv.method == MachineMethod::kConfigApply) {
        handleConfigApply(inv);
        return;
    }

    // 会话入口关闭（未配 sessionStore）= 视同未知方法，落统一错误臂
    //（不进审计——入口未开，无动作发生），与采样入口同口径。
    const bool sessionEntryOpen = config_.sessionStore != nullptr;

    if (sessionEntryOpen && inv.method == MachineMethod::kKickSession) {
        handleKickSession(inv);
        return;
    }

    if (sessionEntryOpen && inv.method == MachineMethod::kListSessions) {
        handleListSessions(inv);
        return;
    }

    if (sessionEntryOpen && inv.method == MachineMethod::kKickSessions) {
        handleKickSessions(inv);
        return;
    }

    // 续期入口关闭（未配 sessionStore 或续期策略 TTL 为 0）= 视同未知
    // 方法——无策略即无命令（缺省安全）。
    if (sessionEntryOpen && config_.sessionRenewalTtl.count() > 0 &&
        inv.method == MachineMethod::kExtendSessions) {
        handleExtendSessions(inv);
        return;
    }

    if (inv.method == MachineMethod::kSetDraining) {
        handleSetDraining(inv);
        return;
    }

    if (inv.method == MachineMethod::kShutdown) {
        handleShutdown(inv);
        return;
    }

    telemetryCounter(kUnknownMethodCount).increment();
    AuditEntry rejected;
    rejected.timestamp = std::chrono::system_clock::now();
    rejected.source = inv.sourceComponent;
    rejected.requestId = inv.requestId;
    rejected.command = inv.method;
    appendAudit(rejected);
    foundation::logWarn("machine.unknown_method", {{"method", inv.method}});
    const auto reason = toBytes("unknown method: " + inv.method);
    sendResponse(inv.sourceComponent, MachineMethod::kError,
                 std::span<const std::byte>(reason));
}

void MachineDaemon::handleAudit(runtime::RuntimeInvocation& inv) {
    std::ostringstream out;
    out << "[";
    for (std::size_t index = 0; index < auditLog_.size(); ++index) {
        if (index != 0) {
            out << ',';
        }
        out << detail::auditEntryJson(auditLog_[index]);
    }
    out << "]";
    const auto json = detail::toBytes(out.str());
    sendResponse(inv.sourceComponent, MachineMethod::kAuditOk,
                 std::span<const std::byte>(json));
}

bool MachineDaemon::hasRole(runtime::ComponentId source,
                            AccessRole required) const {
    return roleMeets(roleFor(config_.roleBindings, source), required);
}

void MachineDaemon::sendResponse(runtime::ComponentId target,
                                 const std::string& method,
                                 std::span<const std::byte> payload) {
    runtime::RuntimeInvocation resp;
    resp.sourceComponent = config_.componentId;
    resp.targetComponent = target;
    resp.entityId = 0;
    resp.method = method;
    resp.payload = std::vector<std::byte>(payload.begin(), payload.end());
    hub_->send(std::move(resp));
    hub_->flush();
}

}  // namespace theseed::control::machine
