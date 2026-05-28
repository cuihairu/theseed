#pragma once

#include "theseed/runtime/Space.h"
#include "theseed/runtime/TickScheduler.h"
#include "theseed/runtime/Witness.h"

#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace theseed::runtime {

class SpaceRuntime final : public ITickable {
public:
    explicit SpaceRuntime(std::unique_ptr<Space> space);

    Space& space();
    const Space& space() const;

    void attach(TickScheduler& scheduler);
    void detach(TickScheduler& scheduler);

    void addEntity(Entity& entity, const Vector3& position);
    void removeEntity(EntityId entityId);

    Witness& ensureWitness(Entity& owner, float viewRange);
    Witness* findWitness(EntityId ownerEntityId) const;
    const std::vector<PropertyDelta>* findStagedDelta(EntityId entityId) const;

    void tick(TickContext& context) override;
    void sync(TickContext& context);

private:
    class RefreshPump final : public ITickable {
    public:
        explicit RefreshPump(SpaceRuntime& owner);
        void tick(TickContext& context) override;

    private:
        SpaceRuntime* owner_ = nullptr;
    };

    class SyncPump final : public ITickable {
    public:
        explicit SyncPump(SpaceRuntime& owner);
        void tick(TickContext& context) override;

    private:
        SpaceRuntime* owner_ = nullptr;
    };

    struct WitnessBinding final {
        std::unique_ptr<Witness> witness;
        std::unique_ptr<WitnessViewTrigger> trigger;
    };

    void refreshWitnesses();
    void processEntityInput();
    void applyVelocity(Duration deltaTime);
    void stageDirtyEntities();
    void collectWitnessDirty();

    std::unique_ptr<Space> space_;
    RefreshPump refreshPump_;
    SyncPump syncPump_;
    std::unordered_map<EntityId, WitnessBinding> witnesses_;
    std::unordered_map<EntityId, std::vector<PropertyDelta>> stagedDeltas_;
};

}  // namespace theseed::runtime
