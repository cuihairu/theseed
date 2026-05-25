#include "theseed/runtime/SpaceRuntime.h"

#include <chrono>
#include <cstdlib>
#include <iostream>

using theseed::runtime::Entity;
using theseed::runtime::EntityDef;
using theseed::runtime::EntitySide;
using theseed::runtime::PropertyType;
using theseed::runtime::SingleCellTopology;
using theseed::runtime::Space;
using theseed::runtime::SpaceConfig;
using theseed::runtime::SpaceRuntime;
using theseed::runtime::TickScheduler;
using theseed::runtime::Vector3;

namespace {

int fail(const char* stage) {
    std::cerr << "space_runtime_test_failed_at=" << stage << '\n';
    return EXIT_FAILURE;
}

}  // namespace

int main() {
    EntityDef def("Avatar");
    const auto hpId = def.addProperty("hp", PropertyType::Int32, sizeof(std::int32_t));

    auto topology = std::make_unique<SingleCellTopology>(1);
    auto space = std::make_unique<Space>(100, "main", std::move(topology));
    space->initialize(SpaceConfig{.name = "main_runtime"});

    SpaceRuntime runtime(std::move(space));

    Entity owner(1, EntitySide::Cell, def);
    Entity visible(2, EntitySide::Cell, def);
    runtime.addEntity(owner, Vector3{0.0F, 0.0F, 0.0F});
    runtime.addEntity(visible, Vector3{3.0F, 0.0F, 4.0F});

    auto& witness = runtime.ensureWitness(owner, 10.0F);
    witness.setDetailDistanceBands(10.0F, 30.0F);

    TickScheduler scheduler(std::chrono::milliseconds{0});
    runtime.attach(scheduler);
    scheduler.runOnce();

    auto* boundWitness = runtime.findWitness(owner.id());
    if (boundWitness == nullptr || !boundWitness->entityInView(visible.id())) {
        return fail("runtime_witness_initial_view");
    }

    visible.setProperty<std::int32_t>(hpId, 88);
    scheduler.runOnce();
    const auto flushed = boundWitness->flushDeltas();
    if (flushed.size() != 1 || flushed[0].entityId != visible.id() ||
        flushed[0].properties.size() != 1 || flushed[0].properties[0].propertyId != hpId) {
        return fail("runtime_witness_collect_dirty");
    }
    if (visible.isPropertyDirty(hpId)) {
        return fail("runtime_witness_clear_dirty");
    }

    scheduler.runOnce();
    if (!boundWitness->flushDeltas().empty()) {
        return fail("runtime_witness_no_duplicate_delta");
    }

    runtime.space().updateEntityPosition(visible.id(), Vector3{20.0F, 0.0F, 0.0F});
    scheduler.runOnce();
    if (boundWitness->entityInView(visible.id())) {
        return fail("runtime_witness_leave_after_tick");
    }

    runtime.space().updateEntityPosition(visible.id(), Vector3{1.0F, 0.0F, 0.0F});
    scheduler.runOnce();
    if (!boundWitness->entityInView(visible.id())) {
        return fail("runtime_witness_reenter_before_remove");
    }

    runtime.removeEntity(visible.id());
    scheduler.runOnce();
    if (boundWitness->entityInView(visible.id())) {
        return fail("runtime_witness_remove_cleanup");
    }

    runtime.detach(scheduler);
    return EXIT_SUCCESS;
}
