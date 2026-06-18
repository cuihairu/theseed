#include "theseed/runtime/LoadProfiler.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace theseed::runtime {

EntityLoadProfiler::EntityLoadProfiler() = default;

EntityLoadProfiler::EntityLoadProfiler(Config config)
    : config_(std::move(config)) {}

EntityLoadProfiler::Scope::Scope(EntityLoadProfiler& profiler,
                                  EntityId id,
                                  std::string type)
    : profiler_(profiler),
      id_(id),
      type_(std::move(type)),
      start_(std::chrono::steady_clock::now()) {}

EntityLoadProfiler::Scope::~Scope() {
    const auto end = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(end - start_).count();
    profiler_.accumulate(id_, type_, ms);
}

EntityLoadProfiler::Scope EntityLoadProfiler::scope(EntityId id, std::string_view type) {
    return Scope(*this, id, std::string(type));
}

void EntityLoadProfiler::tick() {
    for (auto& [_, entry] : entries_) {
        // EMA uses the just-completed tick interval (currentRawLoad), then
        // promotes it to lastRawLoad and zeroes the accumulator for next tick.
        entry.smoothedLoad = config_.emaAlpha * entry.currentRawLoad +
                             (1.0F - config_.emaAlpha) * entry.smoothedLoad;
        entry.lastRawLoad = entry.currentRawLoad;
        entry.currentRawLoad = 0.0F;
    }
}

EntityLoadSnapshot EntityLoadProfiler::snapshot(EntityId id) const {
    EntityLoadSnapshot out;
    out.entityId = id;
    auto it = entries_.find(id);
    if (it == entries_.end()) return out;
    const auto& e = it->second;
    out.entityType = e.entityType;
    out.rawLoad = e.lastRawLoad;
    out.smoothedLoad = e.smoothedLoad;
    out.artificialMinLoad = e.artificialMinLoad;
    out.adjustedLoad = std::max(e.smoothedLoad, e.artificialMinLoad);
    return out;
}

void EntityLoadProfiler::setArtificialMinLoad(EntityId id, float load) {
    auto& e = entries_[id];
    if (e.entityType.empty()) {
        // No type yet — caller must call scope() at least once. We leave it blank;
        // snapshot will reflect an empty type, which is intentional for the
        // "min-load set before first scope" case.
    }
    e.artificialMinLoad = load;
}

std::vector<EntityLoadSnapshot> EntityLoadProfiler::all() const {
    std::vector<EntityLoadSnapshot> out;
    out.reserve(entries_.size());
    for (const auto& [id, entry] : entries_) {
        EntityLoadSnapshot snap;
        snap.entityId = id;
        snap.entityType = entry.entityType;
        snap.rawLoad = entry.lastRawLoad;
        snap.smoothedLoad = entry.smoothedLoad;
        snap.artificialMinLoad = entry.artificialMinLoad;
        snap.adjustedLoad = std::max(entry.smoothedLoad, entry.artificialMinLoad);
        out.push_back(std::move(snap));
    }
    std::sort(out.begin(), out.end(),
              [](const EntityLoadSnapshot& a, const EntityLoadSnapshot& b) {
                  return a.entityId < b.entityId;
              });
    return out;
}

std::size_t EntityLoadProfiler::trackedCount() const noexcept {
    return entries_.size();
}

void EntityLoadProfiler::reset() {
    entries_.clear();
}

float EntityLoadProfiler::emaAlpha() const noexcept {
    return config_.emaAlpha;
}

void EntityLoadProfiler::accumulate(EntityId id, std::string_view type, double ms) {
    auto& entry = entries_[id];
    if (entry.entityType.empty() && !type.empty()) {
        entry.entityType = std::string(type);
    }
    entry.currentRawLoad += static_cast<float>(ms);
}

void EntityTypeLoadAggregator::record(const EntityLoadSnapshot& snapshot) {
    if (snapshot.entityType.empty()) return;
    auto& agg = perType_[snapshot.entityType];
    if (agg.entityTypeId.empty()) {
        agg.entityTypeId = snapshot.entityType;
    }
    agg.entityCount += 1;
    agg.totalRawLoad += snapshot.rawLoad;
    agg.totalSmoothedLoad += snapshot.adjustedLoad;
    agg.maxRawLoad = std::max(agg.maxRawLoad, snapshot.rawLoad);
    agg.maxSmoothedLoad = std::max(agg.maxSmoothedLoad, snapshot.adjustedLoad);
}

EntityTypeLoadSnapshot EntityTypeLoadAggregator::snapshot(const std::string& typeId) const {
    auto it = perType_.find(typeId);
    if (it == perType_.end()) return EntityTypeLoadSnapshot{};
    return it->second;
}

std::vector<EntityTypeLoadSnapshot> EntityTypeLoadAggregator::all() const {
    std::vector<EntityTypeLoadSnapshot> out;
    out.reserve(perType_.size());
    for (const auto& [_, snap] : perType_) {
        out.push_back(snap);
    }
    std::sort(out.begin(), out.end(),
              [](const EntityTypeLoadSnapshot& a, const EntityTypeLoadSnapshot& b) {
                  return a.entityTypeId < b.entityTypeId;
              });
    return out;
}

void EntityTypeLoadAggregator::reset() {
    perType_.clear();
}

}  // namespace theseed::runtime
