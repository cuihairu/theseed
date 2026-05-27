#include "theseed/core/BaseApp.h"
#include "theseed/core/CellApp.h"
#include "theseed/core/IEntityStore.h"
#include "theseed/runtime/Entity.h"
#include "theseed/runtime/EntityDef.h"
#include "theseed/runtime/NetworkTransport.h"
#include "theseed/runtime/RuntimeTypes.h"
#include "theseed/runtime/TcpConnection.h"
#include "theseed/runtime/TcpListener.h"
#include "theseed/runtime/TickScheduler.h"
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
using theseed::runtime::IRuntimeTransport;
using theseed::runtime::NetworkTransport;
using theseed::runtime::SendResult;
using theseed::runtime::TcpConnection;
using theseed::runtime::TcpListener;
using theseed::runtime::TickScheduler;
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
    std::string dir = "test_remote_entity_defs";
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
        for (int i = 0; i < iterations; ++i) {
            clientConn->pump();
            serverConn->pump();
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }
    }

    void close() { listener.close(); }
};

// Test: BaseApp creates base entity, requests cell creation on CellApp, calls entity
static void testRemoteCreateCell() {
    TEST("remote createCell: base requests cell entity on remote CellApp");

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

    // Create base entity
    auto* baseEntity = baseApp.createEntity("Avatar");
    bool ok = baseEntity != nullptr;
    auto entityId = baseEntity->id();

    // Base entity should NOT have cell call yet
    ok = ok && baseEntity->cellEntityCall() == nullptr;

    // Bind method handler BEFORE remote creation
    std::int32_t receivedDamage = 0;
    // Note: cell entity doesn't exist yet, so we can't bind handler on it.
    // We'll bind after the cellReady comes back.

    // Request cell creation on CellApp
    ok = ok && baseApp.requestCreateCell(entityId, "Avatar", Vector3{10, 20, 30}, 2);

    // Pump: createCell -> CellApp, cellReady -> BaseApp
    stack.pump();
    cellApp.runtime().pumpInbound();  // processes entity.createCell
    stack.pump();
    baseApp.runtime().pumpInbound();  // processes entity.cellReady

    // Now base entity should have cell call bound
    ok = ok && baseEntity->cellEntityCall()->isValid();
    ok = ok && baseEntity->cellEntityCall()->targetComponent() == 2;

    // Verify cell entity exists on CellApp
    auto* cellEntity = cellApp.runtime().findEntity(entityId);
    ok = ok && cellEntity != nullptr;

    if (ok) {
        cellEntity->bindBaseEntityCall(1);

        cellEntity->bindMethodHandler("onDamage", [&receivedDamage](Entity& e, std::span<const std::byte> payload) {
            static_cast<void>(e);
            if (payload.size() >= sizeof(std::int32_t)) {
                std::memcpy(&receivedDamage, payload.data(), sizeof(std::int32_t));
            }
        });

        // Now base can call cell entity
        std::int32_t damage = 42;
        std::vector<std::byte> payload(sizeof(damage));
        std::memcpy(payload.data(), &damage, sizeof(damage));
        baseEntity->cellEntityCall()->call(*stack.baseHub, "onDamage", payload);

        stack.pump();
        cellApp.runtime().pumpInbound();

        ok = receivedDamage == 42;
    }

    stack.close();
    TcpConnection::globalShutdown();
    std::filesystem::remove_all(dir);

    if (ok) PASS();
    else FAIL("createCell flow failed");
}

// Test: remote destroyCell
static void testRemoteDestroyCell() {
    TEST("remote destroyCell: base destroys cell entity on remote CellApp");

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

    baseApp.requestCreateCell(entityId, "Avatar", Vector3{0, 0, 0}, 2);
    stack.pump();
    cellApp.runtime().pumpInbound();
    stack.pump();
    baseApp.runtime().pumpInbound();

    bool ok = baseEntity->cellEntityCall()->isValid();
    ok = ok && cellApp.runtime().findEntity(entityId) != nullptr;

    // Request destroy
    ok = ok && baseApp.requestDestroyCell(entityId, 2);

    stack.pump();
    cellApp.runtime().pumpInbound();  // processes entity.destroyCell
    stack.pump();
    baseApp.runtime().pumpInbound();  // processes entity.cellDestroyed

    // Cell entity should be gone
    ok = ok && cellApp.runtime().findEntity(entityId) == nullptr;

    // Base entity cell call should be cleared
    ok = ok && baseEntity->cellEntityCall() == nullptr;

    stack.close();
    TcpConnection::globalShutdown();
    std::filesystem::remove_all(dir);

    if (ok) PASS();
    else FAIL("destroyCell flow failed");
}

// Test: full lifecycle - create, call, destroy over TCP
static void testRemoteFullLifecycle() {
    TEST("remote full lifecycle: create -> call -> destroy over TCP");

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

    // 1. Create base entity and request cell
    auto* baseEntity = baseApp.createEntity("Avatar");
    auto entityId = baseEntity->id();
    bool ok = baseApp.requestCreateCell(entityId, "Avatar", Vector3{5, 0, 0}, 2);

    stack.pump();
    cellApp.runtime().pumpInbound();
    stack.pump();
    baseApp.runtime().pumpInbound();

    ok = ok && baseEntity->cellEntityCall()->isValid();

    // 2. Bind handler and call
    auto* cellEntity = cellApp.runtime().findEntity(entityId);
    ok = ok && cellEntity != nullptr;
    if (cellEntity) {
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

        // Trigger ping-pong
        baseEntity->cellEntityCall()->call(*stack.baseHub, "onDamage", {});
        stack.pump();
        cellApp.runtime().pumpInbound();
        stack.pump();
        baseApp.runtime().pumpInbound();

        ok = ok && callCount == 2;
    }

    // 3. Destroy cell
    ok = ok && baseApp.requestDestroyCell(entityId, 2);
    stack.pump();
    cellApp.runtime().pumpInbound();
    stack.pump();
    baseApp.runtime().pumpInbound();

    ok = ok && baseEntity->cellEntityCall() == nullptr;
    ok = ok && cellApp.runtime().findEntity(entityId) == nullptr;

    // 4. Base entity still exists
    ok = ok && baseApp.findEntity(entityId) != nullptr;

    stack.close();
    TcpConnection::globalShutdown();
    std::filesystem::remove_all(dir);

    if (ok) PASS();
    else FAIL("lifecycle flow failed");
}

int main() {
    std::cout << "Remote entity creation tests:\n";

    testRemoteCreateCell();
    testRemoteDestroyCell();
    testRemoteFullLifecycle();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
