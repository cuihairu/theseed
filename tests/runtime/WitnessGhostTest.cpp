#include "theseed/runtime/Entity.h"
#include "theseed/runtime/GhostManager.h"
#include "theseed/runtime/Witness.h"

#include <array>
#include <cstddef>
#include <cstdlib>
#include <stdexcept>
#include <iostream>

using theseed::runtime::Clock;
using theseed::runtime::DetailLevel;
using theseed::runtime::Entity;
using theseed::runtime::EntityDef;
using theseed::runtime::EntitySide;
using theseed::runtime::GhostManager;
using theseed::runtime::PropertyType;
using theseed::runtime::Vector3;
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

    // GhostManager 剩余分支：非法参数抛异常、ghost 生命周期、路由清理、未附加 forward
    {
        GhostManager g;
        g.attach(visibleB);

        bool threw = false;
        try {
            g.setGhost(0);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        if (!threw) {
            return fail("ghost_set_ghost_zero");
        }

        threw = false;
        try {
            g.createGhost(0);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        if (!threw) {
            return fail("ghost_create_ghost_zero");
        }

        threw = false;
        try {
            g.setRoute(0, std::chrono::seconds{1}, Clock::time_point{});
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        if (!threw) {
            return fail("ghost_set_route_zero");
        }

        threw = false;
        try {
            static_cast<void>(g.ghostTarget());
        } catch (const std::logic_error&) {
            threw = true;
        }
        if (!threw) {
            return fail("ghost_target_unset");
        }

        g.createGhost(5);
        if (g.ghostTarget() != 5) {
            return fail("ghost_target_value");
        }
        g.destroyGhost();
        if (g.hasGhost()) {
            return fail("ghost_destroyed");
        }

        g.setRoute(6, std::chrono::seconds{30}, Clock::time_point{});
        g.clearRoute();
        if (g.routeTarget(Clock::time_point{}).has_value()) {
            return fail("ghost_route_cleared");
        }

        g.detach();
        if (g.owner() != nullptr) {
            return fail("ghost_detached");
        }
        threw = false;
        try {
            static_cast<void>(g.forwardToReal("m", payload));
        } catch (const std::logic_error&) {
            threw = true;
        }
        if (!threw) {
            return fail("ghost_forward_unattached");
        }

        // 已附加但非 ghost：forwardToReal 返回 nullopt
        GhostManager real;
        real.attach(visibleB);
        real.setReal(3);
        if (real.forwardToReal("m", payload).has_value()) {
            return fail("ghost_forward_non_ghost");
        }
    }

    // recordPosition 对不在册实体静默忽略（早退臂），在册实体正常暂存
    {
        Witness w2;
        w2.attach(owner);
        w2.setDetailDistanceBands(10.0F, 30.0F);
        w2.onEnterView(visibleA, 5.0F);
        w2.recordPosition(9999, Vector3{1.0F, 2.0F, 3.0F});   // 未知 id：no-op
        w2.recordPosition(visibleA.id(), Vector3{4.0F, 5.0F, 6.0F});
        // 在册实体仍在视图；未知 id 未被登记
        if (!w2.entityInView(visibleA.id())) return fail("record_position_lost_view");
        if (w2.entityInView(9999)) return fail("unknown_id_registered");
    }

    // 非法 detail bands：throw（参数校验真臂）
    {
        Witness bad;
        bool threw = false;
        try {
            bad.setDetailDistanceBands(-1.0F, 30.0F);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        if (!threw) return fail("negative_near_band");
        threw = false;
        try {
            bad.setDetailDistanceBands(30.0F, 10.0F);  // mid < near
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        if (!threw) return fail("mid_lt_near_band");
    }

    // 未 attach（owner 空）：onEnterView/onLeaveView 静默跳过事件推送
    {
        Witness lone;
        lone.onEnterView(visibleA, 5.0F);
        lone.onLeaveView(visibleA.id());
        if (lone.entityInView(visibleA.id())) return fail("lone_should_have_no_stale_entry");
        if (!lone.flushAoIEvents().empty()) return fail("lone_should_not_emit_events");
    }

    return EXIT_SUCCESS;
}
