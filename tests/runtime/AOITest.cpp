#include "theseed/runtime/AOI.h"
#include "theseed/runtime/Space.h"

#include <cstdlib>
#include <stdexcept>
#include <iostream>
#include <utility>
#include <vector>

using theseed::runtime::CoordinateNode;
using theseed::runtime::CoordinateSystem;
using theseed::runtime::Entity;
using theseed::runtime::EntityDef;
using theseed::runtime::EntitySide;
using theseed::runtime::PropertyType;
using theseed::runtime::RangeTrigger;
using theseed::runtime::Space;
using theseed::runtime::SpaceConfig;
using theseed::runtime::SpaceState;
using theseed::runtime::SingleCellTopology;
using theseed::runtime::Vector3;

namespace {

int fail(const char* stage) {
    std::cerr << "aoi_test_failed_at=" << stage << '\n';
    return EXIT_FAILURE;
}

class RecordingTrigger final : public RangeTrigger {
public:
    using RangeTrigger::RangeTrigger;

    std::vector<std::uint64_t> entered;
    std::vector<std::uint64_t> left;

private:
    void onEnter(CoordinateNode& node, float distance) override {
        static_cast<void>(distance);
        entered.push_back(node.entityId());
    }

    void onLeave(CoordinateNode& node) override {
        left.push_back(node.entityId());
    }
};

}  // namespace

int main() {
    EntityDef def("Avatar");
    static_cast<void>(def.addProperty("hp", PropertyType::Int32, sizeof(std::int32_t)));

    Entity owner(1, EntitySide::Cell, def);
    Entity nearEntity(2, EntitySide::Cell, def);
    Entity farEntity(3, EntitySide::Cell, def);

    CoordinateSystem coordinateSystem;
    CoordinateNode ownerNode(owner, Vector3{0.0F, 0.0F, 0.0F});
    CoordinateNode nearNode(nearEntity, Vector3{3.0F, 0.0F, 4.0F});
    CoordinateNode farNode(farEntity, Vector3{20.0F, 0.0F, 0.0F});
    coordinateSystem.insert(ownerNode);
    coordinateSystem.insert(nearNode);
    coordinateSystem.insert(farNode);

    const auto inRange = coordinateSystem.entitiesInRange(Vector3{0.0F, 0.0F, 0.0F}, 5.0F);
    if (inRange.size() != 2) {
        return fail("range_query");
    }

    RecordingTrigger trigger(owner, 5.0F);
    trigger.install(coordinateSystem);
    if (trigger.entered.size() != 1 || trigger.entered[0] != 2) {
        return fail("trigger_enter_initial");
    }

    coordinateSystem.update(3, Vector3{2.0F, 0.0F, 0.0F});
    trigger.refresh();
    if (trigger.entered.size() != 2 || trigger.entered[1] != 3) {
        return fail("trigger_enter_after_move");
    }

    coordinateSystem.update(2, Vector3{10.0F, 0.0F, 0.0F});
    trigger.refresh();
    if (trigger.left.size() != 1 || trigger.left[0] != 2) {
        return fail("trigger_leave_after_move");
    }

    // --- RangeTrigger 生命周期：访问器、卸载、未安装更新、重装、异常路径 ---
    if (trigger.range() != 5.0F || trigger.owner().id() != 1 || !trigger.installed()) {
        return fail("trigger_accessors");
    }

    trigger.uninstall();
    if (trigger.installed()) {
        return fail("trigger_uninstall");
    }
    trigger.refresh();  // 未安装时静默返回

    trigger.updateRange(7.0F);  // 未安装时只改范围不刷新
    if (trigger.range() != 7.0F) {
        return fail("trigger_update_range_uninstalled");
    }

    trigger.install(coordinateSystem);  // 重新安装：entity 3（距离 2）重新进入
    if (!trigger.installed() || trigger.entered.size() != 3 || trigger.entered[2] != 3) {
        return fail("trigger_reinstall");
    }

    trigger.updateRange(1.0F);  // 已安装时立即刷新：entity 3（距离 2）离开
    if (trigger.range() != 1.0F || trigger.left.size() != 2 || trigger.left[1] != 3) {
        return fail("trigger_update_range_installed");
    }

    // owner 不在坐标系时 refresh 抛 logic_error（install 内部即触发）
    {
        RecordingTrigger rogue(owner, 1.0F);
        CoordinateSystem emptySystem;
        bool threw = false;
        try {
            rogue.install(emptySystem);
        } catch (const std::logic_error&) {
            threw = true;
        }
        if (!threw) {
            return fail("trigger_owner_missing");
        }
    }

    // 更新不存在的实体坐标抛 out_of_range
    {
        bool threw = false;
        try {
            coordinateSystem.update(999, Vector3{0.0F, 0.0F, 0.0F});
        } catch (const std::out_of_range&) {
            threw = true;
        }
        if (!threw) {
            return fail("coordinate_update_unknown");
        }
    }

    auto topology = std::make_unique<SingleCellTopology>(9);
    auto* topologyPtr = topology.get();
    Space space(100, "main", std::move(topology));
    space.initialize(SpaceConfig{.name = "main_runtime"});
    if (space.state() != SpaceState::Running || space.name() != "main_runtime") {
        return fail("space_initialize");
    }

    space.addEntity(owner, Vector3{0.0F, 0.0F, 0.0F});
    space.addEntity(nearEntity, Vector3{1.0F, 0.0F, 1.0F});
    if (space.entityCount() != 2) {
        return fail("space_entity_count");
    }
    if (space.topology().locateCell(Vector3{}) != 9) {
        return fail("space_topology_locate");
    }

    topologyPtr->reportLoad(9, 0.75F);
    if (topologyPtr->lastReportedLoad() != 0.75F) {
        return fail("space_topology_load");
    }

    const auto spaceRange = space.queryRange(Vector3{0.0F, 0.0F, 0.0F}, 2.0F);
    if (spaceRange.size() != 2) {
        return fail("space_query_range");
    }

    space.updateEntityPosition(nearEntity.id(), Vector3{8.0F, 0.0F, 0.0F});
    const auto movedPosition = space.entityPosition(nearEntity.id());
    if (!movedPosition.has_value() || movedPosition->x != 8.0F) {
        return fail("space_update_position");
    }

    space.beginDrain();
    if (space.state() != SpaceState::Draining) {
        return fail("space_begin_drain");
    }

    space.shutdown();
    if (space.state() != SpaceState::Shutdown || space.entityCount() != 0) {
        return fail("space_shutdown");
    }

    return EXIT_SUCCESS;
}
