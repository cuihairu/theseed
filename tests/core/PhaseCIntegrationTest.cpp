#include "theseed/core/CellApp.h"
#include "theseed/runtime/CellRuntime.h"
#include "theseed/runtime/BackupTopologyCoordinator.h"
#include "theseed/runtime/Entity.h"
#include "theseed/runtime/EntityDef.h"
#include "theseed/runtime/LoadProfiler.h"
#include "theseed/scripting/HotUpdate.h"
#include "theseed/runtime/RuntimeTransport.h"
#include "theseed/runtime/TickScheduler.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

using theseed::core::CellApp;
using theseed::runtime::EntityId;
using theseed::runtime::EntityLoadSnapshot;
using theseed::runtime::EntitySide;
using theseed::runtime::InMemoryRuntimeTransport;
using theseed::runtime::MethodSide;
using theseed::runtime::PropertyType;
using theseed::runtime::RuntimeInvocation;
using theseed::runtime::TickScheduler;
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

static void writeFile(const std::string& path, const std::string& content) {
    std::ofstream f(path);
    f << content;
}

static std::string createDefDir() {
    std::string dir = "test_phase_c_defs";
    std::filesystem::create_directory(dir);
    writeFile(dir + "/Avatar.xml",
        "<EntityDef name=\"Avatar\">"
        "  <Properties><Property name=\"level\" type=\"Int32\"/></Properties>"
        "  <Methods><Method name=\"onDamage\" side=\"Cell\"/></Methods>"
        "</EntityDef>");
    return dir;
}

// Phase C-2: CellRuntime 把实体分发负载接入 EntityLoadProfiler。E2E 验证
// dispatch 后 loadProfiler() 能读到该实体的类型与负载（跨 tick EMA 推进）。
static void testLoadProfilerWiredIntoCellRuntime() {
    TEST("Phase C-2: EntityLoadProfiler wired into CellRuntime dispatch");

    auto dir = createDefDir();
    auto transport = std::make_shared<InMemoryRuntimeTransport>();

    CellApp::Config config;
    config.entityDefPath = dir;
    config.componentId = 2;
    CellApp app(config, transport);
    if (!app.init()) { std::filesystem::remove_all(dir); FAIL("init"); return; }

    auto* entity = app.createEntity("Avatar", Vector3{1, 0, 1});
    if (!entity) { std::filesystem::remove_all(dir); FAIL("createEntity"); return; }
    entity->activate();
    int dispatchCount = 0;
    entity->bindMethodHandler("onDamage",
        [&dispatchCount](theseed::runtime::Entity&, std::span<const std::byte>) {
            ++dispatchCount;
            // 确定性忙等：让 Scope 记录到非零 wall-clock 耗时。无实际业务
            // 载荷时 rawLoad 可能为 0，旧断言（>=0）对空转恒真，测不出
            // dispatch 路径是否真的计量了负载（审核发现 vacuous）。
            volatile std::uint64_t sink = 0;
            for (std::uint64_t i = 0; i < 2000000ULL; ++i) {
                sink += i;
            }
            (void)sink;
        });

    // dispatch 一条实体方法，触发 loadProfiler Scope。
    RuntimeInvocation inv;
    inv.entityId = entity->id();
    inv.targetComponent = 2;
    inv.entityType = "Avatar";
    inv.method = "onDamage";
    inv.deliveryClass = theseed::runtime::DeliveryClass::ORDERED_RELIABLE;
    std::vector<std::byte> payload{std::byte{0x01}};
    inv.payload = payload;

    auto& profiler = app.runtime().loadProfiler();
    const auto before = profiler.trackedCount();

    bool dispatched = app.runtime().dispatchInvocation(inv);
    if (!dispatched || dispatchCount != 1) { std::filesystem::remove_all(dir); FAIL("dispatch"); return; }

    // dispatch 后（未 tick）实体应已进入 profiler（raw 在当前 tick 累加器）。
    const auto after = profiler.trackedCount();
    if (after != before + 1) { std::filesystem::remove_all(dir); FAIL("profiler did not track entity"); return; }

    auto snap = profiler.snapshot(entity->id());
    if (snap.entityType != "Avatar") { std::filesystem::remove_all(dir); FAIL("entity type not recorded"); return; }

    // tick 一次推进 EMA：lastRawLoad 应可读取。
    TickScheduler scheduler;
    app.attach(scheduler);
    scheduler.runOnce();
    app.detach(scheduler);

    snap = profiler.snapshot(entity->id());
    // 负载不可能为负——旧断言（>=0）对定时器恒 0 或 Scope 未累计真实耗时
    // 也会通过，未证明 dispatch 路径确实测量了非零负载（审核发现）。必须
    // 严格 > 0：handler 的确定性忙等保证 Scope 记录到非零耗时。
    if (snap.rawLoad <= 0.0F) { std::filesystem::remove_all(dir); FAIL("raw load must be positive"); return; }
    if (snap.smoothedLoad <= 0.0F) { std::filesystem::remove_all(dir); FAIL("smoothed load must be positive"); return; }

    std::filesystem::remove_all(dir);
    PASS();
}

