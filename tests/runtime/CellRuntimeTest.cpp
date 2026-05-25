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
using theseed::runtime::PropertyType;
using theseed::runtime::RuntimeInvocation;
using theseed::runtime::SingleCellTopology;
using theseed::runtime::Space;
using theseed::runtime::SpaceConfig;
using theseed::runtime::SpaceRuntime;
using theseed::runtime::TickScheduler;
using theseed::runtime::Vector3;

namespace {

int fail(const char* stage) {
    std::cerr << "cell_runtime_test_failed_at=" << stage << '\n';
    return EXIT_FAILURE;
}

}  // namespace

int main() {
    EntityDef def("Avatar");
    const auto hpId = def.addProperty("hp", PropertyType::Int32, sizeof(std::int32_t));
    def.addMethod("castSpell", theseed::runtime::MethodSide::Cell);

    auto transport = std::make_shared<InMemoryRuntimeTransport>();

    auto realTopology = std::make_unique<SingleCellTopology>(1);
    auto realSpace = std::make_unique<Space>(100, "real_space", std::move(realTopology));
    realSpace->initialize(SpaceConfig{.name = "real_space"});
    auto realSpaceRuntime = std::make_unique<SpaceRuntime>(std::move(realSpace));
    CellRuntime realRuntime(std::move(realSpaceRuntime), transport, 11);

    auto ghostTopology = std::make_unique<SingleCellTopology>(2);
    auto ghostSpace = std::make_unique<Space>(200, "ghost_space", std::move(ghostTopology));
    ghostSpace->initialize(SpaceConfig{.name = "ghost_space"});
    auto ghostSpaceRuntime = std::make_unique<SpaceRuntime>(std::move(ghostSpace));
    CellRuntime ghostRuntime(std::move(ghostSpaceRuntime), transport, 22);

    Entity realEntity(1, EntitySide::Cell, def);
    Entity localGhostEntity(1, EntitySide::Cell, def);
    Entity ghostProxyEntity(2, EntitySide::Cell, def);

    std::vector<std::byte> realDispatchPayload;
    int realDispatchCount = 0;
    if (!realEntity.bindMethodHandler("castSpell",
                                      [&](Entity&, std::span<const std::byte> payload) {
                                          realDispatchPayload.assign(payload.begin(), payload.end());
                                          realDispatchCount += 1;
                                      })) {
        return fail("real_bind_method");
    }

    realRuntime.addEntity(realEntity, Vector3{0.0F, 0.0F, 0.0F});
    ghostRuntime.addEntity(localGhostEntity, Vector3{7.0F, 0.0F, 0.0F});
    ghostRuntime.addEntity(ghostProxyEntity, Vector3{5.0F, 0.0F, 0.0F});

    realEntity.activate();
    localGhostEntity.activate();
    ghostProxyEntity.activate();

    auto& realGhost = realRuntime.ensureRealGhost(realEntity, 22);
    if (!realGhost.isReal() || !realGhost.hasGhost() || realGhost.ghostTarget() != 22) {
        return fail("real_ghost_binding");
    }

    auto& localGhost = ghostRuntime.ensureGhostProxy(localGhostEntity, 11);
    if (!localGhost.isGhost() || localGhost.isReal()) {
        return fail("local_ghost_binding");
    }

    TickScheduler scheduler(std::chrono::milliseconds{0});
    realRuntime.attach(scheduler);
    ghostRuntime.attach(scheduler);

    realEntity.setProperty<std::int32_t>(hpId, 99);
    scheduler.runOnce();
    if (transport->pendingCount() != 1) {
        return fail("ghost_sync_pending");
    }
    if (realEntity.isPropertyDirty(hpId)) {
        return fail("ghost_sync_clear_dirty");
    }

    scheduler.runOnce();
    if (localGhostEntity.getProperty<std::int32_t>(hpId) != 99) {
        return fail("ghost_sync_apply_value");
    }
    if (transport->pendingCount() != 0) {
        return fail("ghost_sync_pending_after_apply");
    }

    auto& ghostProxy = ghostRuntime.ensureGhostProxy(ghostProxyEntity, 33);
    if (!ghostProxy.isGhost() || ghostProxy.isReal()) {
        return fail("ghost_proxy_binding");
    }

    std::array<RuntimeInvocation, 4> drained{};
    const std::array<std::byte, 3> payload{std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};
    if (!ghostRuntime.forwardGhostMethod(ghostProxyEntity.id(), "castSpell", payload)) {
        return fail("ghost_forward_send");
    }

    const auto forwardCount = transport->drain(drained.data(), drained.size());
    if (forwardCount != 1) {
        return fail("ghost_forward_count");
    }
    if (drained[0].targetComponent != 33 || drained[0].method != "castSpell" ||
        drained[0].payload.size() != payload.size()) {
        return fail("ghost_forward_payload");
    }

    ghostProxy.setRoute(44, std::chrono::seconds{10}, theseed::runtime::Clock::now());
    if (!ghostRuntime.forwardGhostMethod(ghostProxyEntity.id(), "rerouteSpell", payload)) {
        return fail("ghost_route_forward_send");
    }

    const auto routedCount = transport->drain(drained.data(), drained.size());
    if (routedCount != 1) {
        return fail("ghost_route_forward_count");
    }
    if (drained[0].targetComponent != 44 || drained[0].method != "rerouteSpell") {
        return fail("ghost_route_forward_payload");
    }

    RuntimeInvocation dispatchInvocation;
    dispatchInvocation.entityId = realEntity.id();
    dispatchInvocation.targetComponent = 11;
    dispatchInvocation.entityType = realEntity.entityType();
    dispatchInvocation.method = "castSpell";
    dispatchInvocation.payload.assign(payload.begin(), payload.end());
    if (!transport->send(dispatchInvocation)) {
        return fail("runtime_dispatch_enqueue_local");
    }
    scheduler.runOnce();
    if (realDispatchCount != 1 || realDispatchPayload != dispatchInvocation.payload) {
        return fail("runtime_dispatch_local_payload");
    }

    RuntimeInvocation routedInvocation;
    routedInvocation.entityId = ghostProxyEntity.id();
    routedInvocation.targetComponent = 22;
    routedInvocation.entityType = ghostProxyEntity.entityType();
    routedInvocation.method = "castSpell";
    routedInvocation.payload.assign(payload.begin(), payload.end());
    if (!transport->send(routedInvocation)) {
        return fail("runtime_dispatch_enqueue_forward");
    }
    scheduler.runOnce();

    const auto routedDispatchCount = transport->receive(44, drained.data(), drained.size());
    if (routedDispatchCount != 1) {
        return fail("runtime_dispatch_forward_count");
    }
    if (drained[0].targetComponent != 44 || drained[0].method != "castSpell") {
        return fail("runtime_dispatch_forward_payload");
    }

    ghostRuntime.detach(scheduler);
    realRuntime.detach(scheduler);
    return EXIT_SUCCESS;
}
