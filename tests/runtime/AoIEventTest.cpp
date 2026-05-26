#include "theseed/runtime/CellRuntime.h"
#include "theseed/runtime/Entity.h"
#include "theseed/runtime/EntityDef.h"
#include "theseed/runtime/RuntimeTransport.h"
#include "theseed/runtime/SpaceRuntime.h"

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using theseed::runtime::CellRuntime;
using theseed::runtime::Entity;
using theseed::runtime::EntityDef;
using theseed::runtime::EntityId;
using theseed::runtime::EntitySide;
using theseed::runtime::InMemoryRuntimeTransport;
using theseed::runtime::SingleCellTopology;
using theseed::runtime::Space;
using theseed::runtime::SpaceRuntime;
using theseed::runtime::TickContext;
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

static std::shared_ptr<EntityDef> makeDef() {
    auto def = std::make_shared<EntityDef>("Avatar");
    def->addProperty("hp", theseed::runtime::PropertyType::Int32, 4);
    return def;
}

static void testEnterAoI() {
    TEST("entity notified when another enters view range");

    auto transport = std::make_shared<InMemoryRuntimeTransport>();
    auto space = std::make_unique<Space>(1, "test", std::make_unique<SingleCellTopology>(1));
    space->initialize();
    auto spaceRuntime = std::make_unique<SpaceRuntime>(std::move(space));
    CellRuntime rt(std::move(spaceRuntime), transport, 2);

    auto def = makeDef();

    auto observer = std::make_unique<Entity>(100, EntitySide::Cell, *def);
    EntityId enteredId = 0;
    observer->setOnEnterAoI([&enteredId](Entity& self, EntityId other) {
        enteredId = other;
    });

    auto target = std::make_unique<Entity>(200, EntitySide::Cell, *def);

    rt.addEntity(*observer, Vector3{0, 0, 0});
    rt.addEntity(*target, Vector3{5, 0, 0});

    rt.spaceRuntime().ensureWitness(*observer, 50.0f);

    TickContext ctx;
    ctx.deltaTime = std::chrono::milliseconds{16};
    rt.spaceRuntime().tick(ctx);

    if (enteredId == 200) PASS();
    else FAIL("expected enteredId=200, got " + std::to_string(enteredId));
}

static void testLeaveAoI() {
    TEST("entity notified when another leaves view range");

    auto transport = std::make_shared<InMemoryRuntimeTransport>();
    auto space = std::make_unique<Space>(1, "test", std::make_unique<SingleCellTopology>(1));
    space->initialize();
    auto spaceRuntime = std::make_unique<SpaceRuntime>(std::move(space));
    CellRuntime rt(std::move(spaceRuntime), transport, 2);

    auto def = makeDef();

    auto observer = std::make_unique<Entity>(100, EntitySide::Cell, *def);
    EntityId leftId = 0;
    observer->setOnLeaveAoI([&leftId](Entity& self, EntityId other) {
        leftId = other;
    });

    auto target = std::make_unique<Entity>(200, EntitySide::Cell, *def);

    rt.addEntity(*observer, Vector3{0, 0, 0});
    rt.addEntity(*target, Vector3{5, 0, 0});
    rt.spaceRuntime().ensureWitness(*observer, 50.0f);

    TickContext ctx;
    ctx.deltaTime = std::chrono::milliseconds{16};
    rt.spaceRuntime().tick(ctx);

    rt.removeEntity(200);
    rt.spaceRuntime().tick(ctx);

    if (leftId == 200) PASS();
    else FAIL("expected leftId=200, got " + std::to_string(leftId));
}

static void testNoAoICallbackWithoutWitness() {
    TEST("no AoI callbacks without witness setup");

    auto transport = std::make_shared<InMemoryRuntimeTransport>();
    auto space = std::make_unique<Space>(1, "test", std::make_unique<SingleCellTopology>(1));
    space->initialize();
    auto spaceRuntime = std::make_unique<SpaceRuntime>(std::move(space));
    CellRuntime rt(std::move(spaceRuntime), transport, 2);

    auto def = makeDef();

    auto observer = std::make_unique<Entity>(100, EntitySide::Cell, *def);
    bool entered = false;
    observer->setOnEnterAoI([&entered](Entity&, EntityId) { entered = true; });

    auto target = std::make_unique<Entity>(200, EntitySide::Cell, *def);

    rt.addEntity(*observer, Vector3{0, 0, 0});
    rt.addEntity(*target, Vector3{5, 0, 0});

    TickContext ctx;
    ctx.deltaTime = std::chrono::milliseconds{16};
    rt.spaceRuntime().tick(ctx);

    if (!entered) PASS();
    else FAIL("callback fired without witness");
}

