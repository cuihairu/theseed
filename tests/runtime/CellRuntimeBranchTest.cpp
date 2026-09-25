// CellRuntime 分支覆盖补充：handleEntityAction/handleTeleport/handleDestroyCell/
// handlePropertySyncFromBase/handleCreateCell 的畸形与边界 payload、迁移全流程
// （beginMigration → transfer → commit → 路由窗口残留）、requestSpawnEntity、
// 空间增删、定时器取消与广播过滤。
#include "theseed/runtime/CellRuntime.h"
#include "theseed/runtime/EntityMigration.h"
#include "theseed/runtime/PropertyReplication.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <list>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using theseed::runtime::CellRuntime;
using theseed::runtime::Clock;
using theseed::runtime::ComponentId;
using theseed::runtime::DeliveryClass;
using theseed::runtime::Entity;
using theseed::runtime::EntityDef;
using theseed::runtime::EntityId;
using theseed::runtime::EntityMigration;
using theseed::runtime::EntitySide;
using theseed::runtime::InMemoryRuntimeTransport;
using theseed::runtime::PropertyDelta;
using theseed::runtime::PropertyReplication;
using theseed::runtime::PropertyType;
using theseed::runtime::RuntimeInvocation;
using theseed::runtime::SendResult;
using theseed::runtime::SingleCellTopology;
using theseed::runtime::Space;
using theseed::runtime::SpaceConfig;
using theseed::runtime::SpaceId;
using theseed::runtime::SpaceRuntime;
using theseed::runtime::TickScheduler;
using theseed::runtime::Vector3;

namespace {

int g_checked = 0;

int fail(const char* stage) {
    std::cerr << "cell_runtime_branch_test_failed_at=" << stage << '\n';
    return EXIT_FAILURE;
}

RuntimeInvocation makeInv(EntityId entityId, std::uint32_t targetComponent,
                          const std::string& entityType, std::string method,
                          std::vector<std::byte> payload) {
    RuntimeInvocation inv;
    inv.entityId = entityId;
    inv.targetComponent = targetComponent;
    inv.entityType = entityType;
    inv.method = std::move(method);
    inv.deliveryClass = DeliveryClass::ORDERED_RELIABLE;
    inv.payload = std::move(payload);
    return inv;
}

void appendU32(std::vector<std::byte>& out, std::uint32_t v) {
    const auto at = out.size();
    out.resize(at + sizeof(v));
    std::memcpy(out.data() + at, &v, sizeof(v));
}

void appendU64(std::vector<std::byte>& out, std::uint64_t v) {
    const auto at = out.size();
    out.resize(at + sizeof(v));
    std::memcpy(out.data() + at, &v, sizeof(v));
}

void appendF32(std::vector<std::byte>& out, float v) {
    const auto at = out.size();
    out.resize(at + sizeof(v));
    std::memcpy(out.data() + at, &v, sizeof(v));
}

// entity.action wire: nameLen(4) + name + dataLen(4) + data
std::vector<std::byte> actionWire(const std::string& name, const std::string& data) {
    std::vector<std::byte> out;
    out.reserve(8 + name.size() + data.size());
    const auto nameLen = static_cast<std::uint32_t>(name.size());
    appendU32(out, nameLen);
    out.resize(out.size() + nameLen);
    std::memcpy(out.data() + 4, name.data(), nameLen);
    const auto dataLen = static_cast<std::uint32_t>(data.size());
    appendU32(out, dataLen);
    if (dataLen > 0) {
        out.resize(out.size() + dataLen);
        std::memcpy(out.data() + 8 + nameLen, data.data(), dataLen);
    }
    return out;
}

// entity.teleport wire: entityId(8) + spaceId(8，SpaceId 是 uint64) + posX/Y/Z(4*3) = 28
std::vector<std::byte> teleportWire(EntityId entityId, SpaceId spaceId, Vector3 pos) {
    std::vector<std::byte> out;
    out.reserve(28);
    appendU64(out, entityId);
    appendU64(out, spaceId);
    appendF32(out, pos.x);
    appendF32(out, pos.y);
    appendF32(out, pos.z);
    return out;
}

// 与 CellRuntime.cpp 匿名 ns 的 CellCreationPayload 字段序一致：
// spaceId(8, SpaceId) + entityId(8) + baseComponentId(4) + posX/Y/Z(4*3) = 32
// （SpaceId 是 uint64；写成更窄的类型会因结构体 padding 读进栈垃圾）
struct CreateCellWire final {
    SpaceId spaceId = 0;
    EntityId entityId = 0;
    std::uint32_t baseComponentId = 0;
    float posX = 0;
    float posY = 0;
    float posZ = 0;
};

std::vector<std::byte> createCellWire(std::uint32_t spaceId, EntityId entityId,
                                      std::uint32_t baseComponentId, Vector3 pos) {
    CreateCellWire w;
    w.spaceId = spaceId;
    w.entityId = entityId;
    w.baseComponentId = baseComponentId;
    w.posX = pos.x;
    w.posY = pos.y;
    w.posZ = pos.z;
    std::vector<std::byte> out(sizeof(w));
    std::memcpy(out.data(), &w, sizeof(w));
    return out;
}

std::vector<std::byte> epochWire(std::uint64_t epoch) {
    std::vector<std::byte> out(sizeof(epoch));
    std::memcpy(out.data(), &epoch, sizeof(epoch));
    return out;
}

// 清空 transport 全部待收消息（drain(nullptr, 0) 是空操作，不能用来清场）
void clearTransport(InMemoryRuntimeTransport& transport) {
    std::array<RuntimeInvocation, 64> sink{};
    while (transport.drain(sink.data(), sink.size()) > 0) {
    }
}

// 逐条 drain 并逐字段断言
bool drainMatches(InMemoryRuntimeTransport& transport, EntityId entityId,
                  std::uint32_t targetComponent, const std::string& method) {
    std::array<RuntimeInvocation, 8> drained{};
    const auto count = transport.drain(drained.data(), drained.size());
    for (std::size_t i = 0; i < count; ++i) {
        if (drained[i].entityId == entityId && drained[i].targetComponent == targetComponent &&
            drained[i].method == method) {
            return true;
        }
    }
    std::cerr << "  expected " << method << " (entity=" << entityId
              << ", target=" << targetComponent << ") in " << count << " drained\n";
    return false;
}

// send 一律拒绝的 transport：用于触发 beginMigration 的 send 失败分支
class RejectingTransport final : public theseed::runtime::IRuntimeTransport {
public:
    theseed::runtime::SendResult send(RuntimeInvocation) override {
        return theseed::runtime::SendResult::BackPressure;
    }
    std::size_t receive(ComponentId, RuntimeInvocation*, std::size_t) override { return 0; }
    std::size_t pendingCount() const override { return 0; }
    void flush() override {}
    theseed::runtime::TransportStats stats() const override { return {}; }
};

// send 行为可开关的 transport：默认转发给内部 InMemory，reject_ 置真后拒绝，
// 用于触发迁移路由转发时的 send 失败分支
class SwitchableTransport final : public theseed::runtime::IRuntimeTransport {
public:
    explicit SwitchableTransport(std::shared_ptr<InMemoryRuntimeTransport> inner)
        : inner_(std::move(inner)) {}

    SendResult send(RuntimeInvocation invocation) override {
        if (reject_.load(std::memory_order_relaxed)) {
            return SendResult::Closed;
        }
        return inner_->send(std::move(invocation));
    }
    std::size_t receive(ComponentId targetComponent, RuntimeInvocation* out,
                        std::size_t capacity) override {
        return inner_->receive(targetComponent, out, capacity);
    }
    std::size_t pendingCount() const override { return inner_->pendingCount(); }
    void flush() override { inner_->flush(); }
    theseed::runtime::TransportStats stats() const override { return inner_->stats(); }

    std::atomic<bool> reject_{false};
    InMemoryRuntimeTransport& inner() { return *inner_; }

private:
    std::shared_ptr<InMemoryRuntimeTransport> inner_;
};

}  // namespace

