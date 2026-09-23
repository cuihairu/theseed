#include "theseed/foundation/Metrics.h"
#include "theseed/ops/OpsInspector.h"

#include <chrono>
#include <iostream>
#include <string>
#include <string_view>

using theseed::foundation::MetricsRegistry;
using theseed::ops::OpsInspector;
using theseed::ops::ProcessInfo;
using theseed::ops::RuntimeInfo;
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
        std::cout << "FAIL: " << (msg) << "\n";                 \
        ++testsFailed;                                          \
    } while (0)

static bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

static ProcessInfo makeInfo() {
    ProcessInfo info;
    info.role = "TestApp";
    info.version = "9.9.9";
    info.componentId = 42;
    info.startTime = std::chrono::system_clock::now() - std::chrono::seconds(30);
    return info;
}

static void test_health_json_basic() {
    TEST("test_health_json_basic");
    OpsInspector insp(makeInfo(), [] { return RuntimeInfo{}; });
    const auto out = insp.renderHealthJson();

    if (!contains(out, "\"role\":\"TestApp\"")) { FAIL("missing role"); return; }
    if (!contains(out, "\"version\":\"9.9.9\"")) { FAIL("missing version"); return; }
    if (!contains(out, "\"component_id\":42")) { FAIL("missing component_id"); return; }
    if (!contains(out, "\"startup\":true")) { FAIL("missing startup flag"); return; }
    if (!contains(out, "\"liveness\":true")) { FAIL("missing liveness"); return; }
    if (!contains(out, "\"readiness\":true")) { FAIL("missing readiness"); return; }
    if (!contains(out, "\"uptime_s\":")) { FAIL("missing uptime_s"); return; }
    PASS();
}

static void test_health_json_escapes_tab() {
    TEST("test_health_json_escapes_tab");
    auto info = makeInfo();
    info.version = "9.9\t9";
    OpsInspector insp(info, [] { return RuntimeInfo{}; });
    const auto out = insp.renderHealthJson();

    if (!contains(out, "\"version\":\"9.9\\t9\"")) { FAIL("tab not escaped"); return; }
    PASS();
}

static void test_health_uptime_nonneg() {
    TEST("test_health_uptime_nonneg");
    OpsInspector insp(makeInfo(), [] { return RuntimeInfo{}; });
    const auto out = insp.renderHealthJson();
    const auto pos = out.find("\"uptime_s\":");
    if (pos == std::string::npos) { FAIL("uptime_s key missing"); return; }
    const auto colon = out.find(':', pos);
    const auto comma = out.find(',', colon);
    const auto num = out.substr(colon + 1, comma - colon - 1);
    long long v = std::stoll(std::string(num));
    if (v < 0) { FAIL("uptime negative"); return; }
    PASS();
}

static void test_inspect_json_contains_runtime() {
    TEST("test_inspect_json_contains_runtime");
    OpsInspector insp(makeInfo(), [] {
        RuntimeInfo rt;
        rt.entityCount = 5;
        rt.sessionCount = 4;
        rt.entityTypes = {"Avatar"};
        rt.transportStats.messagesSent = 10;
        rt.transportStats.bytesSent = 99;
        rt.transportStats.backPressureEvents = 2;
        return rt;
    });
    const auto out = insp.renderInspectJson();

    if (!contains(out, "\"entity_count\":5")) { FAIL("entity_count missing"); return; }
    if (!contains(out, "\"session_count\":4")) { FAIL("session_count missing"); return; }
    if (!contains(out, "\"Avatar\"")) { FAIL("entityTypes missing"); return; }
    if (!contains(out, "\"messages_sent\":10")) { FAIL("messages_sent missing"); return; }
    if (!contains(out, "\"bytes_sent\":99")) { FAIL("bytes_sent missing"); return; }
    if (!contains(out, "\"backpressure_events\":2")) { FAIL("backpressure_events missing"); return; }
    PASS();
}

static void test_inspect_json_empty_entity_types() {
    TEST("test_inspect_json_empty_entity_types");
    OpsInspector insp(makeInfo(), [] { return RuntimeInfo{}; });
    const auto out = insp.renderInspectJson();
    if (!contains(out, "\"entity_types\":[]")) { FAIL("empty entity_types array missing"); return; }
    PASS();
}

static void test_entities_json_matches_runtime() {
    TEST("test_entities_json_matches_runtime");
    OpsInspector insp(makeInfo(), [] {
        RuntimeInfo rt;
        rt.entityCount = 13;
        rt.entityTypes = {"Avatar", "NPC", "Projectile"};
        return rt;
    });
    const auto out = insp.renderEntitiesJson();
    if (!contains(out, "\"entity_count\":13")) { FAIL("entity_count missing"); return; }
    if (!contains(out, "\"Avatar\"")) { FAIL("Avatar missing"); return; }
    if (!contains(out, "\"NPC\"")) { FAIL("NPC missing"); return; }
    if (!contains(out, "\"Projectile\"")) { FAIL("Projectile missing"); return; }
    PASS();
}

