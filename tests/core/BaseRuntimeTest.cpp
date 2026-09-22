#include "theseed/core/BaseRuntime.h"
#include "theseed/core/IEntityStore.h"
#include "theseed/runtime/Entity.h"
#include "theseed/runtime/EntityDef.h"
#include "theseed/runtime/PropertyReplication.h"
#include "theseed/runtime/RuntimeTransport.h"
#include "theseed/runtime/RuntimeTypes.h"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

using theseed::core::BaseRuntime;
using theseed::core::InMemoryEntityStore;
using theseed::runtime::Entity;
using theseed::runtime::EntityDef;
using theseed::runtime::EntityId;
using theseed::runtime::EntitySide;
using theseed::runtime::EntityState;
using theseed::runtime::InMemoryRuntimeTransport;
using theseed::runtime::PropertyType;
using theseed::runtime::PropertyReplication;
using theseed::runtime::RuntimeInvocation;
using theseed::runtime::SendResult;
using theseed::runtime::TickScheduler;
using theseed::runtime::TransportStats;

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

class RecordingTransport final : public theseed::runtime::IRuntimeTransport {
public:
    SendResult send(RuntimeInvocation invocation) override {
        sent.push_back(std::move(invocation));
        ++stats_.messagesSent;
        return SendResult::Accepted;
    }

    std::size_t receive(theseed::runtime::ComponentId,
                        RuntimeInvocation*,
                        std::size_t) override {
        return 0;
    }

    std::size_t pendingCount() const override {
        return sent.size();
    }

    void flush() override {
        ++flushCount;
    }

    TransportStats stats() const override {
        auto s = stats_;
        s.outboundQueueDepth = sent.size();
        return s;
    }

    std::vector<RuntimeInvocation> sent;
    int flushCount = 0;

private:
    TransportStats stats_{};
};

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

    if (!rt->saveEntity(id)) FAIL("save failed");
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

static void testAutoSaveDoesNotClearRuntimeDirty() {
    TEST("auto-save keeps pending runtime property sync dirty");

    auto store = std::make_shared<InMemoryEntityStore>();
    auto transport = std::make_shared<RecordingTransport>();
    BaseRuntime rt(transport, store, 1);
    auto def = makeAvatarDef();
    rt.registerEntityFactory("Avatar", makeFactory(def));

    auto* entity = rt.createEntity("Avatar");
    auto id = entity->id();
    entity->setProperty<std::int32_t>(0, 123);

    rt.setAutoSaveInterval(std::chrono::milliseconds{100});

    theseed::runtime::TickContext ctx;
    ctx.tickIndex = 1;
    ctx.deltaTime = std::chrono::milliseconds{150};
    rt.tick(ctx);

    bool ok = store->exists(id);
    ok = ok && transport->sent.empty();
    ok = ok && entity->isPropertyDirty(0);

    rt.setCellEntityCall(id, 2);
    ctx.tickIndex = 2;
    ctx.deltaTime = std::chrono::milliseconds{1};
    rt.tick(ctx);

    ok = ok && transport->sent.size() == 1;
    if (ok) {
        ok = ok && transport->sent[0].method == "property.syncToCell";
        auto deltas = PropertyReplication::decodeDelta(transport->sent[0].payload);
        ok = ok && deltas.size() == 1;
        ok = ok && deltas[0].propertyId == 0;
        std::int32_t value = 0;
        std::memcpy(&value, deltas[0].value.data(), sizeof(value));
        ok = ok && value == 123;
    }

    if (ok) PASS(); else FAIL("runtime dirty was consumed by autosave");
}

