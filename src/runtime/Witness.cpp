#include "theseed/runtime/Witness.h"

#include <algorithm>
#include <stdexcept>

namespace theseed::runtime {

void Witness::attach(Entity& owner) {
    owner_ = &owner;
}

void Witness::detach() {
    owner_ = nullptr;
    entries_.clear();
}

bool Witness::attached() const {
    return owner_ != nullptr;
}

Entity* Witness::owner() const {
    return owner_;
}

void Witness::setDetailDistanceBands(float nearDistance, float midDistance) {
    if (nearDistance < 0.0F || midDistance < nearDistance) {
        throw std::invalid_argument("invalid witness detail distance bands");
    }

    nearDistance_ = nearDistance;
    midDistance_ = midDistance;
}

DetailLevel Witness::classifyDistance(float distance) const {
    if (distance < nearDistance_) {
        return 0;
    }
    if (distance < midDistance_) {
        return 1;
    }
    return 2;
}

void Witness::onEnterView(Entity& entity, float distance) {
    Entry entry;
    entry.entity = &entity;
    entry.distance = distance;
    entry.detailLevel = classifyDistance(distance);
    entries_[entity.id()] = std::move(entry);
}

void Witness::onLeaveView(EntityId entityId) {
    entries_.erase(entityId);
}

bool Witness::entityInView(EntityId entityId) const {
    return entries_.contains(entityId);
}

void Witness::updateDistance(EntityId entityId, float distance) {
    auto iter = entries_.find(entityId);
    if (iter == entries_.end()) {
        throw std::out_of_range("witness entity not found");
    }

    iter->second.distance = distance;
    iter->second.detailLevel = classifyDistance(distance);
}

std::vector<WitnessView> Witness::snapshotView() const {
    std::vector<WitnessView> result;
    result.reserve(entries_.size());
    for (const auto& [entityId, entry] : entries_) {
        result.push_back(WitnessView{
            .entityId = entityId,
            .distance = entry.distance,
            .detailLevel = entry.detailLevel,
        });
    }

    std::sort(result.begin(), result.end(), [](const WitnessView& lhs, const WitnessView& rhs) {
        return lhs.entityId < rhs.entityId;
    });
    return result;
}

std::size_t Witness::collectDirty() {
    std::size_t collected = 0;
    for (auto& [entityId, entry] : entries_) {
        static_cast<void>(entityId);
        if (entry.entity == nullptr) {
            continue;
        }

        const auto delta = entry.entity->buildDirtyPropertyDelta();
        if (delta.empty()) {
            continue;
        }

        auto& bucket = entry.stagedDeltas[entry.detailLevel];
        bucket.insert(bucket.end(), delta.begin(), delta.end());
        ++collected;
    }

    return collected;
}

void Witness::recordDirty(EntityId entityId, std::span<const PropertyDelta> properties) {
    auto iter = entries_.find(entityId);
    if (iter == entries_.end()) {
        throw std::out_of_range("witness entity not found");
    }

    auto& bucket = iter->second.stagedDeltas[iter->second.detailLevel];
    bucket.insert(bucket.end(), properties.begin(), properties.end());
}

std::vector<WitnessDelta> Witness::flushDeltas() {
    std::vector<WitnessDelta> result;
    result.reserve(entries_.size());

    for (auto& [entityId, entry] : entries_) {
        auto& bucket = entry.stagedDeltas[entry.detailLevel];
        if (bucket.empty()) {
            continue;
        }

        WitnessDelta delta;
        delta.entityId = entityId;
        delta.detailLevel = entry.detailLevel;
        delta.properties = std::move(bucket);
        bucket.clear();
        result.push_back(std::move(delta));
    }

    std::sort(result.begin(), result.end(), [](const WitnessDelta& lhs, const WitnessDelta& rhs) {
        return lhs.entityId < rhs.entityId;
    });
    return result;
}

WitnessViewTrigger::WitnessViewTrigger(Witness& witness, Entity& owner, float range)
    : RangeTrigger(owner, range), witness_(&witness) {}

void WitnessViewTrigger::onEnter(CoordinateNode& node, float distance) {
    witness_->onEnterView(node.entity(), distance);
}

void WitnessViewTrigger::onLeave(CoordinateNode& node) {
    witness_->onLeaveView(node.entityId());
}

}  // namespace theseed::runtime
