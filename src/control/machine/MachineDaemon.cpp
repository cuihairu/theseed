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

}  // namespace

MachineDaemon::MachineDaemon(Config config, IMachineAgent& agent)
    : config_(std::move(config)), agent_(agent) {}

MachineDaemon::~MachineDaemon() {
    stop();
}

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
        telemetryCounter(kSnapshotCount).increment();
        const auto snapshotJson = toBytes(formatSnapshotJson(agent_.snapshot()));
        sendResponse(inv.sourceComponent, MachineMethod::kSnapshotOk,
                     std::span<const std::byte>(snapshotJson));
        return;
    }

    if (inv.method == MachineMethod::kAudit) {
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
