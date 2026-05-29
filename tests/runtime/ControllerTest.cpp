#include "theseed/runtime/Controller.h"
#include "theseed/runtime/Entity.h"
#include "theseed/runtime/RuntimeTypes.h"
#include "theseed/runtime/Space.h"
#include "theseed/runtime/SpaceRuntime.h"

#include <cstdlib>
#include <iostream>
#include <vector>

using theseed::runtime::ControllerId;
using theseed::runtime::ControllerManager;
using theseed::runtime::ControllerType;
using theseed::runtime::Entity;
using theseed::runtime::EntityDef;
using theseed::runtime::EntityId;
using theseed::runtime::EntitySide;
using theseed::runtime::MoveToEntityController;
using theseed::runtime::MoveToPointController;
using theseed::runtime::PropertyType;
using theseed::runtime::SingleCellTopology;
using theseed::runtime::Space;
using theseed::runtime::SpaceConfig;
using theseed::runtime::SpaceId;
using theseed::runtime::SpaceRuntime;
using theseed::runtime::TickContext;
using theseed::runtime::Vector3;

namespace {

int fail(const char* stage) {
    std::cerr << "controller_test_failed_at=" << stage << '\n';
    return EXIT_FAILURE;
}

}  // namespace

int main() {
    std::cout << "Controller tests:" << std::endl;

    EntityDef def("NPC");
    static_cast<void>(def.addProperty("hp", PropertyType::Int32, sizeof(std::int32_t)));

    // --- Test 1: MoveToPointController basic movement ---
    std::cout << "  moveTo basic... ";
    {
        Entity e(1, EntitySide::Cell, def);
        e.setPosition(Vector3{0.0F, 0.0F, 0.0F});
        e.activate();

        struct Result {
            ControllerId id = 0;
            std::int32_t userArg = 0;
            bool success = false;
            int callCount = 0;
        } result;

        e.setOnControllerComplete([&](Entity&, ControllerId id, std::int32_t arg, bool ok) {
            result.id = id;
            result.userArg = arg;
            result.success = ok;
            result.callCount++;
        });

        auto cid = e.moveTo(Vector3{10.0F, 0.0F, 0.0F}, 5.0F, 0.5F, 42);
        if (cid == 0) return fail("moveTo_returned_zero");
        if (!e.hasVelocity()) return fail("moveTo_velocity_not_set");
        if (e.controllers().count() != 1) return fail("moveTo_count");

        // Simulate ticks: 10 units at 5 units/sec = 2 seconds
        e.controllers().tick(1.0F);
        // After 1s: position should be at x=5 (handled by SpaceRuntime normally, but we test controller)
        // Controller just updates velocity direction, doesn't move entity directly
        if (result.callCount != 0) return fail("moveTo_early_complete");

        // Simulate entity at x=9.6 (close to target at 10.0)
        e.setPosition(Vector3{9.6F, 0.0F, 0.0F});
        e.controllers().tick(0.1F);
        // Distance = 0.4 < threshold 0.5, should complete
        if (result.callCount != 1) return fail("moveTo_not_completed");
        if (!result.success) return fail("moveTo_not_success");
        if (result.userArg != 42) return fail("moveTo_userarg");
        if (e.controllers().count() != 0) return fail("moveTo_not_removed");
        if (e.hasVelocity()) return fail("moveTo_velocity_not_cleared");
    }
    std::cout << "OK" << std::endl;

    // --- Test 2: Cancel controller ---
    std::cout << "  cancel controller... ";
    {
        Entity e(2, EntitySide::Cell, def);
        e.setPosition(Vector3{0.0F, 0.0F, 0.0F});
        e.activate();

        auto cid = e.moveTo(Vector3{100.0F, 0.0F, 0.0F}, 10.0F);
        if (e.controllers().count() != 1) return fail("cancel_before");

        e.cancelController(cid);
        if (e.controllers().count() != 0) return fail("cancel_after");
        if (e.hasVelocity()) return fail("cancel_velocity");
    }
    std::cout << "OK" << std::endl;

    // --- Test 3: Multiple controllers ---
    std::cout << "  multiple controllers... ";
    {
        Entity e(3, EntitySide::Cell, def);
        e.setPosition(Vector3{0.0F, 0.0F, 0.0F});
        e.activate();

        auto c1 = e.moveTo(Vector3{10.0F, 0.0F, 0.0F}, 5.0F);
        // Adding a second moveTo should work (though only one sets velocity at a time)
        auto c2 = e.moveTo(Vector3{0.0F, 0.0F, 10.0F}, 3.0F);

        if (e.controllers().count() != 2) return fail("multi_count");
        if (c1 == c2) return fail("multi_same_id");

        // Cancel both
        e.cancelController(c1);
        e.cancelController(c2);
        if (e.controllers().count() != 0) return fail("multi_cancel_all");
    }
    std::cout << "OK" << std::endl;

    // --- Test 4: Already at target ---
    std::cout << "  already at target... ";
    {
        Entity e(4, EntitySide::Cell, def);
        e.setPosition(Vector3{5.0F, 0.0F, 5.0F});
        e.activate();

        int completions = 0;
        bool success = false;
        e.setOnControllerComplete([&](Entity&, ControllerId, std::int32_t, bool ok) {
            completions++;
            success = ok;
        });

        auto cid = e.moveTo(Vector3{5.0F, 0.0F, 5.0F}, 10.0F, 1.0F);
        // Should complete immediately in start() since distance < threshold
        if (completions != 1) return fail("at_target_no_complete");
        if (!success) return fail("at_target_not_success");
        if (e.controllers().count() != 0) return fail("at_target_not_removed");
    }
    std::cout << "OK" << std::endl;

    // --- Test 5: SpaceRuntime integration ---
    std::cout << "  SpaceRuntime integration... ";
    {
        auto topology = std::make_unique<SingleCellTopology>(1);
        auto space = std::make_unique<Space>(1, "test", std::move(topology));
        space->initialize(SpaceConfig{});

        SpaceRuntime runtime(std::move(space));

        Entity e(5, EntitySide::Cell, def);
        runtime.addEntity(e, Vector3{0.0F, 0.0F, 0.0F});
        e.activate();

        bool completed = false;
        e.setOnControllerComplete([&](Entity&, ControllerId, std::int32_t, bool) {
            completed = true;
        });

        e.moveTo(Vector3{10.0F, 0.0F, 0.0F}, 5.0F, 0.5F);

        // Tick with 1 second delta — entity should move 5 units
        TickContext ctx;
        ctx.deltaTime = std::chrono::milliseconds(1000);
        runtime.tick(ctx);

        auto pos = runtime.space().entityPosition(e.id());
        if (!pos.has_value()) return fail("integration_no_pos");
        // After 1 tick at 5 units/sec: should be at x=5.0
        if (std::abs(pos->x - 5.0F) > 0.1F) return fail("integration_pos_wrong");

        // Tick again — entity should reach target
        runtime.tick(ctx);
        pos = runtime.space().entityPosition(e.id());
        // After 2 ticks: should be past 9.5 (threshold), controller completes
        if (!completed && pos->x < 9.5F) return fail("integration_not_done");

        // Third tick should clean up
        runtime.tick(ctx);
        if (!completed) return fail("integration_never_completed");
    }
    std::cout << "OK" << std::endl;

    // --- Test 6: MoveToEntityController ---
    std::cout << "  moveToEntity... ";
    {
        auto topology = std::make_unique<SingleCellTopology>(1);
        auto space = std::make_unique<Space>(1, "test", std::move(topology));
        space->initialize(SpaceConfig{});

        SpaceRuntime runtime(std::move(space));

        Entity hunter(10, EntitySide::Cell, def);
        Entity prey(11, EntitySide::Cell, def);
        runtime.addEntity(hunter, Vector3{0.0F, 0.0F, 0.0F});
        runtime.addEntity(prey, Vector3{3.0F, 0.0F, 0.0F});
        hunter.activate();
        prey.activate();

        bool completed = false;
        bool success = false;
        hunter.setOnControllerComplete([&](Entity&, ControllerId, std::int32_t, bool ok) {
            completed = true;
            success = ok;
        });

        // speed=2.0, range=1.5 → after 1 tick hunter at x=2, dist to prey=1.0 ≤ 1.5
        hunter.moveToEntity(11, 2.0F, 1.5F);

        TickContext ctx;
        ctx.deltaTime = std::chrono::milliseconds(1000);
        runtime.tick(ctx);

        if (!completed) return fail("moveToEntity_not_done");
        if (!success) return fail("moveToEntity_not_success");

        auto hunterPos = runtime.space().entityPosition(hunter.id());
        if (!hunterPos.has_value()) return fail("moveToEntity_no_pos");
        // Hunter should be at x≈2.0
        if (std::abs(hunterPos->x - 2.0F) > 0.1F) return fail("moveToEntity_final_pos");
    }
    std::cout << "OK" << std::endl;

    // --- Test 7: MoveToEntity target removed ---
    std::cout << "  moveToEntity target removed... ";
    {
        Entity hunter(20, EntitySide::Cell, def);
        Entity prey(21, EntitySide::Cell, def);
        hunter.setPosition(Vector3{0.0F, 0.0F, 0.0F});
        hunter.activate();

        bool completed = false;
        bool success = true;
        hunter.setOnControllerComplete([&](Entity&, ControllerId, std::int32_t, bool ok) {
            completed = true;
            success = ok;
        });

        // Set up position provider that returns nullopt for target
        hunter.setPositionProvider([&](EntityId id) -> std::optional<Vector3> {
            if (id == 21) return std::nullopt;
            return Vector3{0.0F, 0.0F, 0.0F};
        });

        hunter.moveToEntity(21, 5.0F, 1.0F);
        hunter.controllers().tick(0.1F);

        if (!completed) return fail("target_removed_no_complete");
        if (success) return fail("target_removed_false_success");
    }
    std::cout << "OK" << std::endl;

    // --- Test 8: ControllerManager clear ---
    std::cout << "  clear all controllers... ";
    {
        Entity e(30, EntitySide::Cell, def);
        e.setPosition(Vector3{0.0F, 0.0F, 0.0F});
        e.activate();

        e.moveTo(Vector3{10.0F, 0.0F, 0.0F}, 5.0F);
        e.moveTo(Vector3{20.0F, 0.0F, 0.0F}, 10.0F);
        if (e.controllers().count() != 2) return fail("clear_count_before");

        e.controllers().clear();
        if (e.controllers().count() != 0) return fail("clear_count_after");
    }
    std::cout << "OK" << std::endl;

    std::cout << "\nAll Controller tests passed!" << std::endl;
    return EXIT_SUCCESS;
}
