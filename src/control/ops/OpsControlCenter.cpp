#include "theseed/control/ops/OpsControlCenter.h"

#include "theseed/foundation/Logger.h"
#include "theseed/foundation/Metrics.h"

#include <algorithm>
#include <utility>

namespace theseed::control::ops {

namespace {

// 中心侧遥测（05-telemetry MVP 聚合面切片）：节点名册与审计环形的水位
// 用 gauge 暴露，丢审计/掉线摘除用 counter 累计——审计有损与节点抖动
// 是 ops 决策信号。命名与 machine 侧同族（snake_case + _count）。
constexpr const char* kNodesRegistered = "ops_nodes_registered";
constexpr const char* kAuditEntries = "ops_audit_entries";
constexpr const char* kNodesPrunedCount = "ops_nodes_pruned_count";
constexpr const char* kAuditDroppedCount = "ops_audit_dropped_count";
// 中心侧剖面读入口（04 §7）：查询/下载各一对接受/拒绝计数，命名与
// machine 侧 machine_profile_* 同族；副本环形逐出单独计数。
constexpr const char* kProfileQueryAcceptedCount =
    "center_profile_query_accepted_count";
constexpr const char* kProfileQueryRejectedCount =
    "center_profile_query_rejected_count";
constexpr const char* kProfileDownloadAcceptedCount =
    "center_profile_download_accepted_count";
constexpr const char* kProfileDownloadRejectedCount =
    "center_profile_download_rejected_count";
constexpr const char* kProfileArtifactsDroppedCount =
    "center_profile_artifacts_dropped_count";

// 中心侧审计 command 前缀 center.* 与 agent 侧 profiler.* 区分归属。
constexpr const char* kCenterProfileQueryCommand = "center.profiler.query";
constexpr const char* kCenterProfileDownloadCommand =
    "center.profiler.download";

foundation::Gauge& telemetryGauge(const char* name) {
    return foundation::MetricsRegistry::instance().gauge(name);
}

foundation::Counter& telemetryCounter(const char* name) {
    return foundation::MetricsRegistry::instance().counter(name);
}

void syncNodeGauge(std::size_t nodes) {
    telemetryGauge(kNodesRegistered).set(static_cast<std::int64_t>(nodes));
}

void syncAuditGauge(std::size_t entries) {
    telemetryGauge(kAuditEntries).set(static_cast<std::int64_t>(entries));
}

// 中心侧剖面读入口拒绝臂共用出口：计数 + 结构化日志（读动作不进
// trace——与 agent 侧清单/下载同口径）；审计落账由调用方先行处理。
void rejectProfileRead(const machine::AuditEntry& entry,
                       const char* counterName, const std::string& reason) {
    telemetryCounter(counterName).increment();
    const auto sourceValue = static_cast<std::int64_t>(entry.source);
    const foundation::LogAttribute sourceAttr = {"source", sourceValue};
    const foundation::LogAttribute commandAttr = {"command", entry.command};
    const foundation::LogAttribute reasonAttr = {"reason", reason};
    foundation::logWarn("center.profile.rejected",
                        {sourceAttr, commandAttr, reasonAttr});
}

// 审计 args 的查询描述（紧凑确定形，仅非空维度入账）。
std::string describeQuery(const machine::ProfileQuery& query) {
    std::string out;
    if (!query.nodeId.empty()) {
        out += "node:" + query.nodeId + "|";
    }
    if (!query.entityId.empty()) {
        out += "entity:" + query.entityId + "|";
    }
    if (!query.entityType.empty()) {
        out += "type:" + query.entityType + "|";
    }
    if (!out.empty()) {
        out.pop_back();
    }
    return out;
}

}  // namespace

OpsControlCenter::OpsControlCenter() : OpsControlCenter(Config{}) {}

OpsControlCenter::OpsControlCenter(Config config) : config_(config) {}

void OpsControlCenter::registerNode(
    const std::string& nodeId, std::chrono::system_clock::time_point now) {
    if (nodeId.empty()) {
        return;  // 与 publish 同一身份纪律：无身份不入聚合
    }

    auto [iter, inserted] = nodes_.try_emplace(nodeId, machine::NodeReport{});
    if (inserted) {
        iter->second.nodeId = nodeId;
        insertionOrder_.push_back(nodeId);  // 注册即入接入序（与首报同口径）
        evictOldestIfFull();
        foundation::logInfo("ops.node.registered", {{"node_id", nodeId}});
    }
    // 已知节点：注册退化为心跳——只续 lastSeen，不碰已有快照。
    // （新节点若被逐出也只可能是别的节点：新接入者排接入序队尾。）
    iter->second.timestamp = now;
    syncNodeGauge(nodes_.size());
}

bool OpsControlCenter::deregister(const std::string& nodeId) {
    if (nodes_.erase(nodeId) == 0) {
        return false;
    }
    // 与 pruneStale 同一清理纪律：摘节点同时清接入序残留，避免容量
    // 逐出瞄准已不存在的节点。剖面索引属节点状态随摘而清；审计与
    // 产物副本是历史事实，保留（上界由各自容量约束）。
    std::erase(insertionOrder_, nodeId);
    profileIndex_.erase(nodeId);
    syncNodeGauge(nodes_.size());
    foundation::logInfo("ops.node.deregistered", {{"node_id", nodeId}});
    return true;
}

void OpsControlCenter::publish(const machine::NodeReport& report) {
    if (report.nodeId.empty()) {
        return;  // 无身份的上报无法聚合：丢弃（上报方有义务带 nodeId）
    }

    auto [iter, inserted] = nodes_.try_emplace(report.nodeId, report);
    if (inserted) {
        insertionOrder_.push_back(report.nodeId);
        evictOldestIfFull();
        syncNodeGauge(nodes_.size());  // 首报即接入名册（与注册同口径）
    } else {
        iter->second = report;  // 后到覆盖：中心只留每节点最新快照
    }
    // 剖面索引与快照同一快照语义：后到覆盖。无剖面来源的报告照常
    // 清空该节点索引——诚实反映 agent 侧当前已无剖面可查。
    profileIndex_[report.nodeId] = report.profiles;
}

void OpsControlCenter::evictOldestIfFull() {
    if (config_.maxNodes == 0 || nodes_.size() <= config_.maxNodes) {
        return;
    }

    // 首报最早（insertionOrder_ 队首）的节点被挤出；若它已被 pruneStale
    // 摘掉（order 残留），跳过继续找下一个。
    while (!insertionOrder_.empty() && nodes_.size() > config_.maxNodes) {
        const auto oldest = insertionOrder_.front();
        insertionOrder_.erase(insertionOrder_.begin());
        nodes_.erase(oldest);
        profileIndex_.erase(oldest);  // 索引属节点状态，随摘而清
    }
    syncNodeGauge(nodes_.size());
}

bool OpsControlCenter::latest(const std::string& nodeId,
                              machine::NodeReport& out) const {
    const auto iter = nodes_.find(nodeId);
    if (iter == nodes_.end()) {
        return false;
    }
    out = iter->second;
    return true;
}

std::vector<machine::NodeReport> OpsControlCenter::snapshotNodes() const {
    std::vector<machine::NodeReport> reports;
    reports.reserve(nodes_.size());
    for (const auto& [nodeId, report] : nodes_) {
        reports.push_back(report);
    }
    std::sort(reports.begin(), reports.end(),
              [](const machine::NodeReport& a, const machine::NodeReport& b) {
                  return a.nodeId < b.nodeId;
              });
    return reports;
}

std::size_t OpsControlCenter::nodeCount() const {
    return nodes_.size();
}

std::size_t OpsControlCenter::pruneStale(std::chrono::milliseconds ttl,
                                         std::chrono::system_clock::time_point now) {
    std::size_t pruned = 0;
    for (auto iter = nodes_.begin(); iter != nodes_.end();) {
        if (now - iter->second.timestamp > ttl) {
            profileIndex_.erase(iter->first);  // 索引属节点状态，随摘而清
            iter = nodes_.erase(iter);
            ++pruned;
        } else {
            ++iter;
        }
    }

    // insertionOrder_ 只服务于容量逐出的先后语义，残留死节点会让逐出
    // 空转；在这里一并清掉（保序过滤）。
    if (pruned != 0) {
        insertionOrder_.erase(
            std::remove_if(insertionOrder_.begin(), insertionOrder_.end(),
                           [&](const std::string& nodeId) {
                               return nodes_.find(nodeId) == nodes_.end();
                           }),
            insertionOrder_.end());
        syncNodeGauge(nodes_.size());
        telemetryCounter(kNodesPrunedCount).increment(pruned);
        foundation::logWarn("ops.nodes.pruned",
                            {{"count", static_cast<std::int64_t>(pruned)}});
    }
    return pruned;
}

void OpsControlCenter::publish(const machine::NodeAuditEntry& entry) {
    if (entry.nodeId.empty()) {
        return;  // 身份纪律同节点上报：无法归属的节点上报不入聚合
    }
    appendAuditRing(entry);
}

void OpsControlCenter::appendAuditRing(const machine::NodeAuditEntry& entry) {
    if (config_.maxAuditEntries == 0) {
        return;  // 容量 0 = 审计聚合关闭（中心本地动作同样无审计面）
    }
    if (auditEntries_.size() == config_.maxAuditEntries) {
        auditEntries_.erase(auditEntries_.begin());  // 环形：满后丢最旧
        telemetryCounter(kAuditDroppedCount).increment();
        foundation::logWarn("ops.audit.dropped", {{"node_id", entry.nodeId}});
    }
    auditEntries_.push_back(entry);
    syncAuditGauge(auditEntries_.size());
}

void OpsControlCenter::publish(const machine::NodeProfileArtifact& artifact) {
    if (artifact.nodeId.empty()) {
        return;  // 身份纪律同节点上报：无归属不聚合
    }

    // 元数据并入该节点剖面索引（句柄未知则追加）：报告通道未及的窗口，
    // 查询侧也能看到。句柄已知则不动——报告快照是该节点剖面的最新事实。
    auto& metas = profileIndex_[artifact.nodeId];
    const bool known =
        std::any_of(metas.begin(), metas.end(),
                    [&artifact](const machine::ProfileMeta& meta) {
                        return meta.handle == artifact.meta.handle;
                    });
    if (!known) {
        metas.push_back(artifact.meta);
    }

    if (config_.maxProfileArtifacts == 0) {
        return;  // 副本存储关闭：元数据索引照常，字节不留（下载如实报无副本）
    }
    if (profileArtifacts_.size() == config_.maxProfileArtifacts) {
        profileArtifacts_.erase(profileArtifacts_.begin());  // 环形：满后丢最旧
        telemetryCounter(kProfileArtifactsDroppedCount).increment();
        foundation::logWarn("center.profile.artifacts.dropped",
                            {{"node_id", artifact.nodeId}});
    }
    profileArtifacts_.push_back(artifact);
}

bool OpsControlCenter::isProfileAccessAuthorized(
    runtime::ComponentId requester) const {
    const auto& allowed = config_.profilePolicy.canAccess;
    return std::find(allowed.begin(), allowed.end(), requester) !=
           allowed.end();
}

std::vector<machine::NodeProfileEntry> OpsControlCenter::queryProfiles(
    runtime::ComponentId requester, const machine::ProfileQuery& query) {
    machine::AuditEntry entry;
    entry.timestamp = std::chrono::system_clock::now();
    entry.source = requester;
    entry.command = kCenterProfileQueryCommand;
    entry.args = describeQuery(query);

    if (!isProfileAccessAuthorized(requester)) {
        entry.accepted = false;
        appendAuditRing(machine::NodeAuditEntry{std::string(), entry});
        rejectProfileRead(entry, kProfileQueryRejectedCount,
                          "profile query rejected: source component " +
                              std::to_string(requester) + " is not authorized");
        return {};
    }

    // §6.1 角色门（叠加在 canAccess 之上）：inspect 面需 ReadOnly 及以上。
    if (!machine::roleMeets(machine::roleFor(config_.roleBindings, requester),
                            machine::AccessRole::ReadOnly)) {
        entry.accepted = false;
        appendAuditRing(machine::NodeAuditEntry{std::string(), entry});
        rejectProfileRead(entry, kProfileQueryRejectedCount,
                          "profile query rejected: source component " +
                              std::to_string(requester) +
                              " requires ReadOnly role");
        return {};
    }

    std::vector<machine::NodeProfileEntry> matched;
    for (const auto& [nodeId, metas] : profileIndex_) {
        if (!query.nodeId.empty() && query.nodeId != nodeId) {
            continue;
        }
        for (const auto& meta : metas) {
            if (!query.entityId.empty() && query.entityId != meta.entityId) {
                continue;  // 当前无生产者：如实落空（不编造数据）
            }
            if (!query.entityType.empty() &&
                query.entityType != meta.entityType) {
                continue;
            }
            machine::NodeProfileEntry row;
            row.nodeId = nodeId;
            row.meta = meta;
            matched.push_back(row);
        }
    }
    // unordered_map 遍历序不稳定：按 (nodeId, handle) 排序保证输出稳定
    //（与 snapshotNodes 的稳定快照口径一致）。
    std::sort(matched.begin(), matched.end(),
              [](const machine::NodeProfileEntry& a,
                 const machine::NodeProfileEntry& b) {
                  if (a.nodeId != b.nodeId) {
                      return a.nodeId < b.nodeId;
                  }
                  return a.meta.handle < b.meta.handle;
              });

    entry.accepted = true;
    entry.ok = true;
    appendAuditRing(machine::NodeAuditEntry{std::string(), entry});
    telemetryCounter(kProfileQueryAcceptedCount).increment();
    return matched;
}

bool OpsControlCenter::downloadProfileArtifact(runtime::ComponentId requester,
                                               const std::string& nodeId,
                                               std::uint64_t handle,
                                               std::string& out) {
    machine::AuditEntry entry;
    entry.timestamp = std::chrono::system_clock::now();
    entry.source = requester;
    entry.command = kCenterProfileDownloadCommand;
    entry.args = nodeId + ":" + std::to_string(handle);

    // 拒绝臂共用出口：审计先行，再计数/日志。
    const auto reject = [&](const std::string& reason) {
        entry.accepted = false;
        appendAuditRing(machine::NodeAuditEntry{std::string(), entry});
        rejectProfileRead(entry, kProfileDownloadRejectedCount, reason);
        return false;
    };

    if (!isProfileAccessAuthorized(requester)) {
        return reject("profile download rejected: source component " +
                      std::to_string(requester) + " is not authorized");
    }

    // §6.1 角色门（叠加在 canAccess 之上）：inspect 面需 ReadOnly 及以上。
    if (!machine::roleMeets(machine::roleFor(config_.roleBindings, requester),
                            machine::AccessRole::ReadOnly)) {
        return reject("profile download rejected: source component " +
                      std::to_string(requester) + " requires ReadOnly role");
    }

    for (const auto& artifact : profileArtifacts_) {
        if (artifact.nodeId == nodeId && artifact.meta.handle == handle) {
            out = artifact.payload;
            entry.accepted = true;
            entry.ok = true;
            appendAuditRing(machine::NodeAuditEntry{std::string(), entry});
            telemetryCounter(kProfileDownloadAcceptedCount).increment();
            return true;
        }
    }
    return reject("profile download rejected: no local copy of node " +
                  nodeId + " handle " + std::to_string(handle));
}

std::vector<machine::NodeAuditEntry> OpsControlCenter::auditTrail() const {
    return auditEntries_;
}

std::vector<machine::NodeAuditEntry> OpsControlCenter::auditTrail(
    const std::string& nodeId) const {
    std::vector<machine::NodeAuditEntry> trail;
    trail.reserve(auditEntries_.size());
    for (const auto& entry : auditEntries_) {
        if (entry.nodeId == nodeId) {
            trail.push_back(entry);
        }
    }
    return trail;
}

std::size_t OpsControlCenter::auditCount() const {
    return auditEntries_.size();
}

}  // namespace theseed::control::ops
