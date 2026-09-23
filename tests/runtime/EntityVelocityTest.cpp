#include "theseed/runtime/Entity.h"
#include "theseed/runtime/EntityDef.h"
#include "theseed/runtime/Space.h"
#include "theseed/runtime/SpaceRuntime.h"
#include "theseed/runtime/TickScheduler.h"

#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <utility>

using theseed::runtime::Duration;
using theseed::runtime::Entity;
using theseed::runtime::EntityDef;
using theseed::runtime::EntityId;
using theseed::runtime::EntitySide;
using theseed::runtime::SingleCellTopology;
using theseed::runtime::Space;
using theseed::runtime::SpaceRuntime;
using theseed::runtime::TickContext;
using theseed::runtime::TickScheduler;
using theseed::runtime::Vector3;

static int testsPassed = 0;
static int testsFailed = 0;

#define TEST(name)                                              \
    do {                                                        \
        std::cout << "  " << (name) << "... " << std::flush;    \
    } while (0)

#define PASS()                                                  \
    do {                                                        \
        std::cout << "OK\n";                                    \
        ++testsPassed;                                          \
    } while (0)

#define FAIL(msg)                                               \
    do {                                                        \
        std::cout << "FAILED: " << (msg) << "\n";               \
        ++testsFailed;                                          \
    } while (0)

static std::unique_ptr<SpaceRuntime> makeSpaceRuntime() {
    auto space = std::make_unique<Space>(1, "test", std::make_unique<SingleCellTopology>(1));
    space->initialize();
    return std::make_unique<SpaceRuntime>(std::move(space));
}

static void testSetGetVelocity() {
    TEST("setVelocity/getVelocity/clearVelocity/hasVelocity");

    EntityDef def("Entity");
    Entity e(1, EntitySide::Cell, def);

    bool ok = !e.hasVelocity();
    ok = ok && e.velocity().x == 0.0f && e.velocity().y == 0.0f && e.velocity().z == 0.0f;

    e.setVelocity({10.0f, 0.0f, 5.0f});
    ok = ok && e.hasVelocity();
    ok = ok && std::abs(e.velocity().x - 10.0f) < 0.01f;
    ok = ok && std::abs(e.velocity().z - 5.0f) < 0.01f;

    e.clearVelocity();
    ok = ok && !e.hasVelocity();
    ok = ok && e.velocity().x == 0.0f;

    if (ok) PASS();
    else FAIL("velocity mismatch");
}

static void testVelocityMovesEntity() {
    TEST("velocity moves entity in space over tick");

    auto rt = makeSpaceRuntime();
    EntityDef def("Entity");
    auto entity = std::make_unique<Entity>(1, EntitySide::Cell, def);
    entity->setVelocity({10.0f, 0.0f, 0.0f});  // 10 units/sec in X

    rt->addEntity(*entity, {0, 0, 0});

    // Tick with 1 second
    TickContext ctx;
    ctx.deltaTime = std::chrono::seconds(1);
    rt->tick(ctx);

    auto pos = rt->space().entityPosition(1);
    bool ok = pos.has_value();
    ok = ok && std::abs(pos->x - 10.0f) < 0.01f;
    ok = ok && std::abs(pos->y - 0.0f) < 0.01f;
    ok = ok && std::abs(pos->z - 0.0f) < 0.01f;

    if (ok) PASS();
    else FAIL("x=" + std::to_string(pos->x));
}

static void testVelocityAccumulatesOverTicks() {
    TEST("velocity accumulates over multiple ticks");

    auto rt = makeSpaceRuntime();
    EntityDef def("Entity");
    auto entity = std::make_unique<Entity>(1, EntitySide::Cell, def);
    entity->setVelocity({5.0f, 0.0f, 0.0f});

    rt->addEntity(*entity, {0, 0, 0});

    TickContext ctx;
    ctx.deltaTime = std::chrono::milliseconds(500);  // 0.5s per tick

    // 4 ticks * 0.5s = 2s total => 5 * 2 = 10 units
    for (int i = 0; i < 4; ++i) {
        rt->tick(ctx);
    }

    auto pos = rt->space().entityPosition(1);
    bool ok = pos.has_value();
    ok = ok && std::abs(pos->x - 10.0f) < 0.1f;

    if (ok) PASS();
    else FAIL("x=" + std::to_string(pos->x));
}

