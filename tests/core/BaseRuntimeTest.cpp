#include "theseed/core/BaseRuntime.h"
#include "theseed/core/IEntityStore.h"
#include "theseed/runtime/Entity.h"
#include "theseed/runtime/EntityDef.h"
#include "theseed/runtime/RuntimeTransport.h"
#include "theseed/runtime/RuntimeTypes.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>

using theseed::core::BaseRuntime;
using theseed::core::InMemoryEntityStore;
using theseed::runtime::Entity;
using theseed::runtime::EntityDef;
using theseed::runtime::EntityId;
using theseed::runtime::EntitySide;
using theseed::runtime::EntityState;
using theseed::runtime::InMemoryRuntimeTransport;
using theseed::runtime::PropertyType;
using theseed::runtime::RuntimeInvocation;

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

static std::shared_ptr<EntityDef> makeAvatarDef() {
    auto def = std::make_shared<EntityDef>("Avatar");
    def->addProperty("level", PropertyType::Int32);
    def->addProperty("hp", PropertyType::Float32);
    def->addProperty("alive", PropertyType::Bool);
    def->addMethod("onDamage", theseed::runtime::MethodSide::Base);
    return def;
}

static BaseRuntime::EntityFactory makeFactory(std::shared_ptr<EntityDef> def) {
    return [def](EntityId id, EntitySide side) -> std::unique_ptr<Entity> {
        return std::make_unique<Entity>(id, side, *def);
    };
}

static std::unique_ptr<BaseRuntime> makeRuntime(
    std::shared_ptr<InMemoryRuntimeTransport> transport = nullptr,
    std::shared_ptr<InMemoryEntityStore> store = nullptr) {
    if (!transport) {
        transport = std::make_shared<InMemoryRuntimeTransport>();
    }
    if (!store) {
        store = std::make_shared<InMemoryEntityStore>();
    }
    return std::make_unique<BaseRuntime>(transport, store, 1);
}

static void testCreateEntity() {
    TEST("create entity with factory");

    auto rt = makeRuntime();
    auto def = makeAvatarDef();
    rt->registerEntityFactory("Avatar", makeFactory(def));

    auto* entity = rt->createEntity("Avatar");
    bool ok = entity != nullptr;
    ok = ok && entity->entityType() == "Avatar";
    ok = ok && entity->side() == EntitySide::Base;
    ok = ok && entity->state() == EntityState::Active;
    ok = ok && rt->entityCount() == 1;

    if (ok) PASS(); else FAIL("create entity failed");
}

static void testFindEntity() {
    TEST("find entity by id");

    auto rt = makeRuntime();
    auto def = makeAvatarDef();
    rt->registerEntityFactory("Avatar", makeFactory(def));

    auto* entity = rt->createEntity("Avatar");
    auto id = entity->id();

    auto* found = rt->findEntity(id);
    bool ok = found == entity;
    ok = ok && rt->findEntity(99999) == nullptr;

    if (ok) PASS(); else FAIL("find entity failed");
}

static void testDestroyEntity() {
    TEST("destroy entity");

    auto rt = makeRuntime();
    auto def = makeAvatarDef();
    rt->registerEntityFactory("Avatar", makeFactory(def));

    auto* entity = rt->createEntity("Avatar");
    auto id = entity->id();

    bool destroyed = rt->destroyEntity(id);
    bool ok = destroyed;
    ok = ok && rt->findEntity(id) == nullptr;
    ok = ok && rt->entityCount() == 0;
    ok = ok && !rt->destroyEntity(id);

    if (ok) PASS(); else FAIL("destroy entity failed");
}

static void testMultipleEntities() {
    TEST("create and manage multiple entities");

    auto rt = makeRuntime();
    auto def = makeAvatarDef();
    rt->registerEntityFactory("Avatar", makeFactory(def));

    auto* e1 = rt->createEntity("Avatar");
    auto* e2 = rt->createEntity("Avatar");
    auto* e3 = rt->createEntity("Avatar");

    bool ok = rt->entityCount() == 3;
    ok = ok && e1 != e2 && e2 != e3;
    ok = ok && e1->id() != e2->id() && e2->id() != e3->id();

    rt->destroyEntity(e2->id());
    ok = ok && rt->entityCount() == 2;
    ok = ok && rt->findEntity(e1->id()) != nullptr;
    ok = ok && rt->findEntity(e3->id()) != nullptr;

    if (ok) PASS(); else FAIL("multiple entities failed");
}

