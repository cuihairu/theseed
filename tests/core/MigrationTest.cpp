#include "theseed/core/BaseApp.h"
#include "theseed/core/CellApp.h"
#include "theseed/core/IEntityStore.h"
#include "theseed/runtime/Entity.h"
#include "theseed/runtime/EntityDef.h"
#include "theseed/runtime/NetworkNode.h"
#include "theseed/runtime/TcpConnection.h"
#include "theseed/runtime/TickScheduler.h"

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
using theseed::runtime::SpaceId;
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
    std::string dir = "test_migration_defs";
    std::filesystem::create_directory(dir);
    writeFile(dir + "/Avatar.xml", R"(
<EntityDef name="Avatar">
    <Properties>
        <Property name="level" type="Int32"/>
        <Property name="hp" type="Float32"/>
    </Properties>
    <Methods>
        <Method name="test.ping" side="Cell"/>
    </Methods>
</EntityDef>
)");
    return dir;
}

// 3-node cluster: BaseApp(1) + CellApp1(2) + CellApp2(3)
struct TriCluster {
    std::shared_ptr<NetworkNode> baseNode;
    std::shared_ptr<NetworkNode> cell1Node;
    std::shared_ptr<NetworkNode> cell2Node;
    std::unique_ptr<BaseApp> baseApp;
    std::unique_ptr<CellApp> cellApp1;
    std::unique_ptr<CellApp> cellApp2;
    TickScheduler scheduler;
    std::string defDir;
    std::shared_ptr<InMemoryEntityStore> store;

    TriCluster(const std::string& dir)
        : scheduler(std::chrono::milliseconds{8}), defDir(dir) {
        baseNode = std::make_shared<NetworkNode>(NetworkNode::Config{.localComponent = 1, .listenPort = 0});
        cell1Node = std::make_shared<NetworkNode>(NetworkNode::Config{.localComponent = 2, .listenPort = 0});
        cell2Node = std::make_shared<NetworkNode>(NetworkNode::Config{.localComponent = 3, .listenPort = 0});

        // cell1Node accepts incoming: first is base(1), second is cell2(3)
        cell1Node->setOnPeerConnected([this](std::shared_ptr<theseed::runtime::IRuntimeTransport> transport) {
            if (!cell1Node->hasPeer(1)) {
                cell1Node->acceptPeer(1, transport);
            } else {
                cell1Node->acceptPeer(3, transport);
            }
        });
        // cell2Node accepts incoming: first is base(1), second is cell1(2)
        cell2Node->setOnPeerConnected([this](std::shared_ptr<theseed::runtime::IRuntimeTransport> transport) {
            if (!cell2Node->hasPeer(1)) {
                cell2Node->acceptPeer(1, transport);
            } else {
                cell2Node->acceptPeer(2, transport);
            }
        });

        store = std::make_shared<InMemoryEntityStore>();
        baseApp = std::make_unique<BaseApp>(BaseApp::Config{.entityDefPath = dir, .componentId = 1}, baseNode->hub(), store);
        baseApp->init();
        cellApp1 = std::make_unique<CellApp>(CellApp::Config{.entityDefPath = dir, .componentId = 2}, cell1Node->hub());
        cellApp1->init();
        cellApp2 = std::make_unique<CellApp>(CellApp::Config{.entityDefPath = dir, .componentId = 3}, cell2Node->hub());
        cellApp2->init();

        baseNode->attach(scheduler);
        cell1Node->attach(scheduler);
        cell2Node->attach(scheduler);
        baseApp->attach(scheduler);
        cellApp1->attach(scheduler);
        cellApp2->attach(scheduler);
    }

    void connectAll() {
        baseNode->connectToPeer(2, "127.0.0.1", cell1Node->listenPort());
        baseNode->connectToPeer(3, "127.0.0.1", cell2Node->listenPort());
    }

    void connectCellPeers() {
        // Cell1 connects to Cell2, Cell2 accepts as peer 2
        cell1Node->connectToPeer(3, "127.0.0.1", cell2Node->listenPort());
    }

