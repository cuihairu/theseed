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
using theseed::runtime::ComponentId;
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
    std::string dir = "test_multi_cellapp_defs";
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

// Full mesh: BaseApp(1) <-> CellApp1(2), BaseApp(1) <-> CellApp2(3), CellApp1(2) <-> CellApp2(3)
struct MeshStack {
    // Listeners
    TcpListener cell1Listener;
    TcpListener cell2Listener;

    // Connections: BaseApp <-> CellApp1
    std::shared_ptr<TcpConnection> baseToCell1Conn;
    std::shared_ptr<TcpConnection> cell1FromBaseConn;

    // Connections: BaseApp <-> CellApp2
    std::shared_ptr<TcpConnection> baseToCell2Conn;
    std::shared_ptr<TcpConnection> cell2FromBaseConn;

    // Connections: CellApp1 <-> CellApp2
    std::shared_ptr<TcpConnection> cell1ToCell2Conn;
    std::shared_ptr<TcpConnection> cell2FromCell1Conn;

    // Hubs
    std::shared_ptr<TransportHub> baseHub;
    std::shared_ptr<TransportHub> cell1Hub;
    std::shared_ptr<TransportHub> cell2Hub;

    MeshStack() {
        cell1Listener.listen("127.0.0.1", 0);
        cell2Listener.listen("127.0.0.1", 0);
        auto port1 = cell1Listener.localPort();
        auto port2 = cell2Listener.localPort();

        // BaseApp -> CellApp1
        baseToCell1Conn = TcpConnection::create();
        baseToCell1Conn->connect("127.0.0.1", port1);
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        cell1FromBaseConn = cell1Listener.accept();
        std::this_thread::sleep_for(std::chrono::milliseconds{50});

        // BaseApp -> CellApp2
        baseToCell2Conn = TcpConnection::create();
        baseToCell2Conn->connect("127.0.0.1", port2);
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        cell2FromBaseConn = cell2Listener.accept();
        std::this_thread::sleep_for(std::chrono::milliseconds{50});

        // CellApp1 -> CellApp2
        cell1ToCell2Conn = TcpConnection::create();
        cell1ToCell2Conn->connect("127.0.0.1", port2);
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        cell2FromCell1Conn = cell2Listener.accept();
        std::this_thread::sleep_for(std::chrono::milliseconds{50});

        // Create transports and hubs
        auto baseTransport1 = std::make_shared<NetworkTransport>(baseToCell1Conn);
        auto cell1TransportBase = std::make_shared<NetworkTransport>(cell1FromBaseConn);
        auto baseTransport2 = std::make_shared<NetworkTransport>(baseToCell2Conn);
        auto cell2TransportBase = std::make_shared<NetworkTransport>(cell2FromBaseConn);
        auto cell1TransportCell2 = std::make_shared<NetworkTransport>(cell1ToCell2Conn);
        auto cell2TransportCell1 = std::make_shared<NetworkTransport>(cell2FromCell1Conn);

        baseHub = std::make_shared<TransportHub>(1);
        cell1Hub = std::make_shared<TransportHub>(2);
        cell2Hub = std::make_shared<TransportHub>(3);

        // BaseApp: peers are CellApp1(2) and CellApp2(3)
        baseHub->connectPeer(2, baseTransport1);
        baseHub->connectPeer(3, baseTransport2);

        // CellApp1: peers are BaseApp(1) and CellApp2(3)
        cell1Hub->connectPeer(1, cell1TransportBase);
        cell1Hub->connectPeer(3, cell1TransportCell2);

        // CellApp2: peers are BaseApp(1) and CellApp1(2)
        cell2Hub->connectPeer(1, cell2TransportBase);
        cell2Hub->connectPeer(2, cell2TransportCell1);
    }

    void pump(int iterations = 80) {
        for (int i = 0; i < iterations; ++i) {
            baseToCell1Conn->pump();
            cell1FromBaseConn->pump();
            baseToCell2Conn->pump();
            cell2FromBaseConn->pump();
            cell1ToCell2Conn->pump();
            cell2FromCell1Conn->pump();
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }
    }

    void close() {
        cell1Listener.close();
        cell2Listener.close();
    }
};

