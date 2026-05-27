#include "theseed/core/BaseApp.h"
#include "theseed/core/CellApp.h"
#include "theseed/core/IEntityStore.h"
#include "theseed/runtime/Entity.h"
#include "theseed/runtime/EntityDef.h"
#include "theseed/runtime/NetworkNode.h"
#include "theseed/runtime/RuntimeTypes.h"
#include "theseed/runtime/TcpConnection.h"
#include "theseed/runtime/TickScheduler.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

using theseed::core::BaseApp;
using theseed::core::CellApp;
using theseed::core::InMemoryEntityStore;
using theseed::runtime::Entity;
using theseed::runtime::EntityId;
using theseed::runtime::NetworkNode;
using theseed::runtime::SendResult;
using theseed::runtime::TcpConnection;
using theseed::runtime::TickScheduler;
using theseed::runtime::TickPhase;
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
    std::string dir = "test_tick_node_defs";
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

// Shared scheduler ticks: Network nodes (pump transports) + apps (pump inbound)
// Tick order: Network phase -> nodes pump + apps pumpInbound
struct Cluster {
    std::shared_ptr<NetworkNode> baseNode;
    std::shared_ptr<NetworkNode> cellNode;
    std::unique_ptr<BaseApp> baseApp;
    std::unique_ptr<CellApp> cellApp;
    TickScheduler scheduler;
    std::string defDir;
    std::shared_ptr<InMemoryEntityStore> store;
    bool cellAccepted = false;

    Cluster(const std::string& dir)
        : scheduler(std::chrono::milliseconds{8}), defDir(dir) {
        // Network nodes
        cellNode = std::make_shared<NetworkNode>(NetworkNode::Config{.localComponent = 2, .listenPort = 0});
        baseNode = std::make_shared<NetworkNode>(NetworkNode::Config{.localComponent = 1, .listenPort = 0});

        // CellApp accepts incoming from BaseApp
        cellNode->setOnPeerConnected([this](std::shared_ptr<theseed::runtime::IRuntimeTransport> transport) {
            cellNode->acceptPeer(1, transport);
            cellAccepted = true;
        });

        // Apps
        store = std::make_shared<InMemoryEntityStore>();

        BaseApp::Config baseConfig;
        baseConfig.entityDefPath = dir;
        baseConfig.componentId = 1;
        baseApp = std::make_unique<BaseApp>(baseConfig, baseNode->hub(), store);
        baseApp->init();

        CellApp::Config cellConfig;
        cellConfig.entityDefPath = dir;
        cellConfig.componentId = 2;
        cellApp = std::make_unique<CellApp>(cellConfig, cellNode->hub());
        cellApp->init();

        // Attach to scheduler
        baseApp->attach(scheduler);
        cellApp->attach(scheduler);
        baseNode->attach(scheduler);
        cellNode->attach(scheduler);
    }

    // Connect BaseApp to CellApp
    bool connect() {
        return baseNode->connectToPeer(2, "127.0.0.1", cellNode->listenPort());
    }