    template <typename Pred>
    int tickUntil(Pred pred, int max = 600) {
        for (int i = 0; i < max; ++i) {
            scheduler.runOnce();
            if (pred()) return i + 1;
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        return -1;
    }

    ~TriCluster() {
        baseApp->detach(scheduler);
        cellApp1->detach(scheduler);
        cellApp2->detach(scheduler);
        baseNode->detach(scheduler);
        cell1Node->detach(scheduler);
        cell2Node->detach(scheduler);
    }
};

// Test 1: Entity created on CellApp1, migrated to CellApp2, base cellEntityCall auto-updates
static void testMigrationAutoRouting() {
    TEST("Migration: base cellEntityCall auto-updates after migration");

    auto dir = createDefDir();
    theseed::runtime::TcpConnection::globalInit();
    TriCluster c(dir);
    c.connectAll();
    c.connectCellPeers();
    c.tickUntil([&] { return c.baseNode->hasPeer(2) && c.baseNode->hasPeer(3)
                            && c.cell1Node->hasPeer(3) && c.cell2Node->hasPeer(2); });

    // Create entity on base
    auto* baseEntity = c.baseApp->createEntity("Avatar");
    auto entityId = baseEntity->id();

    // Create cell entity on CellApp1 (component 2)
    c.baseApp->requestCreateCell(entityId, "Avatar", Vector3{10, 0, 10}, 2);
    int ticks = c.tickUntil([&] {
        return baseEntity->cellEntityCall() && baseEntity->cellEntityCall()->isValid();
    });
    bool ok = ticks > 0;
    ok = ok && baseEntity->cellEntityCall()->targetComponent() == 2;

    // Verify entity exists on CellApp1
    auto* cellEntity1 = c.cellApp1->runtime().findEntity(entityId);
    ok = ok && cellEntity1 != nullptr;

    // Trigger migration from CellApp1 (2) to CellApp2 (3)
    ok = ok && c.cellApp1->runtime().beginMigration(entityId, 3, 1);

    // Wait for migration to complete and base cellEntityCall to update
    ticks = c.tickUntil([&] {
        auto* cc = baseEntity->cellEntityCall();
        return cc && cc->isValid() && cc->targetComponent() == 3;
    });
    ok = ok && ticks > 0;

    // Verify entity exists on CellApp2, not on CellApp1
    auto* cellEntity2 = c.cellApp2->runtime().findEntity(entityId);
    ok = ok && cellEntity2 != nullptr;
    ok = ok && c.cellApp1->runtime().findEntity(entityId) == nullptr;

    // Verify position preserved
    if (ok && cellEntity2->hasPosition()) {
        auto pos = cellEntity2->position();
        ok = ok && std::abs(pos.x - 10.0f) < 0.01f;
    }

    std::filesystem::remove_all(dir);
    if (ok) PASS();
    else FAIL("ticks=" + std::to_string(ticks));
}

// Test 2: After migration, base→cell call reaches CellApp2
static void testPostMigrationCall() {
    TEST("Migration: base→cell call reaches new CellApp after migration");

    auto dir = createDefDir();
    theseed::runtime::TcpConnection::globalInit();
    TriCluster c(dir);
    c.connectAll();
    c.connectCellPeers();
    c.tickUntil([&] { return c.baseNode->hasPeer(2) && c.baseNode->hasPeer(3)
                            && c.cell1Node->hasPeer(3) && c.cell2Node->hasPeer(2); });

    auto* baseEntity = c.baseApp->createEntity("Avatar");
    auto entityId = baseEntity->id();

    c.baseApp->requestCreateCell(entityId, "Avatar", Vector3{0, 0, 0}, 2);
    c.tickUntil([&] {
        return baseEntity->cellEntityCall() && baseEntity->cellEntityCall()->isValid();
    });

    // Register a handler on CellApp1 to track initial call
    auto* cellEntity1 = c.cellApp1->runtime().findEntity(entityId);
    bool gotCall1 = false;
    cellEntity1->bindMethodHandler("test.ping", [&](Entity&, std::span<const std::byte>) {
        gotCall1 = true;
    });

    // Migrate to CellApp2
    c.cellApp1->runtime().beginMigration(entityId, 3, 1);
    c.tickUntil([&] {
        auto* cc = baseEntity->cellEntityCall();
        return cc && cc->isValid() && cc->targetComponent() == 3;
    });

    // Register handler on CellApp2
    auto* cellEntity2 = c.cellApp2->runtime().findEntity(entityId);
    bool ok = cellEntity2 != nullptr;
    if (!ok) {
        std::filesystem::remove_all(dir);
        FAIL("entity not found on CellApp2");
        return;
    }

    bool gotCall2 = false;
    cellEntity2->bindMethodHandler("test.ping", [&](Entity&, std::span<const std::byte>) {
        gotCall2 = true;
    });

    // Call from base to cell
    auto sendResult = baseEntity->callCell("test.ping");

    c.tickUntil([&] { return gotCall2; }, 300);

    ok = ok && sendResult == theseed::runtime::SendResult::Accepted;
    ok = ok && gotCall2;
    ok = ok && !gotCall1;

    std::filesystem::remove_all(dir);
    if (ok) PASS();
    else FAIL("call did not reach CellApp2");
}

// Test 3: After migration, property sync still works
static void testPostMigrationPropertySync() {
    TEST("Migration: property sync works after migration");

    auto dir = createDefDir();
    theseed::runtime::TcpConnection::globalInit();
    TriCluster c(dir);
    c.connectAll();
    c.connectCellPeers();
    c.tickUntil([&] { return c.baseNode->hasPeer(2) && c.baseNode->hasPeer(3)
                            && c.cell1Node->hasPeer(3) && c.cell2Node->hasPeer(2); });

    auto* baseEntity = c.baseApp->createEntity("Avatar");
    auto entityId = baseEntity->id();

    c.baseApp->requestCreateCell(entityId, "Avatar", Vector3{0, 0, 0}, 2);
    c.tickUntil([&] {
        return baseEntity->cellEntityCall() && baseEntity->cellEntityCall()->isValid();
    });

    // Migrate to CellApp2
    c.cellApp1->runtime().beginMigration(entityId, 3, 1);
    c.tickUntil([&] {
        auto* cc = baseEntity->cellEntityCall();
        return cc && cc->isValid() && cc->targetComponent() == 3;
    });

    auto* cellEntity2 = c.cellApp2->runtime().findEntity(entityId);
    bool ok = cellEntity2 != nullptr;

    // Modify property on cell side, sync should reach base
    if (ok) {
        auto* desc = cellEntity2->propertyBlock().def().findProperty("hp");
        if (desc) {
            cellEntity2->setProperty<float>(desc->id, 99.5f);
        }
    }

    // Tick to trigger sync
    c.tickUntil([&] {
        auto* desc = baseEntity->propertyBlock().def().findProperty("hp");
        if (!desc) return false;
        auto hp = baseEntity->getProperty<float>(desc->id);
        return std::abs(hp - 99.5f) < 0.01f;
    }, 300);

    auto* desc = baseEntity->propertyBlock().def().findProperty("hp");
    if (desc) {
        auto hp = baseEntity->getProperty<float>(desc->id);
        ok = ok && std::abs(hp - 99.5f) < 0.01f;
    }

    std::filesystem::remove_all(dir);
    if (ok) PASS();
    else FAIL("property sync failed");
}

// Test 4: Entity properties preserved during migration
static void testMigrationPropertyPreservation() {
    TEST("Migration: entity properties preserved");

    auto dir = createDefDir();
    theseed::runtime::TcpConnection::globalInit();
    TriCluster c(dir);
    c.connectAll();
    c.connectCellPeers();
    c.tickUntil([&] { return c.baseNode->hasPeer(2) && c.baseNode->hasPeer(3)
                            && c.cell1Node->hasPeer(3) && c.cell2Node->hasPeer(2); });

    auto* baseEntity = c.baseApp->createEntity("Avatar");
    auto entityId = baseEntity->id();

    c.baseApp->requestCreateCell(entityId, "Avatar", Vector3{5, 0, 15}, 2);
    c.tickUntil([&] {
        return baseEntity->cellEntityCall() && baseEntity->cellEntityCall()->isValid();
    });

    // Set properties on CellApp1
    auto* cellEntity1 = c.cellApp1->runtime().findEntity(entityId);
    bool ok = cellEntity1 != nullptr;
    if (ok) {
        auto* levelDesc = cellEntity1->propertyBlock().def().findProperty("level");
        auto* hpDesc = cellEntity1->propertyBlock().def().findProperty("hp");
        if (levelDesc) cellEntity1->setProperty<std::int32_t>(levelDesc->id, 42);
        if (hpDesc) cellEntity1->setProperty<float>(hpDesc->id, 75.5f);
        cellEntity1->clearDirtyFlags();
    }

    // Migrate
    c.cellApp1->runtime().beginMigration(entityId, 3, 2);
    c.tickUntil([&] {
        auto* cc = baseEntity->cellEntityCall();
        return cc && cc->isValid() && cc->targetComponent() == 3;
    });

    // Check properties on CellApp2
    auto* cellEntity2 = c.cellApp2->runtime().findEntity(entityId);
    ok = ok && cellEntity2 != nullptr;
    if (ok) {
        auto* levelDesc = cellEntity2->propertyBlock().def().findProperty("level");
        auto* hpDesc = cellEntity2->propertyBlock().def().findProperty("hp");
        if (levelDesc) {
            auto level = cellEntity2->getProperty<std::int32_t>(levelDesc->id);
            ok = ok && level == 42;
        }
        if (hpDesc) {
            auto hp = cellEntity2->getProperty<float>(hpDesc->id);
            ok = ok && std::abs(hp - 75.5f) < 0.01f;
        }
    }

    std::filesystem::remove_all(dir);
    if (ok) PASS();
    else FAIL("properties not preserved");
}

// Test 5: Migration to same component fails
static void testMigrationSameComponentFails() {
    TEST("Migration: migrating to same CellApp returns false");

    auto dir = createDefDir();
    theseed::runtime::TcpConnection::globalInit();
    TriCluster c(dir);
    c.connectAll();
    c.tickUntil([&] { return c.baseNode->hasPeer(2) && c.baseNode->hasPeer(3); });

    auto* baseEntity = c.baseApp->createEntity("Avatar");
    auto entityId = baseEntity->id();
    c.baseApp->requestCreateCell(entityId, "Avatar", Vector3{0, 0, 0}, 2);
    c.tickUntil([&] {
        return baseEntity->cellEntityCall() && baseEntity->cellEntityCall()->isValid();
    });

    bool result = c.cellApp1->runtime().beginMigration(entityId, 2, 1);
    bool ok = !result;

    std::filesystem::remove_all(dir);
    if (ok) PASS();
    else FAIL("should return false for same component");
}

// Test 6: Migration of non-existent entity fails
static void testMigrationNonExistentFails() {
    TEST("Migration: migrating non-existent entity returns false");

    auto dir = createDefDir();
    theseed::runtime::TcpConnection::globalInit();
    TriCluster c(dir);
    c.connectAll();
    c.tickUntil([&] { return c.baseNode->hasPeer(2); });

    bool result = c.cellApp1->runtime().beginMigration(99999, 3, 1);
    bool ok = !result;

    std::filesystem::remove_all(dir);
    if (ok) PASS();
    else FAIL("should return false for non-existent entity");
}

int main() {
    std::cout << "Migration end-to-end tests:\n";

    testMigrationAutoRouting();
    testPostMigrationCall();
    testPostMigrationPropertySync();
    testMigrationPropertyPreservation();
    testMigrationSameComponentFails();
    testMigrationNonExistentFails();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
