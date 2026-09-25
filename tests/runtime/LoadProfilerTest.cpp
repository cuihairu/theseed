#include "theseed/runtime/LoadProfiler.h"

#include <chrono>
#include <cmath>
#include <iostream>
#include <string>
#include <thread>

using theseed::runtime::EntityLoadProfiler;
using theseed::runtime::EntityLoadSnapshot;
using theseed::runtime::EntityTypeLoadAggregator;
using theseed::runtime::EntityTypeLoadSnapshot;

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

static void sleepMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

static void test_scope_records_raw_load() {
    TEST("test_scope_records_raw_load");
    EntityLoadProfiler p;
    {
        auto s = p.scope(1, "Avatar");
        sleepMs(5);
    }
    p.tick();
    auto snap = p.snapshot(1);
    if (snap.entityType != "Avatar") { FAIL("entity type not captured"); return; }
    if (snap.rawLoad < 1.0F) { FAIL("raw load too small"); return; }
    PASS();
}

static void test_smoothed_load_ema() {
    TEST("test_smoothed_load_ema");
    EntityLoadProfiler::Config cfg;
    cfg.emaAlpha = 0.5F;
    EntityLoadProfiler p(cfg);

    // Tick 1: raw = 10 (simulated via two 5ms scopes)
    {
        auto s1 = p.scope(1, "Avatar");
        // simulate by recording directly via short sleeps — but EMA math is what we test,
        // so verify smoothing via tick() after raw is recorded.
        sleepMs(2);
    }
    p.tick();

    const float first = p.snapshot(1).smoothedLoad;
    // After first tick, smoothed = alpha * raw + (1-alpha) * 0 = alpha * raw.
    const float firstExpected = p.snapshot(1).rawLoad * 0.5F;
    const float tol = 0.5F;
    if (std::abs(first - firstExpected) > tol) {
        FAIL("first smoothed not alpha*raw");
        return;
    }

    // Tick 2: another small scope; verify the EMA recurrence exactly:
    // smoothed2 = alpha * raw2 + (1 - alpha) * smoothed1. Checking the
    // identity (instead of monotonicity) keeps the test deterministic —
    // Windows timer granularity makes raw2 vs raw1 ordering unreliable.
    {
        auto s2 = p.scope(1, "Avatar");
        sleepMs(4);
    }
    p.tick();
    const auto snap2 = p.snapshot(1);
    const float secondExpected = 0.5F * snap2.rawLoad + 0.5F * first;
    const float tol2 = 0.5F;
    if (std::abs(snap2.smoothedLoad - secondExpected) > tol2) {
        FAIL("second smoothed not ema recurrence of raw"); return;
    }
    PASS();
}

static void test_artificial_min_load_overrides_smoothed() {
    TEST("test_artificial_min_load_overrides_smoothed");
    EntityLoadProfiler p;
    p.setArtificialMinLoad(2, 50.0F);
    {
        auto s = p.scope(2, "NPC");
        // do nothing — tiny raw
    }
    p.tick();
    auto snap = p.snapshot(2);
    if (snap.artificialMinLoad < 49.9F) { FAIL("artificial min not stored"); return; }
    if (snap.adjustedLoad < 49.9F) { FAIL("adjusted should equal artificialMin when smoothed < min"); return; }
    if (snap.smoothedLoad >= 50.0F) { FAIL("smoothed should be small"); return; }
    PASS();
}

static void test_artificial_min_does_not_lower_smoothed() {
    TEST("test_artificial_min_does_not_lower_smoothed");
    EntityLoadProfiler p;
    {
        auto s = p.scope(3, "Boss");
        sleepMs(10);
    }
    p.tick();
    p.setArtificialMinLoad(3, 1.0F);  // below smoothed
    auto snap = p.snapshot(3);
    // adjusted should equal smoothed here, not artificial min.
    if (snap.adjustedLoad < snap.smoothedLoad - 0.1F) { FAIL("adjusted below smoothed"); return; }
    PASS();
}

static void test_untracked_entity_returns_empty_snapshot() {
    TEST("test_untracked_entity_returns_empty_snapshot");
    EntityLoadProfiler p;
    auto snap = p.snapshot(999);
    if (snap.entityId != 999) { FAIL("entityId not echoed"); return; }
    if (!snap.entityType.empty()) { FAIL("untracked should have empty type"); return; }
    if (snap.rawLoad != 0.0F || snap.smoothedLoad != 0.0F) { FAIL("untracked loads should be zero"); return; }
    PASS();
}

