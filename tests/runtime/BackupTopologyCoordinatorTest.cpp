#include "theseed/runtime/BackupTopologyCoordinator.h"

#include <iostream>
#include <unordered_set>

using theseed::runtime::BackupRoute;
using theseed::runtime::BackupTopologyCoordinator;
using theseed::runtime::BackupTopologyVersion;
using theseed::runtime::TopologyState;

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
        std::cout << "FAIL: " << (msg) << "\n";                 \
        ++testsFailed;                                          \
    } while (0)

static void test_initial_state_stable() {
    TEST("test_initial_state_stable");
    BackupTopologyCoordinator c;
    if (c.state() != TopologyState::Stable) { FAIL("expected Stable"); return; }
    if (c.processCount() != 0) { FAIL("expected zero processes"); return; }
    if (c.activeVersion().version != 0) { FAIL("expected version 0"); return; }
    PASS();
}

static void test_register_unregister_process() {
    TEST("test_register_unregister_process");
    BackupTopologyCoordinator c;
    if (!c.registerProcess(1)) { FAIL("register 1 failed"); return; }
    if (!c.registerProcess(2)) { FAIL("register 2 failed"); return; }
    if (c.registerProcess(1)) { FAIL("duplicate register should fail"); return; }
    if (c.processCount() != 2) { FAIL("expected 2"); return; }
    if (!c.hasProcess(2)) { FAIL("missing process 2"); return; }
    if (!c.unregisterProcess(1)) { FAIL("unregister 1 failed"); return; }
    if (c.hasProcess(1)) { FAIL("still has process 1"); return; }
    if (c.processCount() != 1) { FAIL("expected 1 after unregister"); return; }
    if (c.unregisterProcess(999)) { FAIL("unregister unknown should fail"); return; }
    PASS();
}

static void test_route_for_requires_two_processes() {
    TEST("test_route_for_requires_two_processes");
    BackupTopologyCoordinator c;
    if (c.routeFor(42).has_value()) { FAIL("route with 0 processes should be nullopt"); return; }
    c.registerProcess(1);
    if (c.routeFor(42).has_value()) { FAIL("route with 1 process should be nullopt"); return; }
    c.registerProcess(2);
    auto r = c.routeFor(42);
    if (!r.has_value()) { FAIL("route with 2 processes should be value"); return; }
    if (r->primary == r->backup) { FAIL("primary and backup must differ"); return; }
    if (r->entityId != 42) { FAIL("entityId not echoed"); return; }
    PASS();
}

static void test_route_for_is_deterministic() {
    TEST("test_route_for_is_deterministic");
    BackupTopologyCoordinator c;
    for (int id : {1, 2, 3, 4, 5}) c.registerProcess(static_cast<theseed::runtime::ComponentId>(id));

    const auto a = c.routeFor(7);
    const auto b = c.routeFor(7);
    if (!a.has_value() || !b.has_value()) { FAIL("route missing"); return; }
    if (a->primary != b->primary || a->backup != b->backup) {
        FAIL("route for same id differs across calls"); return;
    }
    PASS();
}

static void test_route_distributes_across_processes() {
    TEST("test_route_distributes_across_processes");
    BackupTopologyCoordinator c;
    for (int id = 1; id <= 4; ++id) c.registerProcess(static_cast<theseed::runtime::ComponentId>(id));

    std::unordered_set<theseed::runtime::ComponentId> seenPrimaries;
    for (int i = 1; i <= 1000; ++i) {
        const auto r = c.routeFor(static_cast<theseed::runtime::EntityId>(i));
        if (!r.has_value()) { FAIL("route missing"); return; }
        seenPrimaries.insert(r->primary);
    }
    if (seenPrimaries.size() < 3) { FAIL("hashing did not spread across processes"); return; }
    PASS();
}

static void test_begin_rebuild_sets_priming() {
    TEST("test_begin_rebuild_sets_priming");
    BackupTopologyCoordinator c;
    c.registerProcess(1);
    c.registerProcess(2);

    if (!c.beginRebuild(1, 100)) { FAIL("beginRebuild failed"); return; }
    if (c.state() != TopologyState::Priming) { FAIL("state not Priming"); return; }
    auto staging = c.stagingVersion();
    if (!staging.has_value()) { FAIL("staging missing"); return; }
    if (staging->version != 1) { FAIL("version should be 1"); return; }
    if (staging->epoch != 100) { FAIL("epoch mismatch"); return; }
    if (c.beginRebuild(2, 101)) { FAIL("concurrent beginRebuild should fail"); return; }
    PASS();
}

