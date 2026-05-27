#include "theseed/runtime/NetworkTransport.h"
#include "theseed/runtime/TcpConnection.h"
#include "theseed/runtime/TcpListener.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <thread>

using theseed::runtime::DeliveryClass;
using theseed::runtime::IBytePipe;
using theseed::runtime::NetworkTransport;
using theseed::runtime::RuntimeInvocation;
using theseed::runtime::TcpConnection;
using theseed::runtime::TcpListener;

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

static void pumpBoth(const std::shared_ptr<TcpConnection>& a,
                      const std::shared_ptr<TcpConnection>& b,
                      int iterations = 50) {
    for (int i = 0; i < iterations; ++i) {
        a->pump();
        b->pump();
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
}

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

static void testTcpConnectAndSend() {
    TEST("TCP connect and send one invocation");

    TcpListener listener;
    listener.listen("127.0.0.1", 0);
    auto port = listener.localPort();

    auto serverConn = TcpConnection::create();
    auto clientConn = TcpConnection::create();

    // Client connects
    clientConn->connect("127.0.0.1", port);

    // Accept on server
    auto accepted = listener.accept();
    serverConn = accepted;

    // Small delay for connection setup
    std::this_thread::sleep_for(std::chrono::milliseconds{50});

    NetworkTransport clientTransport(clientConn);
    NetworkTransport serverTransport(serverConn);

    clientTransport.send(makeInvocation(42, 1, "hello"));
    pumpBoth(clientConn, serverConn);

    RuntimeInvocation received;
    auto count = serverTransport.receive(1, &received, 1);

    if (count == 1 && received.entityId == 42 && received.method == "hello") PASS();
    else FAIL("count=" + std::to_string(count) + " entityId=" + std::to_string(received.entityId));

    listener.close();
}

static void testTcpBidirectional() {
    TEST("TCP bidirectional communication");

    TcpListener listener;
    listener.listen("127.0.0.1", 0);
    auto port = listener.localPort();

    auto clientConn = TcpConnection::create();
    clientConn->connect("127.0.0.1", port);
    auto serverConn = listener.accept();

    std::this_thread::sleep_for(std::chrono::milliseconds{50});

    NetworkTransport clientTransport(clientConn);
    NetworkTransport serverTransport(serverConn);

    clientTransport.send(makeInvocation(1, 10, "AtoB"));
    serverTransport.send(makeInvocation(2, 20, "BtoA"));
    pumpBoth(clientConn, serverConn);

    RuntimeInvocation fromB, fromA;
    auto countB = serverTransport.receive(10, &fromB, 1);
    auto countA = clientTransport.receive(20, &fromA, 1);

    bool ok = countB == 1 && fromB.entityId == 1 && fromB.method == "AtoB";
    ok = ok && countA == 1 && fromA.entityId == 2 && fromA.method == "BtoA";

    if (ok) PASS();
    else FAIL("countB=" + std::to_string(countB) + " countA=" + std::to_string(countA));

    listener.close();
}

static void testTcpMultipleMessages() {
    TEST("TCP send 10 invocations reliably");

    TcpListener listener;
    listener.listen("127.0.0.1", 0);
    auto port = listener.localPort();

    auto clientConn = TcpConnection::create();
    clientConn->connect("127.0.0.1", port);
    auto serverConn = listener.accept();

    std::this_thread::sleep_for(std::chrono::milliseconds{50});

    NetworkTransport clientTransport(clientConn);
    NetworkTransport serverTransport(serverConn);

    for (std::uint64_t i = 1; i <= 10; ++i) {
        clientTransport.send(makeInvocation(i, 5, "batch"));
    }
    pumpBoth(clientConn, serverConn, 100);

    RuntimeInvocation out[10];
    auto count = serverTransport.receive(5, out, 10);

    bool ok = count == 10;
    for (int i = 0; i < 10 && ok; ++i) {
        ok = out[i].entityId == static_cast<std::uint64_t>(i + 1);
    }

    if (ok) PASS();
    else FAIL("count=" + std::to_string(count));

    listener.close();
}

static void testTcpPayloadRoundtrip() {
    TEST("TCP payload round-trip (4KiB)");

    TcpListener listener;
    listener.listen("127.0.0.1", 0);
    auto port = listener.localPort();

    auto clientConn = TcpConnection::create();
    clientConn->connect("127.0.0.1", port);
    auto serverConn = listener.accept();

    std::this_thread::sleep_for(std::chrono::milliseconds{50});

    NetworkTransport clientTransport(clientConn);
    NetworkTransport serverTransport(serverConn);

    std::vector<std::byte> payload(4096);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<std::byte>(i & 0xFF);
    }

    RuntimeInvocation sent = makeInvocation(99, 1, "data");
    sent.payload = payload;
    clientTransport.send(sent);
    pumpBoth(clientConn, serverConn, 100);

    RuntimeInvocation received;
    auto count = serverTransport.receive(1, &received, 1);

    bool ok = count == 1 && received.payload.size() == 4096;
    if (ok) {
        for (std::size_t i = 0; i < received.payload.size(); ++i) {
            if (received.payload[i] != static_cast<std::byte>(i & 0xFF)) {
                ok = false;
                break;
            }
        }
    }

    if (ok) PASS();
    else FAIL("count=" + std::to_string(count) + " size=" + std::to_string(received.payload.size()));

    listener.close();
}

static void testTcpConnectionClose() {
    TEST("TCP connection close detected");

    TcpListener listener;
    listener.listen("127.0.0.1", 0);
    auto port = listener.localPort();

    auto clientConn = TcpConnection::create();
    clientConn->connect("127.0.0.1", port);
    auto serverConn = listener.accept();

    std::this_thread::sleep_for(std::chrono::milliseconds{50});

    bool ok = clientConn->isConnected() && serverConn->isConnected();

    serverConn->close();
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    clientConn->pump();

    ok = ok && !serverConn->isConnected();

    if (ok) PASS();
    else FAIL("connection state unexpected");

    listener.close();
}

int main() {
    TcpConnection::globalInit();

    std::cout << "TCP transport tests:\n";

    testTcpConnectAndSend();
    testTcpBidirectional();
    testTcpMultipleMessages();
    testTcpPayloadRoundtrip();
    testTcpConnectionClose();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";

    TcpConnection::globalShutdown();
    return testsFailed == 0 ? 0 : 1;
}
