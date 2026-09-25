#include "theseed/runtime/NetworkNode.h"
#include "theseed/runtime/TcpConnection.h"
#include "theseed/runtime/TcpListener.h"
#include "theseed/runtime/TickScheduler.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <thread>

using theseed::runtime::DeliveryClass;
using theseed::runtime::IBytePipe;
using theseed::runtime::NetworkNode;
using theseed::runtime::NetworkTransport;
using theseed::runtime::RuntimeInvocation;
using theseed::runtime::SendResult;
using theseed::runtime::TcpConnection;
using theseed::runtime::TcpListener;
using theseed::runtime::TickContext;
using theseed::runtime::TickScheduler;
using theseed::runtime::TransportHub;

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

static void pumpConns(const std::shared_ptr<TcpConnection>& a,
                       const std::shared_ptr<TcpConnection>& b,
                       int iterations = 50) {
    for (int i = 0; i < iterations; ++i) {
        a->pump();
        b->pump();
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
}

// Test that NetworkNode can create a listening port and connect peers
static void testListenAndConnect() {
    TEST("NetworkNode listens and connects to peer");

    TcpConnection::globalInit();

    NetworkNode server({.localComponent = 2, .listenPort = 0});
    NetworkNode client({.localComponent = 1, .listenPort = 0});

    bool ok = server.listenPort() > 0;
    ok = ok && client.peerCount() == 0;

    client.connectToPeer(2, "127.0.0.1", server.listenPort());
    ok = ok && client.peerCount() == 1 && client.hasPeer(2);

    if (ok) PASS();
    else FAIL("listenPort=" + std::to_string(server.listenPort()) + " peerCount=" + std::to_string(client.peerCount()));

    TcpConnection::globalShutdown();
}

// Full round-trip using TcpListener + NetworkTransport + TransportHub
// (manual setup, validates the stack NetworkNode wraps)
static void testFullStackRoundTrip() {
    TEST("full stack: TCP → NetworkTransport → TransportHub round-trip");

    TcpConnection::globalInit();

    // Server side
    TcpListener listener;
    listener.listen("127.0.0.1", 0);
    auto port = listener.localPort();

    auto hubServer = std::make_shared<TransportHub>(2);

    // Client connects
    auto clientConn = TcpConnection::create();
    clientConn->connect("127.0.0.1", port);
    std::this_thread::sleep_for(std::chrono::milliseconds{50});

    // Server accepts
    auto serverConn = listener.accept();
    std::this_thread::sleep_for(std::chrono::milliseconds{50});

    auto clientTransport = std::make_shared<NetworkTransport>(clientConn);
    auto serverTransport = std::make_shared<NetworkTransport>(serverConn);

    auto hubClient = std::make_shared<TransportHub>(1);
    hubClient->connectPeer(2, clientTransport);
    hubServer->connectPeer(1, serverTransport);

    // Client sends to server
    hubClient->send(makeInvocation(42, 2, "hello"));
    pumpConns(clientConn, serverConn, 80);

    RuntimeInvocation received;
    auto count = hubServer->receive(2, &received, 1);

    bool ok = count == 1 && received.entityId == 42 && received.method == "hello";

    // Server replies
    hubServer->send(makeInvocation(99, 1, "reply"));
    pumpConns(clientConn, serverConn, 80);

    RuntimeInvocation reply;
    auto countReply = hubClient->receive(1, &reply, 1);
    ok = ok && countReply == 1 && reply.entityId == 99 && reply.method == "reply";

    if (ok) PASS();
    else FAIL("count=" + std::to_string(count) + " countReply=" + std::to_string(countReply));

    listener.close();
    TcpConnection::globalShutdown();
}

static void testDisconnectPeer() {
    TEST("disconnect peer prevents send");

    TcpConnection::globalInit();

    NetworkNode node({.localComponent = 1, .listenPort = 0});
    node.connectToPeer(2, "127.0.0.1", 9999);  // Will fail to actually connect but registers in hub

    auto hub = node.hub();
    // Even with failed connection, hub should report peer
    bool ok = node.hasPeer(2);

    node.disconnectPeer(2);
    auto result = hub->send(makeInvocation(2, 2, "after"));

    ok = ok && result == SendResult::NotConnected;

    if (ok) PASS();
    else FAIL("hasPeer=" + std::to_string(node.hasPeer(2)));

    TcpConnection::globalShutdown();
}

static void testPeerCount() {
    TEST("peerCount tracks connect/disconnect");

    TcpConnection::globalInit();

    NetworkNode node({.localComponent = 1, .listenPort = 0});

    bool ok = node.peerCount() == 0 && !node.hasPeer(2);

    node.connectToPeer(2, "127.0.0.1", 9999);
    ok = ok && node.peerCount() == 1 && node.hasPeer(2) && !node.hasPeer(3);

    node.disconnectPeer(2);
    ok = ok && node.peerCount() == 0 && !node.hasPeer(2);

    if (ok) PASS();
    else FAIL("peerCount=" + std::to_string(node.peerCount()));

    TcpConnection::globalShutdown();
}

static void testSchedulerLifecycleAndFailedConnect() {
    TEST("scheduler attach/detach and destructor paths");

    TcpConnection::globalInit();

    // 析构时 scheduler_ 为空：不触发析构内 detach
    {
        NetworkNode node({.localComponent = 5, .listenPort = 0});
        if (node.peerCount() != 0) { FAIL("fresh node should have no peers"); return; }
    }

    // 析构时仍 attach：析构函数负责 unregister
    {
        TickScheduler scheduler(std::chrono::milliseconds{0});
        NetworkNode node({.localComponent = 6, .listenPort = 0});
        node.attach(scheduler);
    }

    // 手动 detach 后析构
    {
        TickScheduler scheduler(std::chrono::milliseconds{0});
        NetworkNode node({.localComponent = 7, .listenPort = 0});
        node.attach(scheduler);
        node.detach(scheduler);
    }

    PASS();
    TcpConnection::globalShutdown();
}

static void testSchedulerRunLoopStops() {
    TEST("scheduler run loop: sleep-paced and busy-paced variants stop cleanly");

    // tickInterval > 0：sleep_until 节拍分支
    TickScheduler paced(std::chrono::milliseconds{2});
    std::thread pacedLoop([&] { paced.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds{12});
    paced.requestStop();
    pacedLoop.join();

    // tickInterval == 0：yield 忙等分支
    TickScheduler busy(std::chrono::milliseconds{0});
    std::thread busyLoop([&] { busy.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
    busy.requestStop();
    busyLoop.join();

    PASS();
}

// listen 条件组合的真臂（host 为 127.0.0.1 / 0.0.0.0 时 port=0 也监听）、
// 未监听 acceptIncoming 早退、acceptPeer 接收 NetworkTransport、onPeerConnected 回调链路。
static void testListenHostAndAcceptPaths() {
    TEST("listen host variants, acceptIncoming, acceptPeer, onPeerConnected");

    TcpConnection::globalInit();

    // host=0.0.0.0 + port=0：第三、第二条件的真臂各自构造一次。
    bool ok = false;
    {
        NetworkNode anyV4({.localComponent = 11, .listenHost = "0.0.0.0", .listenPort = 0});
        ok = anyV4.listenPort() > 0;
    }
    {
        NetworkNode loopback({.localComponent = 12, .listenHost = "127.0.0.1", .listenPort = 0});
        ok = ok && loopback.listenPort() > 0;
    }
    {
        // 未监听节点（bind 到不存在地址）：tick 内 acceptIncoming 走 isListening 早退。
        NetworkNode silent({.localComponent = 13, .listenHost = "10.255.255.1", .listenPort = 0});
        TickContext ctx;
        silent.tick(ctx);   // 不得抛出或阻塞
        ok = ok && silent.listenPort() == 0;
    }
    {
        // acceptPeer 直接接收 NetworkTransport：dynamic_pointer_cast 真臂。
        NetworkNode node({.localComponent = 14, .listenHost = "127.0.0.1", .listenPort = 0});
        node.acceptPeer(21, std::make_shared<NetworkTransport>(TcpConnection::create()));
        ok = ok && node.hasPeer(21);
    }
    {
        // 完整入站链路：客户端 connectToPeer → 服务端 tick → 回调拿到 transport。
        NetworkNode server({.localComponent = 30, .listenHost = "127.0.0.1", .listenPort = 0});
        NetworkNode client({.localComponent = 31});
        int notified = 0;
        server.setOnPeerConnected([&](std::shared_ptr<theseed::runtime::IRuntimeTransport>) { ++notified; });
        ok = ok && client.connectToPeer(30, "127.0.0.1", server.listenPort());
        TickContext ctx;
        for (int i = 0; i < 100 && notified == 0; ++i) {
            server.tick(ctx);
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }
        ok = ok && notified == 1;
    }

    if (ok) PASS();
    else FAIL("listen host / accept paths wrong");
}

int main() {
    std::cout << "NetworkNode tests:\n";

    testListenAndConnect();
    testFullStackRoundTrip();
    testDisconnectPeer();
    testPeerCount();
    testSchedulerLifecycleAndFailedConnect();
    testSchedulerRunLoopStops();
    testListenHostAndAcceptPaths();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
