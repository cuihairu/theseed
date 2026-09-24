// BaseRuntime 分支覆盖测试（第二批）：payload 边界矩阵、回调缺席、
// send 失败臂、timer 正向链路与 sync 时序编排。
// 与 BaseRuntimeTest 互补——那边是功能路径，这边专收防御与边界分支。
#include "theseed/core/BaseRuntime.h"
#include "theseed/core/IEntityStore.h"
#include "theseed/runtime/Entity.h"
#include "theseed/runtime/EntityDef.h"
#include "theseed/runtime/PropertyReplication.h"
#include "theseed/runtime/RuntimeTransport.h"
#include "theseed/runtime/RuntimeTypes.h"
#include "theseed/runtime/TickScheduler.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

using theseed::core::BaseRuntime;
using theseed::core::EntityData;
using theseed::core::InMemoryEntityStore;
using theseed::foundation::TimerHandle;
using theseed::runtime::ComponentId;
using theseed::runtime::DeliveryClass;
using theseed::runtime::Entity;
using theseed::runtime::EntityDef;
using theseed::runtime::EntityId;
using theseed::runtime::EntitySide;
using theseed::runtime::InMemoryRuntimeTransport;
using theseed::runtime::IRuntimeTransport;
using theseed::runtime::PropertyType;
using theseed::runtime::RuntimeInvocation;
using theseed::runtime::SendResult;
using theseed::runtime::SpaceId;
using theseed::runtime::TickContext;
using theseed::runtime::TickPhase;
using theseed::runtime::TickScheduler;
using theseed::runtime::Vector3;

static int testsPassed = 0;
static int testsFailed = 0;

#define TEST(name)                                           \
    do {                                                     \
        std::cout << "  " << (name) << "... " << std::flush; \
    } while (0)

#define PASS()               \
    do {                     \
        std::cout << "OK\n"; \
        ++testsPassed;       \
    } while (0)

#define FAIL(msg)                               \
    do {                                        \
        std::cout << "FAILED: " << (msg) << "\n"; \
        ++testsFailed;                          \
    } while (0)

namespace {

constexpr ComponentId kLocal = 1;

std::shared_ptr<EntityDef> makeAvatarDef() {
    auto def = std::make_shared<EntityDef>("Avatar");
    def->addProperty("level", PropertyType::Int32);
    def->addProperty("hp", PropertyType::Float32);
    def->addMethod("onDamage", theseed::runtime::MethodSide::Base);
    return def;
}

BaseRuntime::EntityFactory makeFactory(std::shared_ptr<EntityDef> def) {
    return [def](EntityId id, EntitySide side) -> std::unique_ptr<Entity> {
        return std::make_unique<Entity>(id, side, *def);
    };
}

std::unique_ptr<BaseRuntime> makeRuntime(std::shared_ptr<IRuntimeTransport> transport = nullptr,
                                         std::shared_ptr<InMemoryEntityStore> store = nullptr) {
    if (!transport) transport = std::make_shared<InMemoryRuntimeTransport>();
    if (!store) store = std::make_shared<InMemoryEntityStore>();
    auto rt = std::make_unique<BaseRuntime>(transport, store, kLocal);
    rt->registerEntityFactory("Avatar", makeFactory(makeAvatarDef()));
    return rt;
}

// send 恒失败的 transport。
class RejectingTransport final : public IRuntimeTransport {
public:
    SendResult send(RuntimeInvocation) override { return SendResult::NotConnected; }
    std::size_t receive(ComponentId, RuntimeInvocation*, std::size_t) override { return 0; }
    std::size_t pendingCount() const override { return 0; }
    void flush() override {}
    theseed::runtime::TransportStats stats() const override { return {}; }
};

void appendU32(std::vector<std::byte>& v, std::uint32_t x) {
    auto* p = reinterpret_cast<const std::byte*>(&x);
    v.insert(v.end(), p, p + 4);
}

void appendU64(std::vector<std::byte>& v, std::uint64_t x) {
    auto* p = reinterpret_cast<const std::byte*>(&x);
    v.insert(v.end(), p, p + 8);
}

void appendF32(std::vector<std::byte>& v, float x) {
    auto* p = reinterpret_cast<const std::byte*>(&x);
    v.insert(v.end(), p, p + 4);
}

void appendU8(std::vector<std::byte>& v, std::uint8_t x) {
    v.push_back(static_cast<std::byte>(x));
}

void appendStr(std::vector<std::byte>& v, const std::string& s) {
    appendU32(v, static_cast<std::uint32_t>(s.size()));
    auto* p = reinterpret_cast<const std::byte*>(s.data());
    v.insert(v.end(), p, p + s.size());
}

RuntimeInvocation makeInv(const std::string& method, std::vector<std::byte> payload = {},
                          EntityId entityId = 0) {
    RuntimeInvocation inv;
    inv.targetComponent = kLocal;
    inv.entityId = entityId;
    inv.method = method;
    inv.payload = std::move(payload);
    return inv;
}

bool dispatch(BaseRuntime& rt, const std::string& method, std::vector<std::byte> payload = {},
              EntityId entityId = 0) {
    return rt.dispatchInvocation(makeInv(method, std::move(payload), entityId));
}

// aoi.enter 的分阶段 wire 构造，便于拼各种截断变体。
// full: observer + target + type + hasPos + [pos]
std::vector<std::byte> aoiEnterWire(EntityId observer, EntityId target, const std::string& type,
                                    bool hasPos, const Vector3& pos) {
    std::vector<std::byte> v;
    appendU64(v, observer);
    appendU64(v, target);
    appendStr(v, type);
    appendU8(v, hasPos ? 1 : 0);
    if (hasPos) {
        appendF32(v, pos.x);
        appendF32(v, pos.y);
        appendF32(v, pos.z);
    }
    return v;
}

// entity.clientEvent：entityId + count + [nameLen+name+dataLen+data]×count
std::vector<std::byte> clientEventWire(EntityId id, std::uint32_t count,
                                       const std::vector<std::pair<std::string, std::string>>& events) {
    std::vector<std::byte> v;
    appendU64(v, id);
    appendU32(v, count);
    for (const auto& [name, data] : events) {
        appendStr(v, name);
        appendU32(v, static_cast<std::uint32_t>(data.size()));
        auto* p = reinterpret_cast<const std::byte*>(data.data());
        v.insert(v.end(), p, p + data.size());
    }
    return v;
}

// witness.propertySync：observer + count + [target + hasPos + [pos] + deltaLen + delta]×count
std::vector<std::byte> witnessSyncWire(EntityId observer,
                                       const std::vector<std::tuple<EntityId, bool, Vector3, std::string>>& items) {
    std::vector<std::byte> v;
    appendU64(v, observer);
    appendU32(v, static_cast<std::uint32_t>(items.size()));
    for (const auto& [target, hasPos, pos, delta] : items) {
        appendU64(v, target);
        appendU8(v, hasPos ? 1 : 0);
        if (hasPos) {
            appendF32(v, pos.x);
            appendF32(v, pos.y);
            appendF32(v, pos.z);
        }
        appendU32(v, static_cast<std::uint32_t>(delta.size()));
        auto* p = reinterpret_cast<const std::byte*>(delta.data());
        v.insert(v.end(), p, p + delta.size());
    }
    return v;
}

}  // namespace