    // Run ticks until predicate returns true or maxIterations reached
    template <typename Pred>
    int tickUntil(Pred pred, int maxIterations = 300) {
        for (int i = 0; i < maxIterations; ++i) {
            scheduler.runOnce();
            if (pred()) return i + 1;
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        return -1;
    }

    ~Cluster() {
        baseApp->detach(scheduler);
        cellApp->detach(scheduler);
        baseNode->detach(scheduler);
        cellNode->detach(scheduler);
    }
};

// Test: NetworkNode connect via acceptPeer, tick-driven
static void testTickDrivenConnect() {
    TEST("tick-driven: NetworkNode connect + acceptPeer");

    auto dir = createDefDir();
    TcpConnection::globalInit();
    Cluster cluster(dir);

    bool connected = cluster.connect();
    bool ok = connected;

    int ticks = cluster.tickUntil([&] {
        return cluster.cellAccepted && cluster.baseNode->hasPeer(2) && cluster.cellNode->hasPeer(1);
    });

    ok = ok && ticks > 0;

    TcpConnection::globalShutdown();
    std::filesystem::remove_all(dir);

    if (ok) PASS();
    else FAIL("ticks=" + std::to_string(ticks) + " accepted=" + std::to_string(cluster.cellAccepted));
}

// Test: Full tick-driven lifecycle: connect, createCell, call, destroyCell
static void testTickDrivenFullLifecycle() {
    TEST("tick-driven: full lifecycle via NetworkNode");

    auto dir = createDefDir();
    TcpConnection::globalInit();
    Cluster cluster(dir);

    cluster.connect();

    // Step 1: Wait for connection
    int ticks = cluster.tickUntil([&] {
        return cluster.baseNode->hasPeer(2) && cluster.cellNode->hasPeer(1);
    });
    bool ok = ticks > 0;

    // Step 2: Create base entity and request cell
    auto* baseEntity = cluster.baseApp->createEntity("Avatar");
    ok = ok && baseEntity != nullptr;
    auto entityId = baseEntity->id();

    ok = ok && cluster.baseApp->requestCreateCell(entityId, "Avatar", Vector3{10, 20, 30}, 2);

    // Wait for cellReady (base entity gets cell call bound)
    ticks = cluster.tickUntil([&] {
        return baseEntity->cellEntityCall() != nullptr && baseEntity->cellEntityCall()->isValid();
    });
    ok = ok && ticks > 0;
    ok = ok && baseEntity->cellEntityCall()->targetComponent() == 2;

    // Step 3: Verify cell entity exists on CellApp
    auto* cellEntity = cluster.cellApp->runtime().findEntity(entityId);
    ok = ok && cellEntity != nullptr;

    // Step 4: Bind handler and send entity call
    if (cellEntity) {
        cellEntity->bindBaseEntityCall(1);

        std::int32_t receivedDamage = 0;
        cellEntity->bindMethodHandler("onDamage", [&receivedDamage](Entity& e, std::span<const std::byte> payload) {
            static_cast<void>(e);
            if (payload.size() >= sizeof(std::int32_t)) {
                std::memcpy(&receivedDamage, payload.data(), sizeof(std::int32_t));
            }
        });

        std::int32_t damage = 77;
        std::vector<std::byte> payload(sizeof(damage));
        std::memcpy(payload.data(), &damage, sizeof(damage));
        baseEntity->cellEntityCall()->call(*cluster.baseNode->hub(), "onDamage", payload);

        ticks = cluster.tickUntil([&] {
            return receivedDamage == 77;
        });
        ok = ok && ticks > 0;
    }

    // Step 5: Destroy cell
    ok = ok && cluster.baseApp->requestDestroyCell(entityId, 2);

    ticks = cluster.tickUntil([&] {
        return baseEntity->cellEntityCall() == nullptr;
    });
    ok = ok && ticks > 0;
    ok = ok && cluster.cellApp->runtime().findEntity(entityId) == nullptr;

    // Step 6: Base entity still exists
    ok = ok && cluster.baseApp->findEntity(entityId) != nullptr;

    TcpConnection::globalShutdown();
    std::filesystem::remove_all(dir);

    if (ok) PASS();
    else FAIL("lifecycle step failed");
}

// Test: Tick-driven bidirectional ping-pong after remote entity creation
static void testTickDrivenPingPongAfterCreate() {
    TEST("tick-driven: ping-pong after remote createCell");

    auto dir = createDefDir();
    TcpConnection::globalInit();
    Cluster cluster(dir);

    cluster.connect();

    // Connect
    cluster.tickUntil([&] {
        return cluster.baseNode->hasPeer(2) && cluster.cellNode->hasPeer(1);
    });

    // Create and wait for cell ready
    auto* baseEntity = cluster.baseApp->createEntity("Avatar");
    auto entityId = baseEntity->id();
    cluster.baseApp->requestCreateCell(entityId, "Avatar", Vector3{0, 0, 0}, 2);

    cluster.tickUntil([&] {
        return baseEntity->cellEntityCall() != nullptr && baseEntity->cellEntityCall()->isValid();
    });

    auto* cellEntity = cluster.cellApp->runtime().findEntity(entityId);
    bool ok = cellEntity != nullptr;
    if (!ok) {
        TcpConnection::globalShutdown();
        std::filesystem::remove_all(dir);
        FAIL("cell entity not found");
        return;
    }

    cellEntity->bindBaseEntityCall(1);

    int callCount = 0;
    cellEntity->bindMethodHandler("onDamage", [&callCount, &cluster](Entity& e, std::span<const std::byte> payload) {
        static_cast<void>(payload);
        ++callCount;
        e.baseEntityCall()->call(*cluster.cellNode->hub(), "onHeal", {});
    });

    baseEntity->bindMethodHandler("onHeal", [&callCount](Entity& e, std::span<const std::byte> payload) {
        static_cast<void>(e);
        static_cast<void>(payload);
        ++callCount;
    });

    // Trigger
    baseEntity->cellEntityCall()->call(*cluster.baseNode->hub(), "onDamage", {});

    int ticks = cluster.tickUntil([&] {
        return callCount >= 2;
    });
    ok = ticks > 0 && callCount == 2;

    TcpConnection::globalShutdown();
    std::filesystem::remove_all(dir);

    if (ok) PASS();
    else FAIL("callCount=" + std::to_string(callCount) + " ticks=" + std::to_string(ticks));
}

// Test: Multiple entities created remotely via tick-driven cluster
static void testTickDrivenMultipleEntities() {
    TEST("tick-driven: multiple remote entities with independent calls");

    auto dir = createDefDir();
    TcpConnection::globalInit();
    Cluster cluster(dir);

    cluster.connect();

    cluster.tickUntil([&] {
        return cluster.baseNode->hasPeer(2) && cluster.cellNode->hasPeer(1);
    });

    // Create 3 base entities
    std::vector<Entity*> baseEntities;
    std::vector<EntityId> entityIds;
    for (int i = 0; i < 3; ++i) {
        auto* e = cluster.baseApp->createEntity("Avatar");
        baseEntities.push_back(e);
        entityIds.push_back(e->id());
        cluster.baseApp->requestCreateCell(e->id(), "Avatar",
            Vector3{static_cast<float>(i * 10), 0, 0}, 2);
    }

    // Wait for all cell entities to be ready
    cluster.tickUntil([&] {
        for (auto* e : baseEntities) {
            if (e->cellEntityCall() == nullptr || !e->cellEntityCall()->isValid()) return false;
        }
        return true;
    });

    bool ok = true;
    std::int32_t damageReceived[3] = {};

    for (int i = 0; i < 3; ++i) {
        auto* cellEntity = cluster.cellApp->runtime().findEntity(entityIds[i]);
        ok = ok && cellEntity != nullptr;
        if (!cellEntity) continue;

        cellEntity->bindBaseEntityCall(1);
        int idx = i;
        cellEntity->bindMethodHandler("onDamage", [idx, &damageReceived](Entity& e, std::span<const std::byte> payload) {
            static_cast<void>(e);
            if (payload.size() >= sizeof(std::int32_t)) {
                std::int32_t dmg = 0;
                std::memcpy(&dmg, payload.data(), sizeof(std::int32_t));
                damageReceived[idx] += dmg;
            }
        });

        // Send unique damage per entity
        std::int32_t damage = (i + 1) * 10;
        std::vector<std::byte> payload(sizeof(damage));
        std::memcpy(payload.data(), &damage, sizeof(damage));
        baseEntities[i]->cellEntityCall()->call(*cluster.baseNode->hub(), "onDamage", payload);
    }

    cluster.tickUntil([&] {
        return damageReceived[0] == 10 && damageReceived[1] == 20 && damageReceived[2] == 30;
    });

    ok = ok && damageReceived[0] == 10 && damageReceived[1] == 20 && damageReceived[2] == 30;

    TcpConnection::globalShutdown();
    std::filesystem::remove_all(dir);

    if (ok) PASS();
    else FAIL("d0=" + std::to_string(damageReceived[0]) +
              " d1=" + std::to_string(damageReceived[1]) +
              " d2=" + std::to_string(damageReceived[2]));
}

int main() {
    std::cout << "Tick-driven NetworkNode lifecycle tests:\n";

    testTickDrivenConnect();
    testTickDrivenFullLifecycle();
    testTickDrivenPingPongAfterCreate();
    testTickDrivenMultipleEntities();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
