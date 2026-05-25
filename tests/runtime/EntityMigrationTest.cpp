#include "theseed/runtime/Entity.h"
#include "theseed/runtime/EntityMigration.h"

#include <cstdlib>
#include <iostream>

using theseed::runtime::DeliveryClass;
using theseed::runtime::Entity;
using theseed::runtime::EntityDef;
using theseed::runtime::EntityMigration;
using theseed::runtime::EntityMigrationSnapshot;
using theseed::runtime::EntitySide;
using theseed::runtime::EntityState;
using theseed::runtime::PropertyType;

namespace {

int fail(const char* stage) {
    std::cerr << "entity_migration_test_failed_at=" << stage << '\n';
    return EXIT_FAILURE;
}

}  // namespace

int main() {
    EntityDef def("Avatar");
    const auto hpId = def.addProperty("hp", PropertyType::Int32, sizeof(std::int32_t));
    const auto speedId = def.addProperty("speed", PropertyType::Float32, sizeof(float));

    Entity source(1001, EntitySide::Cell, def);
    source.setProperty<std::int32_t>(hpId, 66);
    source.setProperty<float>(speedId, 4.5F);
    source.bindBaseEntityCall(11);
    source.bindCellEntityCall(22, DeliveryClass::UNORDERED_LOSSY);
    source.beginMigration();

    const auto snapshot = EntityMigration::capture(source, 7, 11, 22);
    if (snapshot.entityId != 1001 || snapshot.epoch != 7) {
        return fail("snapshot_identity");
    }
    if (!snapshot.baseCall.has_value() || !snapshot.cellCall.has_value()) {
        return fail("snapshot_route_presence");
    }
    if (snapshot.baseCall->targetComponent != 11 ||
        snapshot.cellCall->targetComponent != 22) {
        return fail("snapshot_route_values");
    }

    Entity target(1001, EntitySide::Cell, def);
    EntityMigration::restore(target, snapshot);

    if (target.state() != EntityState::Active) {
        return fail("restore_state");
    }
    if (target.getProperty<std::int32_t>(hpId) != 66) {
        return fail("restore_hp");
    }
    if (target.getProperty<float>(speedId) != 4.5F) {
        return fail("restore_speed");
    }
    if (target.baseEntityCall() == nullptr || target.cellEntityCall() == nullptr) {
        return fail("restore_route_presence");
    }
    if (target.baseEntityCall()->targetComponent() != 11 ||
        target.cellEntityCall()->targetComponent() != 22) {
        return fail("restore_route_values");
    }
    if (target.cellEntityCall()->deliveryClass() != DeliveryClass::UNORDERED_LOSSY) {
        return fail("restore_delivery");
    }

    return EXIT_SUCCESS;
}