static void testNoVelocityNoMovement() {
    TEST("entity without velocity stays in place");

    auto rt = makeSpaceRuntime();
    EntityDef def("Entity");
    auto entity = std::make_unique<Entity>(1, EntitySide::Cell, def);
    // No velocity set

    rt->addEntity(*entity, {5, 10, 15});

    TickContext ctx;
    ctx.deltaTime = std::chrono::seconds(1);
    rt->tick(ctx);

    auto pos = rt->space().entityPosition(1);
    bool ok = pos.has_value();
    ok = ok && std::abs(pos->x - 5.0f) < 0.01f;
    ok = ok && std::abs(pos->y - 10.0f) < 0.01f;
    ok = ok && std::abs(pos->z - 15.0f) < 0.01f;

    if (ok) PASS();
    else FAIL("x=" + std::to_string(pos->x));
}

static void testClearVelocityStopsMovement() {
    TEST("clearVelocity stops entity movement");

    auto rt = makeSpaceRuntime();
    EntityDef def("Entity");
    auto entity = std::make_unique<Entity>(1, EntitySide::Cell, def);
    entity->setVelocity({10, 0, 0});

    rt->addEntity(*entity, {0, 0, 0});

    TickContext ctx;
    ctx.deltaTime = std::chrono::seconds(1);

    // Move for 1 second
    rt->tick(ctx);

    // Clear velocity
    entity->clearVelocity();

    // Another tick should not move
    rt->tick(ctx);

    auto pos = rt->space().entityPosition(1);
    bool ok = pos.has_value();
    ok = ok && std::abs(pos->x - 10.0f) < 0.1f;

    if (ok) PASS();
    else FAIL("x=" + std::to_string(pos->x));
}

static void testDiagonalVelocity() {
    TEST("diagonal velocity moves correctly in 3D");

    auto rt = makeSpaceRuntime();
    EntityDef def("Entity");
    auto entity = std::make_unique<Entity>(1, EntitySide::Cell, def);
    entity->setVelocity({1.0f, 2.0f, 3.0f});

    rt->addEntity(*entity, {0, 0, 0});

    TickContext ctx;
    ctx.deltaTime = std::chrono::seconds(2);
    rt->tick(ctx);

    auto pos = rt->space().entityPosition(1);
    bool ok = pos.has_value();
    ok = ok && std::abs(pos->x - 2.0f) < 0.1f;
    ok = ok && std::abs(pos->y - 4.0f) < 0.1f;
    ok = ok && std::abs(pos->z - 6.0f) < 0.1f;

    if (ok) PASS();
    else FAIL("x=" + std::to_string(pos->x) + " y=" + std::to_string(pos->y)
              + " z=" + std::to_string(pos->z));
}

static void testVelocityChangeMidMovement() {
    TEST("velocity change takes effect on next tick");

    auto rt = makeSpaceRuntime();
    EntityDef def("Entity");
    auto entity = std::make_unique<Entity>(1, EntitySide::Cell, def);
    entity->setVelocity({10, 0, 0});

    rt->addEntity(*entity, {0, 0, 0});

    TickContext ctx;
    ctx.deltaTime = std::chrono::seconds(1);

    rt->tick(ctx);  // pos = (10, 0, 0)

    entity->setVelocity({0, 0, -5});
    rt->tick(ctx);  // pos = (10, 0, -5)

    auto pos = rt->space().entityPosition(1);
    bool ok = pos.has_value();
    ok = ok && std::abs(pos->x - 10.0f) < 0.1f;
    ok = ok && std::abs(pos->y - 0.0f) < 0.1f;
    ok = ok && std::abs(pos->z - (-5.0f)) < 0.1f;

    if (ok) PASS();
    else FAIL("x=" + std::to_string(pos->x) + " z=" + std::to_string(pos->z));
}

static void testMultipleEntitiesWithVelocity() {
    TEST("multiple entities with different velocities");

    auto rt = makeSpaceRuntime();
    EntityDef def("Entity");

    auto e1 = std::make_unique<Entity>(1, EntitySide::Cell, def);
    e1->setVelocity({5, 0, 0});

    auto e2 = std::make_unique<Entity>(2, EntitySide::Cell, def);
    e2->setVelocity({0, 10, 0});

    rt->addEntity(*e1, {0, 0, 0});
    rt->addEntity(*e2, {0, 0, 0});

    TickContext ctx;
    ctx.deltaTime = std::chrono::seconds(1);
    rt->tick(ctx);

    auto pos1 = rt->space().entityPosition(1);
    auto pos2 = rt->space().entityPosition(2);

    bool ok = pos1.has_value() && pos2.has_value();
    ok = ok && std::abs(pos1->x - 5.0f) < 0.1f;
    ok = ok && std::abs(pos2->y - 10.0f) < 0.1f;

    if (ok) PASS();
    else FAIL("e1.x=" + std::to_string(pos1->x) + " e2.y=" + std::to_string(pos2->y));
}

