#include "theseed/core/BaseApp.h"
#include "theseed/core/CellApp.h"
#include "theseed/core/IEntityStore.h"
#include "theseed/runtime/Entity.h"
#include "theseed/runtime/EntityDef.h"
#include "theseed/runtime/NetworkNode.h"
#include "theseed/runtime/RuntimeTypes.h"
#include "theseed/runtime/TcpConnection.h"
#include "theseed/runtime/TickScheduler.h"

#include <cmath>
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
using theseed::runtime::EntitySide;
using theseed::runtime::NetworkNode;
using theseed::runtime::TcpConnection;
using theseed::runtime::TickScheduler;
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

// Def with Base-only property "gold" and Cell-only property "speed"
static std::string createDefDir() {
    std::string dir = "test_sync_dir_defs";
    std::filesystem::create_directory(dir);
    writeFile(dir + "/Avatar.xml", R"(
<EntityDef name="Avatar">
    <Properties>
        <Property name="level" type="Int32"/>
        <Property name="hp" type="Float32"/>
        <Property name="gold" type="Int32" flags="Base"/>
        <Property name="speed" type="Float32" flags="Cell"/>
    </Properties>
    <Methods>
        <Method name="onDamage" side="Cell"/>
        <Method name="onHeal" side="Base"/>
    </Methods>
</EntityDef>
)");
    return dir;
}

struct MiniCluster {
    std::shared_ptr<NetworkNode> baseNode;
    std::shared_ptr<NetworkNode> cellNode;
    std::unique_ptr<BaseApp> baseApp;
    std::unique_ptr<CellApp> cellApp;
    TickScheduler scheduler;
    std::string defDir;
    std::shared_ptr<InMemoryEntityStore> store;

    MiniCluster(const std::string& dir)
        : scheduler(std::chrono::milliseconds{8}), defDir(dir) {
        cellNode = std::make_shared<NetworkNode>(NetworkNode::Config{.localComponent = 2, .listenPort = 0});
        baseNode = std::make_shared<NetworkNode>(NetworkNode::Config{.localComponent = 1, .listenPort = 0});

        cellNode->setOnPeerConnected([this](std::shared_ptr<theseed::runtime::IRuntimeTransport> transport) {
            cellNode->acceptPeer(1, transport);
        });

        store = std::make_shared<InMemoryEntityStore>();
        baseApp = std::make_unique<BaseApp>(BaseApp::Config{.entityDefPath = dir, .componentId = 1}, baseNode->hub(), store);
        baseApp->init();
        cellApp = std::make_unique<CellApp>(CellApp::Config{.entityDefPath = dir, .componentId = 2}, cellNode->hub());
        cellApp->init();

        baseNode->attach(scheduler);
        cellNode->attach(scheduler);
        baseApp->attach(scheduler);
        cellApp->attach(scheduler);
    }

    void connect() {
        baseNode->connectToPeer(2, "127.0.0.1", cellNode->listenPort());
    }