static void testSaveAndLoad() {
    TEST("save and load entity");

    auto store = std::make_shared<InMemoryEntityStore>();
    auto rt = makeRuntime(nullptr, store);
    auto def = makeAvatarDef();
    rt->registerEntityFactory("Avatar", makeFactory(def));

    auto* entity = rt->createEntity("Avatar");
    auto id = entity->id();

    entity->setProperty<std::int32_t>(0, 42);
    entity->setProperty<float>(1, 99.5f);
    entity->setProperty<bool>(2, true);

    bool saved = rt->saveEntity(id);
    rt->destroyEntity(id);

    auto* loaded = rt->loadEntity(id, "Avatar");
    bool ok = loaded != nullptr;
    ok = ok && loaded->id() == id;
    ok = ok && loaded->entityType() == "Avatar";
    ok = ok && loaded->getProperty<std::int32_t>(0) == 42;
    ok = ok && loaded->getProperty<float>(1) == 99.5f;
    ok = ok && loaded->getProperty<bool>(2) == true;

    if (ok) PASS(); else FAIL("save and load failed");
}

static void testAutoSave() {
    TEST("auto-save triggers on interval");

    auto store = std::make_shared<InMemoryEntityStore>();
    auto transport = std::make_shared<InMemoryRuntimeTransport>();
    auto rt = std::make_unique<BaseRuntime>(transport, store, 1);
    auto def = makeAvatarDef();
    rt->registerEntityFactory("Avatar", makeFactory(def));

    auto* entity = rt->createEntity("Avatar");
    auto id = entity->id();
    entity->setProperty<std::int32_t>(0, 100);

    rt->setAutoSaveInterval(std::chrono::milliseconds{100});

    using namespace std::chrono;
    theseed::runtime::TickContext ctx;
    ctx.tickIndex = 1;
    ctx.deltaTime = milliseconds{150};

    rt->tick(ctx);

    bool ok = store->exists(id);

    rt->destroyEntity(id);
    auto* loaded = rt->loadEntity(id, "Avatar");
    ok = ok && loaded != nullptr;
    ok = ok && loaded->getProperty<std::int32_t>(0) == 100;

    if (ok) PASS(); else FAIL("auto-save failed");
}

static void testSetCellEntityCall() {
    TEST("set cell entity call");

    auto rt = makeRuntime();
    auto def = makeAvatarDef();
    rt->registerEntityFactory("Avatar", makeFactory(def));

    auto* entity = rt->createEntity("Avatar");
    auto id = entity->id();

    bool ok = rt->setCellEntityCall(id, 42);
    ok = ok && entity->cellEntityCall() != nullptr;
    ok = ok && entity->cellEntityCall()->targetComponent() == 42;

    ok = ok && rt->clearCellEntityCall(id);
    ok = ok && entity->cellEntityCall() == nullptr;

    ok = ok && !rt->setCellEntityCall(99999, 1);

    if (ok) PASS(); else FAIL("cell entity call failed");
}

static void testDispatchInvocation() {
    TEST("dispatch method invocation");

    auto transport = std::make_shared<InMemoryRuntimeTransport>();
    auto rt = makeRuntime(transport);
    auto def = makeAvatarDef();
    rt->registerEntityFactory("Avatar", makeFactory(def));

    auto* entity = rt->createEntity("Avatar");

    std::int32_t receivedValue = 0;
    entity->bindMethodHandler("onDamage", [&receivedValue](Entity& e, std::span<const std::byte> payload) {
        static_cast<void>(e);
        if (payload.size() >= sizeof(std::int32_t)) {
            std::memcpy(&receivedValue, payload.data(), sizeof(std::int32_t));
        }
    });

    RuntimeInvocation inv;
    inv.entityId = entity->id();
    inv.targetComponent = 1;
    inv.entityType = "Avatar";
    inv.method = "onDamage";
    std::int32_t damage = 50;
    inv.payload.resize(sizeof(damage));
    std::memcpy(inv.payload.data(), &damage, sizeof(damage));

    transport->send(std::move(inv));
    rt->pumpInbound();

    bool ok = receivedValue == 50;

    if (ok) PASS(); else FAIL("dispatch invocation failed, got " + std::to_string(receivedValue));
}

static void testUnknownEntityType() {
    TEST("create unknown entity type returns null");

    auto rt = makeRuntime();
    auto* entity = rt->createEntity("NonExistent");

    if (entity == nullptr) PASS(); else FAIL("expected null for unknown type");
}

static void testLoadNonexistent() {
    TEST("load nonexistent entity returns null");

    auto rt = makeRuntime();
    auto def = makeAvatarDef();
    rt->registerEntityFactory("Avatar", makeFactory(def));

    auto* entity = rt->loadEntity(99999, "Avatar");

    if (entity == nullptr) PASS(); else FAIL("expected null for nonexistent entity");
}

