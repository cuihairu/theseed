#include "theseed/runtime/SpaceRuntime.h"
#include "theseed/runtime/Controller.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

namespace theseed::runtime {

SpaceRuntime::RefreshPump::RefreshPump(SpaceRuntime& owner) : owner_(&owner) {}

void SpaceRuntime::RefreshPump::tick(TickContext& context) {
    owner_->tick(context);
}

SpaceRuntime::SyncPump::SyncPump(SpaceRuntime& owner) : owner_(&owner) {}

void SpaceRuntime::SyncPump::tick(TickContext& context) {
    owner_->sync(context);
}

SpaceRuntime::SpaceRuntime(std::unique_ptr<Space> space)
    : space_(std::move(space)), refreshPump_(*this), syncPump_(*this) {
    if (!space_) {
        throw std::invalid_argument("space runtime requires a space");
    }
}

Space& SpaceRuntime::space() {
    return *space_;
}

const Space& SpaceRuntime::space() const {
    return *space_;
}

void SpaceRuntime::attach(TickScheduler& scheduler) {
    scheduler.registerTickable(TickPhase::Entity, refreshPump_);
    scheduler.registerTickable(TickPhase::SyncBuild, syncPump_);
}

void SpaceRuntime::detach(TickScheduler& scheduler) {
    static_cast<void>(scheduler.unregisterTickable(TickPhase::Entity, refreshPump_));
    static_cast<void>(scheduler.unregisterTickable(TickPhase::SyncBuild, syncPump_));
}

void SpaceRuntime::addEntity(Entity& entity, const Vector3& position) {
    space_->addEntity(entity, position);
    entity.setPositionProvider([this](EntityId id) -> std::optional<Vector3> {
        return space_->entityPosition(id);
    });
}

void SpaceRuntime::removeEntity(EntityId entityId) {
    for (auto it = witnesses_.begin(); it != witnesses_.end();) {
        auto& binding = it->second;
        if (binding.witness != nullptr) {
            if (auto* owner = binding.witness->owner();
                owner != nullptr && owner->id() == entityId) {
                if (binding.trigger != nullptr) {
                    binding.trigger->uninstall();
                }
                binding.witness->detach();
                it = witnesses_.erase(it);
                continue;
            }

            binding.witness->onLeaveView(entityId);
        }

        ++it;
    }

    space_->removeEntity(entityId);
}

Witness& SpaceRuntime::ensureWitness(Entity& owner, float viewRange) {
    auto& binding = witnesses_[owner.id()];
    if (!binding.witness) {
        binding.witness = std::make_unique<Witness>();
        binding.witness->attach(owner);
        binding.trigger = std::make_unique<WitnessViewTrigger>(*binding.witness, owner, viewRange);
        binding.trigger->install(space_->coordinateSystem());
    } else {
        binding.trigger->updateRange(viewRange);
    }

    return *binding.witness;
}

Witness* SpaceRuntime::findWitness(EntityId ownerEntityId) const {
    const auto iter = witnesses_.find(ownerEntityId);
    if (iter == witnesses_.end()) {
        return nullptr;
    }

    return iter->second.witness.get();
}

const std::vector<PropertyDelta>* SpaceRuntime::findStagedDelta(EntityId entityId) const {
    const auto iter = stagedViewDeltas_.find(entityId);
    if (iter == stagedViewDeltas_.end()) {
        return nullptr;
    }

    return &iter->second;
}

std::vector<PropertyDelta> SpaceRuntime::findStagedDelta(EntityId entityId,
                                                         PropertyFlag excludeFlags) const {
    std::vector<PropertyDelta> result;
    const auto iter = stagedRuntimeDeltas_.find(entityId);
    if (iter == stagedRuntimeDeltas_.end()) {
        return result;
    }

    const auto* entity = space_->findEntity(entityId);
    if (entity == nullptr) {
        return result;
    }

    for (const auto& delta : iter->second) {
        const auto& desc = entity->propertyBlock().def().property(delta.propertyId);
        if (static_cast<std::uint32_t>(desc.flags) & static_cast<std::uint32_t>(excludeFlags)) {
            continue;
        }
        result.push_back(delta);
    }
    return result;
}

void SpaceRuntime::tick(TickContext& context) {
    processEntityInput();
    applyVelocity(context.deltaTime);
    tickControllers(context.deltaTime);
    refreshWitnesses();
}

void SpaceRuntime::processEntityInput() {
    for (auto* entity : space_->entities()) {
        if (entity != nullptr && entity->isActive()) {
            entity->processInput();
        }
    }
}

void SpaceRuntime::applyVelocity(Duration deltaTime) {
    auto dt = std::chrono::duration<float>(deltaTime).count();
    if (dt <= 0.0f) return;

    for (auto* entity : space_->entities()) {
        if (entity == nullptr || !entity->hasVelocity()) continue;

        auto pos = space_->entityPosition(entity->id());
        if (!pos.has_value()) continue;

        auto vel = entity->velocity();
        Vector3 newPos{
            pos->x + vel.x * dt,
            pos->y + vel.y * dt,
            pos->z + vel.z * dt,
        };
        space_->updateEntityPosition(entity->id(), newPos);
        entity->notifyPositionChanged(*pos, newPos);
        stagedPositions_[entity->id()] = newPos;
    }
}

void SpaceRuntime::sync(TickContext& context) {
    static_cast<void>(context);
    stageDirtyEntities();
    collectWitnessDirty();
    stagedPositions_.clear();
}

void SpaceRuntime::refreshWitnesses() {
    for (auto& [entityId, binding] : witnesses_) {
        static_cast<void>(entityId);
        binding.trigger->refresh();
    }
}

void SpaceRuntime::tickControllers(Duration deltaTime) {
    auto dt = std::chrono::duration<float>(deltaTime).count();
    if (dt <= 0.0f) return;

    for (auto* entity : space_->entities()) {
        if (entity == nullptr || !entity->isActive()) continue;
        if (entity->controllers().count() == 0) continue;
        entity->controllers().tick(dt);
    }
}

void SpaceRuntime::stageDirtyEntities() {
    stagedViewDeltas_.clear();
    stagedRuntimeDeltas_.clear();
    for (auto* entity : space_->entities()) {
        const auto viewDelta = entity->buildViewDirtyPropertyDelta();
        if (!viewDelta.empty()) {
            stagedViewDeltas_.emplace(entity->id(), viewDelta);
            entity->clearViewDirtyFlags();
        }

        const auto runtimeDelta = entity->buildDirtyPropertyDelta();
        if (!runtimeDelta.empty()) {
            stagedRuntimeDeltas_.emplace(entity->id(), runtimeDelta);
            entity->clearRuntimeDirtyFlags();
        }
    }
}

void SpaceRuntime::collectWitnessDirty() {
    for (auto& [entityId, binding] : witnesses_) {
        static_cast<void>(entityId);
        for (const auto& view : binding.witness->snapshotView()) {
            const auto* delta = findStagedDelta(view.entityId);
            if (delta != nullptr && !delta->empty()) {
                binding.witness->recordDirty(view.entityId, *delta);
            }

            auto posIt = stagedPositions_.find(view.entityId);
            if (posIt != stagedPositions_.end()) {
                binding.witness->recordPosition(view.entityId, posIt->second);
            }
        }
    }
}

std::vector<AoIEvent> SpaceRuntime::collectAoIEvents() {
    std::vector<AoIEvent> events;
    for (auto& [entityId, binding] : witnesses_) {
        static_cast<void>(entityId);
        auto witnessEvents = binding.witness->flushAoIEvents();
        events.insert(events.end(), witnessEvents.begin(), witnessEvents.end());
    }
    return events;
}

std::vector<SpaceRuntime::ObserverDelta> SpaceRuntime::collectWitnessDeltas() {
    std::vector<ObserverDelta> result;
    for (auto& [observerId, binding] : witnesses_) {
        auto deltas = binding.witness->flushDeltas();
        if (deltas.empty()) continue;
        result.push_back({observerId, std::move(deltas)});
    }
    return result;
}

}  // namespace theseed::runtime
