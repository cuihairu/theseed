#include "theseed/runtime/SpaceRuntime.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <utility>

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

    // SingleCellTopology 边缘分支：邻接格、拓扑回调、负载上报、cellId
    {
        auto topo = std::make_unique<SingleCellTopology>(7);
        auto* topoPtr = topo.get();
        auto edgeSpace = std::make_unique<Space>(200, "edge", std::move(topo));

        // const 访问器：topology() / coordinateSystem()
        const auto& constSpace = *edgeSpace;
        static_cast<void>(constSpace.topology().locateCell(Vector3{}));
        static_cast<void>(constSpace.coordinateSystem());

        if (topoPtr->locateCell(Vector3{1, 2, 3}) != 7) {
            return fail("topology_locate");
        }
        const auto adjacent = topoPtr->getAdjacentCells(Vector3{}, 10.0F);
        if (adjacent.size() != 1 || adjacent[0] != 7) {
            return fail("topology_adjacent");
        }
        if (topoPtr->cellId() != 7) {
            return fail("topology_cell_id");
        }

        bool rebalanced = false;
        topoPtr->onTopologyChanged([&] {
            rebalanced = true;
        });
        topoPtr->rebalance();
        if (!rebalanced) {
            return fail("topology_rebalance_callback");
        }

        topoPtr->reportLoad(7, 0.75F);
        if (topoPtr->lastReportedLoad() != 0.75F) {
            return fail("topology_report_load");
        }

        bool threw = false;
        try {
            topoPtr->reportLoad(8, 0.5F);  // 非本格 id → 拒绝
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        if (!threw) {
            return fail("topology_report_wrong_cell");
        }

        // Space 构造：null topology 拒绝
        threw = false;
        try {
            Space bad(201, "bad", nullptr);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        if (!threw) {
            return fail("space_null_topology");
        }

        // addEntity 重复 id 拒绝
        threw = false;
        try {
            Entity dup(1, EntitySide::Cell, def);
            edgeSpace->initialize(SpaceConfig{});
            edgeSpace->addEntity(dup, Vector3{});
            edgeSpace->addEntity(dup, Vector3{1, 1, 1});
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        if (!threw) {
            return fail("space_duplicate_entity");
        }
    }

    return EXIT_SUCCESS;
}