static void testAutoSaveOnDestroy() {
    TEST("destroy entity auto-saves data");

    auto store = std::make_shared<InMemoryEntityStore>();
    auto rt = makeRuntime(nullptr, store);
    auto def = makeAvatarDef();
    rt->registerEntityFactory("Avatar", makeFactory(def));

    auto* entity = rt->createEntity("Avatar");
    auto id = entity->id();
    entity->setProperty<std::int32_t>(0, 77);
    entity->setProperty<float>(1, 55.5f);

    rt->destroyEntity(id);

    bool ok = rt->entityCount() == 0;
    auto* loaded = rt->loadEntity(id, "Avatar");
    ok = ok && loaded != nullptr;
    ok = ok && loaded->getProperty<std::int32_t>(0) == 77;
    ok = ok && std::abs(loaded->getProperty<float>(1) - 55.5f) < 0.01f;

    if (ok) PASS(); else FAIL("auto-save on destroy failed");
}

static void testFindEntitiesByType() {
    TEST("find entities by type");

    auto rt = makeRuntime();
    auto avatarDef = makeAvatarDef();
    auto npcDef = std::make_shared<EntityDef>("Npc");
    npcDef->addProperty("name", PropertyType::Int32);

    rt->registerEntityFactory("Avatar", makeFactory(avatarDef));
    rt->registerEntityFactory("Npc", makeFactory(npcDef));

    auto* a1 = rt->createEntity("Avatar");
    auto* a2 = rt->createEntity("Avatar");
    auto* n1 = rt->createEntity("Npc");

    auto avatars = rt->findEntitiesByType("Avatar");
    auto npcs = rt->findEntitiesByType("Npc");
    auto empty = rt->findEntitiesByType("Monster");

    bool ok = avatars.size() == 2;
    ok = ok && npcs.size() == 1;
    ok = ok && empty.empty();

    std::size_t foundA1 = 0, foundA2 = 0;
    for (auto* e : avatars) {
        if (e == a1) ++foundA1;
        if (e == a2) ++foundA2;
    }
    ok = ok && foundA1 == 1 && foundA2 == 1;
    ok = ok && npcs[0] == n1;

    if (ok) PASS(); else FAIL("avatars=" + std::to_string(avatars.size()));
}

static void testNameBasedPropertyAccess() {
    TEST("name-based property access");

    auto rt = makeRuntime();
    auto def = makeAvatarDef();
    rt->registerEntityFactory("Avatar", makeFactory(def));

    auto* entity = rt->createEntity("Avatar");

    bool setOk = entity->setProperty<std::int32_t>("level", 42);
    setOk = setOk && entity->setProperty<float>("hp", 99.5f);
    bool setBad = entity->setProperty<std::int32_t>("nonexistent", 1);

    auto* level = entity->findProperty<std::int32_t>("level");
    auto* hp = entity->findProperty<float>("hp");
    auto* bad = entity->findProperty<std::int32_t>("nonexistent");

    bool ok = setOk && !setBad;
    ok = ok && level != nullptr && *level == 42;
    ok = ok && hp != nullptr && std::abs(*hp - 99.5f) < 0.01f;
    ok = ok && bad == nullptr;

    if (ok) PASS(); else FAIL("name-based access failed");
}

static void testForEachEntity() {
    TEST("forEachEntity iterates all entities");

    auto rt = makeRuntime();
    auto avatarDef = makeAvatarDef();
    auto npcDef = std::make_shared<EntityDef>("Npc");
    npcDef->addProperty("hp", PropertyType::Int32);

    rt->registerEntityFactory("Avatar", makeFactory(avatarDef));
    rt->registerEntityFactory("Npc", makeFactory(npcDef));

    auto* a1 = rt->createEntity("Avatar");
    auto* a2 = rt->createEntity("Avatar");
    auto* n1 = rt->createEntity("Npc");

    std::vector<EntityId> visited;
    rt->forEachEntity([&visited](Entity& e) {
        visited.push_back(e.id());
    });

    bool ok = visited.size() == 3;
    std::size_t foundA1 = 0, foundA2 = 0, foundN1 = 0;
    for (auto id : visited) {
        if (id == a1->id()) ++foundA1;
        if (id == a2->id()) ++foundA2;
        if (id == n1->id()) ++foundN1;
    }
    ok = ok && foundA1 == 1 && foundA2 == 1 && foundN1 == 1;

    // Empty runtime
    auto emptyRt = makeRuntime();
    std::size_t emptyCount = 0;
    emptyRt->forEachEntity([&emptyCount](Entity&) { ++emptyCount; });
    ok = ok && emptyCount == 0;

    if (ok) PASS(); else FAIL("visited=" + std::to_string(visited.size()));
}

int main() {
    std::cout << "BaseRuntime tests:\n";

    testCreateEntity();
    testFindEntity();
    testDestroyEntity();
    testMultipleEntities();
    testSaveAndLoad();
    testAutoSave();
    testSetCellEntityCall();
    testDispatchInvocation();
    testUnknownEntityType();
    testLoadNonexistent();
    testAutoSaveOnDestroy();
    testFindEntitiesByType();
    testNameBasedPropertyAccess();
    testForEachEntity();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