int main() {
    EntityDef def("Avatar");
    const auto hpId = def.addProperty("hp", PropertyType::Int32, sizeof(std::int32_t));
    def.addMethod("castSpell", theseed::runtime::MethodSide::Cell);

    EntityDef monsterDef("Monster");
    monsterDef.addProperty("hp", PropertyType::Int32, sizeof(std::int32_t));

    // factory 返回 null 的类型：迁移接收按 snapshot.entityType 查 factory
    EntityDef brokenDef("Broken");

    // 实体加入 Space 后被 Space 持指针；必须活到 main 结束，否则 sync 阶段扫到悬垂
    std::list<Entity> keepAlive;

    auto transport = std::make_shared<InMemoryRuntimeTransport>();

    auto makeSpace = [](SpaceId id, const char* name) {
        auto topology = std::make_unique<SingleCellTopology>(id);
        auto space = std::make_unique<Space>(id, name, std::move(topology));
        space->initialize(SpaceConfig{.name = name});
        return std::make_unique<SpaceRuntime>(std::move(space));
    };
    CellRuntime cellA(makeSpace(100, "a_space"), transport, 11);
    CellRuntime cellB(makeSpace(200, "b_space"), transport, 22);
    // send 一律拒绝的独立 runtime：触发 beginMigration 的 send 失败分支
    CellRuntime rejectingRuntime(makeSpace(400, "reject_space"),
                                 std::make_shared<RejectingTransport>(), 33);
    // send 可开关的 runtime：触发迁移路由转发时的 send 拒绝
    auto switchTransport = std::make_shared<SwitchableTransport>(
        std::make_shared<InMemoryRuntimeTransport>());
    CellRuntime switchRuntime(makeSpace(600, "sw_space"), switchTransport, 77);
    // 迁移路由窗口 TTL 1ms：触发路由过期 drop 分支
    CellRuntime shortTtlRuntime(makeSpace(700, "ttl_space"), transport, 88,
                                std::chrono::milliseconds{1});

    auto avatarFactory = [&](EntityId id, EntitySide side) {
        return std::make_unique<Entity>(id, side, def);
    };
    if (!cellA.registerEntityFactory("Avatar", avatarFactory) ||
        !cellB.registerEntityFactory("Avatar", avatarFactory) ||
        !rejectingRuntime.registerEntityFactory("Avatar", avatarFactory) ||
        !switchRuntime.registerEntityFactory("Avatar", avatarFactory) ||
        !shortTtlRuntime.registerEntityFactory("Avatar", avatarFactory)) {
        return fail("register_factory");
    }
    // 空类型名 / 空 factory 拒绝；重复注册为覆盖语义（insert_or_assign）
    if (cellA.registerEntityFactory("", avatarFactory)) return fail("empty_type_accepted");
    if (cellA.registerEntityFactory("Ghost", nullptr)) return fail("null_factory_accepted");
    if (!cellA.registerEntityFactory("Avatar", avatarFactory)) return fail("re_register");
    // factory 返回 null 的类型（迁移接收时触发 factory 空产出分支）
    if (!cellB.registerEntityFactory("Broken", [](EntityId, EntitySide) {
            return std::unique_ptr<Entity>{};
        })) {
        return fail("register_broken_factory");
    }
    ++g_checked;

    int hookCalls = 0;
    cellA.setEntityFactoryHook([&hookCalls](Entity&) { hookCalls += 1; });
    cellB.setEntityFactoryHook([&hookCalls](Entity&) { hookCalls += 1; });

    TickScheduler scheduler(std::chrono::milliseconds{0});
    cellA.attach(scheduler);
    cellB.attach(scheduler);

    auto addLocal = [&](CellRuntime& runtime, Entity& entity, Vector3 pos) {
        runtime.addEntity(entity, pos);
        entity.activate();
    };

    // ---- A. handleEntityAction ----------------------------------------
    {
        auto& target = keepAlive.emplace_back(201, EntitySide::Cell, def);
        std::string gotAction;
        std::string gotData;
        target.setActionHandler([&](Entity&, std::string_view action, std::span<const std::byte> payload) {
            gotAction = std::string(action);
            gotData.assign(reinterpret_cast<const char*>(payload.data()), payload.size());
        });
        addLocal(cellA, target, Vector3{1.0F, 0.0F, 0.0F});

        auto& dormant = keepAlive.emplace_back(202, EntitySide::Cell, def);
        cellA.addEntity(dormant, Vector3{2.0F, 0.0F, 0.0F});  // 不 activate

        RuntimeInvocation inv = makeInv(201, 11, "Avatar", "entity.action", {});
        if (cellA.handleEntityAction(inv)) return fail("action_empty_payload");
        inv.payload = {std::byte{0x03}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};  // nameLen=3 无 name
        if (cellA.handleEntityAction(inv)) return fail("action_truncated_name");
        inv.payload = actionWire("fire", "");
        inv.payload.pop_back();  // 截掉 dataLen 尾字节
        inv.payload.pop_back();
        inv.payload.pop_back();  // dataLen 只剩 1 字节
        if (cellA.handleEntityAction(inv)) return fail("action_truncated_datalen");
        inv.payload = actionWire("fire", "xy");
        const std::uint32_t bogusLen = 0xFFFFFFF0U;
        std::memcpy(inv.payload.data() + 8, &bogusLen, sizeof(bogusLen));  // dataLen 槽位超界
        if (cellA.handleEntityAction(inv)) return fail("action_datalen_overrun");

        inv.payload = actionWire("fire", "xy");
        inv.entityId = 999;
        if (cellA.handleEntityAction(inv)) return fail("action_unknown_entity");
        inv.entityId = 202;
        if (cellA.handleEntityAction(inv)) return fail("action_inactive_entity");
        ++g_checked;

        inv.entityId = 201;
        if (!cellA.handleEntityAction(inv)) return fail("action_ok");
        if (target.pendingInputCount() != 1) return fail("action_queued");
        target.processInput();
        if (gotAction != "fire" || gotData != "xy") return fail("action_dispatch_content");
        ++g_checked;

        // dataLen=0 分支：不进入 memcpy
        inv.payload = actionWire("jump", "");
        if (!cellA.handleEntityAction(inv)) return fail("action_zero_data");
        target.processInput();
        if (gotAction != "jump" || !gotData.empty()) return fail("action_zero_data_content");
        ++g_checked;
    }

    // ---- B. handleTeleport ---------------------------------------------
    {
        auto& traveler = keepAlive.emplace_back(211, EntitySide::Cell, def);
        traveler.bindBaseEntityCall(77);
        addLocal(cellA, traveler, Vector3{0.0F, 0.0F, 0.0F});
        if (!cellA.createSpace(300, "dest300")) return fail("teleport_make_space");

        if (cellA.handleTeleport(makeInv(211, 11, "Avatar", "entity.teleport", {}))) {
            return fail("teleport_short_payload");
        }
        auto wire = teleportWire(211, 300, Vector3{5.0F, 0.0F, 0.0F});
        // handleTeleport 从 payload 读 entityId（invocation.entityId 不参与查找）
        if (cellA.handleTeleport(makeInv(0, 11, "Avatar", "entity.teleport",
                                         teleportWire(999, 300, Vector3{5.0F, 0.0F, 0.0F})))) {
            return fail("teleport_unknown_entity");
        }
        if (cellA.handleTeleport(makeInv(211, 11, "Avatar", "entity.teleport",
                                         teleportWire(211, 100, Vector3{})))) {
            return fail("teleport_same_space");
        }
        ++g_checked;

        clearTransport(*transport);
        if (!cellA.handleTeleport(makeInv(211, 11, "Avatar", "entity.teleport", wire))) {
            return fail("teleport_cross_space");
        }
        if (cellA.findEntitySpace(211) != 300) return fail("teleport_space_map");
        if (!drainMatches(*transport, 211, 77, "entity.spaceChanged")) {
            return fail("teleport_space_changed");
        }
        ++g_checked;

        // 目标空间不存在 → 自动 createSpace
        if (!cellA.handleTeleport(makeInv(211, 11, "Avatar", "entity.teleport",
                                          teleportWire(211, 400, Vector3{})))) {
            return fail("teleport_autocreate");
        }
        if (cellA.findSpaceRuntime(400) == nullptr) return fail("teleport_autocreate_space");
        if (cellA.findEntitySpace(211) != 400) return fail("teleport_autocreate_map");
        clearTransport(*transport);
        ++g_checked;
    }

    // ---- C. handlePropertySyncFromBase ---------------------------------
    {
        auto& synced = keepAlive.emplace_back(221, EntitySide::Cell, def);
        addLocal(cellA, synced, Vector3{3.0F, 0.0F, 0.0F});

        if (cellA.handlePropertySyncFromBase(makeInv(221, 11, "Avatar", "property.syncToCell", {}))) {
            return fail("propsync_empty");
        }
        if (cellA.handlePropertySyncFromBase(makeInv(999, 11, "Avatar", "property.syncToCell",
                                                     {std::byte{0x01}}))) {
            return fail("propsync_unknown_entity");
        }
        // 尾部截断的 delta（count=1 但没有 header）→ decode 抛异常被吞
        std::vector<std::byte> garbage{std::byte{0x01}, std::byte{0x00}, std::byte{0x00}};
        if (cellA.handlePropertySyncFromBase(makeInv(221, 11, "Avatar", "property.syncToCell", garbage))) {
            return fail("propsync_malformed");
        }
        ++g_checked;

        std::vector<std::byte> hpValue(sizeof(std::int32_t));
        std::int32_t hp = 7;
        std::memcpy(hpValue.data(), &hp, sizeof(hp));
        const std::vector<PropertyDelta> deltas{{hpId, hpValue}};
        auto delta = PropertyReplication::encodeDelta(deltas);
        if (!cellA.handlePropertySyncFromBase(makeInv(221, 11, "Avatar", "property.syncToCell", delta))) {
            return fail("propsync_ok");
        }
        if (synced.getProperty<std::int32_t>(hpId) != 7) return fail("propsync_value");
        clearTransport(*transport);
        ++g_checked;
    }

    // ---- D. handleDestroyCell ------------------------------------------
    {
        auto& doomed = keepAlive.emplace_back(231, EntitySide::Cell, def);
        addLocal(cellA, doomed, Vector3{4.0F, 0.0F, 0.0F});
        const auto doomedTimer = cellA.addEntityTimer(231, std::chrono::milliseconds{100}, [] {});
        static_cast<void>(doomedTimer);

        if (cellA.handleDestroyCell(makeInv(231, 11, "Avatar", "entity.destroyCell", {}))) {
            return fail("destroy_short_payload");
        }
        if (cellA.handleDestroyCell(makeInv(999, 11, "Avatar", "entity.destroyCell",
                                            epochWire(0)))) {
            return fail("destroy_unknown_entity");
        }
        // payload 只有 entityId 槽位（全零）→ baseComponentId=0 分支：不发 cellDestroyed
        if (!cellA.handleDestroyCell(makeInv(231, 11, "Avatar", "entity.destroyCell", epochWire(0)))) {
            return fail("destroy_no_base");
        }
        if (cellA.findEntity(231) != nullptr) return fail("destroy_removed");
        if (cellA.cancelTimer(doomedTimer)) return fail("destroy_timers_cancelled");
        std::array<RuntimeInvocation, 8> drained{};
        if (transport->drain(drained.data(), drained.size()) != 0) {
            return fail("destroy_no_base_no_message");
        }
        ++g_checked;

        auto& doomed2 = keepAlive.emplace_back(232, EntitySide::Cell, def);
        addLocal(cellA, doomed2, Vector3{5.0F, 0.0F, 0.0F});
        std::vector<std::byte> wire = epochWire(0);
        const std::uint32_t baseComponentId = 55;
        appendU32(wire, baseComponentId);
        if (!cellA.handleDestroyCell(makeInv(232, 11, "Avatar", "entity.destroyCell", wire))) {
            return fail("destroy_with_base");
        }
        if (!drainMatches(*transport, 232, 55, "entity.cellDestroyed")) {
            return fail("destroy_cell_destroyed_sent");
        }
        ++g_checked;
    }

    // ---- E. handleCreateCell -------------------------------------------
    {
        if (cellA.handleCreateCell(makeInv(241, 11, "Avatar", "entity.createCell", {std::byte{0x00}}))) {
            return fail("createcell_short_payload");
        }
        if (cellA.handleCreateCell(makeInv(241, 11, "Monster", "entity.createCell",
                                           createCellWire(0, 241, 66, Vector3{})))) {
            return fail("createcell_unregistered_type");
        }
        if (cellA.handleCreateCell(makeInv(211, 11, "Avatar", "entity.createCell",
                                           createCellWire(0, 211, 66, Vector3{})))) {
            return fail("createcell_id_conflict");
        }
        ++g_checked;

        const int hooksBefore = hookCalls;
        if (!cellA.handleCreateCell(makeInv(241, 11, "Avatar", "entity.createCell",
                                            createCellWire(0, 241, 66, Vector3{9.0F, 0.0F, 0.0F})))) {
            return fail("createcell_ok");
        }
        auto* created = cellA.findEntity(241);
        if (created == nullptr) return fail("createcell_findable");
        if (!created->isActive()) return fail("createcell_active");
        if (hookCalls != hooksBefore + 1) return fail("createcell_factory_hook");
        if (!drainMatches(*transport, 241, 66, "entity.cellReady")) {
            return fail("createcell_ready_sent");
        }
        ++g_checked;

        // spaceId 非零且不存在 → 自动建空间
        if (!cellA.handleCreateCell(makeInv(242, 11, "Avatar", "entity.createCell",
                                            createCellWire(500, 242, 66, Vector3{})))) {
            return fail("createcell_space_autocreate");
        }
        if (cellA.findSpaceRuntime(500) == nullptr) return fail("createcell_space_created");
        if (cellA.findEntitySpace(242) != 500) return fail("createcell_space_mapped");
        clearTransport(*transport);
        ++g_checked;

        // 附带属性快照：hp=42
        std::vector<std::byte> hpValue(sizeof(std::int32_t));
        const std::int32_t hp = 42;
        std::memcpy(hpValue.data(), &hp, sizeof(hp));
        const std::vector<PropertyDelta> snapshotDeltas{{hpId, hpValue}};
        auto snapshot = PropertyReplication::encodeDelta(snapshotDeltas);
        auto wire = createCellWire(0, 243, 66, Vector3{});
        wire.insert(wire.end(), snapshot.begin(), snapshot.end());
        if (!cellA.handleCreateCell(makeInv(243, 11, "Avatar", "entity.createCell", wire))) {
            return fail("createcell_with_snapshot");
        }
        if (cellA.findEntity(243)->getProperty<std::int32_t>(hpId) != 42) {
            return fail("createcell_snapshot_applied");
        }
        clearTransport(*transport);
        ++g_checked;
    }

    // ---- F. applyGhostSync 负分支 --------------------------------------
    {
        auto& real = keepAlive.emplace_back(251, EntitySide::Cell, def);
        addLocal(cellA, real, Vector3{6.0F, 0.0F, 0.0F});
        static_cast<void>(cellA.ensureRealGhost(real, 22));

        if (cellA.dispatchInvocation(makeInv(251, 999, "Avatar", "ghost.sync", {}))) {
            return fail("ghostsync_wrong_target");
        }
        if (cellA.dispatchInvocation(makeInv(999, 11, "Avatar", "ghost.sync", {}))) {
            return fail("ghostsync_unknown_entity");
        }
        // 实体存在但 ghost manager 是 real → false
        if (cellA.dispatchInvocation(makeInv(251, 11, "Avatar", "ghost.sync", {std::byte{0x00}}))) {
            return fail("ghostsync_real_side");
        }
        clearTransport(*transport);
        ++g_checked;
    }

    // ---- G. applyMigrationTransfer --------------------------------------
    {
        auto& migrant = keepAlive.emplace_back(261, EntitySide::Cell, def);
        migrant.bindBaseEntityCall(88);
        migrant.setProperty<std::int32_t>(hpId, 42);
        addLocal(cellA, migrant, Vector3{7.0F, 0.0F, 0.0F});

        auto snapshot = EntityMigration::capture(migrant, 9, 11, 22,
                                                 Vector3{7.0F, 0.0F, 0.0F}, 100);
        auto transferPayload = EntityMigration::encode(snapshot);

        // target 不匹配的两层拒绝：dispatch 层（具名 method 之前/之后都行）
        if (cellA.dispatchInvocation(makeInv(261, 999, "Avatar", "migration.transfer", transferPayload))) {
            return fail("migrate_transfer_wrong_target");
        }
        // target 匹配但 snapshot 的 targetComponent 是 22 ≠ 本机 11
        if (cellA.dispatchInvocation(makeInv(261, 11, "Avatar", "migration.transfer", transferPayload))) {
            return fail("migrate_transfer_snapshot_target");
        }
        // factory 未注册的实体类型
        auto& monsterEntity = keepAlive.emplace_back(262, EntitySide::Cell, monsterDef);
        auto monsterSnapshot = EntityMigration::capture(monsterEntity, 1, 11, 22,
                                                        Vector3{}, 100);
        if (cellB.dispatchInvocation(makeInv(262, 22, "Monster", "migration.transfer",
                                             EntityMigration::encode(monsterSnapshot)))) {
            return fail("migrate_transfer_factory_miss");
        }
        ++g_checked;

        // 正常接收：baseCall 有值 → cellReady + commit 两条；空间 100 在 B 不存在 → 回落默认 200
        if (!cellB.dispatchInvocation(makeInv(261, 22, "Avatar", "migration.transfer", transferPayload))) {
            return fail("migrate_transfer_accept");
        }
        auto* arrived = cellB.findEntity(261);
        if (arrived == nullptr) return fail("migrate_transfer_entity");
        if (cellB.findEntitySpace(261) != 200) return fail("migrate_transfer_space_fallback");
        if (arrived->getProperty<std::int32_t>(hpId) != 42) return fail("migrate_transfer_state");

        // drainMatches 会取走全部消息，两条回执需在同一次 drain 中分别断言
        std::array<RuntimeInvocation, 8> replies{};
        const auto replyCount = transport->drain(replies.data(), replies.size());
        bool gotReady = false;
        bool gotCommit = false;
        for (std::size_t i = 0; i < replyCount; ++i) {
            if (replies[i].entityId == 261 && replies[i].targetComponent == 88 &&
                replies[i].method == "entity.cellReady") {
                gotReady = true;
            }
            if (replies[i].entityId == 261 && replies[i].targetComponent == 11 &&
                replies[i].method == "migration.commit") {
                gotCommit = true;
            }
        }
        if (!gotReady) return fail("migrate_transfer_cell_ready");
        if (!gotCommit) return fail("migrate_transfer_commit");
        ++g_checked;

        // 同一实体重复 transfer → 已存在拒绝
        if (cellB.dispatchInvocation(makeInv(261, 22, "Avatar", "migration.transfer", transferPayload))) {
            return fail("migrate_transfer_duplicate");
        }
        ++g_checked;
    }

    // ---- H. beginMigration / applyMigrationCommit / 路由窗口 -------------
    {
        auto& escaper = keepAlive.emplace_back(271, EntitySide::Cell, def);
        escaper.bindBaseEntityCall(99);
        addLocal(cellA, escaper, Vector3{8.0F, 0.0F, 0.0F});

        if (cellA.beginMigration(271, 0, 5)) return fail("migrate_begin_zero_target");
        if (cellA.beginMigration(271, 11, 5)) return fail("migrate_begin_self_target");
        if (cellA.beginMigration(999, 22, 5)) return fail("migrate_begin_unknown_entity");
        ++g_checked;

        if (!cellA.beginMigration(271, 22, 5)) return fail("migrate_begin");
        if (!drainMatches(*transport, 271, 22, "migration.transfer")) {
            return fail("migrate_begin_transfer_sent");
        }

        // epoch 不匹配
        if (cellA.dispatchInvocation(makeInv(271, 11, "Avatar", "migration.commit", epochWire(6)))) {
            return fail("migrate_commit_epoch_mismatch");
        }
        // 无路由实体
        if (cellA.dispatchInvocation(makeInv(999, 11, "Avatar", "migration.commit", epochWire(5)))) {
            return fail("migrate_commit_no_route");
        }
        // 实体已被移除（Migrating 状态检查的前置：findEntity null）
        if (cellA.dispatchInvocation(makeInv(271, 22, "Avatar", "migration.commit", epochWire(5)))) {
            return fail("migrate_commit_wrong_target");
        }
        if (!cellA.dispatchInvocation(makeInv(271, 11, "Avatar", "migration.commit", epochWire(5)))) {
            return fail("migrate_commit");
        }
        if (cellA.findEntity(271) != nullptr) return fail("migrate_commit_destroyed");
        ++g_checked;

        // 提交后路由窗口有意保留：残留消息仍转发到目标 CellApp
        if (!cellA.dispatchInvocation(makeInv(271, 11, "Avatar", "castSpell", {}))) {
            return fail("route_window_forward");
        }
        if (!drainMatches(*transport, 271, 22, "castSpell")) {
            return fail("route_window_forward_target");
        }
        if (!cellA.clearMigrationRoute(271)) return fail("route_clear");
        if (cellA.clearMigrationRoute(271)) return fail("route_clear_twice");
        // 路由清除后同一消息不再转发，而是落入 findEntity null → false
        if (cellA.dispatchInvocation(makeInv(271, 11, "Avatar", "castSpell", {}))) {
            return fail("route_cleared_no_forward");
        }
        clearTransport(*transport);
        ++g_checked;

        // 实体移除后 commit：findEntity null 分支
        auto& gone = keepAlive.emplace_back(272, EntitySide::Cell, def);
        addLocal(cellA, gone, Vector3{9.0F, 0.0F, 0.0F});
        if (!cellA.beginMigration(272, 22, 3)) return fail("migrate_begin_272");
        cellA.removeEntity(272);
        if (cellA.dispatchInvocation(makeInv(272, 11, "Avatar", "migration.commit", epochWire(3)))) {
            return fail("migrate_commit_entity_gone");
        }
        cellA.clearMigrationRoute(272);
        clearTransport(*transport);
        ++g_checked;
    }

    // ---- I. requestSpawnEntity ------------------------------------------
    {
        auto& spawner = keepAlive.emplace_back(281, EntitySide::Cell, def);
        addLocal(cellA, spawner, Vector3{10.0F, 0.0F, 0.0F});
        if (cellA.requestSpawnEntity("Avatar", Vector3{}, 281)) return fail("spawn_no_base_call");
        if (cellA.requestSpawnEntity("Avatar", Vector3{}, 999)) return fail("spawn_unknown_creator");

        spawner.bindBaseEntityCall(99);
        if (!cellA.requestSpawnEntity("Avatar", Vector3{1.0F, 2.0F, 3.0F}, 281)) {
            return fail("spawn_ok");
        }
        if (!drainMatches(*transport, 281, 99, "entity.spawnRequest")) {
            return fail("spawn_sent");
        }
        ++g_checked;
    }

    // ---- J. 空间增删 -----------------------------------------------------
    {
        if (!cellA.createSpace(600, "dup")) return fail("space_create");
        if (cellA.createSpace(600, "dup")) return fail("space_create_duplicate");
        if (cellA.destroySpace(0)) return fail("space_destroy_default");
        if (cellA.destroySpace(555)) return fail("space_destroy_unknown");
        if (!cellA.destroySpace(600)) return fail("space_destroy");
        if (cellA.destroySpace(600)) return fail("space_destroy_twice");
        if (!cellA.createSpace(600, "reborn")) return fail("space_recreate");
        if (!cellA.destroySpace(600)) return fail("space_destroy_reborn");
        ++g_checked;
    }

    // ---- K. 定时器 -------------------------------------------------------
    {
        int fired = 0;
        const auto handle = cellA.addTimer(std::chrono::milliseconds{0}, [&fired] { fired += 1; });
        if (!cellA.cancelTimer(handle)) return fail("timer_cancel");
        if (cellA.cancelTimer(handle)) return fail("timer_cancel_twice");

        const auto t1 = cellA.addEntityTimer(282, std::chrono::milliseconds{0}, [&fired] { fired += 1; });
        const auto t2 = cellA.addEntityPeriodicTimer(282, std::chrono::milliseconds{0}, [&fired] { fired += 1; });
        static_cast<void>(t2);
        cellA.cancelEntityTimers(282);
        if (cellA.cancelTimer(t1)) return fail("entity_timers_cancelled");
        cellA.cancelEntityTimers(282);   // 幂等
        cellA.cancelEntityTimers(999);   // 未知实体无害

        cellA.addTimer(std::chrono::milliseconds{0}, [&fired] { fired += 1; });
        scheduler.runOnce();  // TimerPump → advanceTimers 触发零延迟回调
        if (fired != 1) return fail("timer_fired");
        clearTransport(*transport);
        ++g_checked;
    }

    // ---- L. 广播与查询 ---------------------------------------------------
    {
        auto& near = keepAlive.emplace_back(291, EntitySide::Cell, def);
        auto& far = keepAlive.emplace_back(292, EntitySide::Cell, def);
        addLocal(cellA, near, Vector3{0.0F, 0.0F, 0.0F});
        addLocal(cellA, far, Vector3{1000.0F, 0.0F, 0.0F});

        int nearEvents = 0;
        int farEvents = 0;
        near.subscribe("boom", [&](Entity&, std::string_view, std::span<const std::byte>) { nearEvents += 1; });
        far.subscribe("boom", [&](Entity&, std::string_view, std::span<const std::byte>) { farEvents += 1; });

        cellA.broadcastEvent("boom");
        if (nearEvents != 1 || farEvents != 1) return fail("broadcast_all");

        cellA.broadcastEventInRange("boom", Vector3{0.0F, 0.0F, 0.0F}, 10.0F);
        if (nearEvents != 2 || farEvents != 1) return fail("broadcast_range");

        // findEntitiesByTag/queryEntities/forEachEntity 只遍历 ownedEntities_
        // （factory/createCell 创建的实体），外部 addEntity 的引用实体不在其列
        auto* owned = cellA.findEntity(241);
        owned->addTag("tank");
        if (cellA.findEntitiesByTag("tank").size() != 1) return fail("query_by_tag");
        auto filtered = cellA.queryEntities([](const Entity& e) { return e.id() == 242; });
        if (filtered.size() != 1 || filtered.front()->id() != 242) return fail("query_predicate");
        std::size_t seen = 0;
        cellA.forEachEntity([&seen](Entity&) { seen += 1; });
        if (seen < 3) return fail("for_each_entity");
        if (cellA.findEntitySpace(999) != 0) return fail("find_entity_space_unknown");
        clearTransport(*transport);
        ++g_checked;
    }

    // ---- M. 转发与泵 ------------------------------------------------------
    {
        const std::vector<std::byte> payload{std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};
        if (cellA.forwardGhostMethod(999, "castSpell", payload)) return fail("forward_unknown_entity");

        if (cellA.pumpInbound() != 0) return fail("pump_empty");
        if (transport->send(makeInv(291, 11, "Avatar", "castSpell", payload)) != SendResult::Accepted) {
            return fail("pump_enqueue");
        }
        if (cellA.pumpInbound() != 1) return fail("pump_one");
        // 未绑定 handler 的实体收到 method：dispatchInvocation → entity dispatch 不崩即可
        clearTransport(*transport);
        ++g_checked;
    }

    // ---- N. 构造校验 / 访问器 / 迁移与 createCell 分支补充 ----------------
    {
        // 构造校验：空 spaceRuntime / 空 transport / 零 localComponentId
        bool threw = false;
        try {
            CellRuntime bad(std::unique_ptr<SpaceRuntime>{}, transport, 11);
            static_cast<void>(bad);
        } catch (const std::invalid_argument&) { threw = true; }
        if (!threw) return fail("ctor_null_space");

        threw = false;
        try {
            CellRuntime bad(makeSpace(300, "c_space"), std::shared_ptr<InMemoryRuntimeTransport>{}, 11);
            static_cast<void>(bad);
        } catch (const std::invalid_argument&) { threw = true; }
        if (!threw) return fail("ctor_null_transport");

        threw = false;
        try {
            CellRuntime bad(makeSpace(300, "c_space"), transport, 0);
            static_cast<void>(bad);
        } catch (const std::invalid_argument&) { threw = true; }
        if (!threw) return fail("ctor_zero_local_id");
        ++g_checked;

        // 访问器：spaceRuntime / transport / localComponentId / groupManager（const 与非 const）
        if (cellA.spaceRuntime().space().id() != 100) return fail("accessor_space_runtime");
        if (&cellA.transport() != static_cast<theseed::runtime::IRuntimeTransport*>(transport.get())) {
            return fail("accessor_transport");
        }
        if (cellA.localComponentId() != 11) return fail("accessor_local_id");
        static_cast<void>(&cellA.groupManager());
        static_cast<void>(&static_cast<const CellRuntime&>(cellA).groupManager());
        ++g_checked;

        // beginMigration 的 transport 拒绝分支（send != Accepted）
        auto& escaper3 = keepAlive.emplace_back(281, EntitySide::Cell, def);
        addLocal(rejectingRuntime, escaper3, Vector3{1.0F, 0.0F, 0.0F});
        if (rejectingRuntime.beginMigration(281, 44, 2)) return fail("migrate_begin_send_rejected");
        ++g_checked;

        // transfer 的 factory 返回 null 分支：snapshot.entityType="Broken" 的工厂产出空
        // （applyMigrationTransfer 按 snapshot.entityType 查 factory，与 invocation.entityType 无关）
        auto& wouldBe = keepAlive.emplace_back(263, EntitySide::Cell, brokenDef);
        auto brokenSnapshot = EntityMigration::capture(wouldBe, 4, 11, 22,
                                                       Vector3{3.0F, 0.0F, 0.0F}, 100);
        if (cellB.dispatchInvocation(makeInv(263, 22, "Broken", "migration.transfer",
                                             EntityMigration::encode(brokenSnapshot)))) {
            return fail("migrate_transfer_factory_null");
        }
        ++g_checked;

        // commit 载荷长度不为 8 字节：decodeMigrationEpoch 抛 invalid_argument
        auto& router = keepAlive.emplace_back(273, EntitySide::Cell, def);
        addLocal(cellA, router, Vector3{10.0F, 0.0F, 0.0F});
        if (!cellA.beginMigration(273, 22, 7)) return fail("migrate_begin_273");
        clearTransport(*transport);
        threw = false;
        try {
            auto shortEpoch = epochWire(7);
            shortEpoch.pop_back();
            cellA.dispatchInvocation(makeInv(273, 11, "Avatar", "migration.commit", shortEpoch));
        } catch (const std::invalid_argument&) { threw = true; }
        if (!threw) return fail("commit_epoch_size_mismatch");
        if (!cellA.clearMigrationRoute(273)) return fail("route_clear_273");
        clearTransport(*transport);
        ++g_checked;

        // 路由窗口内重建同 id Active 实体：commit 命中路由+epoch、实体在册但非
        // Migrating → 拒绝且重建实体保留
        auto& rebuilt = keepAlive.emplace_back(274, EntitySide::Cell, def);
        addLocal(cellA, rebuilt, Vector3{10.5F, 0.0F, 0.0F});
        if (!cellA.beginMigration(274, 22, 9)) return fail("migrate_begin_274");
        cellA.removeEntity(274);
        auto& revived = keepAlive.emplace_back(274, EntitySide::Cell, def);
        addLocal(cellA, revived, Vector3{10.6F, 0.0F, 0.0F});
        if (cellA.dispatchInvocation(makeInv(274, 11, "Avatar", "migration.commit", epochWire(9)))) {
            return fail("migrate_commit_not_migrating");
        }
        if (cellA.findEntity(274) == nullptr) return fail("migrate_commit_not_migrating_kept");
        cellA.clearMigrationRoute(274);
        clearTransport(*transport);
        ++g_checked;

        // createCell 尾部 snapshot 损坏（decodeDelta 抛）→ 吞掉异常，实体照常创建
        auto badSnapWire = createCellWire(0, 245, 55, Vector3{1.0F, 2.0F, 3.0F});
        badSnapWire.push_back(std::byte{0xFF});
        badSnapWire.push_back(std::byte{0xFF});
        badSnapWire.push_back(std::byte{0xFF});
        if (!cellA.handleCreateCell(makeInv(245, 11, "Avatar", "entity.createCell", badSnapWire))) {
            return fail("create_cell_bad_snapshot");
        }
        auto* snapped = cellA.findEntity(245);
        if (snapped == nullptr) return fail("create_cell_bad_snapshot_entity");
        if (snapped->getProperty<std::int32_t>(hpId) != 0) return fail("create_cell_bad_snapshot_state");
        ++g_checked;

        // createCell 实体的定时器接线：零延迟 one-shot 与 periodic 都经 tick 触发
        auto* timerEntity = cellA.findEntity(243);
        if (timerEntity == nullptr) return fail("create_cell_timer_entity");
        int timerFired = 0;
        auto oneShotHandle = timerEntity->addTimer(std::chrono::milliseconds{0},
                                                   [&timerFired](Entity&) { timerFired += 1; });
        int periodicFired = 0;
        auto periodicHandle = timerEntity->addPeriodicTimer(std::chrono::milliseconds{0},
                                                            [&periodicFired](Entity&) { periodicFired += 1; });
        scheduler.runOnce();
        if (timerFired != 1) return fail("cell_entity_timer_one_shot");
        if (periodicFired < 1) return fail("cell_entity_timer_periodic");
        // 本块结束后还有多处 runOnce：periodic 每拍都会触发，
        // 引用捕获的计数器届时已出 scope，必须在此取消。
        cellA.cancelTimer(oneShotHandle);
        cellA.cancelTimer(periodicHandle);
        ++g_checked;
    }

    // ---- O. dispatch 具名分发 / 迁移 TTL / ghost sync / 路由转发拒绝 -----
    {
        // const spaceRuntime() 访问器
        if (static_cast<const CellRuntime&>(cellA).spaceRuntime().space().id() != 100) {
            return fail("accessor_space_runtime_const");
        }
        ++g_checked;

        // dispatch 具名分发：entity.action / entity.teleport（跨空间到已存在的 400）
        auto& dispatcher = keepAlive.emplace_back(313, EntitySide::Cell, def);
        std::string gotAction;
        dispatcher.setActionHandler(
            [&](Entity&, std::string_view action, std::span<const std::byte>) {
                gotAction = std::string(action);
            });
        addLocal(cellA, dispatcher, Vector3{2.0F, 0.0F, 0.0F});
        if (!cellA.dispatchInvocation(makeInv(313, 11, "Avatar", "entity.action",
                                              actionWire("fire", "")))) {
            return fail("dispatch_action");
        }
        if (dispatcher.pendingInputCount() != 1) return fail("dispatch_action_queued");
        dispatcher.processInput();
        if (gotAction != "fire") return fail("dispatch_action_handler");
        if (!cellA.dispatchInvocation(makeInv(313, 11, "Avatar", "entity.teleport",
                                              teleportWire(313, 400, Vector3{3, 0, 0})))) {
            return fail("dispatch_teleport");
        }
        // dispatch 未知 method：target 不匹配
        if (cellA.dispatchInvocation(makeInv(313, 999, "Avatar", "nope.method", {}))) {
            return fail("dispatch_unknown_method_target");
        }
        // dispatch 未知 method + target 匹配 + 未知实体
        if (cellA.dispatchInvocation(makeInv(999, 11, "Avatar", "nope.method", {}))) {
            return fail("dispatch_unknown_entity");
        }
        clearTransport(*transport);
        ++g_checked;

        // beginMigration：实体仍在 Space 名册但 coordinateSystem 节点丢失
        // （entityPosition 无值）→ false
        auto& lost = keepAlive.emplace_back(305, EntitySide::Cell, def);
        addLocal(cellA, lost, Vector3{4.0F, 0.0F, 0.0F});
        cellA.spaceRuntime().space().coordinateSystem().remove(305);
        if (cellA.beginMigration(305, 22, 1)) return fail("migrate_begin_no_position");
        cellA.removeEntity(305);
        ++g_checked;

        // forwardGhostMethod：manager 为 real 侧 → forwardToReal 返回空 → false
        auto& ghostOwner = keepAlive.emplace_back(306, EntitySide::Cell, def);
        addLocal(cellA, ghostOwner, Vector3{5.0F, 0.0F, 0.0F});
        cellA.ensureRealGhost(ghostOwner, 44);
        if (cellA.forwardGhostMethod(306, "castSpell", {})) return fail("forward_real_manager");
        ++g_checked;

        // syncRealGhosts：real ghost + 非 Active owner → continue
        ghostOwner.beginDestroy();
        scheduler.runOnce();
        ++g_checked;

        // syncRealGhosts：real ghost + staged delta → 向 ghost 推 ghost.sync
        auto& synced = keepAlive.emplace_back(307, EntitySide::Cell, def);
        addLocal(cellA, synced, Vector3{6.0F, 0.0F, 0.0F});
        cellA.ensureRealGhost(synced, 45);
        synced.setProperty<std::int32_t>(hpId, 77);
        scheduler.runOnce();  // stage delta + syncRealGhosts 入队 + flush 发送
        if (!drainMatches(*transport, 307, 45, "ghost.sync")) {
            return fail("ghost_sync_delta_sent");
        }
        clearTransport(*transport);
        ++g_checked;

        // syncRealGhosts：real ghost owner 从未 addEntity（不在 entitySpaceMap_）
        // → delta 查找失败 → continue
        auto& stray = keepAlive.emplace_back(314, EntitySide::Cell, def);
        stray.activate();
        cellA.ensureRealGhost(stray, 46);
        scheduler.runOnce();  // syncRealGhosts → delta lambda map 查找失败
        clearTransport(*transport);
        ++g_checked;

        // syncRealGhosts：owner 的 entitySpaceMap_ 指向已销毁的空间。
        // 实体被直接从 Space 名册移除（绕过 CellRuntime::removeEntity），
        // destroySpace 只清名册内实体 → map 条目与 ghost binding 残留，
        // findSpaceRuntime 返回空 → continue
        if (!cellA.createSpace(700, "orphan_space")) return fail("create_orphan_space");
        auto& orphan = keepAlive.emplace_back(316, EntitySide::Cell, def);
        cellA.addEntity(orphan, Vector3{7.0F, 0.0F, 0.0F}, 700);
        orphan.activate();
        cellA.ensureRealGhost(orphan, 48);
        cellA.findSpaceRuntime(700)->space().removeEntity(316);
        if (!cellA.destroySpace(700)) return fail("destroy_orphan_space");
        scheduler.runOnce();  // syncRealGhosts → 空间已销毁 → continue
        clearTransport(*transport);
        ++g_checked;

        // routeMigratingInvocation：路由未过期但 forward send 被拒绝 → false
        auto& fwd = keepAlive.emplace_back(311, EntitySide::Cell, def);
        addLocal(switchRuntime, fwd, Vector3{1.0F, 0.0F, 0.0F});
        if (!switchRuntime.beginMigration(311, 88, 1)) return fail("switch_migrate_begin");
        switchTransport->reject_.store(true);
        if (switchRuntime.dispatchInvocation(makeInv(311, 77, "Avatar", "castSpell", {}))) {
            return fail("route_forward_rejected");
        }
        switchTransport->reject_.store(false);
        ++g_checked;

        // 路由窗口过期（TTL 1ms）：drop + 计数 + 路由被清除
        auto& exp = keepAlive.emplace_back(312, EntitySide::Cell, def);
        addLocal(shortTtlRuntime, exp, Vector3{1.0F, 0.0F, 0.0F});
        if (!shortTtlRuntime.beginMigration(312, 89, 2)) return fail("ttl_migrate_begin");
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
        if (shortTtlRuntime.dispatchInvocation(makeInv(312, 88, "Avatar", "castSpell", {}))) {
            return fail("route_expired_drop");
        }
        // 过期 drop 顺带清除路由：再 clear 应 false
        if (shortTtlRuntime.clearMigrationRoute(312)) return fail("route_expired_erased");
        clearTransport(*transport);
        ++g_checked;
    }

    // ---- P. 泵路径（syncToBases / flushClientEvents / AoI / witness）与残留失败臂 ----
    {
        // P1. map 残留指向已销毁空间：findEntity / removeEntity 的 sr null 臂
        if (!cellA.createSpace(810, "p_space")) return fail("p_make_space");
        auto& pOrphan = keepAlive.emplace_back(401, EntitySide::Cell, def);
        cellA.addEntity(pOrphan, Vector3{1.0F, 0.0F, 0.0F}, 810);
        pOrphan.activate();
        cellA.findSpaceRuntime(810)->space().removeEntity(401);  // 名册直删，map 残留
        if (!cellA.destroySpace(810)) return fail("p_destroy_space");  // 名册无 401，map 条目残留
        if (cellA.findEntity(401) != nullptr) return fail("p_find_entity_stale_map");
        cellA.removeEntity(401);  // map 残留条目清理：findEntity null + sr null 臂
        cellA.removeEntity(4242);  // 纯未知实体：map find miss 臂
        ++g_checked;

        // P2. teleportEntity 直调失败臂
        auto& tpSrc = keepAlive.emplace_back(402, EntitySide::Cell, def);
        addLocal(cellA, tpSrc, Vector3{2.0F, 0.0F, 0.0F});
        if (cellA.teleportEntity(402, 100, Vector3{})) return fail("p_tele_same_space");
        if (cellA.teleportEntity(4242, 200, Vector3{})) return fail("p_tele_unknown_entity");
        if (cellA.teleportEntity(402, 424242, Vector3{})) return fail("p_tele_no_target_space");
        cellA.spaceRuntime().space().removeEntity(402);  // map 在、名册缺 → findEntity null
        if (cellA.teleportEntity(402, 200, Vector3{})) return fail("p_tele_missing_from_roster");
        cellA.removeEntity(402);
        ++g_checked;

        // P3. 未 attach scheduler 的 runtime：createSpace/destroySpace 的 scheduler_ 判空臂
        {
            CellRuntime detached(makeSpace(900, "detached_space"), transport, 91);
            if (!detached.createSpace(901, "d2")) return fail("p_detached_create");
            if (!detached.destroySpace(901)) return fail("p_detached_destroy");
        }
        ++g_checked;

        // P4. createCell 实体的 timer lambda：实体销毁后触发 → findEntity null → cb 不执行
        {
            if (!cellA.handleCreateCell(makeInv(403, 11, "Avatar", "entity.createCell",
                                                createCellWire(0, 403, 71, Vector3{})))) {
                return fail("p_timer_cell_create");
            }
            auto* pTimerEntity = cellA.findEntity(403);
            if (pTimerEntity == nullptr) return fail("p_timer_entity");
            int neverFired = 0;
            pTimerEntity->addTimer(std::chrono::milliseconds{0},
                                   [&neverFired](Entity&) { neverFired += 1; });
            std::vector<std::byte> destroyWire = epochWire(0);
            const std::uint32_t pBase = 72;
            appendU32(destroyWire, pBase);
            if (!cellA.handleDestroyCell(makeInv(403, 11, "Avatar", "entity.destroyCell",
                                                  destroyWire))) {
                return fail("p_timer_destroy");
            }
            scheduler.runOnce();  // timer 触发：findEntity(403) 已 null → if(e) false 臂
            if (neverFired != 0) return fail("p_timer_should_not_fire");
            clearTransport(*transport);
        }
        ++g_checked;

        // P5. clientEvent 泵：createCell 实体 + emitToClient
        {
            if (!cellA.handleCreateCell(makeInv(411, 11, "Avatar", "entity.createCell",
                                                createCellWire(0, 411, 73, Vector3{})))) {
                return fail("p_ce_create");
            }
            auto* ceEntity = cellA.findEntity(411);
            if (ceEntity == nullptr) return fail("p_ce_entity");
            // 无 baseCall：泵 continue，事件积压
            ceEntity->emitToClient("orphan", std::span<const std::byte>{});
            scheduler.runOnce();
            clearTransport(*transport);
            // bind 后连同积压一起合包发出（含空名空 data 变体）
            ceEntity->bindBaseEntityCall(74);
            ceEntity->emitToClient("ui.ping", std::vector<std::byte>{std::byte{0x01}});
            ceEntity->emitToClient("", std::span<const std::byte>{});
            scheduler.runOnce();
            if (!drainMatches(*transport, 411, 74, "entity.clientEvent")) return fail("p_ce_sent");
            clearTransport(*transport);
        }
        ++g_checked;

        // P6. syncToBases 泵：createCell 实体 + 非 Cell flag 属性 delta（hp 未声明 flag）
        {
            if (!cellA.handleCreateCell(makeInv(412, 11, "Avatar", "entity.createCell",
                                                createCellWire(0, 412, 75, Vector3{})))) {
                return fail("p_sb_create");
            }
            auto* sbEntity = cellA.findEntity(412);
            if (sbEntity == nullptr) return fail("p_sb_entity");
            // 无 baseCall：dirty 但泵 continue
            sbEntity->setProperty<std::int32_t>(hpId, 5);
            scheduler.runOnce();
            clearTransport(*transport);
            sbEntity->bindBaseEntityCall(76);
            sbEntity->setProperty<std::int32_t>(hpId, 6);
            scheduler.runOnce();
            if (!drainMatches(*transport, 412, 76, "property.syncToBase")) return fail("p_sb_sent");
            clearTransport(*transport);
        }
        ++g_checked;

        // P7. AoI enter/leave + witness.propertySync 泵
        {
            if (!cellA.createSpace(820, "aoi_space")) return fail("p_aoi_space");
            auto& watcher = keepAlive.emplace_back(421, EntitySide::Cell, def);
            cellA.addEntity(watcher, Vector3{0.0F, 0.0F, 0.0F}, 820);
            watcher.activate();
            watcher.bindBaseEntityCall(78);
            auto& moving = keepAlive.emplace_back(422, EntitySide::Cell, def);
            cellA.addEntity(moving, Vector3{50.0F, 0.0F, 0.0F}, 820);  // 初始视图外
            moving.activate();
            moving.bindBaseEntityCall(79);

            auto& witness = cellA.findSpaceRuntime(820)->ensureWitness(watcher, 10.0F);
            scheduler.runOnce();  // 初始视图快照：moving 不在视图，无事件
            if (witness.entityInView(moving.id())) return fail("p_aoi_initial_out");
            clearTransport(*transport);

            // 进视图 → aoi.enter（typeName 非空 + pos flag=1 臂）
            cellA.findSpaceRuntime(820)->space().updateEntityPosition(
                moving.id(), Vector3{2.0F, 0.0F, 0.0F});
            scheduler.runOnce();
            if (!witness.entityInView(moving.id())) return fail("p_aoi_now_in");
            if (!drainMatches(*transport, 421, 78, "aoi.enter")) return fail("p_aoi_enter");

            // witness delta：属性脏、位置未动（hasPosition=false 臂）
            moving.setProperty<std::int32_t>(hpId, 33);
            scheduler.runOnce();
            if (!drainMatches(*transport, 421, 78, "witness.propertySync")) {
                return fail("p_witness_sync");
            }
            clearTransport(*transport);

            // 出视图 → aoi.leave
            cellA.findSpaceRuntime(820)->space().updateEntityPosition(
                moving.id(), Vector3{999.0F, 0.0F, 0.0F});
            scheduler.runOnce();
            if (!drainMatches(*transport, 421, 78, "aoi.leave")) return fail("p_aoi_leave");
            clearTransport(*transport);
        }
        ++g_checked;
    }

    // ---- Q. 分支覆盖补充：泵过滤臂、ghost 二次接线、AoI 残影、空类型名 ----
    {
        // Q1. addEntity 未知空间 → 早退，不入 entitySpaceMap_
        auto& drift = keepAlive.emplace_back(501, EntitySide::Cell, def);
        cellA.addEntity(drift, Vector3{1.0F, 0.0F, 0.0F}, 99999);
        if (cellA.findEntitySpace(501) != 0) return fail("q_add_entity_unknown_space");
        ++g_checked;

        // Q2. ensureRealGhost 二次调用（manager 已存在）；destroyGhost 后
        // syncRealGhosts 的 hasGhost 假臂；ensureGhostProxy 二次调用 +
        // proxy 实体过泵（!isReal → continue）
        auto& twincell = keepAlive.emplace_back(502, EntitySide::Cell, def);
        addLocal(cellA, twincell, Vector3{1.0F, 0.0F, 0.0F});
        auto& twinMgr = cellA.ensureRealGhost(twincell, 61);
        static_cast<void>(cellA.ensureRealGhost(twincell, 61));  // manager 已存在分支
        scheduler.runOnce();  // real ghost 无 staged delta → continue
        twinMgr.destroyGhost();
        auto& proxyHost = keepAlive.emplace_back(503, EntitySide::Cell, def);
        addLocal(cellA, proxyHost, Vector3{2.0F, 0.0F, 0.0F});
        static_cast<void>(cellA.ensureGhostProxy(proxyHost, 62));
        static_cast<void>(cellA.ensureGhostProxy(proxyHost, 62));  // manager 已存在分支
        scheduler.runOnce();  // proxy !isReal continue + real 无 ghost（hasGhost 假）continue
        clearTransport(*transport);
        ++g_checked;

        // Q3. migration.commit 时实体存在但非 Migrating：
        // beginMigration 因 send 被拒回滚（无路由），实体保持 Active
        if (rejectingRuntime.beginMigration(281, 44, 2)) return fail("q_begin_rejected");
        if (rejectingRuntime.dispatchInvocation(
                makeInv(281, 33, "Avatar", "migration.commit", epochWire(2)))) {
            return fail("q_commit_not_migrating");
        }
        ++g_checked;

        // Q4. handleCreateCell：factory 注册在但产出 null（"Broken"）
        if (cellB.handleCreateCell(makeInv(511, 22, "Broken", "entity.createCell",
                                           createCellWire(0, 511, 66, Vector3{})))) {
            return fail("q_createcell_factory_null");
        }
        ++g_checked;

        // Q5. createCell 实体销毁后 periodic timer 触发：findEntity null → cb 不执行
        {
            if (!cellA.handleCreateCell(makeInv(512, 11, "Avatar", "entity.createCell",
                                                createCellWire(0, 512, 81, Vector3{})))) {
                return fail("q_periodic_create");
            }
            auto* pe = cellA.findEntity(512);
            if (pe == nullptr) return fail("q_periodic_entity");
            int periodicNever = 0;
            const auto ph = pe->addPeriodicTimer(std::chrono::milliseconds{0},
                                                 [&periodicNever](Entity&) { periodicNever += 1; });
            if (!cellA.handleDestroyCell(makeInv(512, 11, "Avatar", "entity.destroyCell",
                                                 epochWire(0)))) {
                return fail("q_periodic_destroy");
            }
            scheduler.runOnce();  // periodic 触发 → findEntity null → 假臂
            if (periodicNever != 0) return fail("q_periodic_should_not_fire");
            cellA.cancelTimer(ph);
            clearTransport(*transport);
        }
        ++g_checked;

        // Q6. requestSpawnEntity：空类型名合法（typeLen=0 跳过 memcpy 后照常发送）
        if (!cellA.requestSpawnEntity("", Vector3{}, 281)) return fail("q_spawn_empty_type");
        ++g_checked;

        // Q7. owned 实体 beginDestroy 后：syncToBases / flushClientEvents
        // 均在 state != Active 处 continue（脏属性与 client 事件积压不清）
        {
            if (!cellA.handleCreateCell(makeInv(513, 11, "Avatar", "entity.createCell",
                                                createCellWire(0, 513, 82, Vector3{})))) {
                return fail("q_dead_create");
            }
            auto* dead = cellA.findEntity(513);
            if (dead == nullptr) return fail("q_dead_entity");
            dead->bindBaseEntityCall(83);
            dead->setProperty<std::int32_t>(hpId, 9);
            dead->emitToClient("x", std::span<const std::byte>{});
            dead->beginDestroy();
            scheduler.runOnce();
            clearTransport(*transport);
        }
        ++g_checked;

        // Q8. syncToBases：bind 了 base 但无 Cell 脏属性 → deltas 空 continue
        {
            if (!cellA.handleCreateCell(makeInv(514, 11, "Avatar", "entity.createCell",
                                                createCellWire(0, 514, 84, Vector3{})))) {
                return fail("q_nodelta_create");
            }
            auto* nd = cellA.findEntity(514);
            if (nd == nullptr) return fail("q_nodelta_entity");
            nd->bindBaseEntityCall(85);
            scheduler.runOnce();  // 无脏 → findStagedDelta 空 → continue
            clearTransport(*transport);
        }
        ++g_checked;

        // Q9. AoI / witness 泵的残影与缺件臂：观察者无 baseCall、target 直删、
        // 观察者直删、空类型名 target
        {
            if (!cellA.createSpace(830, "aoi2")) return fail("q_aoi2_space");
            auto& w1 = keepAlive.emplace_back(521, EntitySide::Cell, def);
            cellA.addEntity(w1, Vector3{0.0F, 0.0F, 0.0F}, 830);
            w1.activate();  // 不 bind base：flushAoIEvents/witness 的 baseCall 缺件臂
            static_cast<void>(cellA.findSpaceRuntime(830)->ensureWitness(w1, 10.0F));
            auto& mv1 = keepAlive.emplace_back(522, EntitySide::Cell, def);
            cellA.addEntity(mv1, Vector3{100.0F, 0.0F, 0.0F}, 830);
            mv1.activate();
            mv1.bindBaseEntityCall(91);
            scheduler.runOnce();
            clearTransport(*transport);

            // mv1 移入视图：观察者 521 无 baseCall → 事件与 witness 均在缺件处 continue
            cellA.findSpaceRuntime(830)->space().updateEntityPosition(mv1.id(),
                                                                      Vector3{1.0F, 0.0F, 0.0F});
            scheduler.runOnce();
            clearTransport(*transport);

            // target 直删（走 SpaceRuntime 层：正确触发观察者 onLeaveView 臂）
            cellA.findSpaceRuntime(830)->removeEntity(522);
            scheduler.runOnce();
            clearTransport(*transport);

            // 观察者直删（SpaceRuntime 层：trigger uninstall + witness detach + erase）
            cellA.findSpaceRuntime(830)->removeEntity(521);
            scheduler.runOnce();
            clearTransport(*transport);

            // 空类型名 target 进入有 baseCall 观察者的视图：aoi.enter 空名臂
            auto& w2 = keepAlive.emplace_back(523, EntitySide::Cell, def);
            cellA.addEntity(w2, Vector3{50.0F, 0.0F, 0.0F}, 830);
            w2.activate();
            w2.bindBaseEntityCall(92);
            static_cast<void>(cellA.findSpaceRuntime(830)->ensureWitness(w2, 10.0F));
            EntityDef emptyDef("");
            auto& nameless = keepAlive.emplace_back(524, EntitySide::Cell, emptyDef);
            cellA.addEntity(nameless, Vector3{100.0F, 0.0F, 0.0F}, 830);
            nameless.activate();
            scheduler.runOnce();
            clearTransport(*transport);
            cellA.findSpaceRuntime(830)->space().updateEntityPosition(nameless.id(),
                                                                      Vector3{51.0F, 0.0F, 0.0F});
            scheduler.runOnce();  // aoi.enter，typeName 为空 → 不写 name 字节
            clearTransport(*transport);
            if (!cellA.destroySpace(830)) return fail("q_aoi2_destroy");
        }
        ++g_checked;

        // Q9b. Enter 事件入队后 target 经 CellRuntime 层移除：w3 的 witness
        // binding 不随之清除，flush 时 enter 事件的 targetId 悬垂
        {
            if (!cellA.createSpace(831, "aoi3")) return fail("q_aoi3_space");
            auto& w3 = keepAlive.emplace_back(525, EntitySide::Cell, def);
            cellA.addEntity(w3, Vector3{0.0F, 0.0F, 0.0F}, 831);
            w3.activate();
            w3.bindBaseEntityCall(93);
            static_cast<void>(cellA.findSpaceRuntime(831)->ensureWitness(w3, 10.0F));
            auto& mv2 = keepAlive.emplace_back(526, EntitySide::Cell, def);
            cellA.addEntity(mv2, Vector3{100.0F, 0.0F, 0.0F}, 831);
            mv2.activate();
            mv2.bindBaseEntityCall(94);
            scheduler.runOnce();
            clearTransport(*transport);

            // mv2 移入 w3 视野：enter(525→526) 事件入队（尚未 flush）
            cellA.findSpaceRuntime(831)->space().updateEntityPosition(mv2.id(),
                                                                      Vector3{1.0F, 0.0F, 0.0F});
            // CellRuntime 层移除 target：清自身 binding 并对 w3 补 leave，
            // 队列中 enter 事件的 targetId 悬垂
            cellA.removeEntity(526);
            scheduler.runOnce();  // flushAoIEvents：observer 525 在册，findEntity(526) null
            clearTransport(*transport);
            if (!cellA.destroySpace(831)) return fail("q_aoi3_destroy");
        }
        ++g_checked;

        // Q10. broadcastEvent/broadcastEventInRange 扫过非 Active 实体 → state 臂
        // （Q7 的 513 已 beginDestroy 且仍在默认空间名册）
        cellA.broadcastEvent("q.boom");
        cellA.broadcastEventInRange("q.boom", Vector3{}, 1.0e9F);
        ++g_checked;

        // Q11. 第二轮缺口：teleport/beginMigration 残臂、transfer 无位置快照、
        // ghost.sync 无 binding、定时器闭包假臂（绕过 cancelEntityTimers）、
        // baseCall 清除后的各过滤臂
        {
            // Q11a. teleportEntity：map 残留指向已销毁空间 → oldSr null
            if (!cellA.createSpace(840, "q_dead_sr")) return fail("q11a_space");
            auto& ghosted = keepAlive.emplace_back(531, EntitySide::Cell, def);
            cellA.addEntity(ghosted, Vector3{}, 840);
            ghosted.activate();
            // 名册直删 → destroySpace 不清其 map 条目，空间销毁后条目悬空
            cellA.findSpaceRuntime(840)->space().removeEntity(531);
            if (!cellA.destroySpace(840)) return fail("q11a_destroy");
            if (cellA.teleportEntity(531, 100, Vector3{})) return fail("q11a_old_sr_null");
            cellA.removeEntity(531);  // 清残留 map 条目
            ++g_checked;

            // Q11b. teleportEntity：空间健在但名册缺实体 → findEntity null
            if (!cellA.createSpace(841, "q_roster")) return fail("q11b_space");
            auto& vanished = keepAlive.emplace_back(532, EntitySide::Cell, def);
            cellA.addEntity(vanished, Vector3{}, 841);
            vanished.activate();
            cellA.findSpaceRuntime(841)->space().removeEntity(532);
            if (cellA.teleportEntity(532, 100, Vector3{})) return fail("q11b_roster_missing");
            cellA.removeEntity(532);
            if (!cellA.destroySpace(841)) return fail("q11b_destroy");
            ++g_checked;

            // Q11c. teleport 成功但实体无 baseCall：不发 spaceChanged
            // （space 200 挂在 cellB 上；cellA 侧自建目标空间 843）
            if (!cellA.createSpace(843, "q11c_dest")) return fail("q11c_space");
            auto& quiet = keepAlive.emplace_back(533, EntitySide::Cell, def);
            addLocal(cellA, quiet, Vector3{3.0F, 0.0F, 0.0F});
            if (!cellA.teleportEntity(533, 843, Vector3{3.0F, 1.0F, 0.0F})) {
                return fail("q11c_teleport");
            }
            clearTransport(*transport);
            ++g_checked;

            // Q11d. beginMigration：实体存在但非 Active（beginDestroy 后）
            auto& retired = keepAlive.emplace_back(534, EntitySide::Cell, def);
            addLocal(cellA, retired, Vector3{4.0F, 0.0F, 0.0F});
            retired.beginDestroy();
            if (cellA.beginMigration(534, 22, 1)) return fail("q11d_not_active");
            ++g_checked;

            // Q11e. migration.transfer：快照无 position（capture 可选参不传），
            // 接收方 localComponentId 与快照 target 一致才能走到 position 检查
            auto& posless = keepAlive.emplace_back(535, EntitySide::Cell, def);
            addLocal(cellA, posless, Vector3{5.0F, 0.0F, 0.0F});
            auto poslessSnapshot =
                EntityMigration::capture(posless, 8, 11, 22, std::nullopt, 100);
            if (cellB.dispatchInvocation(makeInv(535, 22, "Avatar", "migration.transfer",
                                                 EntityMigration::encode(poslessSnapshot)))) {
                return fail("q11e_no_position");
            }
            cellA.removeEntity(535);
            ++g_checked;

            // Q11f. ghost.sync：实体存在但无 ghost binding → manager null 臂
            if (cellA.dispatchInvocation(makeInv(533, 11, "Avatar", "ghost.sync", {}))) {
                return fail("q11f_no_binding");
            }
            ++g_checked;

            // Q11g. createCell 定时器闭包：名册直删绕过 cancelEntityTimers，
            // 定时器仍触发但 findEntity 为 null → one-shot/periodic 的 if(e) 假臂
            {
                if (!cellA.handleCreateCell(makeInv(536, 11, "Avatar", "entity.createCell",
                                                    createCellWire(0, 536, 86, Vector3{})))) {
                    return fail("q11g_create");
                }
                auto* qe = cellA.findEntity(536);
                if (qe == nullptr) return fail("q11g_entity");
                int closureFired = 0;
                qe->addTimer(std::chrono::milliseconds{0},
                             [&closureFired](Entity&) { closureFired += 1; });
                qe->addPeriodicTimer(std::chrono::milliseconds{0},
                                     [&closureFired](Entity&) { closureFired += 1; });
                cellA.findSpaceRuntime(cellA.findEntitySpace(536))->space().removeEntity(536);
                scheduler.runOnce();  // 闭包触发 → findEntity null → cb 不执行
                if (closureFired != 0) return fail("q11g_should_not_fire");
                cellA.cancelEntityTimers(536);
                clearTransport(*transport);
            }
            ++g_checked;

            // Q11h. clearBaseEntityCall（baseCall 指针为 null）后的各过滤臂：
            // spawn 拒绝、teleport 不发 spaceChanged、syncToBases/flushClientEvents continue
            {
                if (!cellA.handleCreateCell(makeInv(537, 11, "Avatar", "entity.createCell",
                                                    createCellWire(0, 537, 87, Vector3{})))) {
                    return fail("q11h_create");
                }
                auto* be = cellA.findEntity(537);
                if (be == nullptr) return fail("q11h_entity");
                be->clearBaseEntityCall();
                if (cellA.requestSpawnEntity("Avatar", Vector3{}, 537)) {
                    return fail("q11h_spawn_no_base");
                }
                be->setProperty<std::int32_t>(hpId, 3);
                be->emitToClient("y", std::span<const std::byte>{});
                if (!cellA.teleportEntity(537, 843, Vector3{1.0F, 0.0F, 0.0F})) {
                    return fail("q11h_teleport");
                }
                scheduler.runOnce();
                clearTransport(*transport);
                if (!cellA.handleDestroyCell(makeInv(537, 11, "Avatar", "entity.destroyCell",
                                                     epochWire(0)))) {
                    return fail("q11h_destroy");
                }
            }
            ++g_checked;

            // Q11i. syncRealGhosts：owner 仍在（裸指针）但 staged delta 为空 → continue；
            // 顺带验证名册直删后 binding 残留的路径
            {
                if (!cellA.createSpace(842, "q_ghost_roster")) return fail("q11i_space");
                auto& stale = keepAlive.emplace_back(538, EntitySide::Cell, def);
                cellA.addEntity(stale, Vector3{}, 842);
                stale.activate();
                cellA.ensureRealGhost(stale, 63);
                cellA.findSpaceRuntime(842)->space().removeEntity(538);
                scheduler.runOnce();  // binding 在、owner Active、staged 空 → continue
                clearTransport(*transport);
                if (!cellA.destroySpace(842)) return fail("q11i_destroy");
            }
            ++g_checked;
        }
        ++g_checked;
    }

    cellB.detach(scheduler);
    cellA.detach(scheduler);
    std::cout << "cell_runtime_branch_test checks=" << g_checked << '\n';
    return EXIT_SUCCESS;
}
