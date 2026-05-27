#include "theseed/runtime/CellRuntime.h"

#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <vector>

using theseed::runtime::CellRuntime;
using theseed::runtime::Entity;
using theseed::runtime::EntityDef;
using theseed::runtime::EntitySide;
using theseed::runtime::InMemoryRuntimeTransport;
using theseed::runtime::MethodSide;
using theseed::runtime::PropertyType;
using theseed::runtime::RuntimeInvocation;
using theseed::runtime::SendResult;
using theseed::runtime::SingleCellTopology;
using theseed::runtime::Space;
using theseed::runtime::SpaceConfig;
using theseed::runtime::SpaceRuntime;
using theseed::runtime::TickScheduler;
using theseed::runtime::Vector3;

namespace {

int fail(const char* stage) {
    std::cerr << "migration_runtime_test_failed_at=" << stage << '\n';
    return EXIT_FAILURE;
}

}  // namespace

int main() {
    EntityDef def("Avatar");
    const auto hpId = def.addProperty("hp", PropertyType::Int32, sizeof(std::int32_t));
    def.addMethod("castSpell", MethodSide::Cell);

    auto transport = std::make_shared<InMemoryRuntimeTransport>();

    auto sourceTopology = std::make_unique<SingleCellTopology>(1);
    auto sourceSpace = std::make_unique<Space>(100, "source_space", std::move(sourceTopology));
    sourceSpace->initialize(SpaceConfig{.name = "source_space"});
    auto sourceSpaceRuntime = std::make_unique<SpaceRuntime>(std::move(sourceSpace));
    CellRuntime sourceRuntime(std::move(sourceSpaceRuntime), transport, 11);

    auto targetTopology = std::make_unique<SingleCellTopology>(2);
    auto targetSpace = std::make_unique<Space>(200, "target_space", std::move(targetTopology));
    targetSpace->initialize(SpaceConfig{.name = "target_space"});
    auto targetSpaceRuntime = std::make_unique<SpaceRuntime>(std::move(targetSpace));
    CellRuntime targetRuntime(std::move(targetSpaceRuntime), transport, 22);

    std::vector<std::byte> migratedPayload;
    int migratedDispatchCount = 0;
    if (!targetRuntime.registerEntityFactory("Avatar",
                                             [&](theseed::runtime::EntityId entityId,
                                                 EntitySide side) {
                                                 auto entity = std::make_unique<Entity>(entityId, side, def);
                                                 const auto bound =
                                                     entity->bindMethodHandler("castSpell",
                                                                               [&](Entity&,
                                                                                   std::span<const std::byte> payload) {
                                                                                   migratedPayload.assign(payload.begin(),
                                                                                                         payload.end());
                                                                                   migratedDispatchCount += 1;
                                                                               });
                                                 if (!bound) {
                                                     return std::unique_ptr<Entity>{};
                                                 }
                                                 return entity;
                                             })) {
        return fail("register_factory");
    }

    Entity sourceEntity(1, EntitySide::Cell, def);
    sourceEntity.setProperty<std::int32_t>(hpId, 77);
    sourceEntity.clearDirtyFlags();
    sourceRuntime.addEntity(sourceEntity, Vector3{9.0F, 0.0F, 4.0F});
    sourceEntity.activate();

    TickScheduler scheduler(std::chrono::milliseconds{0});
    sourceRuntime.attach(scheduler);
    targetRuntime.attach(scheduler);

    if (!sourceRuntime.beginMigration(sourceEntity.id(), 22, 7)) {
        return fail("begin_migration");
    }
    if (sourceEntity.state() != theseed::runtime::EntityState::Migrating) {
        return fail("source_migrating_state");
    }
    if (transport->pendingCount() != 1) {
        return fail("migration_transfer_pending");
    }

    scheduler.runOnce();

    auto* migratedEntity = targetRuntime.findEntity(sourceEntity.id());
    if (migratedEntity == nullptr) {
        return fail("migration_restore_missing");
    }
    if (migratedEntity->state() != theseed::runtime::EntityState::Active) {
        return fail("migration_restore_state");
    }
    if (migratedEntity->getProperty<std::int32_t>(hpId) != 77) {
        return fail("migration_restore_property");
    }

    const auto migratedPosition =
        targetRuntime.spaceRuntime().space().entityPosition(sourceEntity.id());
    if (!migratedPosition.has_value() || migratedPosition->x != 9.0F || migratedPosition->z != 4.0F) {
        return fail("migration_restore_position");
    }
    if (sourceRuntime.findEntity(sourceEntity.id()) == nullptr) {
        return fail("migration_source_still_present_before_commit");
    }

    scheduler.runOnce();
    if (sourceRuntime.findEntity(sourceEntity.id()) != nullptr) {
        return fail("migration_commit_remove_source_entity");
    }
    if (sourceEntity.state() != theseed::runtime::EntityState::Destroyed) {
        return fail("migration_commit_destroy_source_state");
    }

    const std::array<std::byte, 3> payload{std::byte{0x11}, std::byte{0x22}, std::byte{0x33}};
    RuntimeInvocation oldAddressInvocation;
    oldAddressInvocation.entityId = sourceEntity.id();
    oldAddressInvocation.targetComponent = 11;
    oldAddressInvocation.entityType = sourceEntity.entityType();
    oldAddressInvocation.method = "castSpell";
    oldAddressInvocation.payload.assign(payload.begin(), payload.end());

    if (transport->send(oldAddressInvocation) != SendResult::Accepted) {
        return fail("enqueue_old_address_invocation");
    }
    scheduler.runOnce();
    if (migratedDispatchCount != 1 || migratedPayload != oldAddressInvocation.payload) {
        return fail("migration_route_forward_live");
    }
    const std::array<std::byte, 2> secondPayload{std::byte{0xAA}, std::byte{0xBB}};
    RuntimeInvocation removedEntityInvocation;
    removedEntityInvocation.entityId = sourceEntity.id();
    removedEntityInvocation.targetComponent = 11;
    removedEntityInvocation.entityType = sourceEntity.entityType();
    removedEntityInvocation.method = "castSpell";
    removedEntityInvocation.payload.assign(secondPayload.begin(), secondPayload.end());

    if (transport->send(removedEntityInvocation) != SendResult::Accepted) {
        return fail("enqueue_removed_entity_invocation");
    }
    scheduler.runOnce();
    if (migratedDispatchCount != 2 || migratedPayload != removedEntityInvocation.payload) {
        return fail("migration_route_forward_after_remove");
    }

    if (!sourceRuntime.clearMigrationRoute(sourceEntity.id())) {
        return fail("clear_migration_route");
    }

    RuntimeInvocation droppedInvocation;
    droppedInvocation.entityId = sourceEntity.id();
    droppedInvocation.targetComponent = 11;
    droppedInvocation.entityType = sourceEntity.entityType();
    droppedInvocation.method = "castSpell";
    droppedInvocation.payload.assign(payload.begin(), payload.end());
    if (transport->send(droppedInvocation) != SendResult::Accepted) {
        return fail("enqueue_dropped_invocation");
    }
    scheduler.runOnce();
    if (migratedDispatchCount != 2) {
        return fail("migration_route_cleared");
    }

    targetRuntime.detach(scheduler);
    sourceRuntime.detach(scheduler);
    return EXIT_SUCCESS;
}