static void test_metrics_render_includes_registered() {
    TEST("test_metrics_render_includes_registered");
    MetricsRegistry::instance().reset();
    auto& c = MetricsRegistry::instance().counter("ops_test_counter_total", "ops test");
    c.increment(7);

    OpsInspector insp(makeInfo());
    const auto out = insp.renderMetrics();
    if (!contains(out, "ops_test_counter_total")) { FAIL("counter name missing"); return; }
    if (!contains(out, "7")) { FAIL("value 7 missing"); return; }
    if (!contains(out, "# HELP")) { FAIL("HELP header missing"); return; }
    MetricsRegistry::instance().reset();
    PASS();
}

static void test_json_escapes_special_chars() {
    TEST("test_json_escapes_special_chars");
    ProcessInfo info;
    info.role = "Test\"App\nCarriage\rReturn";
    info.version = "0.1\\0";
    OpsInspector insp(info, [] {
        RuntimeInfo rt;
        rt.entityTypes = {"Ty\"pe"};
        return rt;
    });
    const auto out = insp.renderInspectJson();
    // expect: "Ty\"pe"  (C++ literal: "\"Ty\\\"pe\"")
    if (contains(out, "\"Ty\"pe\"")) { FAIL("double quote not escaped"); return; }
    if (!contains(out, "\"Ty\\\"pe\"")) { FAIL("escaped quote missing"); return; }
    // expect: Test\"App\n  (C++ literal: "Test\\\"App\\n")
    if (!contains(out, "Test\\\"App\\n")) { FAIL("role escape missing"); return; }
    if (!contains(out, "\\r")) { FAIL("carriage return escape missing"); return; }
    // expect: 0.1\\0  (C++ literal: "0.1\\\\0")
    if (!contains(out, "0.1\\\\0")) { FAIL("version escape missing"); return; }
    PASS();
}

static void test_snapshot_returns_provider_value() {
    TEST("test_snapshot_returns_provider_value");
    OpsInspector insp(makeInfo(), [] {
        RuntimeInfo rt;
        rt.entityCount = 123;
        return rt;
    });
    const auto snap = insp.snapshot();
    if (snap.entityCount != 123) { FAIL("snapshot not propagated"); return; }
    // process() 访问器返回构造时注入的节点信息。
    if (insp.process().version != "9.9.9") { FAIL("process info not propagated"); return; }
    PASS();
}

static void test_snapshot_without_provider_is_default() {
    TEST("test_snapshot_without_provider_is_default");
    OpsInspector insp(makeInfo());
    const auto snap = insp.snapshot();
    if (snap.entityCount != 0) { FAIL("default entityCount wrong"); return; }
    if (!snap.entityTypes.empty()) { FAIL("entityTypes should be empty"); return; }
    PASS();
}

static void test_runtime_transport_stats_reflected() {
    TEST("test_runtime_transport_stats_reflected");
    OpsInspector insp(makeInfo(), [] {
        RuntimeInfo rt;
        rt.transportStats.messagesReceived = 555;
        rt.transportStats.bytesReceived = 4096;
        rt.transportStats.outboundQueueDepth = 8;
        rt.transportStats.inboundQueueDepth = 6;
        return rt;
    });
    const auto out = insp.renderInspectJson();
    if (!contains(out, "\"messages_received\":555")) { FAIL("messages_received missing"); return; }
    if (!contains(out, "\"bytes_received\":4096")) { FAIL("bytes_received missing"); return; }
    if (!contains(out, "\"outbound_queue_depth\":8")) { FAIL("outbound_queue_depth missing"); return; }
    if (!contains(out, "\"inbound_queue_depth\":6")) { FAIL("inbound_queue_depth missing"); return; }
    PASS();
}

int main() {
    test_health_json_basic();
    test_health_json_escapes_tab();
    test_health_uptime_nonneg();
    test_inspect_json_contains_runtime();
    test_inspect_json_empty_entity_types();
    test_entities_json_matches_runtime();
    test_metrics_render_includes_registered();
    test_json_escapes_special_chars();
    test_snapshot_returns_provider_value();
    test_snapshot_without_provider_is_default();
    test_runtime_transport_stats_reflected();

    std::cout << "  passed=" << testsPassed << " failed=" << testsFailed << "\n";
    return testsFailed == 0 ? 0 : 1;
}
