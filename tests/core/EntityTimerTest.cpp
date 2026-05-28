#include "theseed/core/BaseRuntime.h"
#include "theseed/core/IEntityStore.h"
#include "theseed/runtime/Entity.h"
#include "theseed/runtime/EntityDef.h"
#include "theseed/runtime/RuntimeTransport.h"
#include "theseed/runtime/TickScheduler.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>

using theseed::core::BaseRuntime;
using theseed::core::InMemoryEntityStore;
using theseed::core::TimerHandle;
using theseed::runtime::Entity;
using theseed::runtime::EntityDef;
using theseed::runtime::EntityId;
using theseed::runtime::EntitySide;
using theseed::runtime::InMemoryRuntimeTransport;
using theseed::runtime::PropertyType;
using theseed::runtime::TickScheduler;

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
    def->addProperty("level", PropertyType::Int32);
    return def;
}

static std::unique_ptr<BaseRuntime> makeRuntime() {
    auto transport = std::make_shared<InMemoryRuntimeTransport>();
    auto store = std::make_shared<InMemoryEntityStore>();
    return std::make_unique<BaseRuntime>(transport, store, 1);
}

static void testAddTimer() {
    TEST("add timer fires after delay");

    auto rt = makeRuntime();
    bool fired = false;
    rt->addTimer(std::chrono::milliseconds{100}, [&fired]() { fired = true; });

    theseed::runtime::TickContext ctx;
    ctx.deltaTime = std::chrono::milliseconds{150};
    rt->tick(ctx);

    if (fired) PASS(); else FAIL("timer did not fire");
}

static void testAddEntityTimer() {
    TEST("add entity timer fires");

    auto rt = makeRuntime();
    auto def = makeDef();
    rt->registerEntityFactory("Avatar", [def](EntityId id, EntitySide side) {
        return std::make_unique<Entity>(id, side, *def);
    });

    auto* entity = rt->createEntity("Avatar");
    auto id = entity->id();

    bool fired = false;
    rt->addEntityTimer(id, std::chrono::milliseconds{50}, [&fired]() { fired = true; });

    theseed::runtime::TickContext ctx;
    ctx.deltaTime = std::chrono::milliseconds{100};
    rt->tick(ctx);

    if (fired) PASS(); else FAIL("entity timer did not fire");
}

static void testPeriodicTimer() {
    TEST("periodic entity timer fires multiple times");

    auto rt = makeRuntime();
    auto def = makeDef();
    rt->registerEntityFactory("Avatar", [def](EntityId id, EntitySide side) {
        return std::make_unique<Entity>(id, side, *def);
    });

    auto* entity = rt->createEntity("Avatar");
    auto id = entity->id();

    int count = 0;
    rt->addEntityPeriodicTimer(id, std::chrono::milliseconds{50}, [&count]() { ++count; });

    theseed::runtime::TickContext ctx;
    ctx.deltaTime = std::chrono::milliseconds{120};
    rt->tick(ctx);

    bool ok = count >= 2;

    if (ok) PASS(); else FAIL("periodic timer count=" + std::to_string(count));
}

static void testCancelTimer() {
    TEST("cancel timer prevents callback");

    auto rt = makeRuntime();
    bool fired = false;
    auto handle = rt->addTimer(std::chrono::milliseconds{100}, [&fired]() { fired = true; });

    rt->cancelTimer(handle);

    theseed::runtime::TickContext ctx;
    ctx.deltaTime = std::chrono::milliseconds{200};
    rt->tick(ctx);

    if (!fired) PASS(); else FAIL("cancelled timer fired");
}

static void testDestroyEntityCancelsTimers() {
    TEST("destroy entity cancels its timers");

    auto rt = makeRuntime();
    auto def = makeDef();
    rt->registerEntityFactory("Avatar", [def](EntityId id, EntitySide side) {
        return std::make_unique<Entity>(id, side, *def);
    });

    auto* entity = rt->createEntity("Avatar");
    auto id = entity->id();

    bool fired = false;
    rt->addEntityTimer(id, std::chrono::milliseconds{50}, [&fired]() { fired = true; });

    rt->destroyEntity(id);

    theseed::runtime::TickContext ctx;
    ctx.deltaTime = std::chrono::milliseconds{100};
    rt->tick(ctx);

    if (!fired) PASS(); else FAIL("entity timer fired after destroy");
}

