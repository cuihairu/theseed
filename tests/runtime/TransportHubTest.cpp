#include "theseed/runtime/RuntimeTransport.h"
#include "theseed/runtime/TransportHub.h"

#include <cstddef>
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

static void testAwaitingIdentityLifecycle() {
    TEST("hub counts and flushes server transports awaiting identity");

    auto hub = TransportHub(20);
    auto transport = std::make_shared<InMemoryRuntimeTransport>();
    hub.attachServerTransport(transport);

    RuntimeInvocation inv;
    inv.sourceComponent = 20;
    inv.targetComponent = 30;
    inv.method = "hello";
    if (hub.send(inv) != SendResult::NotConnected) { FAIL("awaiting peer should not route"); return; }

    static_cast<void>(hub.pendingCount());  // 覆盖 awaitingIdentity_ 的 pendingCount 汇总
    hub.flush();                            // 覆盖 awaitingIdentity_ 的 flush
    bool ok = !hub.hasPeer(30);

    if (ok) PASS();
    else FAIL("awaiting identity lifecycle wrong");
}

// 防御臂：connectPeer 拒绝空 transport 与 0 号 peer；attachServerTransport 拒绝空；receive 容量 0/空缓冲早退。
static void testDefensiveArms() {
    TEST("connectPeer/attachServerTransport/receive defensive arms");

    auto hub = TransportHub(1);
    auto transport = std::make_shared<InMemoryRuntimeTransport>();

    hub.connectPeer(0, transport);          // peerId == 0 → 忽略
    hub.connectPeer(2, nullptr);            // 空 transport → 忽略
    bool ok = hub.peerCount() == 0;

    hub.attachServerTransport(nullptr);     // 空 transport → 忽略
    ok = ok && hub.pendingCount() == 0;

    RuntimeInvocation inv;
    ok = ok && hub.receive(1, nullptr, 4) == 0;   // 空缓冲
    ok = ok && hub.receive(1, &inv, 0) == 0;      // 容量 0

    if (ok) PASS();
    else FAIL("defensive arms wrong");
}

// receive 容量打满即 break：peers 段与 awaitingIdentity 段各覆盖一次。
static void testReceiveCapacityBreaks() {
    TEST("receive stops at capacity in both loops");

    auto hub = TransportHub(1);
    auto p2 = std::make_shared<InMemoryRuntimeTransport>();
    auto p3 = std::make_shared<InMemoryRuntimeTransport>();
    hub.connectPeer(2, p2);
    hub.connectPeer(3, p3);
    p2->send(makeInvocation(10, 1, "a"));
    p3->send(makeInvocation(20, 1, "b"));

    RuntimeInvocation out[2]{};
    auto n = hub.receive(1, out, 1);        // 容量 1：拉到一条即 break，第二个 peer 不拉
    bool ok = n == 1;
    // peers_ 是 unordered_map，遍历顺序不固定：两次收取合计应恰好 a、b 各一条。
    const std::string first = out[0].method;
    RuntimeInvocation rest[1]{};
    ok = ok && hub.receive(1, rest, 2) == 1;
    ok = ok && (first == "a" ? rest[0].method == "b" : rest[0].method == "a");

    // awaitingIdentity 段：容量 1 时同样 break，剩余 transport 留在队列。
    auto server = TransportHub(20);
    auto t1 = std::make_shared<InMemoryRuntimeTransport>();
    auto t2 = std::make_shared<InMemoryRuntimeTransport>();
    RuntimeInvocation hello1;
    hello1.sourceComponent = 30; hello1.targetComponent = 20; hello1.method = "hi1";
    RuntimeInvocation hello2;
    hello2.sourceComponent = 31; hello2.targetComponent = 20; hello2.method = "hi2";
    t1->send(hello1);
    t2->send(hello2);
    server.attachServerTransport(t1);
    server.attachServerTransport(t2);
    RuntimeInvocation one[1]{};
    ok = ok && server.receive(20, one, 1) == 1 && one[0].method == "hi1";

    if (ok) PASS();
    else FAIL("capacity break wrong");
}

// 服务端连接自学习：首条消息的 sourceComponent 注册为 peer，回复可路由。
static void testServerIdentitySelfLearning() {
    TEST("server transport self-registers peer from first message source");

    auto hub = TransportHub(20);
    auto t = std::make_shared<InMemoryRuntimeTransport>();
    RuntimeInvocation hello;
    hello.sourceComponent = 30; hello.targetComponent = 20; hello.method = "hello";
    t->send(hello);
    hub.attachServerTransport(t);

    RuntimeInvocation out[1]{};
    auto n = hub.receive(20, out, 1);
    bool ok = n == 1 && out[0].method == "hello";
    ok = ok && hub.hasPeer(30) && hub.pendingCount() == 0;

    // 自学习后按 sourceComponent 路由回复。
    ok = ok && hub.send(makeInvocation(1, 30, "reply")) == SendResult::Accepted;
    ok = ok && t->receive(30, out, 1) == 1 && out[0].method == "reply";

    if (ok) PASS();
    else FAIL("self learning wrong");
}

// awaitingIdentity 短路链假臂：无匹配消息（count==0）与身份缺失（sourceComponent==0）均不自学习。
static void testAwaitingIdentitySkipArms() {
    TEST("awaiting identity: zero-count and zero-source arms");

    auto hub = TransportHub(1);
    auto silent = std::make_shared<InMemoryRuntimeTransport>();   // 无消息 → count==0 臂
    auto anonymous = std::make_shared<InMemoryRuntimeTransport>();
    RuntimeInvocation inv;
    inv.entityId = 7;
    inv.targetComponent = 1;   // 匹配 localComponent，可被取出
    inv.method = "anon";
    inv.sourceComponent = 0;   // 身份缺失 → source==0 臂
    anonymous->send(inv);

    hub.attachServerTransport(silent);
    hub.attachServerTransport(anonymous);

    RuntimeInvocation out[4];
    auto total = hub.receive(1, out, 4);
    bool ok = total == 1 && out[0].method == "anon";
    ok = ok && hub.peerCount() == 0;   // 两笔都未自学习

    if (ok) PASS();
    else FAIL("total=" + std::to_string(total));
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
    testAwaitingIdentityLifecycle();
    testDefensiveArms();
    testReceiveCapacityBreaks();
    testAwaitingIdentitySkipArms();
    testServerIdentitySelfLearning();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
