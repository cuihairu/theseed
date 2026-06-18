#include "theseed/foundation/Metrics.h"
#include "theseed/runtime/RuntimeTypes.h"
#include "theseed/runtime/TransportStatsCollector.h"

#include <iostream>
#include <string>

using theseed::foundation::MetricsRegistry;
using theseed::runtime::TransportStats;
using theseed::runtime::TransportStatsCollector;

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

static std::uint64_t counterValue(const std::string& name) {
    for (const auto& s : MetricsRegistry::instance().collect()) {
        if (s.name == name) {
            if (auto* p = std::get_if<std::uint64_t>(&s.value)) return *p;
        }
    }
    return 0;
}

static std::int64_t gaugeValue(const std::string& name) {
    for (const auto& s : MetricsRegistry::instance().collect()) {
        if (s.name == name) {
            if (auto* p = std::get_if<std::int64_t>(&s.value)) return *p;
        }
    }
    return 0;
}

static void testCollectDelta() {
    TEST("collector emits delta increments");
    MetricsRegistry::instance().reset();
    TransportStatsCollector c;

    TransportStats a{};
    a.messagesSent = 10;
    a.messagesReceived = 5;
    a.bytesSent = 1000;
    a.bytesReceived = 500;
    a.outboundQueueDepth = 3;
    a.inboundQueueDepth = 1;
    a.backPressureEvents = 0;
    c.collect(a);

    bool ok = counterValue("transport_messages_sent_total") == 10;
    ok = ok && counterValue("transport_bytes_sent_total") == 1000;
    ok = ok && counterValue("transport_messages_received_total") == 5;
    ok = ok && counterValue("transport_backpressure_events_total") == 0;
    ok = ok && gaugeValue("queue_backlog") == 3;
    ok = ok && gaugeValue("inbound_queue_depth") == 1;
    ok = ok && gaugeValue("transport_backpressure") == 0;

    TransportStats b = a;
    b.messagesSent = 25;  // +15
    b.bytesSent = 1750;   // +750
    b.outboundQueueDepth = 7;
    b.backPressureEvents = 1;
    c.collect(b);

    ok = ok && counterValue("transport_messages_sent_total") == 25;  // cumulative 10+15
    ok = ok && counterValue("transport_bytes_sent_total") == 1750;
    ok = ok && counterValue("transport_backpressure_events_total") == 1;
    ok = ok && gaugeValue("queue_backlog") == 7;
    ok = ok && gaugeValue("transport_backpressure") == 1;

    if (ok) PASS(); else FAIL("delta accumulation wrong");
}

static void testReset() {
    TEST("collector reset clears previous snapshot");

    MetricsRegistry::instance().reset();
    TransportStatsCollector c;
    TransportStats a{};
    a.messagesSent = 100;
    c.collect(a);

    // Registry counter is now 100.
    bool ok = counterValue("transport_messages_sent_total") == 100;

    c.reset();

    TransportStats b{};
    b.messagesSent = 30;
    c.collect(b);

    // After collector reset, the next collect() emits raw totals again —
    // registry counter becomes 100 + 30 = 130 (not 100 + delta-from-100).
    ok = ok && counterValue("transport_messages_sent_total") == 130;

    if (ok) PASS(); else FAIL("reset did not revert collector to raw-total mode");
}

static void testWraparound() {
    TEST("collector tolerates counter wraparound");

    MetricsRegistry::instance().reset();
    TransportStatsCollector c;
    TransportStats a{};
    a.messagesSent = 1000;
    c.collect(a);

    // Simulate transport stats reset (peer disconnected, stats rebuilt from 0).
    TransportStats b{};
    b.messagesSent = 5;
    c.collect(b);

    // Delta clamped to zero — total must remain 1000.
    if (counterValue("transport_messages_sent_total") == 1000) PASS();
    else FAIL("wraparound produced negative delta");
}

int main() {
    std::cout << "TransportStatsCollector tests:\n";

    testCollectDelta();
    testReset();
    testWraparound();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