static void testZeroDeltaTimeNoMovement() {
    TEST("zero deltaTime produces no movement");

    auto rt = makeSpaceRuntime();
    EntityDef def("Entity");
    auto entity = std::make_unique<Entity>(1, EntitySide::Cell, def);
    entity->setVelocity({100, 0, 0});

    rt->addEntity(*entity, {0, 0, 0});

    TickContext ctx;
    ctx.deltaTime = Duration{};
    rt->tick(ctx);

    auto pos = rt->space().entityPosition(1);
    bool ok = pos.has_value();
    ok = ok && std::abs(pos->x - 0.0f) < 0.01f;

    if (ok) PASS();
    else FAIL("x=" + std::to_string(pos->x));
}

static void testPositionChangedCallback() {
    TEST("onPositionChanged fires with correct old/new positions");

    auto rt = makeSpaceRuntime();
    EntityDef def("Entity");
    auto entity = std::make_unique<Entity>(1, EntitySide::Cell, def);
    entity->setVelocity({10, 0, 0});

    Vector3 capturedOld{0, 0, 0};
    Vector3 capturedNew{0, 0, 0};
    int callCount = 0;

    entity->setOnPositionChanged([&](Entity&, Vector3 oldPos, Vector3 newPos) {
        capturedOld = oldPos;
        capturedNew = newPos;
        ++callCount;
    });

    rt->addEntity(*entity, {5, 0, 0});

    TickContext ctx;
    ctx.deltaTime = std::chrono::seconds(1);
    rt->tick(ctx);

    bool ok = callCount == 1;
    ok = ok && std::abs(capturedOld.x - 5.0f) < 0.01f;
    ok = ok && std::abs(capturedNew.x - 15.0f) < 0.01f;

    if (ok) PASS();
    else FAIL("old.x=" + std::to_string(capturedOld.x) + " new.x=" + std::to_string(capturedNew.x)
              + " count=" + std::to_string(callCount));
}

static void testNoCallbackWithoutVelocity() {
    TEST("no position callback for stationary entity");

    auto rt = makeSpaceRuntime();
    EntityDef def("Entity");
    auto entity = std::make_unique<Entity>(1, EntitySide::Cell, def);

    int callCount = 0;
    entity->setOnPositionChanged([&](Entity&, Vector3, Vector3) {
        ++callCount;
    });

    rt->addEntity(*entity, {0, 0, 0});

    TickContext ctx;
    ctx.deltaTime = std::chrono::seconds(1);
    rt->tick(ctx);

    if (callCount == 0) PASS();
    else FAIL("count=" + std::to_string(callCount));
}

static void testPositionCallbackReceivesEntityId() {
    TEST("position callback receives correct entity reference");

    auto rt = makeSpaceRuntime();
    EntityDef def("Entity");
    auto entity = std::make_unique<Entity>(42, EntitySide::Cell, def);
    entity->setVelocity({1, 0, 0});

    EntityId capturedId = 0;
    entity->setOnPositionChanged([&](Entity& e, Vector3, Vector3) {
        capturedId = e.id();
    });

    rt->addEntity(*entity, {0, 0, 0});

    TickContext ctx;
    ctx.deltaTime = std::chrono::seconds(1);
    rt->tick(ctx);

    if (capturedId == 42) PASS();
    else FAIL("id=" + std::to_string(capturedId));
}

static void testPositionCallbackAccumulates() {
    TEST("position callback fires on each velocity tick");

    auto rt = makeSpaceRuntime();
    EntityDef def("Entity");
    auto entity = std::make_unique<Entity>(1, EntitySide::Cell, def);
    entity->setVelocity({5, 0, 0});

    int callCount = 0;
    entity->setOnPositionChanged([&](Entity&, Vector3, Vector3) {
        ++callCount;
    });

    rt->addEntity(*entity, {0, 0, 0});

    TickContext ctx;
    ctx.deltaTime = std::chrono::milliseconds(500);

    for (int i = 0; i < 4; ++i) {
        rt->tick(ctx);
    }

    if (callCount == 4) PASS();
    else FAIL("count=" + std::to_string(callCount));
}

int main() {
    std::cout << "EntityVelocity tests:\n";

    testSetGetVelocity();
    testVelocityMovesEntity();
    testVelocityAccumulatesOverTicks();
    testNoVelocityNoMovement();
    testClearVelocityStopsMovement();
    testDiagonalVelocity();
    testVelocityChangeMidMovement();
    testMultipleEntitiesWithVelocity();
    testZeroDeltaTimeNoMovement();

    // Position changed callback tests
    testPositionChangedCallback();
    testNoCallbackWithoutVelocity();
    testPositionCallbackReceivesEntityId();
    testPositionCallbackAccumulates();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
