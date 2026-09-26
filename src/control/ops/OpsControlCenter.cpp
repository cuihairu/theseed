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
    // 逐出瞄准已不存在的节点。
    std::erase(insertionOrder_, nodeId);
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
    if (entry.nodeId.empty() || config_.maxAuditEntries == 0) {
        return;  // 身份纪律同节点上报；容量 0 = 审计聚合关闭
    }
    if (auditEntries_.size() == config_.maxAuditEntries) {
        auditEntries_.erase(auditEntries_.begin());  // 环形：满后丢最旧
        telemetryCounter(kAuditDroppedCount).increment();
        foundation::logWarn("ops.audit.dropped", {{"node_id", entry.nodeId}});
    }
    auditEntries_.push_back(entry);
    syncAuditGauge(auditEntries_.size());
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