static void testNoCallbackIfNotSet() {
    TEST("no crash when AoI callbacks not set");

    auto transport = std::make_shared<InMemoryRuntimeTransport>();
    auto space = std::make_unique<Space>(1, "test", std::make_unique<SingleCellTopology>(1));
    space->initialize();
    auto spaceRuntime = std::make_unique<SpaceRuntime>(std::move(space));
    CellRuntime rt(std::move(spaceRuntime), transport, 2);

    auto def = makeDef();

    auto observer = std::make_unique<Entity>(100, EntitySide::Cell, *def);
    auto target = std::make_unique<Entity>(200, EntitySide::Cell, *def);

    rt.addEntity(*observer, Vector3{0, 0, 0});
    rt.addEntity(*target, Vector3{5, 0, 0});
    rt.spaceRuntime().ensureWitness(*observer, 50.0f);

    TickContext ctx;
    ctx.deltaTime = std::chrono::milliseconds{16};
    rt.spaceRuntime().tick(ctx);

    PASS();
}

static void testOutOfRangeNoEnter() {
    TEST("no enter callback when entity is out of range");

    auto transport = std::make_shared<InMemoryRuntimeTransport>();
    auto space = std::make_unique<Space>(1, "test", std::make_unique<SingleCellTopology>(1));
    space->initialize();
    auto spaceRuntime = std::make_unique<SpaceRuntime>(std::move(space));
    CellRuntime rt(std::move(spaceRuntime), transport, 2);

    auto def = makeDef();

    auto observer = std::make_unique<Entity>(100, EntitySide::Cell, *def);
    bool entered = false;
    observer->setOnEnterAoI([&entered](Entity&, EntityId) { entered = true; });

    auto target = std::make_unique<Entity>(200, EntitySide::Cell, *def);

    rt.addEntity(*observer, Vector3{0, 0, 0});
    rt.addEntity(*target, Vector3{200, 0, 0});
    rt.spaceRuntime().ensureWitness(*observer, 50.0f);

    TickContext ctx;
    ctx.deltaTime = std::chrono::milliseconds{16};
    rt.spaceRuntime().tick(ctx);

    if (!entered) PASS();
    else FAIL("callback fired for out-of-range entity");
}

static void testCallbackReceivesSelfReference() {
    TEST("AoI callback receives correct self entity reference");

    auto transport = std::make_shared<InMemoryRuntimeTransport>();
    auto space = std::make_unique<Space>(1, "test", std::make_unique<SingleCellTopology>(1));
    space->initialize();
    auto spaceRuntime = std::make_unique<SpaceRuntime>(std::move(space));
    CellRuntime rt(std::move(spaceRuntime), transport, 2);

    auto def = makeDef();

    auto observer = std::make_unique<Entity>(100, EntitySide::Cell, *def);
    EntityId selfId = 0;
    observer->setOnEnterAoI([&selfId](Entity& self, EntityId) { selfId = self.id(); });

    auto target = std::make_unique<Entity>(200, EntitySide::Cell, *def);

    rt.addEntity(*observer, Vector3{0, 0, 0});
    rt.addEntity(*target, Vector3{5, 0, 0});
    rt.spaceRuntime().ensureWitness(*observer, 50.0f);

    TickContext ctx;
    ctx.deltaTime = std::chrono::milliseconds{16};
    rt.spaceRuntime().tick(ctx);

    if (selfId == 100) PASS();
    else FAIL("expected selfId=100, got " + std::to_string(selfId));
}

int main() {
    std::cout << "AoI Event tests:\n";

    testEnterAoI();
    testLeaveAoI();
    testNoAoICallbackWithoutWitness();
    testNoCallbackIfNotSet();
    testOutOfRangeNoEnter();
    testCallbackReceivesSelfReference();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
