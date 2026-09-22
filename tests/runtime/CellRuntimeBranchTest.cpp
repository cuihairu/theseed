// CellRuntime 分支覆盖补充：handleEntityAction/handleTeleport/handleDestroyCell/
// handlePropertySyncFromBase/handleCreateCell 的畸形与边界 payload、迁移全流程
// （beginMigration → transfer → commit → 路由窗口残留）、requestSpawnEntity、
// 空间增删、定时器取消与广播过滤。
#include "theseed/runtime/CellRuntime.h"
#include "theseed/runtime/EntityMigration.h"
#include "theseed/runtime/PropertyReplication.h"

#include <array>
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

    auto avatarFactory = [&](EntityId id, EntitySide side) {
        return std::make_unique<Entity>(id, side, def);
    };
    if (!cellA.registerEntityFactory("Avatar", avatarFactory) ||
        !cellB.registerEntityFactory("Avatar", avatarFactory) ||
        !rejectingRuntime.registerEntityFactory("Avatar", avatarFactory)) {
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
        timerEntity->addTimer(std::chrono::milliseconds{0},
                              [&timerFired](Entity&) { timerFired += 1; });
        int periodicFired = 0;
        timerEntity->addPeriodicTimer(std::chrono::milliseconds{0},
                                      [&periodicFired](Entity&) { periodicFired += 1; });
        scheduler.runOnce();
        if (timerFired != 1) return fail("cell_entity_timer_one_shot");
        if (periodicFired < 1) return fail("cell_entity_timer_periodic");
        ++g_checked;
    }

    cellB.detach(scheduler);
    cellA.detach(scheduler);
    std::cout << "cell_runtime_branch_test checks=" << g_checked << '\n';
    return EXIT_SUCCESS;
}