// ---------------------------------------------------------------------------
// B1 timer 正向链路 + factory hook
// ---------------------------------------------------------------------------

static void testEntityTimerPositivePath() {
    TEST("entity timer scheduling runs callbacks while alive");

    auto rt = makeRuntime();
    int delayFired = 0;
    int periodicFired = 0;

    {
        auto* e = rt->createEntity("Avatar");
        e->addTimer(std::chrono::milliseconds{1}, [&](Entity&) { ++delayFired; });
        e->addPeriodicTimer(std::chrono::milliseconds{1}, [&](Entity&) { ++periodicFired; });
    }

    TickContext ctx;
    ctx.deltaTime = std::chrono::milliseconds{5};
    rt->tick(ctx);

    // TimerWheel 有 tick 粒度：一次 advance 每个到期 timer 至多触发一次。
    bool ok = delayFired == 1;
    ok = ok && periodicFired >= 1;

    // 销毁后 timer 被取消，回调不再触发。
    int loadFired = 0;
    int loadPeriodicFired = 0;
    {
        auto rt2 = makeRuntime();
        auto* created = rt2->createEntity("Avatar");
        created->setProperty<std::int32_t>(0, 7);
        rt2->saveEntity(created->id());
        const EntityId savedId = created->id();   // 销毁后 created 即失效，先存 id
        ok = ok && rt2->destroyEntity(savedId);   // restore 语义：先出册再加载

        auto* e = rt2->loadEntity(savedId, "Avatar");
        if (e == nullptr) {
            FAIL("loadEntity returned null");
            return;
        }
        e->addTimer(std::chrono::milliseconds{1}, [&](Entity&) { ++loadFired; });
        e->addPeriodicTimer(std::chrono::milliseconds{1}, [&](Entity&) { ++loadPeriodicFired; });
        rt2->destroyEntity(e->id());
        rt2->tick(ctx);
        ok = ok && loadFired == 0 && loadPeriodicFired == 0;
    }

    if (ok) PASS(); else FAIL("delayFired=" + std::to_string(delayFired) + " periodicFired=" + std::to_string(periodicFired) + " loadFired=" + std::to_string(loadFired));
}

static void testFactoryHook() {
    TEST("entity factory hook invoked on create and load");

    auto store = std::make_shared<InMemoryEntityStore>();
    auto rt = makeRuntime(nullptr, store);

    int hooks = 0;
    rt->setEntityFactoryHook([&](Entity&) { ++hooks; });

    auto* created = rt->createEntity("Avatar");
    bool ok = hooks == 1;

    created->setProperty<std::int32_t>(0, 9);
    rt->saveEntity(created->id());
    auto* loaded = rt->loadEntity(created->id(), "Avatar");
    ok = ok && loaded != nullptr;
    ok = ok && hooks == 2;

    if (ok) PASS(); else FAIL("factory hook");
}

