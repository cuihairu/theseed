#include "theseed/runtime/RuntimeTransport.h"
#include "theseed/runtime/TransportHub.h"

#include <cstdint>
#include <iostream>
#include <memory>

using theseed::runtime::DeliveryClass;
using theseed::runtime::InMemoryRuntimeTransport;
using theseed::runtime::RuntimeInvocation;
using theseed::runtime::SendResult;
using theseed::runtime::TransportHub;
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

static RuntimeInvocation makeInvocation(std::uint64_t entityId,
                                         std::uint32_t target,
                                         const std::string& method) {
    RuntimeInvocation inv;
    inv.entityId = entityId;
    inv.targetComponent = target;
    inv.entityType = "Avatar";
    inv.method = method;
    inv.deliveryClass = DeliveryClass::ORDERED_RELIABLE;
    return inv;
}

static void testRouteToCorrectPeer() {
    TEST("route invocation to correct peer");

    TransportHub hub(1);
    auto transport2 = std::make_shared<InMemoryRuntimeTransport>();
    auto transport3 = std::make_shared<InMemoryRuntimeTransport>();

    hub.connectPeer(2, transport2);
    hub.connectPeer(3, transport3);

    auto r1 = hub.send(makeInvocation(10, 2, "toCell2"));
    auto r2 = hub.send(makeInvocation(20, 3, "toCell3"));

    if (r1 != SendResult::Accepted || r2 != SendResult::Accepted) {
        FAIL("send results unexpected");
        return;
    }

    RuntimeInvocation out[4];
    auto count2 = transport2->receive(2, out, 4);
    bool ok = count2 == 1 && out[0].entityId == 10 && out[0].method == "toCell2";

    auto count3 = transport3->receive(3, out, 4);
    ok = ok && count3 == 1 && out[0].entityId == 20 && out[0].method == "toCell3";

    if (ok) PASS();
    else FAIL("count2=" + std::to_string(count2) + " count3=" + std::to_string(count3));
}

static void testReceiveFromMultiplePeers() {
    TEST("receive aggregates from multiple peers");

    TransportHub hub(1);
    auto transport2 = std::make_shared<InMemoryRuntimeTransport>();
    auto transport3 = std::make_shared<InMemoryRuntimeTransport>();

    hub.connectPeer(2, transport2);
    hub.connectPeer(3, transport3);

    // Simulate CellApp(2) and CellApp(3) sending to BaseApp(1)
    transport2->send(makeInvocation(100, 1, "fromCell2"));
    transport3->send(makeInvocation(200, 1, "fromCell3"));

    RuntimeInvocation out[4];
    auto count = hub.receive(1, out, 4);

    bool ok = count == 2;
    // Order is not guaranteed, check both exist
    if (ok) {
        bool found100 = false, found200 = false;
        for (std::size_t i = 0; i < count; ++i) {
            if (out[i].entityId == 100 && out[i].method == "fromCell2") found100 = true;
            if (out[i].entityId == 200 && out[i].method == "fromCell3") found200 = true;
        }
        ok = found100 && found200;
    }

    if (ok) PASS();
    else FAIL("count=" + std::to_string(count));
}

static void testNoPeerReturnsNotConnected() {
    TEST("send to unknown peer returns NotConnected");

    TransportHub hub(1);
    auto result = hub.send(makeInvocation(1, 99, "nowhere"));

    if (result == SendResult::NotConnected) PASS();
    else FAIL("expected NotConnected");
}

static void testDisconnectPeer() {
    TEST("disconnect peer prevents send");

    TransportHub hub(1);
    auto transport = std::make_shared<InMemoryRuntimeTransport>();
    hub.connectPeer(2, transport);

    auto r1 = hub.send(makeInvocation(1, 2, "before"));
    hub.disconnectPeer(2);
    auto r2 = hub.send(makeInvocation(2, 2, "after"));

    bool ok = r1 == SendResult::Accepted && r2 == SendResult::NotConnected;

    if (ok) PASS();
    else FAIL("r1=" + std::to_string(static_cast<int>(r1)) + " r2=" + std::to_string(static_cast<int>(r2)));
}

static void testFlushAllPeers() {
    TEST("flush propagates to all peers");

    TransportHub hub(1);
    auto t2 = std::make_shared<InMemoryRuntimeTransport>();
    auto t3 = std::make_shared<InMemoryRuntimeTransport>();

    hub.connectPeer(2, t2);
    hub.connectPeer(3, t3);

    // flush() should not crash on empty transports
    hub.flush();

    // Send some data and flush
    hub.send(makeInvocation(1, 2, "a"));
    hub.send(makeInvocation(2, 3, "b"));
    hub.flush();

    RuntimeInvocation out[4];
    auto c2 = t2->receive(2, out, 4);
    auto c3 = t3->receive(3, out, 4);

    if (c2 == 1 && c3 == 1) PASS();
    else FAIL("c2=" + std::to_string(c2) + " c3=" + std::to_string(c3));
}

static void testStatsAggregation() {
    TEST("stats aggregates from all peers");

    TransportHub hub(1);
    auto t2 = std::make_shared<InMemoryRuntimeTransport>();
    auto t3 = std::make_shared<InMemoryRuntimeTransport>();

    hub.connectPeer(2, t2);
    hub.connectPeer(3, t3);

    t2->send(makeInvocation(1, 1, "a"));
    t2->send(makeInvocation(2, 1, "b"));
    t3->send(makeInvocation(3, 1, "c"));

    TransportStats stats = hub.stats();

    if (stats.messagesSent == 3) PASS();
    else FAIL("messagesSent=" + std::to_string(stats.messagesSent));
}

static void testPendingCountAggregation() {
    TEST("pendingCount aggregates from all peers");

    TransportHub hub(1);
    auto t2 = std::make_shared<InMemoryRuntimeTransport>();
    auto t3 = std::make_shared<InMemoryRuntimeTransport>();

    hub.connectPeer(2, t2);
    hub.connectPeer(3, t3);

    hub.send(makeInvocation(1, 2, "a"));
    hub.send(makeInvocation(2, 2, "b"));
    hub.send(makeInvocation(3, 3, "c"));

    auto pending = hub.pendingCount();

    if (pending == 3) PASS();
    else FAIL("pending=" + std::to_string(pending));
}

static void testPeerCount() {
    TEST("peerCount tracks connect/disconnect");

    TransportHub hub(1);

    bool ok = hub.peerCount() == 0 && !hub.hasPeer(2);

    auto t2 = std::make_shared<InMemoryRuntimeTransport>();
    hub.connectPeer(2, t2);
    ok = ok && hub.peerCount() == 1 && hub.hasPeer(2) && !hub.hasPeer(3);

    auto t3 = std::make_shared<InMemoryRuntimeTransport>();
    hub.connectPeer(3, t3);
    ok = ok && hub.peerCount() == 2;

    hub.disconnectPeer(2);
    ok = ok && hub.peerCount() == 1 && !hub.hasPeer(2);

    if (ok) PASS();
    else FAIL("peerCount=" + std::to_string(hub.peerCount()));
}

int main() {
    std::cout << "TransportHub tests:\n";

    testRouteToCorrectPeer();
    testReceiveFromMultiplePeers();
    testNoPeerReturnsNotConnected();
    testDisconnectPeer();
    testFlushAllPeers();
    testStatsAggregation();
    testPendingCountAggregation();
    testPeerCount();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
