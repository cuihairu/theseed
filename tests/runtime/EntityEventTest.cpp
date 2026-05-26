#include "theseed/runtime/CellRuntime.h"
#include "theseed/runtime/Entity.h"
#include "theseed/runtime/EntityDef.h"
#include "theseed/runtime/RuntimeTransport.h"
#include "theseed/runtime/SpaceRuntime.h"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>

using theseed::runtime::CellRuntime;
using theseed::runtime::Entity;
using theseed::runtime::EntityDef;
using theseed::runtime::EntitySide;
using theseed::runtime::InMemoryRuntimeTransport;
using theseed::runtime::SingleCellTopology;
using theseed::runtime::Space;
using theseed::runtime::SpaceRuntime;
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

static std::unique_ptr<CellRuntime> makeCellRuntime() {
    auto transport = std::make_shared<InMemoryRuntimeTransport>();
    auto space = std::make_unique<Space>(1, "default", std::make_unique<SingleCellTopology>(1));
    space->initialize();
    auto spaceRuntime = std::make_unique<SpaceRuntime>(std::move(space));
    return std::make_unique<CellRuntime>(std::move(spaceRuntime), transport, 2);
}

static void testSubscribeAndEmit() {
    TEST("subscribe and emit triggers callback");

    EntityDef def("Entity");
    Entity e(1, EntitySide::Cell, def);

    bool triggered = false;
    e.subscribe("test_event", [&triggered](Entity&, std::string_view evt, std::span<const std::byte>) {
        triggered = (evt == "test_event");
    });

    e.emit("test_event");

    if (triggered) PASS();
    else FAIL("callback not triggered");
}

static void testEmitWithData() {
    TEST("emit delivers event data to callback");

    EntityDef def("Entity");
    Entity e(1, EntitySide::Cell, def);

    std::vector<std::byte> received;
    e.subscribe("damage", [&received](Entity&, std::string_view, std::span<const std::byte> data) {
        received.assign(data.begin(), data.end());
    });

    std::vector<std::byte> payload = {std::byte{0x42}, std::byte{0xFF}};
    e.emit("damage", payload);

    if (received.size() == 2 && received[0] == std::byte{0x42}) PASS();
    else FAIL("data mismatch");
}

static void testUnsubscribe() {
    TEST("unsubscribe prevents callback");

    EntityDef def("Entity");
    Entity e(1, EntitySide::Cell, def);

    int count = 0;
    e.subscribe("tick", [&count](Entity&, std::string_view, std::span<const std::byte>) { ++count; });

    e.emit("tick");
    e.unsubscribe("tick");
    e.emit("tick");

    if (count == 1) PASS();
    else FAIL("expected count=1, got " + std::to_string(count));
}

static void testMultipleSubscribers() {
    TEST("multiple subscribers for same event");

    EntityDef def("Entity");
    Entity e(1, EntitySide::Cell, def);

    int a = 0, b = 0;
    e.subscribe("hit", [&a](Entity&, std::string_view, std::span<const std::byte>) { ++a; });
    e.subscribe("hit", [&b](Entity&, std::string_view, std::span<const std::byte>) { ++b; });

    e.emit("hit");

    if (a == 1 && b == 1) PASS();
    else FAIL("a=" + std::to_string(a) + " b=" + std::to_string(b));
}

static void testEmitNoSubscribers() {
    TEST("emit with no subscribers does not crash");

    EntityDef def("Entity");
    Entity e(1, EntitySide::Cell, def);

    e.emit("nonexistent");

    PASS();
}

static void testBroadcastEvent() {
    TEST("CellRuntime broadcasts event to all entities");

    auto rt = makeCellRuntime();
    EntityDef def("Entity");

    auto e1 = std::make_unique<Entity>(1, EntitySide::Cell, def);
    auto e2 = std::make_unique<Entity>(2, EntitySide::Cell, def);

    int count = 0;
    e1->subscribe("alert", [&count](Entity&, std::string_view, std::span<const std::byte>) { ++count; });
    e2->subscribe("alert", [&count](Entity&, std::string_view, std::span<const std::byte>) { ++count; });

    e1->activate();
    e2->activate();
    rt->addEntity(*e1, Vector3{0, 0, 0});
    rt->addEntity(*e2, Vector3{10, 0, 0});

    rt->broadcastEvent("alert");

    if (count == 2) PASS();
    else FAIL("expected count=2, got " + std::to_string(count));
}

static void testBroadcastEventInRange() {
    TEST("CellRuntime broadcasts event only to entities in range");

    auto rt = makeCellRuntime();
    EntityDef def("Entity");

    auto nearEntity = std::make_unique<Entity>(1, EntitySide::Cell, def);
    auto farEntity = std::make_unique<Entity>(2, EntitySide::Cell, def);

    bool nearReceived = false;
    bool farReceived = false;
    nearEntity->subscribe("ping", [&nearReceived](Entity&, std::string_view, std::span<const std::byte>) { nearReceived = true; });
    farEntity->subscribe("ping", [&farReceived](Entity&, std::string_view, std::span<const std::byte>) { farReceived = true; });

    nearEntity->activate();
    farEntity->activate();
    rt->addEntity(*nearEntity, Vector3{5, 0, 0});
    rt->addEntity(*farEntity, Vector3{200, 0, 0});

    rt->broadcastEventInRange("ping", Vector3{0, 0, 0}, 50.0f);

    if (nearReceived && !farReceived) PASS();
    else FAIL("near=" + std::string(nearReceived ? "T" : "F") + " far=" + std::string(farReceived ? "T" : "F"));
}

static void testCallbackReceivesSelfReference() {
    TEST("event callback receives correct entity reference");

    EntityDef def("Entity");
    Entity e(42, EntitySide::Cell, def);

    theseed::runtime::EntityId receivedId = 0;
    e.subscribe("self_test", [&receivedId](Entity& self, std::string_view, std::span<const std::byte>) {
        receivedId = self.id();
    });

    e.emit("self_test");

    if (receivedId == 42) PASS();
    else FAIL("expected id=42, got " + std::to_string(receivedId));
}

int main() {
    std::cout << "EntityEvent tests:\n";

    testSubscribeAndEmit();
    testEmitWithData();
    testUnsubscribe();
    testMultipleSubscribers();
    testEmitNoSubscribers();
    testBroadcastEvent();
    testBroadcastEventInRange();
    testCallbackReceivesSelfReference();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