static void test_ack_and_promote() {
    TEST("test_ack_and_promote");
    BackupTopologyCoordinator c;
    c.registerProcess(1);
    c.registerProcess(2);
    c.registerProcess(3);

    if (!c.beginRebuild(1, 50)) { FAIL("beginRebuild failed"); return; }
    if (c.allAcked()) { FAIL("nothing acked yet"); return; }
    if (c.promote()) { FAIL("promote should fail without acks"); return; }

    const auto staging = *c.stagingVersion();
    if (!c.ackPrimed(1, staging)) { FAIL("ack 1 failed"); return; }
    if (c.allAcked()) { FAIL("only 1 of 3 acked"); return; }
    if (c.ackPrimed(2, BackupTopologyVersion{99, 99})) { FAIL("ack with wrong version should fail"); return; }
    if (c.ackPrimed(99, staging)) { FAIL("ack from unknown process should fail"); return; }

    if (!c.ackPrimed(2, staging)) { FAIL("ack 2 failed"); return; }
    if (!c.ackPrimed(3, staging)) { FAIL("ack 3 failed"); return; }
    if (!c.allAcked()) { FAIL("all should be acked"); return; }

    if (!c.promote()) { FAIL("promote failed"); return; }
    if (c.state() != TopologyState::Stable) { FAIL("state should be Stable after promote"); return; }
    if (c.stagingVersion().has_value()) { FAIL("staging should be cleared"); return; }
    if (c.activeVersion().version != 1 || c.activeVersion().epoch != 50) {
        FAIL("active version not promoted"); return;
    }
    PASS();
}

static void test_abort_returns_to_stable() {
    TEST("test_abort_returns_to_stable");
    BackupTopologyCoordinator c;
    c.registerProcess(1);
    c.registerProcess(2);
    if (!c.beginRebuild(1, 7)) { FAIL("beginRebuild failed"); return; }
    if (!c.abort()) { FAIL("abort failed"); return; }
    if (c.state() != TopologyState::Stable) { FAIL("state not Stable after abort"); return; }
    if (c.stagingVersion().has_value()) { FAIL("staging should be cleared"); return; }
    // After abort, can begin again.
    if (!c.beginRebuild(1, 8)) { FAIL("beginRebuild after abort failed"); return; }
    PASS();
}

static void test_route_uses_active_epoch() {
    TEST("test_route_uses_active_epoch");
    BackupTopologyCoordinator c;
    c.registerProcess(1);
    c.registerProcess(2);
    // activeVersion_.epoch starts at 0.
    auto r1 = c.routeFor(5);
    if (!r1.has_value() || r1->epoch != 0) { FAIL("epoch should be 0 initially"); return; }

    if (!c.beginRebuild(1, 999)) { FAIL("beginRebuild failed"); return; }
    const auto staging = *c.stagingVersion();
    if (!c.ackPrimed(1, staging)) { FAIL("ack 1 failed"); return; }
    if (!c.ackPrimed(2, staging)) { FAIL("ack 2 failed"); return; }
    if (!c.promote()) { FAIL("promote failed"); return; }

    auto r2 = c.routeFor(5);
    if (!r2.has_value() || r2->epoch != 999) { FAIL("epoch not updated to 999"); return; }
    PASS();
}

static void test_unregister_reindexes() {
    TEST("test_unregister_reindexes");
    BackupTopologyCoordinator c;
    for (int id = 1; id <= 4; ++id) c.registerProcess(static_cast<theseed::runtime::ComponentId>(id));
    if (!c.unregisterProcess(2)) { FAIL("unregister 2 failed"); return; }
    if (!c.hasProcess(3) || !c.hasProcess(4)) { FAIL("reindex lost 3 or 4"); return; }
    auto r = c.routeFor(100);
    if (!r.has_value()) { FAIL("route should still work with 3 processes"); return; }
    PASS();
}

