#include "theseed/runtime/CellRuntime.h"
#include "theseed/runtime/Entity.h"
#include "theseed/runtime/EntityDef.h"
#include "theseed/runtime/RuntimeTransport.h"
#include "theseed/runtime/SpaceRuntime.h"
#include "theseed/core/BaseRuntime.h"
#include "theseed/core/IEntityStore.h"

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using theseed::core::BaseRuntime;
using theseed::core::InMemoryEntityStore;
using theseed::runtime::CellRuntime;
using theseed::runtime::Entity;
using theseed::runtime::EntityDef;
using theseed::runtime::EntityId;
using theseed::runtime::EntitySide;
using theseed::runtime::InMemoryRuntimeTransport;
using theseed::runtime::SingleCellTopology;
using theseed::runtime::Space;
using theseed::runtime::SpaceId;
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

static std::shared_ptr<EntityDef> makeDef() {
    auto def = std::make_shared<EntityDef>("Avatar");
    def->addProperty("hp", theseed::runtime::PropertyType::Int32, 4);
    return def;
}

static std::unique_ptr<BaseRuntime> makeBaseRuntime() {
    auto transport = std::make_shared<InMemoryRuntimeTransport>();
    auto store = std::make_shared<InMemoryEntityStore>();
    return std::make_unique<BaseRuntime>(transport, store, 1);
}

static std::unique_ptr<CellRuntime> makeCellRuntime() {
    auto transport = std::make_shared<InMemoryRuntimeTransport>();
    auto space = std::make_unique<Space>(1, "default", std::make_unique<SingleCellTopology>(1));
    space->initialize();
    auto spaceRuntime = std::make_unique<SpaceRuntime>(std::move(space));
    return std::make_unique<CellRuntime>(std::move(spaceRuntime), transport, 2);
}

static void testOnCreate() {
    TEST("createEntity triggers onCreate callback");

    auto rt = makeBaseRuntime();
    auto def = makeDef();
    bool triggered = false;

    rt->registerEntityFactory("Avatar",
        [def, &triggered](EntityId id, auto side) -> std::unique_ptr<Entity> {
            auto e = std::make_unique<Entity>(id, side, *def);
            e->setOnCreate([&triggered](Entity& entity) {
                triggered = true;
            });
            return e;
        });

    auto* entity = rt->createEntity("Avatar");
    if (entity && triggered) PASS(); else FAIL("callback not triggered");
}

static void testOnDestroy() {
    TEST("destroyEntity triggers onDestroy callback");

    auto rt = makeBaseRuntime();
    auto def = makeDef();
    bool triggered = false;
    EntityId destroyedId = 0;

    rt->registerEntityFactory("Avatar",
        [def, &triggered, &destroyedId](EntityId id, auto side) -> std::unique_ptr<Entity> {
            auto e = std::make_unique<Entity>(id, side, *def);
            e->setOnDestroy([&triggered, &destroyedId](Entity& entity) {
                triggered = true;
                destroyedId = entity.id();
            });
            return e;
        });

    auto* entity = rt->createEntity("Avatar");
    EntityId id = entity->id();
    rt->destroyEntity(id);

    if (triggered && destroyedId == id) PASS();
    else FAIL("callback not triggered or wrong id");
}

static void testOnEnterSpace() {
    TEST("addEntity triggers onEnterSpace with correct SpaceId");

    auto rt = makeCellRuntime();
    auto def = makeDef();
    bool triggered = false;
    SpaceId enteredSpace = 0;

    auto entity = std::make_unique<Entity>(100, EntitySide::Cell, *def);
    entity->setOnEnterSpace([&triggered, &enteredSpace](Entity& e, SpaceId sid) {
        triggered = true;
        enteredSpace = sid;
    });

    rt->addEntity(*entity, Vector3{0, 0, 0});

    if (triggered && enteredSpace == 1) PASS();
    else FAIL("callback not triggered or wrong space id=" + std::to_string(enteredSpace));
}