static void testCellDeltaMarksPersistenceWithoutRuntimeEcho() {
    TEST("cell delta marks persistence without base->cell echo");

    auto store = std::make_shared<InMemoryEntityStore>();
    auto transport = std::make_shared<RecordingTransport>();
    BaseRuntime rt(transport, store, 1);
    auto def = makeAvatarDef();
    rt.registerEntityFactory("Avatar", makeFactory(def));

    auto* entity = rt.createEntity("Avatar");
    auto id = entity->id();
    rt.setCellEntityCall(id, 2);
    entity->clearDirtyFlags();

    std::int32_t level = 44;
    theseed::runtime::PropertyDelta delta;
    delta.propertyId = 0;
    delta.value.resize(sizeof(level));
    std::memcpy(delta.value.data(), &level, sizeof(level));
    auto encoded = PropertyReplication::encodeDelta(std::span<const theseed::runtime::PropertyDelta>(&delta, 1));

    RuntimeInvocation invocation;
    invocation.entityId = id;
    invocation.targetComponent = 1;
    invocation.entityType = "Avatar";
    invocation.method = "property.syncToBase";
    invocation.payload = std::move(encoded);

    bool ok = rt.dispatchInvocation(invocation);
    ok = ok && entity->getProperty<std::int32_t>(0) == 44;
    ok = ok && !entity->isPropertyDirty(0);
    ok = ok && entity->propertyBlock().persistenceDirtyMask().isDirty(0);

    rt.setAutoSaveInterval(std::chrono::milliseconds{100});
    theseed::runtime::TickContext ctx;
    ctx.tickIndex = 1;
    ctx.deltaTime = std::chrono::milliseconds{150};
    rt.tick(ctx);

    ok = ok && transport->sent.empty();

    theseed::core::EntityData stored;
    ok = ok && store->load(id, "Avatar", stored);
    if (ok) {
        bool foundLevel = false;
        for (const auto& prop : stored.properties) {
            if (prop.id != 0 || prop.rawValue.size() != sizeof(level)) continue;
            std::int32_t storedLevel = 0;
            std::memcpy(&storedLevel, prop.rawValue.data(), sizeof(storedLevel));
            foundLevel = true;
            ok = ok && storedLevel == 44;
        }
        ok = ok && foundLevel;
    }

    if (ok) PASS(); else FAIL("cell delta persistence/runtime dirty separation failed");
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

static void testSchedulerPhasesSyncBeforeFlush() {
    TEST("scheduler phases: sync build stages before flush sends");

    class SyncProbe final : public theseed::runtime::ITickable {
    public:
        explicit SyncProbe(std::shared_ptr<RecordingTransport> transport)
            : transport_(std::move(transport)) {}

        void tick(theseed::runtime::TickContext&) override {
            sentDuringSync = transport_->sent.size();
        }

        std::size_t sentDuringSync = 0;

    private:
        std::shared_ptr<RecordingTransport> transport_;
    };

    auto transport = std::make_shared<RecordingTransport>();
    auto store = std::make_shared<InMemoryEntityStore>();
    BaseRuntime rt(transport, store, 1);
    auto def = makeAvatarDef();
    rt.registerEntityFactory("Avatar", makeFactory(def));

    auto* entity = rt.createEntity("Avatar");
    rt.setCellEntityCall(entity->id(), 2);
    entity->clearDirtyFlags();
    entity->setProperty<std::int32_t>(0, 7);

    TickScheduler scheduler(std::chrono::milliseconds{0});
    rt.attach(scheduler);
    SyncProbe probe(transport);
    scheduler.registerTickable(theseed::runtime::TickPhase::SyncBuild, probe);

    scheduler.runOnce();

    bool ok = transport->sent.size() == 1;
    ok = ok && probe.sentDuringSync == 0;
    ok = ok && transport->flushCount == 1;
    ok = ok && transport->sent[0].method == "property.syncToCell";
    ok = ok && !entity->isPropertyDirty(0);

    static_cast<void>(scheduler.unregisterTickable(theseed::runtime::TickPhase::SyncBuild, probe));
    rt.detach(scheduler);

    if (ok) PASS(); else FAIL("sent=" + std::to_string(transport->sent.size())
                              + " syncSent=" + std::to_string(probe.sentDuringSync)
                              + " flush=" + std::to_string(transport->flushCount));
}

// 只拒绝 save 的 store：验证 saveEntity 失败分支
class SaveFailingStore final : public theseed::core::IEntityStore {
public:
    explicit SaveFailingStore(std::shared_ptr<InMemoryEntityStore> backing)
        : backing_(std::move(backing)) {}

    bool load(EntityId id, const std::string& entityType, theseed::core::EntityData& out) override {
        return backing_->load(id, entityType, out);
    }
    bool save(EntityId, const theseed::core::EntityData&) override { return false; }
    bool remove(EntityId id) override { return backing_->remove(id); }
    EntityId allocId() override { return backing_->allocId(); }
    std::vector<EntityId> listIdsByType(const std::string& entityType) override {
        return backing_->listIdsByType(entityType);
    }
    std::vector<std::string> listEntityTypes() override { return backing_->listEntityTypes(); }

private:
    std::shared_ptr<InMemoryEntityStore> backing_;
};

static void testConstructorValidation() {
    TEST("constructor rejects invalid arguments");

    auto transport = std::make_shared<InMemoryRuntimeTransport>();
    auto store = std::make_shared<InMemoryEntityStore>();
    bool threwTransport = false;
    bool threwStore = false;
    bool threwComponent = false;
    try { BaseRuntime(nullptr, store, 1); } catch (const std::invalid_argument&) { threwTransport = true; }
    try { BaseRuntime(transport, nullptr, 1); } catch (const std::invalid_argument&) { threwStore = true; }
    try { BaseRuntime(transport, store, 0); } catch (const std::invalid_argument&) { threwComponent = true; }

    bool ok = threwTransport && threwStore && threwComponent;
    if (ok) PASS(); else FAIL("constructor validation");
}

static void testRegisterFactoryValidation() {
    TEST("register factory rejects empty type / null factory");

    auto rt = makeRuntime();
    bool ok = !rt->registerEntityFactory("", makeFactory(makeAvatarDef()));
    ok = ok && !rt->registerEntityFactory("Avatar", nullptr);

    if (ok) PASS(); else FAIL("register factory validation");
}

static void testCreateEntityFactoryReturnsNull() {
    TEST("create entity with null-returning factory");

    auto rt = makeRuntime();
    rt->registerEntityFactory("Broken", [](EntityId, EntitySide) -> std::unique_ptr<Entity> {
        return nullptr;
    });

    bool ok = rt->createEntity("Broken") == nullptr;
    ok = ok && rt->createEntity("NeverRegistered") == nullptr;

    if (ok) PASS(); else FAIL("null factory not handled");
}

static void testLoadEntityBranches() {
    TEST("load entity branches and restored timers");

    auto transport = std::make_shared<InMemoryRuntimeTransport>();
    auto store = std::make_shared<InMemoryEntityStore>();
    auto rt = std::make_unique<BaseRuntime>(transport, store, 1);
    auto def = makeAvatarDef();
    rt->registerEntityFactory("Avatar", makeFactory(def));
    rt->registerEntityFactory("Broken", [](EntityId, EntitySide) -> std::unique_ptr<Entity> {
        return nullptr;
    });

    // factory 未注册的类型
    bool ok = rt->loadEntity(1, "NeverRegistered") == nullptr;

    // factory 存在但返回 null（store 里需有存档才走到 factory）
    theseed::core::EntityData brokenData;
    brokenData.id = 501;
    brokenData.entityType = "Broken";
    store->save(501, brokenData);
    ok = ok && rt->loadEntity(501, "Broken") == nullptr;

    // 正常恢复：实体定时器经注入的 schedule fn 注册，tick 触发
    auto created = rt->createEntity("Avatar");
    const auto id = created->id();
    ok = ok && rt->saveEntity(id);
    ok = ok && rt->destroyEntity(id);
    auto* restored = rt->loadEntity(id, "Avatar");
    ok = ok && restored != nullptr;

    int fired = 0;
    int periodicFired = 0;
    restored->addTimer(std::chrono::milliseconds{0}, [&](Entity&) { fired += 1; });
    restored->addPeriodicTimer(std::chrono::milliseconds{0}, [&](Entity&) { periodicFired += 1; });
    theseed::runtime::TickContext ctx;
    ctx.deltaTime = std::chrono::milliseconds{50};
    rt->tick(ctx);
    ok = ok && fired == 1 && periodicFired >= 1;

    if (ok) PASS(); else FAIL("load entity branches");
}

static void testSaveEntityBranches() {
    TEST("save entity failure branches");

    auto rt = makeRuntime();
    auto def = makeAvatarDef();
    rt->registerEntityFactory("Avatar", makeFactory(def));

    bool ok = !rt->clearCellEntityCall(999);
    ok = ok && !rt->saveEntity(999);

    auto failing = std::make_shared<InMemoryEntityStore>();
    auto rtFail = std::make_unique<BaseRuntime>(
        std::make_shared<InMemoryRuntimeTransport>(),
        std::make_shared<SaveFailingStore>(failing), 1);
    rtFail->registerEntityFactory("Avatar", makeFactory(def));
    auto* entity = rtFail->createEntity("Avatar");
    ok = ok && !rtFail->saveEntity(entity->id());

    if (ok) PASS(); else FAIL("save entity branches");
}

static void testDispatchBranches() {
    TEST("dispatch invocation target/entity guards");

    auto rt = makeRuntime();
    auto def = makeAvatarDef();
    rt->registerEntityFactory("Avatar", makeFactory(def));

    RuntimeInvocation wrongTarget;
    wrongTarget.targetComponent = 999;
    wrongTarget.method = "entity.cellReady";

    RuntimeInvocation unknownEntity;
    unknownEntity.targetComponent = 1;
    unknownEntity.entityId = 12345;
    unknownEntity.method = "onDamage";

    bool ok = !rt->dispatchInvocation(wrongTarget);
    ok = ok && !rt->dispatchInvocation(unknownEntity);

    if (ok) PASS(); else FAIL("dispatch guards");
}

static void testPropertySyncFromCellMalformed() {
    TEST("property sync from cell rejects malformed payload");

    auto rt = makeRuntime();
    auto def = makeAvatarDef();
    rt->registerEntityFactory("Avatar", makeFactory(def));
    auto* entity = rt->createEntity("Avatar");

    RuntimeInvocation inv;
    inv.targetComponent = 1;
    inv.entityId = entity->id();
    inv.method = "property.syncToBase";
    inv.payload = {std::byte{0x01}, std::byte{0x00}, std::byte{0x00}};  // 截断的 count

    RuntimeInvocation empty;
    empty.targetComponent = 1;
    empty.method = "property.syncToBase";

    bool ok = !rt->dispatchInvocation(inv);
    ok = ok && !rt->dispatchInvocation(empty);

    if (ok) PASS(); else FAIL("malformed property sync");
}

static void testHandleSpaceChanged() {
    TEST("space changed dispatch");

    auto rt = makeRuntime();
    auto def = makeAvatarDef();
    rt->registerEntityFactory("Avatar", makeFactory(def));

    theseed::runtime::EntityId seenEntity = 0;
    theseed::runtime::SpaceId seenSpace = 0;
    rt->setOnSpaceChange([&](theseed::runtime::EntityId id, theseed::runtime::SpaceId space,
                             const theseed::runtime::Vector3&) {
        seenEntity = id;
        seenSpace = space;
    });

    const std::uint64_t entityId = 77;
    const std::uint64_t spaceId = 300;
    const float x = 1.5F;
    std::vector<std::byte> payload;
    payload.resize(sizeof(entityId) + sizeof(spaceId) + sizeof(float) * 3);
    auto* p = payload.data();
    std::memcpy(p, &entityId, sizeof(entityId)); p += sizeof(entityId);
    std::memcpy(p, &spaceId, sizeof(spaceId)); p += sizeof(spaceId);
    std::memcpy(p, &x, sizeof(float));

    RuntimeInvocation inv;
    inv.targetComponent = 1;
    inv.entityId = entityId;
    inv.method = "entity.spaceChanged";
    inv.payload = payload;

    RuntimeInvocation shortInv;
    shortInv.targetComponent = 1;
    shortInv.method = "entity.spaceChanged";

    bool ok = rt->dispatchInvocation(inv);
    ok = ok && seenEntity == entityId && seenSpace == spaceId;
    ok = ok && !rt->dispatchInvocation(shortInv);

    if (ok) PASS(); else FAIL("space changed");
}

static void testGroupManagerAccess() {
    TEST("group manager accessors");

    auto rt = makeRuntime();
    bool ok = &rt->groupManager() == &static_cast<const BaseRuntime&>(*rt).groupManager();

    if (ok) PASS(); else FAIL("group manager");
}

static void testHandleSpawnRequestBranches() {
    TEST("spawn request branches");

    auto transport = std::make_shared<InMemoryRuntimeTransport>();
    auto rt = std::make_unique<BaseRuntime>(transport, std::make_shared<InMemoryEntityStore>(), 1);
    auto def = makeAvatarDef();
    rt->registerEntityFactory("Avatar", makeFactory(def));

    auto spawnWire = [&](const std::string& type) {
        std::vector<std::byte> out;
        const std::uint32_t typeLen = static_cast<std::uint32_t>(type.size());
        out.resize(4 + typeLen + sizeof(float) * 3);
        auto* p = out.data();
        std::memcpy(p, &typeLen, 4); p += 4;
        std::memcpy(p, type.data(), typeLen); p += typeLen;
        const float x = 3.0F;
        std::memcpy(p, &x, sizeof(float));
        return out;
    };

    auto dispatchSpawn = [&](EntityId requesterId, const std::string& type) {
        RuntimeInvocation inv;
        inv.targetComponent = 1;
        inv.entityId = requesterId;
        inv.method = "entity.spawnRequest";
        inv.payload = spawnWire(type);
        return rt->dispatchInvocation(inv);
    };

    // 未知类型 → createEntity 失败
    bool ok = !dispatchSpawn(1, "NeverRegistered");

    // 请求者不存在 → 新实体被回滚销毁
    ok = ok && !dispatchSpawn(999, "Avatar");
    ok = ok && rt->findEntitiesByType("Avatar").empty();

    // 请求者无 cell call → 同样回滚
    auto* requester = rt->createEntity("Avatar");
    ok = ok && !dispatchSpawn(requester->id(), "Avatar");
    ok = ok && rt->findEntitiesByType("Avatar").size() == 1;

    // 有 cell call → 向 CellApp 发 createCell
    requester->bindCellEntityCall(42);
    ok = ok && dispatchSpawn(requester->id(), "Avatar");
    ok = ok && rt->findEntitiesByType("Avatar").size() == 2;

    std::array<RuntimeInvocation, 8> drained{};
    const auto count = transport->drain(drained.data(), drained.size());
    bool sentCreateCell = false;
    for (std::size_t i = 0; i < count; ++i) {
        if (drained[i].method == "entity.createCell" && drained[i].targetComponent == 42) {
            sentCreateCell = true;
        }
    }
    ok = ok && sentCreateCell;

    if (ok) PASS(); else FAIL("spawn request branches");
}

static void testVariablePropertyPersistence() {
    TEST("variable property persistence round trip");

    auto def = std::make_shared<EntityDef>("Avatar");
    def->addProperty("name", PropertyType::String);
    def->addProperty("level", PropertyType::Int32);

    auto rt = makeRuntime();
    rt->registerEntityFactory("Avatar", makeFactory(def));

    auto* entity = rt->createEntity("Avatar");
    const auto nameId = def->findProperty("name")->id;
    const auto levelId = def->findProperty("level")->id;
    entity->setString(nameId, "gamma");
    entity->setProperty<std::int32_t>(levelId, 9);
    bool ok = rt->saveEntity(entity->id());
    const auto id = entity->id();
    ok = ok && rt->destroyEntity(id);

    // 干净 round trip：blob 属性经 getBlob/setBlob 落盘再恢复
    auto* roundTripped = rt->loadEntity(id, "Avatar");
    ok = ok && roundTripped != nullptr;
    ok = ok && roundTripped->getString(nameId) == "gamma";
    ok = ok && roundTripped->getProperty<std::int32_t>(levelId) == 9;

    // 篡改存档：未知属性名 + 类型标签与 def 不符（宽度 2≠4，dataToEntity 跳过）
    // + 正常 blob，第二个 runtime 加载。注意 store 会对数据做 encode/decode
    // round trip，所以篡改必须保持自洽：rawValue 尺寸跟随声明的 DataType。
    {
        const std::string nameValue = "restored";
        theseed::core::PropertyData unknown;
        unknown.id = 99;
        unknown.name = "not_in_def";
        unknown.type = theseed::core::DataType::Int32;
        unknown.rawValue = {std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};
        theseed::core::PropertyData badLevel;
        badLevel.id = levelId;
        badLevel.name = "level";
        badLevel.type = theseed::core::DataType::Int16;  // def 里是 Int32
        badLevel.rawValue = {std::byte{0x00}, std::byte{0x00}};
        theseed::core::PropertyData goodName;
        goodName.id = nameId;
        goodName.name = "name";
        goodName.type = theseed::core::DataType::String;
        goodName.rawValue.reserve(nameValue.size());
        for (char c : nameValue)
            goodName.rawValue.push_back(static_cast<std::byte>(c));

        theseed::core::EntityData tampered;
        tampered.id = id;
        tampered.entityType = "Avatar";
        tampered.properties = {unknown, badLevel, goodName};

        auto backing = std::make_shared<InMemoryEntityStore>();
        backing->save(id, tampered);
        auto rt2 = std::make_unique<BaseRuntime>(
            std::make_shared<InMemoryRuntimeTransport>(), backing, 1);
        rt2->registerEntityFactory("Avatar", makeFactory(def));
        auto* restored = rt2->loadEntity(id, "Avatar");
        ok = ok && restored != nullptr;
        ok = ok && restored->getString(nameId) == nameValue;           // blob 路径恢复
        ok = ok && restored->getProperty<std::int32_t>(levelId) == 0;  // 尺寸不符被跳过
    }

    if (ok) PASS(); else FAIL("variable property persistence");
}

int main() {
    std::cout << "BaseRuntime tests:\n";

    testCreateEntity();
    testFindEntity();
    testDestroyEntity();
    testMultipleEntities();
    testSaveAndLoad();
    testAutoSave();
    testAutoSaveDoesNotClearRuntimeDirty();
    testCellDeltaMarksPersistenceWithoutRuntimeEcho();
    testSetCellEntityCall();
    testDispatchInvocation();
    testUnknownEntityType();
    testLoadNonexistent();
    testAutoSaveOnDestroy();
    testFindEntitiesByType();
    testNameBasedPropertyAccess();
    testForEachEntity();
    testSchedulerPhasesSyncBeforeFlush();
    testConstructorValidation();
    testRegisterFactoryValidation();
    testCreateEntityFactoryReturnsNull();
    testLoadEntityBranches();
    testSaveEntityBranches();
    testDispatchBranches();
    testPropertySyncFromCellMalformed();
    testHandleSpaceChanged();
    testGroupManagerAccess();
    testHandleSpawnRequestBranches();
    testVariablePropertyPersistence();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