// Test: BaseApp routes entity call to correct CellApp
static void testMultiCellAppRouting() {
    TEST("multi-CellApp routing: base calls correct CellApp");

    auto dir = createDefDir();
    TcpConnection::globalInit();
    MeshStack mesh;

    auto store = std::make_shared<InMemoryEntityStore>();

    BaseApp::Config baseConfig;
    baseConfig.entityDefPath = dir;
    baseConfig.componentId = 1;
    BaseApp baseApp(baseConfig, mesh.baseHub, store);
    baseApp.init();

    CellApp::Config cell1Config;
    cell1Config.entityDefPath = dir;
    cell1Config.componentId = 2;
    CellApp cellApp1(cell1Config, mesh.cell1Hub);
    cellApp1.init();

    CellApp::Config cell2Config;
    cell2Config.entityDefPath = dir;
    cell2Config.componentId = 3;
    CellApp cellApp2(cell2Config, mesh.cell2Hub);
    cellApp2.init();

    // Create entities on different CellApps
    auto* baseEntity1 = baseApp.createEntity("Avatar");
    auto* baseEntity2 = baseApp.createEntity("Avatar");
    auto id1 = baseEntity1->id();
    auto id2 = baseEntity2->id();

    auto* cellEntity1 = cellApp1.createEntity("Avatar", Vector3{10, 0, 0}, id1);
    auto* cellEntity2 = cellApp2.createEntity("Avatar", Vector3{20, 0, 0}, id2);

    baseEntity1->bindCellEntityCall(2);  // -> CellApp1
    baseEntity2->bindCellEntityCall(3);  // -> CellApp2
    cellEntity1->bindBaseEntityCall(1);
    cellEntity2->bindBaseEntityCall(1);

    std::int32_t damageReceived1 = 0;
    std::int32_t damageReceived2 = 0;

    cellEntity1->bindMethodHandler("onDamage", [&damageReceived1](Entity& e, std::span<const std::byte> payload) {
        static_cast<void>(e);
        if (payload.size() >= sizeof(std::int32_t)) {
            std::memcpy(&damageReceived1, payload.data(), sizeof(std::int32_t));
        }
    });

    cellEntity2->bindMethodHandler("onDamage", [&damageReceived2](Entity& e, std::span<const std::byte> payload) {
        static_cast<void>(e);
        if (payload.size() >= sizeof(std::int32_t)) {
            std::memcpy(&damageReceived2, payload.data(), sizeof(std::int32_t));
        }
    });

    // Send damage to entity1 (CellApp1)
    std::int32_t damage1 = 100;
    std::vector<std::byte> payload1(sizeof(damage1));
    std::memcpy(payload1.data(), &damage1, sizeof(damage1));
    baseEntity1->cellEntityCall()->call(*mesh.baseHub, "onDamage", payload1);

    // Send damage to entity2 (CellApp2)
    std::int32_t damage2 = 200;
    std::vector<std::byte> payload2(sizeof(damage2));
    std::memcpy(payload2.data(), &damage2, sizeof(damage2));
    baseEntity2->cellEntityCall()->call(*mesh.baseHub, "onDamage", payload2);

    mesh.pump();
    cellApp1.runtime().pumpInbound();
    cellApp2.runtime().pumpInbound();

    bool ok = damageReceived1 == 100 && damageReceived2 == 200;

    mesh.close();
    TcpConnection::globalShutdown();
    std::filesystem::remove_all(dir);

    if (ok) PASS();
    else FAIL("d1=" + std::to_string(damageReceived1) + " d2=" + std::to_string(damageReceived2));
}