static void testOnLeaveSpace() {
    TEST("removeEntity triggers onLeaveSpace with correct SpaceId");

    auto rt = makeCellRuntime();
    auto def = makeDef();
    bool triggered = false;
    SpaceId leftSpace = 0;

    auto entity = std::make_unique<Entity>(200, EntitySide::Cell, *def);
    entity->setOnLeaveSpace([&triggered, &leftSpace](Entity& e, SpaceId sid) {
        triggered = true;
        leftSpace = sid;
    });

    rt->addEntity(*entity, Vector3{0, 0, 0});
    rt->removeEntity(200);

    if (triggered && leftSpace == 1) PASS();
    else FAIL("callback not triggered or wrong space id");
}

static void testNoCallbackIfNotSet() {
    TEST("no crash when callbacks are not set");

    auto rt = makeCellRuntime();
    auto def = makeDef();

    auto entity = std::make_unique<Entity>(300, EntitySide::Cell, *def);
    rt->addEntity(*entity, Vector3{0, 0, 0});
    rt->removeEntity(300);

    PASS();
}

static void testCallbackCanAccessEntity() {
    TEST("onCreate callback can read/write entity properties");

    auto rt = makeBaseRuntime();
    auto def = makeDef();
    int32_t readHp = 0;

    rt->registerEntityFactory("Avatar",
        [def, &readHp](EntityId id, auto side) -> std::unique_ptr<Entity> {
            auto e = std::make_unique<Entity>(id, side, *def);
            e->setOnCreate([&readHp](Entity& entity) {
                entity.setProperty<int32_t>(0, 42);
                readHp = entity.getProperty<int32_t>(0);
            });
            return e;
        });

    auto* entity = rt->createEntity("Avatar");
    if (entity && readHp == 42) PASS();
    else FAIL("property read/write failed, hp=" + std::to_string(readHp));
}

static void testLoadEntityTriggersCreate() {
    TEST("loadEntity triggers onCreate callback");

    auto transport = std::make_shared<InMemoryRuntimeTransport>();
    auto store = std::make_shared<InMemoryEntityStore>();
    auto def = makeDef();
    bool triggered = false;

    BaseRuntime rt(transport, store, 1);
    rt.registerEntityFactory("Avatar",
        [def, &triggered](EntityId id, auto side) -> std::unique_ptr<Entity> {
            auto e = std::make_unique<Entity>(id, side, *def);
            e->setOnCreate([&triggered](Entity& entity) {
                triggered = true;
            });
            return e;
        });

    theseed::core::EntityData data;
    data.id = 10;
    data.entityType = "Avatar";
    theseed::core::PropertyData prop;
    prop.id = 0;
    prop.name = "hp";
    prop.type = theseed::core::DataType::Int32;
    int32_t hp = 100;
    prop.rawValue.resize(sizeof(hp));
    std::memcpy(prop.rawValue.data(), &hp, sizeof(hp));
    data.properties.push_back(std::move(prop));
    store->save(10, data);

    auto* entity = rt.loadEntity(10, "Avatar");
    if (entity && triggered) PASS();
    else FAIL("loadEntity did not trigger onCreate");
}

static void testDestroyCallbackBeforeCleanup() {
    TEST("onDestroy called before entity cleanup, can access entity state");

    auto rt = makeBaseRuntime();
    auto def = makeDef();
    int32_t readHp = 0;
    bool stateWasDestroying = false;

    rt->registerEntityFactory("Avatar",
        [def, &readHp, &stateWasDestroying](EntityId id, auto side) -> std::unique_ptr<Entity> {
            auto e = std::make_unique<Entity>(id, side, *def);
            e->setOnCreate([](Entity& entity) {
                entity.setProperty<int32_t>(0, 99);
            });
            e->setOnDestroy([&readHp, &stateWasDestroying](Entity& entity) {
                readHp = entity.getProperty<int32_t>(0);
                stateWasDestroying = (entity.state() == theseed::runtime::EntityState::Destroying);
            });
            return e;
        });

    auto* entity = rt->createEntity("Avatar");
    rt->destroyEntity(entity->id());

    if (readHp == 99 && stateWasDestroying) PASS();
    else FAIL("entity state not accessible, hp=" + std::to_string(readHp));
}

int main() {
    std::cout << "EntityLifecycle tests:\n";

    testOnCreate();
    testOnDestroy();
    testOnEnterSpace();
    testOnLeaveSpace();
    testNoCallbackIfNotSet();
    testCallbackCanAccessEntity();
    testLoadEntityTriggersCreate();
    testDestroyCallbackBeforeCleanup();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
