#include "theseed/runtime/Entity.h"
#include "theseed/runtime/EntityDef.h"
#include "theseed/runtime/Space.h"
#include "theseed/runtime/SpaceRuntime.h"
#include "theseed/runtime/TickScheduler.h"

#include <cstring>
#include <iostream>
#include <memory>
#include <string>

using theseed::runtime::Duration;
using theseed::runtime::Entity;
using theseed::runtime::EntityDef;
using theseed::runtime::EntityId;
using theseed::runtime::EntitySide;
using theseed::runtime::SingleCellTopology;
using theseed::runtime::Space;
using theseed::runtime::SpaceRuntime;
using theseed::runtime::TickContext;

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

static void testPushAndProcessInput() {
    TEST("pushInput + processInput dispatches to handler");

    EntityDef def("Player");
    Entity e(1, EntitySide::Cell, def);

    std::string lastAction;
    std::int32_t lastValue = 0;

    e.setActionHandler([&](Entity&, std::string_view action, std::span<const std::byte> payload) {
        lastAction = action;
        if (payload.size() >= sizeof(std::int32_t)) {
            std::memcpy(&lastValue, payload.data(), sizeof(std::int32_t));
        }
    });

    std::int32_t damage = 42;
    Entity::InputAction input;
    input.name = "takeDamage";
    input.payload.resize(sizeof(damage));
    std::memcpy(input.payload.data(), &damage, sizeof(damage));
    e.pushInput(std::move(input));

    bool ok = e.pendingInputCount() == 1;
    e.processInput();
    ok = ok && lastAction == "takeDamage";
    ok = ok && lastValue == 42;
    ok = ok && e.pendingInputCount() == 0;

    if (ok) PASS();
    else FAIL("action=" + lastAction + " value=" + std::to_string(lastValue)
              + " pending=" + std::to_string(e.pendingInputCount()));
}

static void testMultipleInputsProcessedInOrder() {
    TEST("multiple inputs processed in FIFO order");

    EntityDef def("Entity");
    Entity e(1, EntitySide::Cell, def);

    std::vector<std::string> order;

    e.setActionHandler([&](Entity&, std::string_view action, std::span<const std::byte>) {
        order.emplace_back(action);
    });

    e.pushInput({"move", {}});
    e.pushInput({"attack", {}});
    e.pushInput({"move", {}});

    e.processInput();

    bool ok = order.size() == 3;
    ok = ok && order[0] == "move";
    ok = ok && order[1] == "attack";
    ok = ok && order[2] == "move";

    if (ok) PASS();
    else FAIL("size=" + std::to_string(order.size()));
}

static void testClearInput() {
    TEST("clearInput discards pending inputs");

    EntityDef def("Entity");
    Entity e(1, EntitySide::Cell, def);

    int callCount = 0;
    e.setActionHandler([&](Entity&, std::string_view, std::span<const std::byte>) {
        ++callCount;
    });

    e.pushInput({"a", {}});
    e.pushInput({"b", {}});

    bool ok = e.pendingInputCount() == 2;
    e.clearInput();
    ok = ok && e.pendingInputCount() == 0;

    e.processInput();
    ok = ok && callCount == 0;

    if (ok) PASS();
    else FAIL("count=" + std::to_string(callCount));
}

static void testNoHandlerClearsQueue() {
    TEST("processInput without handler clears queue");

    EntityDef def("Entity");
    Entity e(1, EntitySide::Cell, def);

    e.pushInput({"a", {}});
    e.pushInput({"b", {}});

    bool ok = e.pendingInputCount() == 2;
    e.processInput();
    ok = ok && e.pendingInputCount() == 0;

    if (ok) PASS();
    else FAIL("pending=" + std::to_string(e.pendingInputCount()));
}

static void testEmptyPayload() {
    TEST("action with empty payload");

    EntityDef def("Entity");
    Entity e(1, EntitySide::Cell, def);

    bool payloadEmpty = false;
    e.setActionHandler([&](Entity&, std::string_view action, std::span<const std::byte> payload) {
        if (action == "jump") {
            payloadEmpty = payload.empty();
        }
    });

    e.pushInput({"jump", {}});
    e.processInput();

    if (payloadEmpty) PASS();
    else FAIL("expected empty payload");
}

static void testHandlerCanModifyEntity() {
    TEST("action handler can modify entity state");

    EntityDef def("Player");
    def.addProperty("hp", theseed::runtime::PropertyType::Int32);
    Entity e(1, EntitySide::Cell, def);

    e.setProperty<std::int32_t>(0, 100);

    e.setActionHandler([&](Entity& entity, std::string_view action, std::span<const std::byte> payload) {
        if (action == "takeDamage" && payload.size() >= sizeof(std::int32_t)) {
            std::int32_t dmg = 0;
            std::memcpy(&dmg, payload.data(), sizeof(dmg));
            entity.setProperty<std::int32_t>(0, entity.getProperty<std::int32_t>(0) - dmg);
        }
    });

    std::int32_t damage = 30;
    Entity::InputAction input;
    input.name = "takeDamage";
    input.payload.resize(sizeof(damage));
    std::memcpy(input.payload.data(), &damage, sizeof(damage));
    e.pushInput(std::move(input));

    e.processInput();

    bool ok = e.getProperty<std::int32_t>(0) == 70;

    if (ok) PASS();
    else FAIL("hp=" + std::to_string(e.getProperty<std::int32_t>(0)));
}

