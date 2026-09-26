#include "theseed/control/ops/OpsControlCenter.h"

#include <algorithm>
#include <utility>

namespace theseed::control::ops {

OpsControlCenter::OpsControlCenter() : OpsControlCenter(Config{}) {}

OpsControlCenter::OpsControlCenter(Config config) : config_(config) {}

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

}  // namespace theseed::control::ops