// Test: both CellApps call back to BaseApp
static void testMultiCellAppReverseCalls() {
    TEST("multi-CellApp: both cells call back to base");

    auto dir = createDefDir();
    TcpConnection::globalInit();
    MeshStack mesh;

    auto store = std::make_shared<InMemoryEntityStore>();

    BaseApp::Config baseConfig;
    baseConfig.entityDefPath = dir;
    baseConfig.componentId = 1;
    BaseApp baseApp(baseConfig, mesh.baseHub, store);
    baseApp.init();

    CellApp::Config cell1Config;
    cell1Config.entityDefPath = dir;
    cell1Config.componentId = 2;
    CellApp cellApp1(cell1Config, mesh.cell1Hub);
    cellApp1.init();

    CellApp::Config cell2Config;
    cell2Config.entityDefPath = dir;
    cell2Config.componentId = 3;
    CellApp cellApp2(cell2Config, mesh.cell2Hub);
    cellApp2.init();

    auto* baseEntity1 = baseApp.createEntity("Avatar");
    auto* baseEntity2 = baseApp.createEntity("Avatar");
    auto id1 = baseEntity1->id();
    auto id2 = baseEntity2->id();

    auto* cellEntity1 = cellApp1.createEntity("Avatar", Vector3{0, 0, 0}, id1);
    auto* cellEntity2 = cellApp2.createEntity("Avatar", Vector3{0, 0, 0}, id2);

    baseEntity1->bindCellEntityCall(2);
    baseEntity2->bindCellEntityCall(3);
    cellEntity1->bindBaseEntityCall(1);
    cellEntity2->bindBaseEntityCall(1);

    float totalHeal = 0;
    baseEntity1->bindMethodHandler("onHeal", [&totalHeal](Entity& e, std::span<const std::byte> payload) {
        static_cast<void>(e);
        if (payload.size() >= sizeof(float)) {
            float h = 0;
            std::memcpy(&h, payload.data(), sizeof(float));
            totalHeal += h;
        }
    });
    baseEntity2->bindMethodHandler("onHeal", [&totalHeal](Entity& e, std::span<const std::byte> payload) {
        static_cast<void>(e);
        if (payload.size() >= sizeof(float)) {
            float h = 0;
            std::memcpy(&h, payload.data(), sizeof(float));
            totalHeal += h;
        }
    });

    // CellApp1 sends heal for entity1
    float heal1 = 10.0f;
    std::vector<std::byte> hp1(sizeof(heal1));
    std::memcpy(hp1.data(), &heal1, sizeof(heal1));
    cellEntity1->baseEntityCall()->call(*mesh.cell1Hub, "onHeal", hp1);

    // CellApp2 sends heal for entity2
    float heal2 = 20.0f;
    std::vector<std::byte> hp2(sizeof(heal2));
    std::memcpy(hp2.data(), &heal2, sizeof(heal2));
    cellEntity2->baseEntityCall()->call(*mesh.cell2Hub, "onHeal", hp2);

    mesh.pump();
    baseApp.runtime().pumpInbound();

    bool ok = totalHeal == 30.0f;

    mesh.close();
    TcpConnection::globalShutdown();
    std::filesystem::remove_all(dir);

    if (ok) PASS();
    else FAIL("totalHeal=" + std::to_string(totalHeal));
}

// Test: cross-CellApp entity migration over TCP
static void testCrossCellAppMigration() {
    TEST("cross-CellApp migration over TCP");

    auto dir = createDefDir();
    TcpConnection::globalInit();
    MeshStack mesh;

    auto store = std::make_shared<InMemoryEntityStore>();

    BaseApp::Config baseConfig;
    baseConfig.entityDefPath = dir;
    baseConfig.componentId = 1;
    BaseApp baseApp(baseConfig, mesh.baseHub, store);
    baseApp.init();

    CellApp::Config cell1Config;
    cell1Config.entityDefPath = dir;
    cell1Config.componentId = 2;
    CellApp cellApp1(cell1Config, mesh.cell1Hub);
    cellApp1.init();

    CellApp::Config cell2Config;
    cell2Config.entityDefPath = dir;
    cell2Config.componentId = 3;
    CellApp cellApp2(cell2Config, mesh.cell2Hub);
    cellApp2.init();

    // Create entity on CellApp1
    auto* baseEntity = baseApp.createEntity("Avatar");
    auto entityId = baseEntity->id();
    baseEntity->bindCellEntityCall(2);

    auto* cellEntity = cellApp1.createEntity("Avatar", Vector3{5, 0, 0}, entityId);
    cellEntity->bindBaseEntityCall(1);
    cellEntity->setProperty<std::int32_t>(0, 42);

    // Verify entity exists on CellApp1
    bool ok = cellApp1.runtime().findEntity(entityId) != nullptr;
    ok = ok && cellApp2.runtime().findEntity(entityId) == nullptr;

    // Initiate migration from CellApp1(2) to CellApp2(3)
    ok = ok && cellApp1.runtime().beginMigration(entityId, 3, 1);

    // Pump: transfer -> CellApp2, commit -> CellApp1
    mesh.pump();
    cellApp2.runtime().pumpInbound();  // receives migration.transfer, sends migration.commit
    mesh.pump();
    cellApp1.runtime().pumpInbound();  // receives migration.commit, destroys local entity

    // Verify entity moved to CellApp2
    ok = ok && cellApp1.runtime().findEntity(entityId) == nullptr;
    ok = ok && cellApp2.runtime().findEntity(entityId) != nullptr;

    mesh.close();
    TcpConnection::globalShutdown();
    std::filesystem::remove_all(dir);

    if (ok) PASS();
    else FAIL("migration failed");
}