static void testDestroyClearsInput() {
    TEST("destroy clears input queue");

    EntityDef def("Entity");
    Entity e(1, EntitySide::Cell, def);
    e.activate();

    e.pushInput({"a", {}});
    e.pushInput({"b", {}});

    bool ok = e.pendingInputCount() == 2;

    e.beginDestroy();
    e.destroy();
    ok = ok && e.pendingInputCount() == 0;

    if (ok) PASS();
    else FAIL("pending=" + std::to_string(e.pendingInputCount()));
}

static void testHandlerReceivesEntityReference() {
    TEST("action handler receives correct entity reference");

    EntityDef def("Entity");
    Entity e(42, EntitySide::Base, def);

    EntityId capturedId = 0;
    e.setActionHandler([&](Entity& entity, std::string_view, std::span<const std::byte>) {
        capturedId = entity.id();
    });

    e.pushInput({"test", {}});
    e.processInput();

    if (capturedId == 42) PASS();
    else FAIL("id=" + std::to_string(capturedId));
}

static void testProcessInputAllowsNewPushes() {
    TEST("handler can push new inputs during processing");

    EntityDef def("Entity");
    Entity e(1, EntitySide::Cell, def);

    std::vector<std::string> processed;
    e.setActionHandler([&](Entity& ent, std::string_view action, std::span<const std::byte>) {
        processed.emplace_back(action);
        if (action == "chain") {
            ent.pushInput({"chained", {}});
        }
    });

    e.pushInput({"chain", {}});
    e.processInput();

    // First processInput handles "chain", pushes "chained"
    bool ok = processed.size() == 1 && processed[0] == "chain";
    ok = ok && e.pendingInputCount() == 1;

    // Second processInput handles "chained"
    processed.clear();
    e.processInput();
    ok = ok && processed.size() == 1 && processed[0] == "chained";
    ok = ok && e.pendingInputCount() == 0;

    if (ok) PASS();
    else FAIL("processed=" + std::to_string(processed.size())
              + " pending=" + std::to_string(e.pendingInputCount()));
}

// --- Integration tests with SpaceRuntime ---

static void testSpaceRuntimeTickProcessesInput() {
    TEST("SpaceRuntime tick auto-processes entity input");

    auto space = std::make_unique<Space>(1, "test", std::make_unique<SingleCellTopology>(1));
    space->initialize();
    auto rt = std::make_unique<SpaceRuntime>(std::move(space));

    EntityDef def("Player");
    auto entity = std::make_unique<Entity>(1, EntitySide::Cell, def);
    entity->activate();

    std::string lastAction;
    entity->setActionHandler([&](Entity&, std::string_view action, std::span<const std::byte>) {
        lastAction = action;
    });

    entity->pushInput({"attack", {}});
    rt->addEntity(*entity, {0, 0, 0});

    bool ok = entity->pendingInputCount() == 1;

    TickContext ctx;
    ctx.deltaTime = std::chrono::milliseconds(16);
    rt->tick(ctx);

    ok = ok && lastAction == "attack";
    ok = ok && entity->pendingInputCount() == 0;

    if (ok) PASS();
    else FAIL("action=" + lastAction + " pending=" + std::to_string(entity->pendingInputCount()));
}

static void testInputProcessedBeforeVelocity() {
    TEST("input sets velocity, then velocity moves entity in same tick");

    auto space = std::make_unique<Space>(1, "test", std::make_unique<SingleCellTopology>(1));
    space->initialize();
    auto rt = std::make_unique<SpaceRuntime>(std::move(space));

    EntityDef def("Player");
    auto entity = std::make_unique<Entity>(1, EntitySide::Cell, def);
    entity->activate();

    entity->setActionHandler([&](Entity& e, std::string_view action, std::span<const std::byte>) {
        if (action == "startMoving") {
            e.setVelocity({10, 0, 0});
        }
    });

    rt->addEntity(*entity, {0, 0, 0});
    entity->pushInput({"startMoving", {}});

    TickContext ctx;
    ctx.deltaTime = std::chrono::seconds(1);
    rt->tick(ctx);

    auto pos = rt->space().entityPosition(1);
    bool ok = pos.has_value();
    ok = ok && std::abs(pos->x - 10.0f) < 0.1f;

    if (ok) PASS();
    else FAIL("x=" + std::to_string(pos->x));
}

int main() {
    std::cout << "EntityAction tests:\n";

    testPushAndProcessInput();
    testMultipleInputsProcessedInOrder();
    testClearInput();
    testNoHandlerClearsQueue();
    testEmptyPayload();
    testHandlerCanModifyEntity();
    testDestroyClearsInput();
    testHandlerReceivesEntityReference();
    testProcessInputAllowsNewPushes();

    // Integration: SpaceRuntime tick processes input
    testSpaceRuntimeTickProcessesInput();
    testInputProcessedBeforeVelocity();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