static void test_all_snapshots_sorted_by_id() {
    TEST("test_all_snapshots_sorted_by_id");
    EntityLoadProfiler p;
    {
        auto a = p.scope(7, "Avatar");
        auto b = p.scope(3, "NPC");
        auto c = p.scope(11, "Boss");
        static_cast<void>(a); static_cast<void>(b); static_cast<void>(c);
    }
    p.tick();
    const auto snaps = p.all();
    if (snaps.size() != 3) { FAIL("expected 3 snapshots"); return; }
    if (snaps[0].entityId != 3 || snaps[1].entityId != 7 || snaps[2].entityId != 11) {
        FAIL("snapshots not sorted ascending");
        return;
    }
    PASS();
}

static void test_reset_clears_all() {
    TEST("test_reset_clears_all");
    EntityLoadProfiler p;
    {
        auto s = p.scope(1, "Avatar");
        static_cast<void>(s);
    }
    p.tick();
    if (p.trackedCount() != 1) { FAIL("expected 1 tracked"); return; }
    p.reset();
    if (p.trackedCount() != 0) { FAIL("reset did not clear"); return; }
    PASS();
}

static void test_aggregator_groups_by_type() {
    TEST("test_aggregator_groups_by_type");
    EntityTypeLoadAggregator agg;
    EntityLoadSnapshot a;
    a.entityId = 1;
    a.entityType = "Avatar";
    a.rawLoad = 5.0F;
    a.smoothedLoad = 4.0F;
    a.adjustedLoad = 4.0F;

    EntityLoadSnapshot b;
    b.entityId = 2;
    b.entityType = "Avatar";
    b.rawLoad = 7.0F;
    b.smoothedLoad = 6.0F;
    b.adjustedLoad = 6.0F;

    EntityLoadSnapshot c;
    c.entityId = 3;
    c.entityType = "NPC";
    c.rawLoad = 1.0F;
    c.smoothedLoad = 1.0F;
    c.adjustedLoad = 1.0F;

    agg.record(a);
    agg.record(b);
    agg.record(c);

    auto av = agg.snapshot("Avatar");
    if (av.entityCount != 2) { FAIL("avatar count wrong"); return; }
    if (std::abs(av.totalRawLoad - 12.0F) > 0.01F) { FAIL("avatar totalRaw wrong"); return; }
    if (std::abs(av.totalSmoothedLoad - 10.0F) > 0.01F) { FAIL("avatar totalSmoothed wrong"); return; }
    if (std::abs(av.maxRawLoad - 7.0F) > 0.01F) { FAIL("avatar maxRaw wrong"); return; }
    if (std::abs(av.maxSmoothedLoad - 6.0F) > 0.01F) { FAIL("avatar maxSmoothed wrong"); return; }

    auto npc = agg.snapshot("NPC");
    if (npc.entityCount != 1) { FAIL("npc count wrong"); return; }

    auto none = agg.snapshot("Unknown");
    if (none.entityCount != 0) { FAIL("unknown should be zero"); return; }
    PASS();
}

static void test_aggregator_ignores_empty_type() {
    TEST("test_aggregator_ignores_empty_type");
    EntityTypeLoadAggregator agg;
    EntityLoadSnapshot s;
    s.entityId = 1;
    s.entityType = "";  // empty
    agg.record(s);
    if (!agg.all().empty()) { FAIL("empty type should not be recorded"); return; }
    PASS();
}

static void test_multiple_scope_in_one_tick_accumulates() {
    TEST("test_multiple_scope_in_one_tick_accumulates");
    EntityLoadProfiler p;
    {
        auto s1 = p.scope(1, "Avatar");
        sleepMs(2);
    }
    {
        auto s2 = p.scope(1, "Avatar");
        sleepMs(2);
    }
    p.tick();
    auto snap = p.snapshot(1);
    // Two scopes should accumulate to at least 3ms combined.
    if (snap.rawLoad < 3.0F) { FAIL("multiple scopes did not accumulate"); return; }
    PASS();
}