// Test: after migration, base can call entity on new CellApp
static void testPostMigrationCall() {
    TEST("post-migration: base calls entity on new CellApp");

    auto dir = createDefDir();
    TcpConnection::globalInit();
    MeshStack mesh;

    auto store = std::make_shared<InMemoryEntityStore>();

    BaseApp::Config baseConfig;
    baseConfig.entityDefPath = dir;
    baseConfig.componentId = 1;
    BaseApp baseApp(baseConfig, mesh.baseHub, store);
    baseApp.init();

    CellApp::Config cell1Config;
    cell1Config.entityDefPath = dir;
    cell1Config.componentId = 2;
    CellApp cellApp1(cell1Config, mesh.cell1Hub);
    cellApp1.init();

    CellApp::Config cell2Config;
    cell2Config.entityDefPath = dir;
    cell2Config.componentId = 3;
    CellApp cellApp2(cell2Config, mesh.cell2Hub);
    cellApp2.init();

    auto* baseEntity = baseApp.createEntity("Avatar");
    auto entityId = baseEntity->id();
    baseEntity->bindCellEntityCall(2);  // initially on CellApp1

    auto* cellEntity = cellApp1.createEntity("Avatar", Vector3{5, 0, 0}, entityId);
    cellEntity->bindBaseEntityCall(1);

    // Migrate from CellApp1 to CellApp2
    cellApp1.runtime().beginMigration(entityId, 3, 1);
    mesh.pump();
    cellApp2.runtime().pumpInbound();
    mesh.pump();
    cellApp1.runtime().pumpInbound();

    bool ok = cellApp2.runtime().findEntity(entityId) != nullptr;

    if (!ok) {
        mesh.close();
        TcpConnection::globalShutdown();
        std::filesystem::remove_all(dir);
        FAIL("migration failed in setup");
        return;
    }

    // Update base routing to point to CellApp2
    baseEntity->bindCellEntityCall(3);

    // Bind handler on migrated entity
    auto* migratedEntity = cellApp2.runtime().findEntity(entityId);
    ok = ok && migratedEntity != nullptr;
    if (!migratedEntity) {
        mesh.close();
        TcpConnection::globalShutdown();
        std::filesystem::remove_all(dir);
        FAIL("migrated entity is null");
        return;
    }

    migratedEntity->bindBaseEntityCall(1);

    std::int32_t receivedDamage = 0;
    migratedEntity->bindMethodHandler("onDamage", [&receivedDamage](Entity& e, std::span<const std::byte> payload) {
        static_cast<void>(e);
        if (payload.size() >= sizeof(std::int32_t)) {
            std::memcpy(&receivedDamage, payload.data(), sizeof(std::int32_t));
        }
    });

    // Base calls entity on new CellApp2
    std::int32_t damage = 99;
    std::vector<std::byte> payload(sizeof(damage));
    std::memcpy(payload.data(), &damage, sizeof(damage));
    baseEntity->cellEntityCall()->call(*mesh.baseHub, "onDamage", payload);

    mesh.pump();
    cellApp2.runtime().pumpInbound();

    ok = ok && receivedDamage == 99;

    mesh.close();
    TcpConnection::globalShutdown();
    std::filesystem::remove_all(dir);

    if (ok) PASS();
    else FAIL("receivedDamage=" + std::to_string(receivedDamage));
}

int main() {
    std::cout << "Multi-CellApp TCP tests:\n";

    testMultiCellAppRouting();
    testMultiCellAppReverseCalls();
    testCrossCellAppMigration();
    testPostMigrationCall();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
