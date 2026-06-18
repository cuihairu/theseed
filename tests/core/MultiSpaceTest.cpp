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
    std::string dir = "test_multi_space_defs";
    std::filesystem::create_directory(dir);
    writeFile(dir + "/Avatar.xml", R"(
<EntityDef name="Avatar">
    <Properties>
        <Property name="level" type="Int32"/>
    </Properties>
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

// Test: Default space (spaceId=0) works — backward compatible
static void testDefaultSpace() {
    TEST("Multi-space: default space backward compatible");

    auto dir = createDefDir();
    theseed::runtime::TcpConnection::globalInit();
    MiniCluster c(dir);
    c.connect();
    c.tickUntil([&] { return c.baseNode->hasPeer(2); });

    auto* baseEntity = c.baseApp->createEntity("Avatar");
    auto entityId = baseEntity->id();

    c.baseApp->requestCreateCell(entityId, "Avatar", Vector3{0, 0, 0}, 2);
    int ticks = c.tickUntil([&] {
        return baseEntity->cellEntityCall() && baseEntity->cellEntityCall()->isValid();
    });

    bool ok = ticks > 0;
    auto* cellEntity = c.cellApp->runtime().findEntity(entityId);
    ok = ok && cellEntity != nullptr;

    std::filesystem::remove_all(dir);
    if (ok) PASS();
    else FAIL("ticks=" + std::to_string(ticks));
}

// Test: Create entity in a specific space
static void testCreateInNamedSpace() {
    TEST("Multi-space: entity created in specific space");

    auto dir = createDefDir();
    theseed::runtime::TcpConnection::globalInit();
    MiniCluster c(dir);
    c.connect();
    c.tickUntil([&] { return c.baseNode->hasPeer(2); });

    // Create space 100 on cell side
    bool spaceCreated = c.cellApp->runtime().createSpace(100, "dungeon_1");
    bool ok = spaceCreated;
    ok = ok && c.cellApp->runtime().findSpaceRuntime(100) != nullptr;

    auto* baseEntity = c.baseApp->createEntity("Avatar");
    auto entityId = baseEntity->id();

    // Create cell entity in space 100
    c.baseApp->requestCreateCell(entityId, "Avatar", Vector3{10, 0, 10}, 2, 100);
    int ticks = c.tickUntil([&] {
        return baseEntity->cellEntityCall() && baseEntity->cellEntityCall()->isValid();
    });

    ok = ok && ticks > 0;
    auto* cellEntity = c.cellApp->runtime().findEntity(entityId);
    ok = ok && cellEntity != nullptr;
    ok = ok && c.cellApp->runtime().findEntitySpace(entityId) == 100;

    std::filesystem::remove_all(dir);
    if (ok) PASS();
    else FAIL("ticks=" + std::to_string(ticks));
}

// Test: AOI isolation between spaces
static void testAoIIsolation() {
    TEST("Multi-space: AOI isolated between spaces");

    auto dir = createDefDir();
    theseed::runtime::TcpConnection::globalInit();
    MiniCluster c(dir);
    c.connect();
    c.tickUntil([&] { return c.baseNode->hasPeer(2); });

    // Create space 200 for second entity
    c.cellApp->runtime().createSpace(200, "dungeon_2");

    // Entity A in default space
    auto* baseA = c.baseApp->createEntity("Avatar");
    auto idA = baseA->id();
    c.baseApp->requestCreateCell(idA, "Avatar", Vector3{0, 0, 0}, 2);
    c.tickUntil([&] { return baseA->cellEntityCall() && baseA->cellEntityCall()->isValid(); });

    // Entity B in space 200, same position
    auto* baseB = c.baseApp->createEntity("Avatar");
    auto idB = baseB->id();
    c.baseApp->requestCreateCell(idB, "Avatar", Vector3{0, 0, 0}, 2, 200);
    c.tickUntil([&] { return baseB->cellEntityCall() && baseB->cellEntityCall()->isValid(); });

    // Both should exist in cell
    auto* cellA = c.cellApp->runtime().findEntity(idA);
    auto* cellB = c.cellApp->runtime().findEntity(idB);
    bool ok = cellA != nullptr && cellB != nullptr;
    ok = ok && c.cellApp->runtime().findEntitySpace(idA) != c.cellApp->runtime().findEntitySpace(idB);

    std::filesystem::remove_all(dir);
    if (ok) PASS();
    else FAIL("entities in same space or missing");
}

// Test: Destroy space cleans up entities
static void testDestroySpace() {
    TEST("Multi-space: destroy space cleans up entities");

    auto dir = createDefDir();
    theseed::runtime::TcpConnection::globalInit();
    MiniCluster c(dir);
    c.connect();
    c.tickUntil([&] { return c.baseNode->hasPeer(2); });

    // Create space 300
    c.cellApp->runtime().createSpace(300, "temp_dungeon");
    auto* space300 = c.cellApp->runtime().findSpaceRuntime(300);

    // Create entity directly in space 300 via CellApp
    auto* entity = c.cellApp->createEntity("Avatar", Vector3{0, 0, 0});
    auto entityId = entity->id();
    c.cellApp->runtime().addEntity(*entity, Vector3{5, 0, 5}, 300);

    bool ok = space300 != nullptr;
    ok = ok && c.cellApp->runtime().findEntitySpace(entityId) == 300;

    // Destroy the space
    ok = ok && c.cellApp->runtime().destroySpace(300);
    ok = ok && c.cellApp->runtime().findSpaceRuntime(300) == nullptr;

    // Entity should be gone
    ok = ok && c.cellApp->runtime().findEntity(entityId) == nullptr;

    std::filesystem::remove_all(dir);
    if (ok) PASS();
    else FAIL("space or entity still exists");
}

// Test: Cannot destroy default space
static void testCannotDestroyDefaultSpace() {
    TEST("Multi-space: cannot destroy default space");

    auto dir = createDefDir();
    theseed::runtime::TcpConnection::globalInit();
    MiniCluster c(dir);
    c.connect();
    c.tickUntil([&] { return c.baseNode->hasPeer(2); });

    bool result = c.cellApp->runtime().destroySpace(1);  // default space ID is 1
    bool ok = !result;  // should fail

    std::filesystem::remove_all(dir);
    if (ok) PASS();
    else FAIL("should not allow destroying default space");
}

// Test: Auto-create space from createCell payload
static void testAutoCreateSpace() {
    TEST("Multi-space: space auto-created from createCell");

    auto dir = createDefDir();
    theseed::runtime::TcpConnection::globalInit();
    MiniCluster c(dir);
    c.connect();
    c.tickUntil([&] { return c.baseNode->hasPeer(2); });

    // Space 500 doesn't exist yet
    bool ok = c.cellApp->runtime().findSpaceRuntime(500) == nullptr;

    auto* baseEntity = c.baseApp->createEntity("Avatar");
    auto entityId = baseEntity->id();

    // Create cell with spaceId=500 — should auto-create
    c.baseApp->requestCreateCell(entityId, "Avatar", Vector3{0, 0, 0}, 2, 500);
    int ticks = c.tickUntil([&] {
        return baseEntity->cellEntityCall() && baseEntity->cellEntityCall()->isValid();
    });

    ok = ok && ticks > 0;
    ok = ok && c.cellApp->runtime().findSpaceRuntime(500) != nullptr;
    ok = ok && c.cellApp->runtime().findEntitySpace(entityId) == 500;

    std::filesystem::remove_all(dir);
    if (ok) PASS();
    else FAIL("ticks=" + std::to_string(ticks));
}

// Test: Teleport entity between spaces
static void testTeleportBetweenSpaces() {
    TEST("Multi-space: teleport entity between spaces");

    auto dir = createDefDir();
    theseed::runtime::TcpConnection::globalInit();
    MiniCluster c(dir);
    c.connect();
    c.tickUntil([&] { return c.baseNode->hasPeer(2); });

    // Create two spaces
    c.cellApp->runtime().createSpace(100, "overworld");
    c.cellApp->runtime().createSpace(200, "dungeon");

    // Create entity in space 100
    auto* entity = c.cellApp->createEntity("Avatar", Vector3{10, 0, 10});
    auto entityId = entity->id();
    c.cellApp->runtime().addEntity(*entity, Vector3{10, 0, 10}, 100);

    bool ok = c.cellApp->runtime().findEntitySpace(entityId) == 100;

    // Teleport to space 200
    ok = ok && c.cellApp->runtime().teleportEntity(entityId, 200, Vector3{50, 0, 50});
    ok = ok && c.cellApp->runtime().findEntitySpace(entityId) == 200;

    // Verify position updated
    auto* sr200 = c.cellApp->runtime().findSpaceRuntime(200);
    ok = ok && sr200 != nullptr;
    if (sr200) {
        auto pos = sr200->space().entityPosition(entityId);
        ok = ok && pos.has_value();
        if (pos) {
            ok = ok && std::abs(pos->x - 50.0f) < 0.01f;
            ok = ok && std::abs(pos->z - 50.0f) < 0.01f;
        }
    }

    // Verify entity no longer in space 100
    auto* sr100 = c.cellApp->runtime().findSpaceRuntime(100);
    if (sr100) {
        ok = ok && sr100->space().findEntity(entityId) == nullptr;
    }

    std::filesystem::remove_all(dir);
    if (ok) PASS();
    else FAIL("teleport failed");
}

// Test: Teleport to same space returns false
static void testTeleportSameSpace() {
    TEST("Multi-space: teleport to same space returns false");

    auto dir = createDefDir();
    theseed::runtime::TcpConnection::globalInit();
    MiniCluster c(dir);
    c.connect();
    c.tickUntil([&] { return c.baseNode->hasPeer(2); });

    auto* entity = c.cellApp->createEntity("Avatar", Vector3{0, 0, 0});
    auto entityId = entity->id();

    bool result = c.cellApp->runtime().teleportEntity(entityId, 1, Vector3{0, 0, 0});
    bool ok = !result;

    std::filesystem::remove_all(dir);
    if (ok) PASS();
    else FAIL("should return false for same space");
}

// Test: Teleport non-existent entity returns false
static void testTeleportNonExistent() {
    TEST("Multi-space: teleport non-existent entity returns false");

    auto dir = createDefDir();
    theseed::runtime::TcpConnection::globalInit();
    MiniCluster c(dir);
    c.connect();
    c.tickUntil([&] { return c.baseNode->hasPeer(2); });

    c.cellApp->runtime().createSpace(100, "target");

    bool result = c.cellApp->runtime().teleportEntity(99999, 100, Vector3{0, 0, 0});
    bool ok = !result;

    std::filesystem::remove_all(dir);
    if (ok) PASS();
    else FAIL("should return false");
}

int main() {
    std::cout << "Multi-space tests:\n";

    testDefaultSpace();
    testCreateInNamedSpace();
    testAoIIsolation();
    testDestroySpace();
    testCannotDestroyDefaultSpace();
    testAutoCreateSpace();
    testTeleportBetweenSpaces();
    testTeleportSameSpace();
    testTeleportNonExistent();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