static void testMultipleEntityTimers() {
    TEST("multiple entities have independent timers");

    auto rt = makeRuntime();
    auto def = makeDef();
    rt->registerEntityFactory("Avatar", [def](EntityId id, EntitySide side) {
        return std::make_unique<Entity>(id, side, *def);
    });

    auto* e1 = rt->createEntity("Avatar");
    auto* e2 = rt->createEntity("Avatar");
    auto id1 = e1->id();
    auto id2 = e2->id();

    int count1 = 0;
    int count2 = 0;
    rt->addEntityTimer(id1, std::chrono::milliseconds{50}, [&count1]() { ++count1; });
    rt->addEntityTimer(id2, std::chrono::milliseconds{50}, [&count2]() { ++count2; });

    rt->destroyEntity(id1);

    theseed::runtime::TickContext ctx;
    ctx.deltaTime = std::chrono::milliseconds{100};
    rt->tick(ctx);

    bool ok = count1 == 0 && count2 == 1;

    if (ok) PASS(); else FAIL("independent timers failed, c1=" + std::to_string(count1) + " c2=" + std::to_string(count2));
}

// --- Entity-level timer tests (via Entity::addTimer/addPeriodicTimer) ---

static void testEntityLevelOneShotTimer() {
    TEST("entity.addTimer one-shot fires via TickScheduler");

    auto transport = std::make_shared<InMemoryRuntimeTransport>();
    auto store = std::make_shared<InMemoryEntityStore>();
    auto runtime = std::make_unique<BaseRuntime>(transport, store, 1);
    auto def = makeDef();
    runtime->registerEntityFactory("Avatar", [def](EntityId id, EntitySide side) {
        return std::make_unique<Entity>(id, side, *def);
    });

    TickScheduler scheduler(std::chrono::milliseconds{10});
    runtime->attach(scheduler);

    auto* entity = runtime->createEntity("Avatar");
    int fireCount = 0;
    entity->addTimer(std::chrono::milliseconds{30},
        [&](Entity& e) {
            ++fireCount;
            static_cast<void>(e);
        });

    for (int i = 0; i < 10; ++i) {
        scheduler.runOnce();
    }

    bool ok = fireCount == 1;

    runtime->detach(scheduler);
    if (ok) PASS();
    else FAIL("fireCount=" + std::to_string(fireCount));
}

static void testEntityLevelPeriodicTimer() {
    TEST("entity.addPeriodicTimer fires multiple times via TickScheduler");

    auto transport = std::make_shared<InMemoryRuntimeTransport>();
    auto store = std::make_shared<InMemoryEntityStore>();
    auto runtime = std::make_unique<BaseRuntime>(transport, store, 1);
    auto def = makeDef();
    runtime->registerEntityFactory("Avatar", [def](EntityId id, EntitySide side) {
        return std::make_unique<Entity>(id, side, *def);
    });

    TickScheduler scheduler(std::chrono::milliseconds{10});
    runtime->attach(scheduler);

    auto* entity = runtime->createEntity("Avatar");
    int fireCount = 0;
    entity->addPeriodicTimer(std::chrono::milliseconds{20},
        [&](Entity& e) {
            ++fireCount;
            static_cast<void>(e);
        });

    for (int i = 0; i < 20; ++i) {
        scheduler.runOnce();
    }

    bool ok = fireCount >= 5;

    runtime->detach(scheduler);
    if (ok) PASS();
    else FAIL("fireCount=" + std::to_string(fireCount) + " expected >= 5");
}

