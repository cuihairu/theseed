#pragma once

#include "theseed/runtime/AOI.h"
#include "theseed/runtime/Entity.h"
#include "theseed/runtime/PropertyReplication.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace theseed::runtime {

struct WitnessView final {
    EntityId entityId = 0;
    float distance = 0.0F;
    DetailLevel detailLevel = 0;
};

struct WitnessDelta final {
    EntityId entityId = 0;
    DetailLevel detailLevel = 0;
    std::vector<PropertyDelta> properties;
};

class Witness final {
public:
    Witness() = default;

    void attach(Entity& owner);
    void detach();
    bool attached() const;
    Entity* owner() const;

    void setDetailDistanceBands(float nearDistance, float midDistance);
    DetailLevel classifyDistance(float distance) const;

    void onEnterView(Entity& entity, float distance);
    void onLeaveView(EntityId entityId);
    bool entityInView(EntityId entityId) const;

    void updateDistance(EntityId entityId, float distance);
    std::vector<WitnessView> snapshotView() const;

    std::size_t collectDirty();
    void recordDirty(EntityId entityId, std::span<const PropertyDelta> properties);
    std::vector<WitnessDelta> flushDeltas();

private:
    struct Entry final {
        Entity* entity = nullptr;
        float distance = 0.0F;
        DetailLevel detailLevel = 0;
        std::array<std::vector<PropertyDelta>, 3> stagedDeltas;
    };

    Entity* owner_ = nullptr;
    float nearDistance_ = 25.0F;
    float midDistance_ = 60.0F;
    std::unordered_map<EntityId, Entry> entries_;
};

class WitnessViewTrigger final : public RangeTrigger {
public:
    WitnessViewTrigger(Witness& witness, Entity& owner, float range);

private:
    void onEnter(CoordinateNode& node, float distance) override;
    void onLeave(CoordinateNode& node) override;

    Witness* witness_ = nullptr;
};

}  // namespace theseed::runtime
