#include "theseed/runtime/BackupTopologyCoordinator.h"

#include <algorithm>
#include <stdexcept>

namespace theseed::runtime {

namespace {

std::uint64_t hashEntity(EntityId id) {
    // FNV-1a 64-bit — deterministic across runs and platforms for placement.
    std::uint64_t h = 1469598103934665603ULL;
    for (int shift = 0; shift < 64; shift += 8) {
        const std::uint64_t byte = (id >> shift) & 0xFFULL;
        h ^= byte;
        h *= 1099511628211ULL;
    }
    return h;
}

}  // namespace

bool BackupTopologyCoordinator::registerProcess(ComponentId id) {
    if (id == 0) return false;
    if (processIndex_.find(id) != processIndex_.end()) return false;
    processes_.push_back(ProcessEntry{id, false});
    processIndex_[id] = processes_.size() - 1;
    return true;
}

bool BackupTopologyCoordinator::unregisterProcess(ComponentId id) {
    auto it = processIndex_.find(id);
    if (it == processIndex_.end()) return false;
    const auto idx = it->second;
    processes_.erase(processes_.begin() + idx);
    processIndex_.clear();
    for (std::size_t i = 0; i < processes_.size(); ++i) {
        processIndex_[processes_[i].id] = i;
    }
    return true;
}

std::size_t BackupTopologyCoordinator::processCount() const noexcept {
    return processes_.size();
}

std::vector<ComponentId> BackupTopologyCoordinator::processes() const {
    std::vector<ComponentId> out;
    out.reserve(processes_.size());
    for (const auto& p : processes_) out.push_back(p.id);
    std::sort(out.begin(), out.end());
    return out;
}

bool BackupTopologyCoordinator::hasProcess(ComponentId id) const {
    return processIndex_.find(id) != processIndex_.end();
}

std::optional<BackupRoute> BackupTopologyCoordinator::routeFor(EntityId entityId) const {
    if (processes_.size() < 2) return std::nullopt;

    const auto h = hashEntity(entityId);
    const auto primaryIdx = h % processes_.size();
    const auto backupIdx = (h / processes_.size() + 1) % processes_.size();

    BackupRoute route;
    route.entityId = entityId;
    route.primary = processes_[primaryIdx].id;
    route.backup = processes_[backupIdx == primaryIdx ? (backupIdx + 1) % processes_.size() : backupIdx].id;
    route.epoch = activeVersion_.epoch;
    return route;
}

BackupTopologyVersion BackupTopologyCoordinator::activeVersion() const noexcept {
    return activeVersion_;
}

std::optional<BackupTopologyVersion> BackupTopologyCoordinator::stagingVersion() const noexcept {
    if (!hasStaging_) return std::nullopt;
    return stagingVersion_;
}

TopologyState BackupTopologyCoordinator::state() const noexcept {
    return state_;
}

bool BackupTopologyCoordinator::beginRebuild(ComponentId reasonProcess, std::uint64_t newEpoch) {
    if (state_ == TopologyState::Priming || state_ == TopologyState::Promoting) {
        return false;
    }
    if (processes_.empty()) return false;

    stagingVersion_ = BackupTopologyVersion{
        .version = activeVersion_.version + 1,
        .epoch = newEpoch,
    };
    hasStaging_ = true;
    state_ = TopologyState::Priming;
    rebuildReason_ = reasonProcess;
    for (auto& p : processes_) p.acked = false;
    return true;
}

bool BackupTopologyCoordinator::ackPrimed(ComponentId processId, BackupTopologyVersion version) {
    if (state_ != TopologyState::Priming) return false;
    if (!hasStaging_ || !(version == stagingVersion_)) return false;

    auto it = processIndex_.find(processId);
    if (it == processIndex_.end()) return false;
    processes_[it->second].acked = true;
    return true;
}

bool BackupTopologyCoordinator::allAcked() const {
    if (state_ != TopologyState::Priming) return false;
    for (const auto& p : processes_) {
        if (!p.acked) return false;
    }
    return true;
}

bool BackupTopologyCoordinator::promote() {
    if (state_ != TopologyState::Priming || !allAcked()) {
        return false;
    }
    state_ = TopologyState::Promoting;
    activeVersion_ = stagingVersion_;
    hasStaging_ = false;
    state_ = TopologyState::Stable;
    return true;
}

bool BackupTopologyCoordinator::abort() {
    if (state_ == TopologyState::Stable || state_ == TopologyState::Aborted) return false;
    // Cancel the in-progress rebuild. State goes Aborted then back to Stable
    // so callers can immediately attempt another beginRebuild if they want to.
    hasStaging_ = false;
    rebuildReason_ = 0;
    state_ = TopologyState::Stable;
    return true;
}

void BackupTopologyCoordinator::reset() {
    processes_.clear();
    processIndex_.clear();
    activeVersion_ = {};
    stagingVersion_ = {};
    hasStaging_ = false;
    state_ = TopologyState::Stable;
    rebuildReason_ = 0;
}

}  // namespace theseed::runtime