static void test_aggregator_all_sorted_and_reset() {
    TEST("test_aggregator_all_sorted_and_reset");
    EntityTypeLoadAggregator agg;
    EntityLoadSnapshot a;
    a.entityId = 1;
    a.entityType = "NPC";
    a.rawLoad = 1.0F;
    a.adjustedLoad = 1.0F;
    EntityLoadSnapshot b;
    b.entityId = 2;
    b.entityType = "Avatar";
    b.rawLoad = 2.0F;
    b.adjustedLoad = 2.0F;
    agg.record(a);
    agg.record(b);

    auto all = agg.all();
    if (all.size() != 2) { FAIL("all should hold two types"); return; }
    // all() 按 entityTypeId 字母序：Avatar < NPC
    if (all[0].entityTypeId != "Avatar" || all[1].entityTypeId != "NPC") {
        FAIL("all not sorted by type id"); return;
    }

    agg.reset();
    if (!agg.all().empty()) { FAIL("reset should clear per-type state"); return; }
    PASS();
}

static void test_ema_alpha_accessor() {
    TEST("test_ema_alpha_accessor");
    EntityLoadProfiler::Config cfg;
    cfg.emaAlpha = 0.25F;
    EntityLoadProfiler p(cfg);
    if (std::abs(p.emaAlpha() - 0.25F) > 1e-6F) { FAIL("emaAlpha mismatch"); return; }
    PASS();
}

static void test_scope_with_empty_type_keeps_first_type() {
    TEST("test_scope_with_empty_type_keeps_first_type");
    EntityLoadProfiler p;
    {
        auto s = p.scope(21, "Avatar");
        static_cast<void>(s);
    }
    p.tick();
    {
        // 第二次 scope 传空 type：accumulate 的 !type.empty() 假臂，保留首类型。
        auto s = p.scope(21, "");
        static_cast<void>(s);
    }
    p.tick();
    auto snap = p.snapshot(21);
    if (snap.entityType != "Avatar") { FAIL("empty type should not overwrite"); return; }
    if (snap.rawLoad <= 0.0F) { FAIL("raw should accumulate across both scopes"); return; }

    // 全新 entry 首次就是空 type：accumulate 的 entityType 空 && type 空短路臂，
    // entry 照常建立并累计负载，但类型保持空。
    {
        auto s = p.scope(31, "");
        static_cast<void>(s);
    }
    p.tick();
    auto bare = p.snapshot(31);
    if (bare.entityId != 31) { FAIL("typeless entry should still be tracked"); return; }
    if (!bare.entityType.empty()) { FAIL("fresh empty type should keep entry typeless"); return; }
    if (bare.rawLoad <= 0.0F) { FAIL("typeless entry should still accumulate load"); return; }
    PASS();
}

// 同一实体第二次 accumulate（经公开 scope() RAII 触发，accumulate 为 private）：
// entityType 已记录，走短路假臂不再覆盖。
static void test_accumulate_twice_keeps_type() {
    TEST("test_accumulate_twice_keeps_type");
    EntityLoadProfiler p;
    {
        auto s1 = p.scope(7, "Avatar");
        sleepMs(1);
    }
    {
        auto s2 = p.scope(7, "NPC");   // entityType 已非空：不得被 NPC 覆盖
        sleepMs(1);
    }
    p.tick();  // currentRawLoad → lastRawLoad，供 snapshot 读取
    if (p.snapshot(7).entityType != "Avatar") { FAIL("type overwritten"); return; }
    if (p.snapshot(7).rawLoad <= 0.0F) { FAIL("second accumulate lost"); return; }
    PASS();
}

int main() {
    test_scope_records_raw_load();
    test_smoothed_load_ema();
    test_artificial_min_load_overrides_smoothed();
    test_artificial_min_does_not_lower_smoothed();
    test_untracked_entity_returns_empty_snapshot();
    test_all_snapshots_sorted_by_id();
    test_reset_clears_all();
    test_aggregator_groups_by_type();
    test_aggregator_ignores_empty_type();
    test_multiple_scope_in_one_tick_accumulates();
    test_aggregator_all_sorted_and_reset();
    test_ema_alpha_accessor();
    test_scope_with_empty_type_keeps_first_type();
    test_accumulate_twice_keeps_type();

    std::cout << "  passed=" << testsPassed << " failed=" << testsFailed << "\n";
    return testsFailed == 0 ? 0 : 1;
}
