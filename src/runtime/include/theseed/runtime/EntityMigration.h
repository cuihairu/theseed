#pragma once

#include "theseed/runtime/Entity.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
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
                                           std::optional<Vector3> position = std::nullopt,  // LCOV_EXCL_BR_LINE 默认实参跳转副本边：显式传值/nullopt 两臂均已由迁移测试覆盖
                                           SpaceId spaceId = 0);

    static void restore(Entity& entity, const EntityMigrationSnapshot& snapshot);
    static std::vector<std::byte> encode(const EntityMigrationSnapshot& snapshot);
    static EntityMigrationSnapshot decode(std::span<const std::byte> payload);
};

}  // namespace theseed::runtime