// ---------------------------------------------------------------------------
// B2 load/restore/destroy 边界
// ---------------------------------------------------------------------------

static void testLoadRestoreDestroyEdges() {
    TEST("loadEntity / restoreEntities / destroyEntity edges");

    auto rt = makeRuntime();

    // 未知类型的 loadEntity。
    bool ok = rt->loadEntity(500, "Nope") == nullptr;

    // restoreEntities：store 有存量数据时恢复；空类型返回 0。
    ok = ok && rt->restoreEntities("Nope") == 0;
    {
        auto* created = rt->createEntity("Avatar");
        created->setProperty<std::int32_t>(0, 33);
        ok = ok && rt->saveEntity(created->id());
    }
    ok = ok && rt->destroyEntity(rt->findEntitiesByType("Avatar").front()->id());
    ok = ok && rt->restoreEntities("Avatar") == 1;
    ok = ok && rt->findEntitiesByType("Avatar").size() == 1;

    // destroyEntity 未知 id。
    ok = ok && !rt->destroyEntity(424242);

    if (ok) PASS(); else FAIL("load/restore/destroy edges");
}

// ---------------------------------------------------------------------------
// B3 pumpInbound 分批
// ---------------------------------------------------------------------------

static void testPumpInboundBatches() {
    TEST("pumpInbound drains across receive batches");

    auto transport = std::make_shared<InMemoryRuntimeTransport>();
    auto rt = makeRuntime(transport);

    int leaves = 0;
    rt->setOnAoILeave([&](EntityId, EntityId) { ++leaves; });

    // 空队列：pump 直接返回 0。
    bool ok = rt->pumpInbound() == 0;

    // 33 条：首批 32 + 二批 1 + 三轮空。
    for (std::uint64_t i = 0; i < 33; ++i) {
        RuntimeInvocation inv = makeInv("aoi.leave");
        appendU64(inv.payload, i);
        appendU64(inv.payload, i + 100);
        inv.targetComponent = kLocal;
        transport->send(std::move(inv));
    }
    ok = ok && rt->pumpInbound() == 33;
    ok = ok && leaves == 33;

    // 恰好 32 条：单批吃满。
    leaves = 0;
    for (std::uint64_t i = 0; i < 32; ++i) {
        RuntimeInvocation inv = makeInv("aoi.leave");
        appendU64(inv.payload, i);
        appendU64(inv.payload, i + 200);
        transport->send(std::move(inv));
    }
    ok = ok && rt->pumpInbound() == 32;
    ok = ok && leaves == 32;
    ok = ok && rt->pumpInbound() == 0;

    if (ok) PASS(); else FAIL("pumpInbound batches");
}

// ---------------------------------------------------------------------------
// B4 autoSave / sync 时序边界
// ---------------------------------------------------------------------------

