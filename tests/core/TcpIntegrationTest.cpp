#include "theseed/core/BaseApp.h"
#include "theseed/core/CellApp.h"
#include "theseed/core/IEntityStore.h"
#include "theseed/runtime/Entity.h"
#include "theseed/runtime/EntityDef.h"
#include "theseed/runtime/NetworkTransport.h"
#include "theseed/runtime/RuntimeTypes.h"
#include "theseed/runtime/TcpConnection.h"
#include "theseed/runtime/TcpListener.h"
#include "theseed/runtime/TransportHub.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using theseed::core::BaseApp;
using theseed::core::CellApp;
using theseed::core::InMemoryEntityStore;
using theseed::runtime::Entity;
using theseed::runtime::EntityId;
using theseed::runtime::EntitySide;
using theseed::runtime::IRuntimeTransport;
using theseed::runtime::NetworkTransport;
using theseed::runtime::RuntimeInvocation;
using theseed::runtime::SendResult;
using theseed::runtime::TcpConnection;
using theseed::runtime::TcpListener;
using theseed::runtime::TransportHub;
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

static bool writeFile(const std::string& path, const std::string& content) {
    std::ofstream f(path);
    if (!f.is_open()) return false;
    f << content;
    return true;
}

static std::string createDefDir() {
    std::string dir = "test_tcp_integration_defs";
    std::filesystem::create_directory(dir);

    writeFile(dir + "/Avatar.xml", R"(
<EntityDef name="Avatar">
    <Properties>
        <Property name="level" type="Int32"/>
        <Property name="hp" type="Float32"/>
    </Properties>
    <Methods>
        <Method name="onDamage" side="Cell"/>
        <Method name="onHeal" side="Base"/>
    </Methods>
</EntityDef>
)");

    return dir;
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

// Shared setup: creates two TCP-connected TransportHubs
struct TcpStack {
    TcpListener listener;
    std::shared_ptr<TcpConnection> clientConn;
    std::shared_ptr<TcpConnection> serverConn;
    std::shared_ptr<NetworkTransport> clientTransport;
    std::shared_ptr<NetworkTransport> serverTransport;
    std::shared_ptr<TransportHub> baseHub;
    std::shared_ptr<TransportHub> cellHub;

    TcpStack() {
        listener.listen("127.0.0.1", 0);
        auto port = listener.localPort();

        clientConn = TcpConnection::create();
        clientConn->connect("127.0.0.1", port);
        std::this_thread::sleep_for(std::chrono::milliseconds{50});

        serverConn = listener.accept();
        std::this_thread::sleep_for(std::chrono::milliseconds{50});

        clientTransport = std::make_shared<NetworkTransport>(clientConn);
        serverTransport = std::make_shared<NetworkTransport>(serverConn);

        baseHub = std::make_shared<TransportHub>(1);
        cellHub = std::make_shared<TransportHub>(2);

        baseHub->connectPeer(2, clientTransport);
        cellHub->connectPeer(1, serverTransport);
    }

    void pump(int iterations = 80) {
        pumpConns(clientConn, serverConn, iterations);
    }

    void close() {
        listener.close();
    }
};

// Test: base -> cell entity call over TCP
static void testTcpBaseToCellCall() {
    TEST("TCP: base -> cell entity call");

    auto dir = createDefDir();
    TcpConnection::globalInit();
    TcpStack stack;

    auto store = std::make_shared<InMemoryEntityStore>();

    BaseApp::Config baseConfig;
    baseConfig.entityDefPath = dir;
    baseConfig.componentId = 1;
    BaseApp baseApp(baseConfig, stack.baseHub, store);
    baseApp.init();

    CellApp::Config cellConfig;
    cellConfig.entityDefPath = dir;
    cellConfig.componentId = 2;
    CellApp cellApp(cellConfig, stack.cellHub);
    cellApp.init();

    auto* baseEntity = baseApp.createEntity("Avatar");
    auto entityId = baseEntity->id();
    auto* cellEntity = cellApp.createEntity("Avatar", Vector3{0, 0, 0}, entityId);

    baseEntity->bindCellEntityCall(2);
    cellEntity->bindBaseEntityCall(1);

    std::int32_t receivedDamage = 0;
    cellEntity->bindMethodHandler("onDamage", [&receivedDamage](Entity& e, std::span<const std::byte> payload) {
        static_cast<void>(e);
        if (payload.size() >= sizeof(std::int32_t)) {
            std::memcpy(&receivedDamage, payload.data(), sizeof(std::int32_t));
        }
    });

    std::int32_t damage = 25;
    std::vector<std::byte> damagePayload(sizeof(damage));
    std::memcpy(damagePayload.data(), &damage, sizeof(damage));

    auto result = baseEntity->cellEntityCall()->call(*stack.baseHub, "onDamage", damagePayload);
    bool ok = result == SendResult::Accepted;

    stack.pump();
    cellApp.runtime().pumpInbound();

    ok = ok && receivedDamage == 25;

    stack.close();
    TcpConnection::globalShutdown();
    std::filesystem::remove_all(dir);

    if (ok) PASS();
    else FAIL("receivedDamage=" + std::to_string(receivedDamage));
}

// Test: cell -> base entity call over TCP
static void testTcpCellToBaseCall() {
    TEST("TCP: cell -> base entity call");

    auto dir = createDefDir();
    TcpConnection::globalInit();
    TcpStack stack;

    auto store = std::make_shared<InMemoryEntityStore>();

    BaseApp::Config baseConfig;
    baseConfig.entityDefPath = dir;
    baseConfig.componentId = 1;
    BaseApp baseApp(baseConfig, stack.baseHub, store);
    baseApp.init();

    CellApp::Config cellConfig;
    cellConfig.entityDefPath = dir;
    cellConfig.componentId = 2;
    CellApp cellApp(cellConfig, stack.cellHub);
    cellApp.init();

    auto* baseEntity = baseApp.createEntity("Avatar");
    auto entityId = baseEntity->id();
    auto* cellEntity = cellApp.createEntity("Avatar", Vector3{0, 0, 0}, entityId);

    baseEntity->bindCellEntityCall(2);
    cellEntity->bindBaseEntityCall(1);

    float receivedHeal = 0;
    baseEntity->bindMethodHandler("onHeal", [&receivedHeal](Entity& e, std::span<const std::byte> payload) {
        static_cast<void>(e);
        if (payload.size() >= sizeof(float)) {
            std::memcpy(&receivedHeal, payload.data(), sizeof(float));
        }
    });

    float heal = 50.5f;
    std::vector<std::byte> healPayload(sizeof(heal));
    std::memcpy(healPayload.data(), &heal, sizeof(heal));

    auto result = cellEntity->baseEntityCall()->call(*stack.cellHub, "onHeal", healPayload);
    bool ok = result == SendResult::Accepted;

    stack.pump();
    baseApp.runtime().pumpInbound();

    ok = ok && receivedHeal == 50.5f;

    stack.close();
    TcpConnection::globalShutdown();
    std::filesystem::remove_all(dir);

    if (ok) PASS();
    else FAIL("receivedHeal=" + std::to_string(receivedHeal));
}

// Test: bidirectional ping-pong over TCP
static void testTcpBidirectionalPingPong() {
    TEST("TCP: bidirectional ping-pong");

    auto dir = createDefDir();
    TcpConnection::globalInit();
    TcpStack stack;

    auto store = std::make_shared<InMemoryEntityStore>();

    BaseApp::Config baseConfig;
    baseConfig.entityDefPath = dir;
    baseConfig.componentId = 1;
    BaseApp baseApp(baseConfig, stack.baseHub, store);
    baseApp.init();

    CellApp::Config cellConfig;
    cellConfig.entityDefPath = dir;
    cellConfig.componentId = 2;
    CellApp cellApp(cellConfig, stack.cellHub);
    cellApp.init();

    auto* baseEntity = baseApp.createEntity("Avatar");
    auto entityId = baseEntity->id();
    auto* cellEntity = cellApp.createEntity("Avatar", Vector3{0, 0, 0}, entityId);

    baseEntity->bindCellEntityCall(2);
    cellEntity->bindBaseEntityCall(1);

    int callCount = 0;
    cellEntity->bindMethodHandler("onDamage", [&callCount, &stack](Entity& e, std::span<const std::byte> payload) {
        static_cast<void>(payload);
        ++callCount;
        e.baseEntityCall()->call(*stack.cellHub, "onHeal", {});
    });

    baseEntity->bindMethodHandler("onHeal", [&callCount](Entity& e, std::span<const std::byte> payload) {
        static_cast<void>(e);
        static_cast<void>(payload);
        ++callCount;
    });

    baseEntity->cellEntityCall()->call(*stack.baseHub, "onDamage", {});

    // Base sends damage -> cell receives -> cell sends heal -> base receives
    stack.pump();
    cellApp.runtime().pumpInbound();
    stack.pump();
    baseApp.runtime().pumpInbound();

    bool ok = callCount == 2;

    stack.close();
    TcpConnection::globalShutdown();
    std::filesystem::remove_all(dir);

    if (ok) PASS();
    else FAIL("callCount=" + std::to_string(callCount));
}

// Test: multiple sequential calls over same TCP connection
static void testTcpMultipleCalls() {
    TEST("TCP: multiple sequential calls on same connection");

    auto dir = createDefDir();
    TcpConnection::globalInit();
    TcpStack stack;

    auto store = std::make_shared<InMemoryEntityStore>();

    BaseApp::Config baseConfig;
    baseConfig.entityDefPath = dir;
    baseConfig.componentId = 1;
    BaseApp baseApp(baseConfig, stack.baseHub, store);
    baseApp.init();

    CellApp::Config cellConfig;
    cellConfig.entityDefPath = dir;
    cellConfig.componentId = 2;
    CellApp cellApp(cellConfig, stack.cellHub);
    cellApp.init();

    auto* baseEntity = baseApp.createEntity("Avatar");
    auto entityId = baseEntity->id();
    auto* cellEntity = cellApp.createEntity("Avatar", Vector3{0, 0, 0}, entityId);

    baseEntity->bindCellEntityCall(2);
    cellEntity->bindBaseEntityCall(1);

    std::int32_t totalDamage = 0;
    cellEntity->bindMethodHandler("onDamage", [&totalDamage](Entity& e, std::span<const std::byte> payload) {
        static_cast<void>(e);
        if (payload.size() >= sizeof(std::int32_t)) {
            std::int32_t dmg = 0;
            std::memcpy(&dmg, payload.data(), sizeof(std::int32_t));
            totalDamage += dmg;
        }
    });

    // Send 3 damage calls
    std::int32_t damages[] = {10, 20, 30};
    for (auto dmg : damages) {
        std::vector<std::byte> payload(sizeof(dmg));
        std::memcpy(payload.data(), &dmg, sizeof(dmg));
        baseEntity->cellEntityCall()->call(*stack.baseHub, "onDamage", payload);
    }

    stack.pump();
    cellApp.runtime().pumpInbound();

    bool ok = totalDamage == 60;

    stack.close();
    TcpConnection::globalShutdown();
    std::filesystem::remove_all(dir);

    if (ok) PASS();
    else FAIL("totalDamage=" + std::to_string(totalDamage));
}

int main() {
    std::cout << "TCP Integration tests:\n";

    testTcpBaseToCellCall();
    testTcpCellToBaseCall();
    testTcpBidirectionalPingPong();
    testTcpMultipleCalls();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
