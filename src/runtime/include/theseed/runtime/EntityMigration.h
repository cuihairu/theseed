#pragma once

#include "theseed/runtime/Entity.h"

#include <optional>
#include <vector>

namespace theseed::runtime {

struct RouteBinding final {
    ComponentId targetComponent = 0;
    DeliveryClass deliveryClass = DeliveryClass::ORDERED_RELIABLE;
};

struct EntityMigrationSnapshot final {
    EntityId entityId = 0;
    EntitySide side = EntitySide::Base;
    std::string entityType;
    MigrationEpoch epoch = 0;
    ComponentId sourceComponent = 0;
    ComponentId targetComponent = 0;
    SpaceId spaceId = 0;
    std::optional<Vector3> position;
    std::vector<std::byte> propertyStorage;
    std::optional<RouteBinding> baseCall;
    std::optional<RouteBinding> cellCall;
};

class EntityMigration final {
public:
    static EntityMigrationSnapshot capture(const Entity& entity,
                                           MigrationEpoch epoch,
                                           ComponentId sourceComponent,
                                           ComponentId targetComponent,
                                           std::optional<Vector3> position = std::nullopt,
                                           SpaceId spaceId = 0);

    static void restore(Entity& entity, const EntityMigrationSnapshot& snapshot);
    static std::vector<std::byte> encode(const EntityMigrationSnapshot& snapshot);
    static EntityMigrationSnapshot decode(std::span<const std::byte> payload);
};

}  // namespace theseed::runtime