static void testAutoSaveTickEdges() {
    TEST("autoSave interval edges and Destroying skip");

    auto store = std::make_shared<InMemoryEntityStore>();
    auto rt = makeRuntime(nullptr, store);

    // 未设 interval：tick 不走 autoSave 分支。
    TickContext ctx;
    ctx.deltaTime = std::chrono::milliseconds{10};
    rt->tick(ctx);

    // 大 interval 小步长：累计未达阈值，不触发。
    rt->setAutoSaveInterval(std::chrono::milliseconds{10'000});
    rt->tick(ctx);

    // Destroying 实体（cell 销毁 pending 中）被 autoSaveAll 跳过。
    bool ok = true;
    auto* e = rt->createEntity("Avatar");
    auto id = e->id();
    rt->setCellEntityCall(id, 2);
    e->setProperty<std::int32_t>(0, 55);
    ok = ok && rt->destroyEntity(id);   // 有 cell：仅 pending，实体仍在册
    ok = ok && rt->findEntity(id) != nullptr;
    rt->setAutoSaveInterval(std::chrono::milliseconds{1});
    rt->tick(ctx);
    ok = ok && !store->exists(id);      // 非 Active：不落库

    if (ok) PASS(); else FAIL("autoSave tick edges");
}

namespace {

// SyncBuild 相内、syncBuildPump 之后的探针：销毁实体，制造
// "sync 已入 pending、flush 时实体已不在册" 的时序。
class DestroyBetweenSyncAndFlush final : public theseed::runtime::ITickable {
public:
    explicit DestroyBetweenSyncAndFlush(BaseRuntime& rt, EntityId target)
        : rt_(rt), target_(target) {}

    void tick(TickContext&) override {
        rt_.destroyEntity(target_);                 // beginDestroy + pending
        RuntimeInvocation inv = makeInv("entity.cellDestroyed");
        appendU64(inv.payload, target_);
        static_cast<void>(rt_.dispatchInvocation(inv));  // completeBaseDestruction：出册
    }

private:
    BaseRuntime& rt_;
    EntityId target_;
};

}  // namespace

static void testFlushSkipsClearedEntity() {
    TEST("flush after entity removal leaves pending sync without clearing dirty");

    auto transport = std::make_shared<InMemoryRuntimeTransport>();
    auto rt = makeRuntime(transport);
    auto* e = rt->createEntity("Avatar");
    auto id = e->id();
    rt->setCellEntityCall(id, 2);
    e->setProperty<std::int32_t>(0, 66);   // runtime dirty → syncToCells 入 pending

    TickScheduler scheduler;
    rt->attach(scheduler);
    DestroyBetweenSyncAndFlush probe(*rt, id);
    scheduler.registerTickable(TickPhase::SyncBuild, probe);  // 同相内排在 syncBuildPump 之后

    scheduler.runOnce();

    // 实体已出册；flush 阶段 send 成功但 clearDirty 找不到实体（防御臂）。
    bool ok = rt->findEntity(id) == nullptr;

    std::vector<RuntimeInvocation> flushed;
    std::array<RuntimeInvocation, 16> drained{};
    const auto n = transport->drain(drained.data(), drained.size());
    bool sawSyncToCell = false;
    for (std::size_t i = 0; i < n; ++i) {
        if (drained[i].method == "property.syncToCell") sawSyncToCell = true;
    }
    ok = ok && sawSyncToCell;   // pending 确实生成并成功发出

    if (ok) PASS(); else FAIL("flush after entity removal");
}

// ---------------------------------------------------------------------------
// B5 cellReady / cellDestroyed / syncToBase 守卫
// ---------------------------------------------------------------------------

static void testCellReadyDestroyedGuards() {
    TEST("cellReady / cellDestroyed payload guards");

    auto rt = makeRuntime();
    auto* e = rt->createEntity("Avatar");
    auto id = e->id();

    // cellReady：截断 payload / 未知实体 / 正向。
    bool ok = !dispatch(*rt, "entity.cellReady", [&] {
        std::vector<std::byte> v;
        appendU64(v, id);            // 缺 cellComponentId
        return v;
    }());
    ok = ok && !dispatch(*rt, "entity.cellReady", [&] {
        std::vector<std::byte> v;
        appendU64(v, 987654);
        appendU64(v, 2);
        return v;
    }());

    // cellDestroyed：截断 / 未知实体且非 pending。
    ok = ok && !dispatch(*rt, "entity.cellDestroyed", [&] {
        std::vector<std::byte> v;
        appendU32(v, 1);             // 不足 8 字节
        return v;
    }());
    ok = ok && !dispatch(*rt, "entity.cellDestroyed", [&] {
        std::vector<std::byte> v;
        appendU64(v, 987654);
        return v;
    }());

    // property.syncToBase：空 payload / 已知实体 + 非法 delta（decode 抛异常）。
    ok = ok && !dispatch(*rt, "property.syncToBase");
    ok = ok && !dispatch(*rt, "property.syncToBase", [&] {
        std::vector<std::byte> v;
        appendU32(v, 0xFFFFFFFFu);   // count 巨大 → decode 必抛
        return v;
    }(), id);

    // 正向 cellReady 仍工作。
    ok = ok && dispatch(*rt, "entity.cellReady", [&] {
        std::vector<std::byte> v;
        appendU64(v, id);
        appendU64(v, 2);
        return v;
    }());

    if (ok) PASS(); else FAIL("cellReady/cellDestroyed guards");
}

// ---------------------------------------------------------------------------
// B6 request* 直调失败臂
// ---------------------------------------------------------------------------

static void testRequestSendFailures() {
    TEST("requestCreateCell / requestDestroyCell / requestTeleport failure arms");

    // send 失败：createCell 返回 false。
    auto rejecting = std::make_unique<BaseRuntime>(std::make_shared<RejectingTransport>(),
                                                   std::make_shared<InMemoryEntityStore>(), kLocal);
    rejecting->registerEntityFactory("Avatar", makeFactory(makeAvatarDef()));
    auto* e = rejecting->createEntity("Avatar");
    bool ok = !rejecting->requestCreateCell(e->id(), "Avatar", Vector3{1, 2, 3}, 7, 9);

    // teleport：未 bind cell → false（cellCall 为 null 的臂）。
    ok = ok && !rejecting->requestTeleport(e->id(), 5, Vector3{});

    // destroy：未知实体（cellCall 为 null）走立即销毁，send 失败不影响返回值。
    ok = ok && rejecting->destroyEntity(e->id());

    // 正常 transport 下 teleport 未知实体同样 false。
    auto rt = makeRuntime();
    ok = ok && !rt->requestTeleport(777, 5, Vector3{});

    if (ok) PASS(); else FAIL("request send failures");
}

// ---------------------------------------------------------------------------
// B7 syncToCells 跳过未 bind 实体
// ---------------------------------------------------------------------------

static void testSyncToCellsSkipsUnbound() {
    TEST("syncToCells skips entities without a bound cell");

    auto transport = std::make_shared<InMemoryRuntimeTransport>();
    auto rt = makeRuntime(transport);

    auto* bound = rt->createEntity("Avatar");
    rt->setCellEntityCall(bound->id(), 2);
    bound->setProperty<std::int32_t>(0, 1);

    auto* unbound = rt->createEntity("Avatar");
    unbound->setProperty<std::int32_t>(0, 2);   // dirty 但无 cell call

    TickContext ctx;
    ctx.deltaTime = std::chrono::milliseconds{1};
    rt->tick(ctx);

    std::array<RuntimeInvocation, 16> drained{};
    const auto n = transport->drain(drained.data(), drained.size());
    std::size_t syncs = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (drained[i].method == "property.syncToCell") ++syncs;
    }

    bool ok = syncs == 1;   // 只有 bound 的产生 sync

    if (ok) PASS(); else FAIL("syncToCells unbound skip");
}

// ---------------------------------------------------------------------------
// B8 aoi.enter / aoi.leave 矩阵
// ---------------------------------------------------------------------------

static void testAoIEnterVariants() {
    TEST("aoi.enter payload variants");

    auto rt = makeRuntime();
    struct Seen {
        EntityId observer = 0;
        EntityId target = 0;
        std::string type;
        bool hasPos = false;
        Vector3 pos;
    };
    Seen seen;
    rt->setOnAoIEnter([&](EntityId o, EntityId t, const std::string& type, bool hasPos,
                          const Vector3& pos) {
        seen = Seen{o, t, type, hasPos, pos};
    });

    const auto full = aoiEnterWire(9, 10, "Monster", true, Vector3{1.f, 2.f, 3.f});

    bool ok = !dispatch(*rt, "aoi.enter", std::vector<std::byte>(full.begin(), full.begin() + 15));
    ok = ok && !dispatch(*rt, "aoi.enter", std::vector<std::byte>(full.begin(), full.begin() + 16));
    // typeLen 指向超出 payload 的长度。
    {
        std::vector<std::byte> v;
        appendU64(v, 9);
        appendU64(v, 10);
        appendU32(v, 1000);
        ok = ok && !dispatch(*rt, "aoi.enter", std::move(v));
    }
    // 恰好到 type 末尾：hasPos 字节缺失 → hasPosition=false 仍回调。
    {
        std::vector<std::byte> v;
        appendU64(v, 9);
        appendU64(v, 10);
        appendStr(v, "Monster");
        ok = ok && dispatch(*rt, "aoi.enter", v);
        ok = ok && seen.observer == 9 && seen.target == 10 && seen.type == "Monster";
        ok = ok && !seen.hasPos;
    }
    // hasPos=0：完整回调，无位置。
    ok = ok && dispatch(*rt, "aoi.enter", aoiEnterWire(11, 12, "NPC", false, Vector3{}));
    ok = ok && seen.observer == 11 && !seen.hasPos;
    // hasPos=1 但位置截断：hasPosition=true、位置为零值。
    {
        auto wire = aoiEnterWire(13, 14, "Mob", true, Vector3{4.f, 5.f, 6.f});
        wire.resize(wire.size() - 5);   // 削掉 z 与部分 y
        ok = ok && dispatch(*rt, "aoi.enter", std::move(wire));
        ok = ok && seen.observer == 13 && seen.hasPos && seen.pos.x == 0.f;
    }
    // 正向：hasPos=1 + 完整位置。
    ok = ok && dispatch(*rt, "aoi.enter", aoiEnterWire(15, 16, "Boss", true, Vector3{7.f, 8.f, 9.f}));
    ok = ok && seen.hasPos && seen.pos.z == 9.f;

    // 未设回调：payload 合法也返回 true。
    auto rt2 = makeRuntime();
    ok = ok && dispatch(*rt2, "aoi.enter", aoiEnterWire(1, 2, "X", false, Vector3{}));

    if (ok) PASS(); else FAIL("aoi.enter variants");
}

static void testAoILeaveVariants() {
    TEST("aoi.leave payload variants");

    auto rt = makeRuntime();
    EntityId seenObserver = 0;
    rt->setOnAoILeave([&](EntityId o, EntityId) { seenObserver = o; });

    bool ok = !dispatch(*rt, "aoi.leave", std::vector<std::byte>(8));   // 截断
    ok = ok && dispatch(*rt, "aoi.leave", [&] {
        std::vector<std::byte> v;
        appendU64(v, 21);
        appendU64(v, 22);
        return v;
    }());
    ok = ok && seenObserver == 21;

    // 未设回调。
    auto rt2 = makeRuntime();
    ok = ok && dispatch(*rt2, "aoi.leave", [&] {
        std::vector<std::byte> v;
        appendU64(v, 1);
        appendU64(v, 2);
        return v;
    }());

    if (ok) PASS(); else FAIL("aoi.leave variants");
}

// ---------------------------------------------------------------------------
// B9 spawnRequest 截断矩阵
// ---------------------------------------------------------------------------

static void testSpawnRequestTruncations() {
    TEST("spawnRequest truncation variants");

    auto transport = std::make_shared<InMemoryRuntimeTransport>();
    auto rt = makeRuntime(transport);

    bool ok = !dispatch(*rt, "entity.spawnRequest", std::vector<std::byte>(3));
    {
        std::vector<std::byte> v;
        appendU32(v, 1000);   // typeLen 超界
        ok = ok && !dispatch(*rt, "entity.spawnRequest", std::move(v));
    }
    // typeLen 后无位置：位置缺省仍创建，requester 未知 → 回滚。
    {
        std::vector<std::byte> v;
        appendStr(v, "Avatar");
        ok = ok && !dispatch(*rt, "entity.spawnRequest", std::move(v), 999);
        ok = ok && rt->findEntitiesByType("Avatar").empty();
    }
    // 未知类型。
    ok = ok && !dispatch(*rt, "entity.spawnRequest", [&] {
        std::vector<std::byte> v;
        appendStr(v, "Ghost");
        appendF32(v, 0.f);
        appendF32(v, 0.f);
        appendF32(v, 0.f);
        return v;
    }());

    if (ok) PASS(); else FAIL("spawnRequest truncations");
}

// ---------------------------------------------------------------------------
// B10 entity.clientEvent 矩阵
// ---------------------------------------------------------------------------

static void testClientEventMatrix() {
    TEST("entity.clientEvent guard matrix");

    auto rt = makeRuntime();
    int events = 0;
    rt->setOnClientEvent([&](EntityId, const std::string&, const std::span<const std::byte>) { ++events; });

    const auto full = clientEventWire(31, 2, {{"hit", "ab"}, {"die", ""}});

    // 头截断（entityId+count 不足）。
    bool ok = !dispatch(*rt, "entity.clientEvent", std::vector<std::byte>(full.begin(), full.begin() + 11));

    // count=1 但 nameLen 缺失。
    {
        std::vector<std::byte> v;
        appendU64(v, 31);
        appendU32(v, 1);
        ok = ok && !dispatch(*rt, "entity.clientEvent", std::move(v));
    }
    // nameLen 超界。
    {
        std::vector<std::byte> v;
        appendU64(v, 31);
        appendU32(v, 1);
        appendU32(v, 999);
        ok = ok && !dispatch(*rt, "entity.clientEvent", std::move(v));
    }
    // name 之后 dataLen 缺失。
    {
        std::vector<std::byte> v;
        appendU64(v, 31);
        appendU32(v, 1);
        appendStr(v, "hit");
        ok = ok && !dispatch(*rt, "entity.clientEvent", std::move(v));
    }
    // dataLen 超界。
    {
        std::vector<std::byte> v;
        appendU64(v, 31);
        appendU32(v, 1);
        appendStr(v, "hit");
        appendU32(v, 999);
        ok = ok && !dispatch(*rt, "entity.clientEvent", std::move(v));
    }
    // 正向：两个事件（循环回边）。
    ok = ok && dispatch(*rt, "entity.clientEvent", clientEventWire(31, 2, {{"hit", "ab"}, {"die", ""}}));
    ok = ok && events == 2;
    // 空数据事件。
    ok = ok && dispatch(*rt, "entity.clientEvent", clientEventWire(31, 1, {{"ping", ""}}));
    ok = ok && events == 3;

    // 未设回调：合法 payload 也 false。
    auto rt2 = makeRuntime();
    ok = ok && !dispatch(*rt2, "entity.clientEvent", clientEventWire(1, 1, {{"x", ""}}));

    if (ok) PASS(); else FAIL("clientEvent matrix");
}

// ---------------------------------------------------------------------------
// B11 witness.propertySync 矩阵
// ---------------------------------------------------------------------------

static void testWitnessSyncMatrix() {
    TEST("witness.propertySync guard matrix");

    auto rt = makeRuntime();
    int syncs = 0;
    EntityId lastTarget = 0;
    rt->setOnWitnessSync([&](EntityId, EntityId target, const std::span<const std::byte>, bool,
                             const Vector3&) {
        ++syncs;
        lastTarget = target;
    });

    const auto full = witnessSyncWire(41, {{42, true, Vector3{1.f, 2.f, 3.f}, "DEADBEEF"},
                                           {43, false, Vector3{}, ""}});

    // 头截断。
    bool ok = !dispatch(*rt, "witness.propertySync", std::vector<std::byte>(full.begin(), full.begin() + 11));
    // count=1 但条目缺失。
    {
        std::vector<std::byte> v;
        appendU64(v, 41);
        appendU32(v, 1);
        ok = ok && !dispatch(*rt, "witness.propertySync", std::move(v));
    }
    // hasPos=1 但位置缺失。
    {
        std::vector<std::byte> v;
        appendU64(v, 41);
        appendU32(v, 1);
        appendU64(v, 42);
        appendU8(v, 1);
        ok = ok && !dispatch(*rt, "witness.propertySync", std::move(v));
    }
    // hasPos=0 但 deltaLen 缺失。
    {
        std::vector<std::byte> v;
        appendU64(v, 41);
        appendU32(v, 1);
        appendU64(v, 42);
        appendU8(v, 0);
        ok = ok && !dispatch(*rt, "witness.propertySync", std::move(v));
    }
    // deltaLen 超界。
    {
        std::vector<std::byte> v;
        appendU64(v, 41);
        appendU32(v, 1);
        appendU64(v, 42);
        appendU8(v, 0);
        appendU32(v, 500);
        ok = ok && !dispatch(*rt, "witness.propertySync", std::move(v));
    }
    // 正向：hasPos=1 + pos + delta；再 hasPos=0 + 空 delta。
    ok = ok && dispatch(*rt, "witness.propertySync", std::move(full));
    ok = ok && syncs == 2 && lastTarget == 43;

    // 未设回调。
    auto rt2 = makeRuntime();
    ok = ok && !dispatch(*rt2, "witness.propertySync", witnessSyncWire(1, {}));

    if (ok) PASS(); else FAIL("witness sync matrix");
}

// ---------------------------------------------------------------------------
// B12 spaceChanged 无回调
// ---------------------------------------------------------------------------

static void testSpaceChangedNoCallback() {
    TEST("spaceChanged without callback still succeeds");

    auto rt = makeRuntime();
    std::vector<std::byte> v;
    appendU64(v, 51);
    appendU64(v, 300);
    appendF32(v, 1.f);
    appendF32(v, 2.f);
    appendF32(v, 3.f);

    bool ok = dispatch(*rt, "entity.spaceChanged", std::move(v));

    if (ok) PASS(); else FAIL("spaceChanged no callback");
}

// store 包装：可指定 load / save 失败，探 store 失败臂。
class FlakyStore final : public theseed::core::IEntityStore {
public:
    explicit FlakyStore(std::shared_ptr<InMemoryEntityStore> inner) : inner_(std::move(inner)) {}

    bool load(EntityId id, const std::string& t, EntityData& out) override {
        if (failLoads_.count(id)) return false;
        return inner_->load(id, t, out);
    }
    bool save(EntityId id, const EntityData& d) override {
        return saveOk_ && inner_->save(id, d);
    }
    bool remove(EntityId id) override { return inner_->remove(id); }
    EntityId allocId() override { return inner_->allocId(); }
    std::vector<EntityId> listIdsByType(const std::string& t) override {
        return inner_->listIdsByType(t);
    }
    std::vector<std::string> listEntityTypes() override { return inner_->listEntityTypes(); }

    std::unordered_set<EntityId> failLoads_;
    bool saveOk_ = true;

private:
    std::shared_ptr<InMemoryEntityStore> inner_;
};

static void testCellCallEdgeVariants() {
    TEST("cellCall edge variants (target=0 is valid; null-cell spawn rollback)");

    auto transport = std::make_shared<InMemoryRuntimeTransport>();
    auto rt = makeRuntime(transport);

    // EntityCall::isValid 只看 targetComponent 是否有值——bind(0) 也是合法
    // cell：销毁走 cell pending 路径，实体留在册等 cellDestroyed 收尾。
    auto* e = rt->createEntity("Avatar");
    bool ok = rt->setCellEntityCall(e->id(), 0);
    ok = ok && rt->destroyEntity(e->id());
    ok = ok && rt->findEntity(e->id()) != nullptr;

    // requestCreateCell：未知实体 → false（findEntity 空早退）。
    ok = ok && !rt->requestCreateCell(999, "Avatar", Vector3{}, 7, 9);

    // syncToCells：bound 无 dirty → continue；Destroying+bound → state continue。
    auto* e4 = rt->createEntity("Avatar");
    ok = ok && rt->setCellEntityCall(e4->id(), 5);
    auto* e5 = rt->createEntity("Avatar");
    ok = ok && rt->setCellEntityCall(e5->id(), 5);
    e5->setProperty<std::int32_t>(0, 7);
    ok = ok && rt->destroyEntity(e5->id());   // cell pending → state Destroying

    TickContext ctx;
    ctx.deltaTime = std::chrono::milliseconds{1};
    rt->tick(ctx);

    std::array<RuntimeInvocation, 16> drained{};
    const auto n = transport->drain(drained.data(), drained.size());
    ok = ok && n == 2;   // 仅 e / e5 的 destroyCell，syncToCell 全被拦下

    // spawnRequest：requester 在册但从未 bind cell → 创建后回滚。
    auto* e6 = rt->createEntity("Avatar");     // 无 cell
    const auto before = rt->findEntitiesByType("Avatar").size();
    {
        std::vector<std::byte> v;
        appendStr(v, "Avatar");
        appendF32(v, 0.f); appendF32(v, 0.f); appendF32(v, 0.f);
        ok = ok && !dispatch(*rt, "entity.spawnRequest", std::move(v), e6->id());
        ok = ok && rt->findEntitiesByType("Avatar").size() == before;
    }

    // cellDestroyed：pending 中的 e 完成 base 清理；非 pending 的 e4 清 cell 调用。
    ok = ok && dispatch(*rt, "entity.cellDestroyed", [&] {
        std::vector<std::byte> v;
        appendU64(v, e->id());
        return v;
    }());
    ok = ok && rt->findEntity(e->id()) == nullptr;
    ok = ok && dispatch(*rt, "entity.cellDestroyed", [&] {
        std::vector<std::byte> v;
        appendU64(v, e4->id());
        return v;
    }());

    if (ok) PASS(); else FAIL("cellCall edge variants");
}

static void testPropertySyncUnknownEntityAndFlushFailure() {
    TEST("property.syncToBase unknown entity; flush with failing send");

    auto rt = makeRuntime();
    // syncToBase：未知实体 → false（findEntity 空早退）。
    bool ok = !dispatch(*rt, "property.syncToBase",
                        std::vector<std::byte>{std::byte{1}, std::byte{2}}, 424242);

    // flush：pending 生成后 send 恒失败 → 不清 dirty，实体保留。
    auto rejecting = std::make_unique<BaseRuntime>(std::make_shared<RejectingTransport>(),
                                                   std::make_shared<InMemoryEntityStore>(), kLocal);
    rejecting->registerEntityFactory("Avatar", makeFactory(makeAvatarDef()));
    auto* e = rejecting->createEntity("Avatar");
    ok = ok && rejecting->setCellEntityCall(e->id(), 2);
    e->setProperty<std::int32_t>(0, 9);   // runtime dirty → syncToCells 入 pending

    TickContext ctx;
    ctx.deltaTime = std::chrono::milliseconds{1};
    rejecting->tick(ctx);   // SyncBuild push → Flush send NotConnected（不清 dirty）
    rejecting->tick(ctx);   // dirty 未清 → 再次 push、再次失败
    ok = ok && rejecting->findEntity(e->id()) != nullptr;

    if (ok) PASS(); else FAIL("syncToBase unknown / flush failure");
}

static void testStoreFailureEdges() {
    TEST("store load/save failure edges in restore and autoSave");

    // 正常 runtime 先落两个存档。
    bool ok = true;
    auto inner = std::make_shared<InMemoryEntityStore>();
    {
        auto rt = makeRuntime(nullptr, inner);
        auto* a = rt->createEntity("Avatar");
        a->setProperty<std::int32_t>(0, 1);
        auto* b = rt->createEntity("Avatar");
        b->setProperty<std::int32_t>(0, 2);
        rt->setAutoSaveInterval(std::chrono::milliseconds{1});
        TickContext ctx;
        ctx.deltaTime = std::chrono::milliseconds{10};
        rt->tick(ctx);   // 两个实体都落库
        ok = ok && inner->count() == 2;
    }

    auto flaky = std::make_shared<FlakyStore>(inner);
    // restore：第二个 id load 失败 → 只恢复 1 个。
    {
        auto rt = std::make_unique<BaseRuntime>(
            std::make_shared<InMemoryRuntimeTransport>(), flaky, kLocal);
        rt->registerEntityFactory("Avatar", makeFactory(makeAvatarDef()));
        const auto ids = inner->listIdsByType("Avatar");
        ok = ok && ids.size() == 2;
        flaky->failLoads_.insert(ids[1]);   // loadEntity 失败一次
        const auto restored = rt->restoreEntities("Avatar");
        ok = ok && restored == 1;
        ok = ok && rt->findEntity(ids[0]) != nullptr;
        ok = ok && rt->findEntity(ids[1]) == nullptr;
    }
    // autoSave：save 失败 → 不清 persistence dirty，store 不落新档。
    {
        flaky->saveOk_ = false;
        auto rt = std::make_unique<BaseRuntime>(
            std::make_shared<InMemoryRuntimeTransport>(), flaky, kLocal);
        rt->registerEntityFactory("Avatar", makeFactory(makeAvatarDef()));
        auto* c = rt->createEntity("Avatar");
        c->setProperty<std::int32_t>(0, 3);
        rt->setAutoSaveInterval(std::chrono::milliseconds{1});
        TickContext ctx;
        ctx.deltaTime = std::chrono::milliseconds{10};
        rt->tick(ctx);
        ok = ok && !inner->exists(c->id());   // save 被拦截
        ok = ok && rt->findEntity(c->id()) != nullptr;
    }

    if (ok) PASS(); else FAIL("store failure edges");
}

int main() {
    std::cout << "BaseRuntime branch tests:\n";

    testEntityTimerPositivePath();
    testFactoryHook();
    testLoadRestoreDestroyEdges();
    testPumpInboundBatches();
    testAutoSaveTickEdges();
    testFlushSkipsClearedEntity();
    testCellReadyDestroyedGuards();
    testRequestSendFailures();
    testSyncToCellsSkipsUnbound();
    testAoIEnterVariants();
    testAoILeaveVariants();
    testSpawnRequestTruncations();
    testClientEventMatrix();
    testWitnessSyncMatrix();
    testSpaceChangedNoCallback();
    testCellCallEdgeVariants();
    testPropertySyncUnknownEntityAndFlushFailure();
    testStoreFailureEdges();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
