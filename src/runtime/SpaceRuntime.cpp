#include "theseed/runtime/SpaceRuntime.h"

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
    const auto iter = stagedDeltas_.find(entityId);
    if (iter == stagedDeltas_.end()) {
        return nullptr;
    }

    return &iter->second;
}

void SpaceRuntime::tick(TickContext& context) {
    processEntityInput();
    applyVelocity(context.deltaTime);
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
    }
}

void SpaceRuntime::sync(TickContext& context) {
    static_cast<void>(context);
    stageDirtyEntities();
    collectWitnessDirty();
}

void SpaceRuntime::refreshWitnesses() {
    for (auto& [entityId, binding] : witnesses_) {
        static_cast<void>(entityId);
        binding.trigger->refresh();
    }
}

void SpaceRuntime::stageDirtyEntities() {
    stagedDeltas_.clear();
    for (auto* entity : space_->entities()) {
        if (entity == nullptr) {
            continue;
        }

        const auto delta = entity->buildDirtyPropertyDelta();
        if (delta.empty()) {
            continue;
        }

        stagedDeltas_.emplace(entity->id(), delta);
        entity->clearDirtyFlags();
    }
}

void SpaceRuntime::collectWitnessDirty() {
    for (auto& [entityId, binding] : witnesses_) {
        static_cast<void>(entityId);
        for (const auto& view : binding.witness->snapshotView()) {
            const auto* delta = findStagedDelta(view.entityId);
            if (delta == nullptr || delta->empty()) {
                continue;
            }

            binding.witness->recordDirty(view.entityId, *delta);
        }
    }
}

}  // namespace theseed::runtime