// Phase C-4: BackupTopologyCoordinator 路由权限与 CellApp 进程注册。
// 端到端验证通过 CellApp 侧 API（registerProcess → routeFor）能查出
// 主备链路；主备不足时如实返回空。
static void testBackupTopologyWiredIntoCellApp() {
    TEST("Phase C-4: BackupTopologyCoordinator wired into CellApp");

    auto dir = createDefDir();
    auto transport = std::make_shared<InMemoryRuntimeTransport>();

    CellApp::Config config;
    config.entityDefPath = dir;
    config.componentId = 2;
    CellApp app(config, transport);
    if (!app.init()) { std::filesystem::remove_all(dir); FAIL("init"); return; }

    auto& topology = app.backupTopology();
    if (!topology.registerProcess(2)) { std::filesystem::remove_all(dir); FAIL("register self"); return; }
    if (!topology.registerProcess(3)) { std::filesystem::remove_all(dir); FAIL("register peer"); return; }
    if (topology.processCount() != 2) { std::filesystem::remove_all(dir); FAIL("process count"); return; }

    const auto route = topology.routeFor(42);
    if (!route.has_value()) { std::filesystem::remove_all(dir); FAIL("routeFor"); return; }
    if (route->entityId != 42) { std::filesystem::remove_all(dir); FAIL("route entityId"); return; }
    if (route->primary == route->backup) { std::filesystem::remove_all(dir); FAIL("primary == backup"); return; }

    // 显式注册版本推进（beginRebuild → ack → promote）走一轮端到端。
    if (!topology.beginRebuild(3, 7)) { std::filesystem::remove_all(dir); FAIL("beginRebuild"); return; }
    if (!topology.ackPrimed(2, topology.stagingVersion().value_or(
            theseed::runtime::BackupTopologyVersion{}))) { std::filesystem::remove_all(dir); FAIL("ack self"); return; }
    if (!topology.ackPrimed(3, topology.stagingVersion().value_or(
            theseed::runtime::BackupTopologyVersion{}))) { std::filesystem::remove_all(dir); FAIL("ack peer"); return; }
    if (!topology.promote()) { std::filesystem::remove_all(dir); FAIL("promote"); return; }
    if (topology.activeVersion().epoch != 7) { std::filesystem::remove_all(dir); FAIL("epoch"); return; }

    std::filesystem::remove_all(dir);
    PASS();
}

// Phase C-5: HotUpdateManager 走 L1 配置 + L2 脚本体替换端到端路径，
// 结构性变更被拒并保持版本不变。
static void testHotUpdateWiredIntoCellApp() {
    TEST("Phase C-5: HotUpdateManager wired into CellApp");

    auto dir = createDefDir();
    auto transport = std::make_shared<InMemoryRuntimeTransport>();

    CellApp::Config config;
    config.entityDefPath = dir;
    config.componentId = 2;
    CellApp app(config, transport);
    if (!app.init()) { std::filesystem::remove_all(dir); FAIL("init"); return; }

    auto& hotUpdate = app.hotUpdate();
    const auto initialVersion = hotUpdate.currentVersion();

    theseed::scripting::DiffResult diff;
    theseed::scripting::DiffChange configChange;
    configChange.type = theseed::scripting::ChangeType::ConfigValueChange;
    configChange.key = "game.hpRecoveryPerTick";
    configChange.detail = "12";
    diff.changes.push_back(configChange);

    const auto result = hotUpdate.apply(diff);
    if (!result.success) { std::filesystem::remove_all(dir); FAIL("L1 apply"); return; }
    if (hotUpdate.configStore().at("game.hpRecoveryPerTick") != "12") { std::filesystem::remove_all(dir); FAIL("config value"); return; }
    if (hotUpdate.currentVersion() == initialVersion) { std::filesystem::remove_all(dir); FAIL("version did not advance"); return; }

    // L4 变更必须拒绝且不改变版本/存储。
    theseed::scripting::DiffResult forbidden;
    theseed::scripting::DiffChange removeProp;
    removeProp.type = theseed::scripting::ChangeType::RemoveProperty;
    removeProp.key = "level";
    forbidden.changes.push_back(removeProp);
    const auto rejected = hotUpdate.apply(forbidden);
    if (rejected.success) { std::filesystem::remove_all(dir); FAIL("L4 accepted"); return; }
    if (hotUpdate.currentVersion() != result.appliedVersion) { std::filesystem::remove_all(dir); FAIL("L4 advanced version"); return; }

    // 回滚回初始版本。
    const auto rollback = hotUpdate.rollback(initialVersion);
    if (!rollback.success) { std::filesystem::remove_all(dir); FAIL("rollback"); return; }
    if (!hotUpdate.configStore().empty()) { std::filesystem::remove_all(dir); FAIL("rollback left config"); return; }

    std::filesystem::remove_all(dir);
    PASS();
}

int main() {
    std::cout << "Phase C end-to-end integration tests:\n";
    testLoadProfilerWiredIntoCellRuntime();
    testBackupTopologyWiredIntoCellApp();
    testHotUpdateWiredIntoCellApp();
    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