static void test_processes_listing_and_reset() {
    TEST("test_processes_listing_and_reset");
    BackupTopologyCoordinator c;
    c.registerProcess(3);
    c.registerProcess(1);
    c.registerProcess(2);
    auto ids = c.processes();
    if (ids.size() != 3 || ids[0] != 1 || ids[1] != 2 || ids[2] != 3) {
        FAIL("processes() should return sorted ids"); return;
    }
    c.reset();
    if (c.processCount() != 0) { FAIL("reset should clear processes"); return; }
    if (c.state() != TopologyState::Stable) { FAIL("reset should return to Stable"); return; }
    if (c.activeVersion().version != 0) { FAIL("reset should clear active version"); return; }
    if (!c.processes().empty()) { FAIL("reset should clear listing"); return; }
    PASS();
}

// 防御与状态机假臂：0 号进程注册、空表 beginRebuild、Stable 下的 ack/allAcked/promote/abort、版本相等比较。
static void test_defensive_and_stable_arms() {
    TEST("test_defensive_and_stable_arms");
    BackupTopologyCoordinator c;

    if (c.registerProcess(0)) { FAIL("process id 0 should be rejected"); return; }

    if (c.beginRebuild(1, 7)) { FAIL("beginRebuild with no processes should fail"); return; }

    // Stable 态：ack / allAcked / promote / abort 全部拒绝。
    BackupTopologyVersion v{1, 7};
    if (c.ackPrimed(1, v)) { FAIL("ack in Stable should fail"); return; }
    if (c.allAcked()) { FAIL("allAcked in Stable should be false"); return; }
    if (c.promote()) { FAIL("promote in Stable should fail"); return; }
    if (c.abort()) { FAIL("abort in Stable should fail"); return; }

    // 版本比较：version 同 epoch 异 → 不等；全同 → 相等（36 行两臂）。
    if (!(BackupTopologyVersion{1, 2} == BackupTopologyVersion{1, 2})) {
        FAIL("equal versions compare unequal"); return;
    }
    if (BackupTopologyVersion{1, 2} == BackupTopologyVersion{1, 3}) {
        FAIL("different epoch compares equal"); return;
    }
    if (BackupTopologyVersion{2, 2} == BackupTopologyVersion{1, 2}) {
        FAIL("different version compares equal"); return;
    }

    PASS();
}

// 完整 rebuild 生命周期：ack 齐后 promote 落回 Stable，再次 promote / 重复 ack 被拒。
static void test_rebuild_lifecycle_repeat_promote() {
    TEST("test_rebuild_lifecycle_repeat_promote");
    BackupTopologyCoordinator c;
    c.registerProcess(1);
    c.registerProcess(2);
    if (!c.beginRebuild(1, 10)) { FAIL("beginRebuild failed"); return; }
    auto staging = c.stagingVersion();
    if (!staging.has_value()) { FAIL("staging missing"); return; }
    if (!c.ackPrimed(1, *staging) || !c.ackPrimed(2, *staging)) { FAIL("acks failed"); return; }
    if (!c.promote()) { FAIL("promote failed"); return; }
    // 落回 Stable 后：再次 promote 与旧版本 ack 均拒绝（126/108 Stable 短路真臂）。
    if (c.promote()) { FAIL("repeat promote should fail"); return; }
    if (c.ackPrimed(1, *staging)) { FAIL("stale ack after promote should fail"); return; }
    if (c.allAcked()) { FAIL("allAcked after promote should be false"); return; }
    PASS();
}

int main() {
    test_initial_state_stable();
    test_register_unregister_process();
    test_route_for_requires_two_processes();
    test_route_for_is_deterministic();
    test_route_distributes_across_processes();
    test_begin_rebuild_sets_priming();
    test_ack_and_promote();
    test_defensive_and_stable_arms();
    test_rebuild_lifecycle_repeat_promote();
    test_abort_returns_to_stable();
    test_route_uses_active_epoch();
    test_unregister_reindexes();
    test_processes_listing_and_reset();

    std::cout << "  passed=" << testsPassed << " failed=" << testsFailed << "\n";
    return testsFailed == 0 ? 0 : 1;
}
