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

// 治理动作拒绝（枚举/处置共用）：计数 + span 标记 + 结构化日志。span 可空
// （枚举是只读动作，不进 trace）；审计与错误响应由调用方处理。
void rejectGovernAction(const AuditEntry& entry,
                        foundation::SpanScope* span,
                        const char* counterName,
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
    foundation::logWarn("machine.process.govern.rejected",
                        {sourceAttr, commandAttr, reasonAttr});
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

    if (inv.method == MachineMethod::kProcesses) {
        handleProcesses(inv);
        return;
    }

    if (inv.method == MachineMethod::kTerminate) {
        handleTerminate(inv);
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
        rejectGovernAction(entry, nullptr, kProcessListRejectedCount, reason);
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
        rejectGovernAction(entry, &span, kTerminateRejectedCount, reason);
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