static void testEntityTimerCancelledOnDestroy() {
    TEST("entity timer cancelled when entity destroyed via TickScheduler");

    auto transport = std::make_shared<InMemoryRuntimeTransport>();
    auto store = std::make_shared<InMemoryEntityStore>();
    auto runtime = std::make_unique<BaseRuntime>(transport, store, 1);
    auto def = makeDef();
    runtime->registerEntityFactory("Avatar", [def](EntityId id, EntitySide side) {
        return std::make_unique<Entity>(id, side, *def);
    });

    TickScheduler scheduler(std::chrono::milliseconds{10});
    runtime->attach(scheduler);

    auto* entity = runtime->createEntity("Avatar");
    auto id = entity->id();

    int fireCount = 0;
    entity->addPeriodicTimer(std::chrono::milliseconds{20},
        [&](Entity&) { ++fireCount; });

    // Tick a bit to verify timer works
    for (int i = 0; i < 5; ++i) {
        scheduler.runOnce();
    }
    int before = fireCount;
    bool ok = before > 0;

    runtime->destroyEntity(id);

    // Tick more - timer should not fire
    for (int i = 0; i < 10; ++i) {
        scheduler.runOnce();
    }
    ok = ok && fireCount == before;

    runtime->detach(scheduler);
    if (ok) PASS();
    else FAIL("before=" + std::to_string(before) + " after=" + std::to_string(fireCount));
}

static void testEntityTimerCallbackReceivesEntity() {
    TEST("entity timer callback receives correct entity reference");

    auto transport = std::make_shared<InMemoryRuntimeTransport>();
    auto store = std::make_shared<InMemoryEntityStore>();
    auto runtime = std::make_unique<BaseRuntime>(transport, store, 1);
    auto def = makeDef();
    runtime->registerEntityFactory("Avatar", [def](EntityId id, EntitySide side) {
        return std::make_unique<Entity>(id, side, *def);
    });

    TickScheduler scheduler(std::chrono::milliseconds{10});
    runtime->attach(scheduler);

    auto* entity = runtime->createEntity("Avatar");
    auto expectedId = entity->id();

    EntityId capturedId = 0;
    entity->addTimer(std::chrono::milliseconds{20},
        [&](Entity& e) { capturedId = e.id(); });

    for (int i = 0; i < 5; ++i) {
        scheduler.runOnce();
    }

    bool ok = capturedId == expectedId;

    runtime->detach(scheduler);
    if (ok) PASS();
    else FAIL("captured=" + std::to_string(capturedId) + " expected=" + std::to_string(expectedId));
}

static void testEntityTimerModifiesProperty() {
    TEST("entity timer modifies property");

    auto transport = std::make_shared<InMemoryRuntimeTransport>();
    auto store = std::make_shared<InMemoryEntityStore>();
    auto runtime = std::make_unique<BaseRuntime>(transport, store, 1);
    auto def = makeDef();
    runtime->registerEntityFactory("Avatar", [def](EntityId id, EntitySide side) {
        return std::make_unique<Entity>(id, side, *def);
    });

    TickScheduler scheduler(std::chrono::milliseconds{10});
    runtime->attach(scheduler);

    auto* entity = runtime->createEntity("Avatar");
    entity->setProperty<std::int32_t>(0, 1);

    entity->addTimer(std::chrono::milliseconds{20},
        [](Entity& e) { e.setProperty<std::int32_t>(0, 42); });

    for (int i = 0; i < 5; ++i) {
        scheduler.runOnce();
    }

    bool ok = entity->getProperty<std::int32_t>(0) == 42;

    runtime->detach(scheduler);
    if (ok) PASS();
    else FAIL("level=" + std::to_string(entity->getProperty<std::int32_t>(0)));
}

int main() {
    std::cout << "EntityTimer tests:\n";

    testAddTimer();
    testAddEntityTimer();
    testPeriodicTimer();
    testCancelTimer();
    testDestroyEntityCancelsTimers();
    testMultipleEntityTimers();

    // Entity-level timer tests
    testEntityLevelOneShotTimer();
    testEntityLevelPeriodicTimer();
    testEntityTimerCancelledOnDestroy();
    testEntityTimerCallbackReceivesEntity();
    testEntityTimerModifiesProperty();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
