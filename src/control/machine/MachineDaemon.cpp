#include "theseed/control/machine/MachineDaemon.h"

#include "theseed/control/machine/MachineSnapshotCodec.h"
#include "theseed/foundation/Logger.h"
#include "theseed/foundation/Metrics.h"
#include "theseed/foundation/Tracing.h"
#include "theseed/runtime/NetworkTransport.h"
#include "theseed/runtime/TcpConnection.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstring>
#include <sstream>
#include <utility>
#include <vector>

namespace theseed::control::machine {

namespace {

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

    std::ostringstream out;
    out << "{\"ts\":" << millis << ",\"source\":" << entry.source << ","
        << commandField << "," << argsField << "," << acceptedField << ","
        << okField << "}";
    return out.str();
}

// 控制面遥测命名（05-telemetry MVP：结构化 logs + 基础 metrics + 关键
// traces 的控制面切片）。指标名进程级单例、snake_case + _count 后缀，
// 与 LoginApp/DBApp 同族口径。
constexpr const char* kSnapshotCount = "machine_snapshot_count";
constexpr const char* kAuditCount = "machine_audit_count";
constexpr const char* kExecuteAcceptedCount = "machine_execute_accepted_count";
constexpr const char* kExecuteRejectedCount = "machine_execute_rejected_count";
constexpr const char* kUnknownMethodCount = "machine_unknown_method_count";
constexpr const char* kExecuteDurationMs = "machine_execute_duration_ms";
// 治理切片（05 遥测同族口径）：枚举只读与处置各一对接受/拒绝计数。
constexpr const char* kProcessListCount = "machine_process_list_count";
constexpr const char* kProcessListRejectedCount =
    "machine_process_list_rejected_count";
constexpr const char* kTerminateAcceptedCount = "machine_terminate_accepted_count";
constexpr const char* kTerminateRejectedCount =
    "machine_terminate_rejected_count";
// 审计命令口径：治理动作与 execute 审计同族（command 串入审计条目，
// 中心侧按命令名可辨动作类型）。
constexpr const char* kGovernListCommand = "process.list";
constexpr const char* kGovernKillCommand = "process.kill";
// 诊断采样入口（04 §7）：触发（写侧，消耗性能预算）与访问（读侧，
// 明细外泄面）各一对接受/拒绝计数。
constexpr const char* kProfileTriggerAcceptedCount =
    "machine_profile_trigger_accepted_count";
constexpr const char* kProfileTriggerRejectedCount =
    "machine_profile_trigger_rejected_count";
constexpr const char* kProfileAccessAcceptedCount =
    "machine_profile_access_accepted_count";
constexpr const char* kProfileAccessRejectedCount =
    "machine_profile_access_rejected_count";
// 审计命令口径同治理：触发/清单/下载三命令名入审计条目。
constexpr const char* kProfileTriggerCommand = "profiler.trigger";
constexpr const char* kProfileListCommand = "profiler.list";
constexpr const char* kProfileDownloadCommand = "profiler.download";
// §6.1 角色门落到只读面后，快照/审计查询的拒绝也要有同款计数（此前
// 只读面无拒绝路径，计数器是单数；拒绝臂补齐成对口径）。
constexpr const char* kSnapshotRejectedCount = "machine_snapshot_rejected_count";
constexpr const char* kAuditRejectedCount = "machine_audit_rejected_count";
// §6.3 运行时配置热改：Admin 级动作一对接受/拒绝计数。
constexpr const char* kConfigApplyAcceptedCount =
    "machine_config_apply_accepted_count";
constexpr const char* kConfigApplyRejectedCount =
    "machine_config_apply_rejected_count";
// 只读面拒绝也入审计（拒绝是动作，照记不漏）；命令名与 RPC 方法同名。
constexpr const char* kSnapshotCommand = "machine.snapshot";
constexpr const char* kAuditQueryCommand = "machine.audit";
constexpr const char* kConfigApplyCommand = "config.apply";

// §6.3 禁改四类（协议定义 / 持久化 schema / entity property flags /
// 迁移语义）：键前缀 → 类别名。命中即拒绝且指认类别——在线修改会破坏
// 滚动升级 / 存量数据兼容 / 迁移正确性，不属于运维热改的授权范围。
struct ProtectedConfigClass final {
    const char* prefix;
    const char* className;
};
constexpr ProtectedConfigClass kProtectedConfigClasses[] = {
    {"protocol.", "protocol definition"},
    {"persistence.", "persistence schema"},
    {"entity.property", "entity property flags"},
    {"migration.", "migration semantics"},
};

// §6.3 白名单：允许在线生效的配置键 → 语义说明。白名单是热改的唯一
// 通道：不在表内的键一律拒绝（含禁改四类之外的任意新键——扩面须改码
// 评审，不允许配置自身把门打开）。
constexpr const char* kConfigReportIntervalKey = "ops.report_interval_ms";

foundation::Counter& telemetryCounter(const char* name) {
    return foundation::MetricsRegistry::instance().counter(name);
}

foundation::Histogram& executeDurationHistogram() {
    return foundation::MetricsRegistry::instance().histogram(
        kExecuteDurationMs,
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

// machine.terminate 请求载荷 = pid 十进制串；pid 0 无意义（不是合法治理
// 目标），解析失败一律 ok=false。
struct ParsedPid {
    bool ok = false;
    std::uint32_t value = 0;
};

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

// 诊断产物句柄载荷解析：与 pid 同为十进制整串，但句柄是 uint64——
// 单独成解析体，不复用不硬转。0 是"无产物"哨兵，不作为查询目标。
struct ParsedHandle {
    bool ok = false;
    std::uint64_t value = 0;
};

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

// machine.config.apply 请求载荷 = key '\0' value（与 execute 同一 NUL
// 分隔约定）；key 不得为空。value 的合法性由白名单命中后的解析负责。
struct ParsedConfigPayload {
    bool ok = false;
    std::string key;
    std::string value;
};

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

// 白名单键的值解析：毫秒数（十进制、全串消费）。0 合法 = 关闭该可调项
// （与 reportInterval 的 0 = 关闭口径一致）。
struct ParsedMillis {
    bool ok = false;
    std::uint64_t value = 0;
};

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
}

void MachineDaemon::tick() {
    if (!hub_) {
        return;  // 未 start/已 stop：静默空转
    }

    acceptConnections();
    hub_->tick();
    processMessages();
    reportIfDue();
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
        auto transport = std::make_shared<runtime::NetworkTransport>(conn);
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
    if (inv.method == MachineMethod::kSnapshot) {
        // §6.1 角色门：inspect 面需 ReadOnly 及以上。接受的快照照旧不留
        // 审计（避免只读噪声）；拒绝是动作，照记审计与计数。
        if (!hasRole(inv.sourceComponent, AccessRole::ReadOnly)) {
            AuditEntry entry;
            entry.timestamp = std::chrono::system_clock::now();
            entry.source = inv.sourceComponent;
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

    telemetryCounter(kUnknownMethodCount).increment();
    AuditEntry rejected;
    rejected.timestamp = std::chrono::system_clock::now();
    rejected.source = inv.sourceComponent;
    rejected.command = inv.method;
    appendAudit(rejected);
    foundation::logWarn("machine.unknown_method", {{"method", inv.method}});
    const auto reason = toBytes("unknown method: " + inv.method);
    sendResponse(inv.sourceComponent, MachineMethod::kError,
                 std::span<const std::byte>(reason));
}

void MachineDaemon::handleAudit(runtime::RuntimeInvocation& inv) {    std::ostringstream out;
    out << "[";
    for (std::size_t index = 0; index < auditLog_.size(); ++index) {
        if (index != 0) {
            out << ',';
        }
        out << auditEntryJson(auditLog_[index]);
    }
    out << "]";
    const auto json = toBytes(out.str());
    sendResponse(inv.sourceComponent, MachineMethod::kAuditOk,
                 std::span<const std::byte>(json));
}

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

bool MachineDaemon::isTriggerAuthorized(runtime::ComponentId source) const {
    const auto& allowed = config_.diagnosticsPolicy.canTrigger;
    return std::find(allowed.begin(), allowed.end(), source) != allowed.end();
}

bool MachineDaemon::isDiagnosticsAccessAuthorized(
    runtime::ComponentId source) const {
    const auto& allowed = config_.diagnosticsPolicy.canAccess;
    return std::find(allowed.begin(), allowed.end(), source) != allowed.end();
}

bool MachineDaemon::hasRole(runtime::ComponentId source,
                            AccessRole required) const {
    return roleMeets(roleFor(config_.roleBindings, source), required);
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
