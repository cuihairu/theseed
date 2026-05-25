#include "theseed/runtime/Entity.h"
#include "theseed/runtime/GhostManager.h"
#include "theseed/runtime/Witness.h"

#include <array>
#include <cstdlib>
#include <iostream>

using theseed::runtime::Clock;
using theseed::runtime::DetailLevel;
using theseed::runtime::Entity;
using theseed::runtime::EntityDef;
using theseed::runtime::EntitySide;
using theseed::runtime::GhostManager;
using theseed::runtime::PropertyType;
using theseed::runtime::Witness;

namespace {

int fail(const char* stage) {
    std::cerr << "witness_ghost_test_failed_at=" << stage << '\n';
    return EXIT_FAILURE;
}

}  // namespace

int main() {
    EntityDef def("Avatar");
    const auto hpId = def.addProperty("hp", PropertyType::Int32, sizeof(std::int32_t));
    const auto speedId = def.addProperty("speed", PropertyType::Float32, sizeof(float));

    Entity owner(1, EntitySide::Cell, def);
    Entity visibleA(2, EntitySide::Cell, def);
    Entity visibleB(3, EntitySide::Cell, def);

    Witness witness;
    witness.attach(owner);
    witness.setDetailDistanceBands(10.0F, 30.0F);
    witness.onEnterView(visibleA, 5.0F);
    witness.onEnterView(visibleB, 20.0F);

    if (!witness.attached() || witness.owner() != &owner) {
        return fail("witness_attach");
    }
    if (!witness.entityInView(2) || !witness.entityInView(3)) {
        return fail("witness_view_membership");
    }

    const auto initialView = witness.snapshotView();
    if (initialView.size() != 2) {
        return fail("witness_snapshot_size");
    }
    if (initialView[0].detailLevel != 0 || initialView[1].detailLevel != 1) {
        return fail("witness_detail_level_initial");
    }

    visibleA.setProperty<std::int32_t>(hpId, 50);
    visibleB.setProperty<float>(speedId, 3.0F);
    if (witness.collectDirty() != 2) {
        return fail("witness_collect_dirty");
    }
    visibleA.clearDirtyFlags();
    visibleB.clearDirtyFlags();

    auto flushed = witness.flushDeltas();
    if (flushed.size() != 2) {
        return fail("witness_flush_count");
    }
    if (flushed[0].entityId != 2 || flushed[0].detailLevel != 0) {
        return fail("witness_flush_first");
    }
    if (flushed[1].entityId != 3 || flushed[1].detailLevel != 1) {
        return fail("witness_flush_second");
    }

    witness.updateDistance(3, 40.0F);
    const auto updatedView = witness.snapshotView();
    if (updatedView[1].detailLevel != 2) {
        return fail("witness_detail_level_update");
    }

    visibleB.setProperty<float>(speedId, 7.0F);
    if (witness.collectDirty() != 1) {
        return fail("witness_collect_dirty_after_detail_change");
    }
    visibleB.clearDirtyFlags();
    const auto detailChangedFlush = witness.flushDeltas();
    if (detailChangedFlush.size() != 1 || detailChangedFlush[0].entityId != 3 ||
        detailChangedFlush[0].detailLevel != 2) {
        return fail("witness_flush_after_detail_change");
    }

    witness.onLeaveView(2);
    if (witness.entityInView(2)) {
        return fail("witness_leave");
    }

    GhostManager ghost;
    ghost.attach(owner);
    ghost.setReal(10);
    ghost.createGhost(20);
    if (!ghost.isReal() || ghost.isGhost() || !ghost.hasGhost() || ghost.ghostTarget() != 20) {
        return fail("ghost_real_state");
    }

    ghost.setRoute(30, std::chrono::seconds{5}, Clock::time_point{});
    const auto route = ghost.routeTarget(Clock::time_point{} + std::chrono::seconds{1});
    if (!route.has_value() || *route != 30) {
        return fail("ghost_route_valid");
    }
    if (ghost.routeTarget(Clock::time_point{} + std::chrono::seconds{6}).has_value()) {
        return fail("ghost_route_expired");
    }

    GhostManager proxyGhost;
    proxyGhost.attach(visibleA);
    proxyGhost.setGhost(77);
    if (!proxyGhost.isGhost() || proxyGhost.isReal()) {
        return fail("ghost_proxy_state");
    }

    const std::array<std::byte, 2> payload{std::byte{0x01}, std::byte{0x02}};
    const auto invocation = proxyGhost.forwardToReal("castSpell", payload);
    if (!invocation.has_value()) {
        return fail("ghost_forward_missing");
    }
    if (invocation->targetComponent != 77 || invocation->entityId != visibleA.id()) {
        return fail("ghost_forward_target");
    }
    if (invocation->method != "castSpell" || invocation->payload.size() != payload.size()) {
        return fail("ghost_forward_payload");
    }

    return EXIT_SUCCESS;
}