    template <typename Pred>
    int tickUntil(Pred pred, int max = 400) {
        for (int i = 0; i < max; ++i) {
            scheduler.runOnce();
            if (pred()) return i + 1;
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        return -1;
    }

    ~MiniCluster() {
        baseApp->detach(scheduler);
        cellApp->detach(scheduler);
        baseNode->detach(scheduler);
        cellNode->detach(scheduler);
    }
};

// Property layout: level=0, hp=1, gold=2, speed=3

// Test: Base-only property does NOT sync to cell
static void testBaseOnlyPropertyNotSyncedToCell() {
    TEST("Base-only property (gold) does not sync to cell");

    auto dir = createDefDir();
    TcpConnection::globalInit();
    MiniCluster c(dir);

    c.connect();
    c.tickUntil([&] { return c.baseNode->hasPeer(2); });

    auto* baseEntity = c.baseApp->createEntity("Avatar");
    auto entityId = baseEntity->id();
    baseEntity->setProperty<std::int32_t>(0, 5);    // level = 5
    baseEntity->setProperty<float>(1, 100.0f);       // hp = 100
    baseEntity->setProperty<std::int32_t>(2, 500);   // gold = 500 (Base-only)

    c.baseApp->requestCreateCell(entityId, "Avatar", Vector3{0, 0, 0}, 2);
    c.tickUntil([&] {
        return baseEntity->cellEntityCall() != nullptr && baseEntity->cellEntityCall()->isValid();
    });

    auto* cellEntity = c.cellApp->runtime().findEntity(entityId);
    bool ok = cellEntity != nullptr;
    if (!ok) { TcpConnection::globalShutdown(); std::filesystem::remove_all(dir); FAIL("cell entity missing"); return; }

    // level and hp should be synced (no Base/Cell flag)
    ok = ok && cellEntity->getProperty<std::int32_t>(0) == 5;
    ok = ok && std::abs(cellEntity->getProperty<float>(1) - 100.0f) < 0.01f;
    // gold should NOT be in snapshot (Base-only), so it should be 0
    ok = ok && cellEntity->getProperty<std::int32_t>(2) == 0;

    TcpConnection::globalShutdown();
    std::filesystem::remove_all(dir);

    if (ok) PASS();
    else FAIL("gold=" + std::to_string(cellEntity->getProperty<std::int32_t>(2)));
}

// Test: Cell-only property does NOT sync to base
static void testCellOnlyPropertyNotSyncedToBase() {
    TEST("Cell-only property (speed) does not sync to base");

    auto dir = createDefDir();
    TcpConnection::globalInit();
    MiniCluster c(dir);

    c.connect();
    c.tickUntil([&] { return c.baseNode->hasPeer(2); });

    auto* baseEntity = c.baseApp->createEntity("Avatar");
    auto entityId = baseEntity->id();
    baseEntity->setProperty<std::int32_t>(0, 1);
    baseEntity->setProperty<float>(1, 100.0f);

    c.baseApp->requestCreateCell(entityId, "Avatar", Vector3{0, 0, 0}, 2);
    c.tickUntil([&] {
        return baseEntity->cellEntityCall() != nullptr && baseEntity->cellEntityCall()->isValid();
    });

    auto* cellEntity = c.cellApp->runtime().findEntity(entityId);
    bool ok = cellEntity != nullptr;
    if (!ok) { TcpConnection::globalShutdown(); std::filesystem::remove_all(dir); FAIL("cell entity missing"); return; }

    // Set speed on cell (Cell-only), and also change hp (no flags)
    cellEntity->setProperty<float>(3, 5.5f);     // speed = 5.5 (Cell-only)
    cellEntity->setProperty<float>(1, 80.0f);     // hp = 80 (no flags, should sync)

    int ticks = c.tickUntil([&] {
        return std::abs(baseEntity->getProperty<float>(1) - 80.0f) < 0.01f;
    }, 200);

    ok = ok && ticks > 0;
    ok = ok && std::abs(baseEntity->getProperty<float>(1) - 80.0f) < 0.01f;
    // speed should NOT have synced to base (Cell-only)
    ok = ok && std::abs(baseEntity->getProperty<float>(3) - 0.0f) < 0.01f;

    TcpConnection::globalShutdown();
    std::filesystem::remove_all(dir);

    if (ok) PASS();
    else FAIL("hp=" + std::to_string(baseEntity->getProperty<float>(1))
              + " speed=" + std::to_string(baseEntity->getProperty<float>(3))
              + " ticks=" + std::to_string(ticks));
}

// Test: Base→Cell sync respects Base-only exclusion
static void testBaseToCellExcludesBaseOnly() {
    TEST("base→cell sync excludes Base-only properties on update");

    auto dir = createDefDir();
    TcpConnection::globalInit();
    MiniCluster c(dir);

    c.connect();
    c.tickUntil([&] { return c.baseNode->hasPeer(2); });

    auto* baseEntity = c.baseApp->createEntity("Avatar");
    auto entityId = baseEntity->id();
    c.baseApp->requestCreateCell(entityId, "Avatar", Vector3{0, 0, 0}, 2);
    c.tickUntil([&] {
        return baseEntity->cellEntityCall() != nullptr && baseEntity->cellEntityCall()->isValid();
    });

    auto* cellEntity = c.cellApp->runtime().findEntity(entityId);
    bool ok = cellEntity != nullptr;
    if (!ok) { TcpConnection::globalShutdown(); std::filesystem::remove_all(dir); FAIL("cell entity missing"); return; }

    // Change level (syncs) and gold (Base-only, should NOT sync)
    baseEntity->setProperty<std::int32_t>(0, 10);    // level (no flag)
    baseEntity->setProperty<std::int32_t>(2, 999);    // gold (Base-only)

    int ticks = c.tickUntil([&] {
        return cellEntity->getProperty<std::int32_t>(0) == 10;
    }, 200);

    ok = ok && ticks > 0;
    ok = ok && cellEntity->getProperty<std::int32_t>(0) == 10;
    // gold should not have changed on cell
    ok = ok && cellEntity->getProperty<std::int32_t>(2) == 0;

    TcpConnection::globalShutdown();
    std::filesystem::remove_all(dir);

    if (ok) PASS();
    else FAIL("level=" + std::to_string(cellEntity->getProperty<std::int32_t>(0))
              + " gold=" + std::to_string(cellEntity->getProperty<std::int32_t>(2)));
}

// Test: Unflagged properties sync bidirectionally
static void testUnflaggedPropertiesSyncBothWays() {
    TEST("unflagged properties (level, hp) sync both ways");

    auto dir = createDefDir();
    TcpConnection::globalInit();
    MiniCluster c(dir);

    c.connect();
    c.tickUntil([&] { return c.baseNode->hasPeer(2); });

    auto* baseEntity = c.baseApp->createEntity("Avatar");
    auto entityId = baseEntity->id();
    baseEntity->setProperty<std::int32_t>(0, 1);
    baseEntity->setProperty<float>(1, 100.0f);

    c.baseApp->requestCreateCell(entityId, "Avatar", Vector3{0, 0, 0}, 2);
    c.tickUntil([&] {
        return baseEntity->cellEntityCall() != nullptr && baseEntity->cellEntityCall()->isValid();
    });

    auto* cellEntity = c.cellApp->runtime().findEntity(entityId);
    bool ok = cellEntity != nullptr;
    if (!ok) { TcpConnection::globalShutdown(); std::filesystem::remove_all(dir); FAIL("cell entity missing"); return; }

    // base→cell: change level
    baseEntity->setProperty<std::int32_t>(0, 7);
    c.tickUntil([&] { return cellEntity->getProperty<std::int32_t>(0) == 7; }, 200);
    ok = ok && cellEntity->getProperty<std::int32_t>(0) == 7;

    // cell→base: change hp
    cellEntity->setProperty<float>(1, 50.0f);
    c.tickUntil([&] { return std::abs(baseEntity->getProperty<float>(1) - 50.0f) < 0.01f; }, 200);
    ok = ok && std::abs(baseEntity->getProperty<float>(1) - 50.0f) < 0.01f;

    TcpConnection::globalShutdown();
    std::filesystem::remove_all(dir);

    if (ok) PASS();
    else FAIL("cellLevel=" + std::to_string(cellEntity->getProperty<std::int32_t>(0))
              + " baseHp=" + std::to_string(baseEntity->getProperty<float>(1)));
}

int main() {
    std::cout << "Property sync direction tests:\n";

    testBaseOnlyPropertyNotSyncedToCell();
    testCellOnlyPropertyNotSyncedToBase();
    testBaseToCellExcludesBaseOnly();
    testUnflaggedPropertiesSyncBothWays();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
