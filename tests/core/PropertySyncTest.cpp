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

static std::string createDefDir() {
    std::string dir = "test_prop_sync_defs";
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

// Test: cell property change syncs to base entity via property.syncToBase
static void testPropertySyncCellToBase() {
    TEST("property sync: cell property change -> base entity updated");

    auto dir = createDefDir();
    TcpConnection::globalInit();
    MiniCluster c(dir);

    c.connect();
    c.tickUntil([&] { return c.baseNode->hasPeer(2); });

    auto* baseEntity = c.baseApp->createEntity("Avatar");
    auto entityId = baseEntity->id();
    baseEntity->setProperty<std::int32_t>(0, 1);    // level = 1
    baseEntity->setProperty<float>(1, 100.0f);       // hp = 100

    c.baseApp->requestCreateCell(entityId, "Avatar", Vector3{0, 0, 0}, 2);
    c.tickUntil([&] {
        return baseEntity->cellEntityCall() != nullptr && baseEntity->cellEntityCall()->isValid();
    });

    auto* cellEntity = c.cellApp->runtime().findEntity(entityId);
    bool ok = cellEntity != nullptr;
    if (!ok) { TcpConnection::globalShutdown(); std::filesystem::remove_all(dir); FAIL("cell entity missing"); return; }

    // Cell entity should already have initial values from createCell snapshot
    ok = ok && cellEntity->getProperty<std::int32_t>(0) == 1;
    ok = ok && std::abs(cellEntity->getProperty<float>(1) - 100.0f) < 0.01f;
    if (!ok) { TcpConnection::globalShutdown(); std::filesystem::remove_all(dir); FAIL("cell initial snapshot wrong"); return; }

    // Change cell entity properties
    cellEntity->setProperty<float>(1, 75.0f);  // hp = 75 (property index 1)

    // Tick: SyncBuild phase triggers syncToBases -> property.syncToBase sent
    // Next tick: Network phase pumps it -> BaseRuntime processes
    int ticks = c.tickUntil([&] {
        return baseEntity->getProperty<float>(1) < 80.0f;
    }, 200);

    ok = ok && ticks > 0;
    ok = ok && baseEntity->getProperty<float>(1) == 75.0f;

    // level should be unchanged
    ok = ok && baseEntity->getProperty<std::int32_t>(0) == 1;

    TcpConnection::globalShutdown();
    std::filesystem::remove_all(dir);

    if (ok) PASS();
    else FAIL("hp=" + std::to_string(baseEntity->getProperty<float>(1)) + " ticks=" + std::to_string(ticks));
}

// Test: multiple property changes sync in one delta
static void testPropertySyncMultipleProperties() {
    TEST("property sync: multiple dirty properties in one delta");

    auto dir = createDefDir();
    TcpConnection::globalInit();
    MiniCluster c(dir);

    c.connect();
    c.tickUntil([&] { return c.baseNode->hasPeer(2); });

    auto* baseEntity = c.baseApp->createEntity("Avatar");
    auto entityId = baseEntity->id();
    baseEntity->setProperty<std::int32_t>(0, 5);    // level = 5
    baseEntity->setProperty<float>(1, 200.0f);       // hp = 200

    c.baseApp->requestCreateCell(entityId, "Avatar", Vector3{0, 0, 0}, 2);
    c.tickUntil([&] {
        return baseEntity->cellEntityCall() != nullptr && baseEntity->cellEntityCall()->isValid();
    });

    auto* cellEntity = c.cellApp->runtime().findEntity(entityId);
    bool ok = cellEntity != nullptr;
    if (!ok) { TcpConnection::globalShutdown(); std::filesystem::remove_all(dir); FAIL("cell entity missing"); return; }

    // Change both properties
    cellEntity->setProperty<std::int32_t>(0, 10);    // level = 10
    cellEntity->setProperty<float>(1, 150.0f);        // hp = 150

    int ticks = c.tickUntil([&] {
        return baseEntity->getProperty<std::int32_t>(0) == 10 && std::abs(baseEntity->getProperty<float>(1) - 150.0f) < 0.01f;
    }, 200);

    ok = ok && ticks > 0;

    TcpConnection::globalShutdown();
    std::filesystem::remove_all(dir);

    if (ok) PASS();
    else FAIL("level=" + std::to_string(baseEntity->getProperty<std::int32_t>(0))
              + " hp=" + std::to_string(baseEntity->getProperty<float>(1))
              + " ticks=" + std::to_string(ticks));
}

// Test: sequential property updates (change, sync, change again, sync)
static void testPropertySyncSequential() {
    TEST("property sync: sequential updates propagate correctly");

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

    // Round 1: hp = 80
    cellEntity->setProperty<float>(1, 80.0f);
    c.tickUntil([&] { return std::abs(baseEntity->getProperty<float>(1) - 80.0f) < 0.01f; }, 200);
    ok = ok && std::abs(baseEntity->getProperty<float>(1) - 80.0f) < 0.01f;

    // Round 2: hp = 30
    cellEntity->setProperty<float>(1, 30.0f);
    c.tickUntil([&] { return std::abs(baseEntity->getProperty<float>(1) - 30.0f) < 0.01f; }, 200);
    ok = ok && std::abs(baseEntity->getProperty<float>(1) - 30.0f) < 0.01f;

    TcpConnection::globalShutdown();
    std::filesystem::remove_all(dir);

    if (ok) PASS();
    else FAIL("hp=" + std::to_string(baseEntity->getProperty<float>(1)));
}

// Test: base property change syncs to cell entity via property.syncToCell
static void testPropertySyncBaseToCell() {
    TEST("property sync: base property change -> cell entity updated");

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

    // Change base entity property
    baseEntity->setProperty<std::int32_t>(0, 5);

    // Flush phase sends syncToCell -> next tick Network phase delivers
    int ticks = c.tickUntil([&] {
        return cellEntity->getProperty<std::int32_t>(0) == 5;
    }, 200);

    ok = ok && ticks > 0;
    ok = ok && cellEntity->getProperty<std::int32_t>(0) == 5;
    // hp was synced to cell via initial syncToCell (base set it to 100 before createCell)
    ok = ok && std::abs(cellEntity->getProperty<float>(1) - 100.0f) < 0.01f;

    TcpConnection::globalShutdown();
    std::filesystem::remove_all(dir);

    if (ok) PASS();
    else FAIL("level=" + std::to_string(cellEntity->getProperty<std::int32_t>(0))
              + " hp=" + std::to_string(cellEntity->getProperty<float>(1))
              + " ticks=" + std::to_string(ticks));
}

// Test: multiple base property changes sync in one delta to cell
static void testPropertySyncBaseToCellMultiple() {
    TEST("property sync: base -> cell multiple dirty properties");

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

    // Change both properties on base
    baseEntity->setProperty<std::int32_t>(0, 10);
    baseEntity->setProperty<float>(1, 250.0f);

    int ticks = c.tickUntil([&] {
        return cellEntity->getProperty<std::int32_t>(0) == 10
            && std::abs(cellEntity->getProperty<float>(1) - 250.0f) < 0.01f;
    }, 200);

    ok = ok && ticks > 0;

    TcpConnection::globalShutdown();
    std::filesystem::remove_all(dir);

    if (ok) PASS();
    else FAIL("level=" + std::to_string(cellEntity->getProperty<std::int32_t>(0))
              + " hp=" + std::to_string(cellEntity->getProperty<float>(1))
              + " ticks=" + std::to_string(ticks));
}

// Test: bidirectional sync - base→cell and cell→base in the same test
static void testPropertySyncBidirectional() {
    TEST("property sync: bidirectional base<->cell");

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

    // Round 1: base→cell (level)
    baseEntity->setProperty<std::int32_t>(0, 5);
    c.tickUntil([&] { return cellEntity->getProperty<std::int32_t>(0) == 5; }, 200);
    ok = ok && cellEntity->getProperty<std::int32_t>(0) == 5;

    // Round 2: cell→base (hp)
    cellEntity->setProperty<float>(1, 60.0f);
    c.tickUntil([&] { return std::abs(baseEntity->getProperty<float>(1) - 60.0f) < 0.01f; }, 200);
    ok = ok && std::abs(baseEntity->getProperty<float>(1) - 60.0f) < 0.01f;

    // Round 3: base→cell again
    baseEntity->setProperty<std::int32_t>(0, 8);
    c.tickUntil([&] { return cellEntity->getProperty<std::int32_t>(0) == 8; }, 200);
    ok = ok && cellEntity->getProperty<std::int32_t>(0) == 8;

    TcpConnection::globalShutdown();
    std::filesystem::remove_all(dir);

    if (ok) PASS();
    else FAIL("cellLevel=" + std::to_string(cellEntity->getProperty<std::int32_t>(0))
              + " baseHp=" + std::to_string(baseEntity->getProperty<float>(1)));
}

// Test: createCell carries initial property snapshot - cell entity has correct values immediately
static void testCreateCellWithPropertySnapshot() {
    TEST("createCell: cell entity receives initial property snapshot");

    auto dir = createDefDir();
    TcpConnection::globalInit();
    MiniCluster c(dir);

    c.connect();
    c.tickUntil([&] { return c.baseNode->hasPeer(2); });

    auto* baseEntity = c.baseApp->createEntity("Avatar");
    auto entityId = baseEntity->id();
    baseEntity->setProperty<std::int32_t>(0, 3);    // level = 3
    baseEntity->setProperty<float>(1, 80.0f);        // hp = 80

    c.baseApp->requestCreateCell(entityId, "Avatar", Vector3{10, 20, 30}, 2);
    c.tickUntil([&] {
        return baseEntity->cellEntityCall() != nullptr && baseEntity->cellEntityCall()->isValid();
    });

    auto* cellEntity = c.cellApp->runtime().findEntity(entityId);
    bool ok = cellEntity != nullptr;
    if (!ok) { TcpConnection::globalShutdown(); std::filesystem::remove_all(dir); FAIL("cell entity missing"); return; }

    // Cell entity should immediately have the correct initial values from the snapshot
    // (no additional ticks needed for syncToCells)
    ok = ok && cellEntity->getProperty<std::int32_t>(0) == 3;
    ok = ok && std::abs(cellEntity->getProperty<float>(1) - 80.0f) < 0.01f;

    TcpConnection::globalShutdown();
    std::filesystem::remove_all(dir);

    if (ok) PASS();
    else FAIL("level=" + std::to_string(cellEntity->getProperty<std::int32_t>(0))
              + " hp=" + std::to_string(cellEntity->getProperty<float>(1)));
}

int main() {
    std::cout << "Property sync tests:\n";

    testPropertySyncCellToBase();
    testPropertySyncMultipleProperties();
    testPropertySyncSequential();
    testPropertySyncBaseToCell();
    testPropertySyncBaseToCellMultiple();
    testPropertySyncBidirectional();
    testCreateCellWithPropertySnapshot();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
