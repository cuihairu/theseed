#include "theseed/control/ops/OpsControlCenter.h"

#include <algorithm>
#include <utility>

namespace theseed::control::ops {

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
    }
    // 已知节点：注册退化为心跳——只续 lastSeen，不碰已有快照。
    // （新节点若被逐出也只可能是别的节点：新接入者排接入序队尾。）
    iter->second.timestamp = now;
}

bool OpsControlCenter::deregister(const std::string& nodeId) {
    if (nodes_.erase(nodeId) == 0) {
        return false;
    }
    // 与 pruneStale 同一清理纪律：摘节点同时清接入序残留，避免容量
    // 逐出瞄准已不存在的节点。
    std::erase(insertionOrder_, nodeId);
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
    }
    return pruned;
}

void OpsControlCenter::publish(const machine::NodeAuditEntry& entry) {
    if (entry.nodeId.empty() || config_.maxAuditEntries == 0) {
        return;  // 身份纪律同节点上报；容量 0 = 审计聚合关闭
    }
    if (auditEntries_.size() == config_.maxAuditEntries) {
        auditEntries_.erase(auditEntries_.begin());  // 环形：满后丢最旧
    }
    auditEntries_.push_back(entry);
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
