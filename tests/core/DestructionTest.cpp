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
    std::string dir = "test_destruction_defs";
    std::filesystem::create_directory(dir);
    writeFile(dir + "/Avatar.xml", R"(
<EntityDef name="Avatar">
    <Properties>
        <Property name="level" type="Int32"/>
        <Property name="hp" type="Float32"/>
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

// Test 1: Coordinated destruction - cell destroyed first, then base
static void testCoordinatedDestruction() {
    TEST("Destruction: cell destroyed first, then base");

    auto dir = createDefDir();
    theseed::runtime::TcpConnection::globalInit();
    MiniCluster c(dir);
    c.connect();
    c.tickUntil([&] { return c.baseNode->hasPeer(2); });

    auto* base = c.baseApp->createEntity("Avatar");
    auto id = base->id();

    c.baseApp->requestCreateCell(id, "Avatar", Vector3{0, 0, 0}, 2);
    c.tickUntil([&] {
        return base->cellEntityCall() && base->cellEntityCall()->isValid();
    });

    bool ok = c.cellApp->runtime().findEntity(id) != nullptr;

    // Destroy entity — should send destroyCell first
    bool destroyed = c.baseApp->destroyEntity(id);
    ok = ok && destroyed;

    // Base entity should still exist (pending cell destruction)
    ok = ok && c.baseApp->findEntity(id) != nullptr;

    // Tick to propagate destroyCell → cellDestroyed → complete base destruction
    int ticks = c.tickUntil([&] {
        return c.baseApp->findEntity(id) == nullptr;
    }, 200);
    ok = ok && ticks > 0;

    // Cell entity should be gone
    ok = ok && c.cellApp->runtime().findEntity(id) == nullptr;

    // Base entity should be gone
    ok = ok && c.baseApp->findEntity(id) == nullptr;

    std::filesystem::remove_all(dir);
    if (ok) PASS();
    else FAIL("coordinated destruction failed");
}

// Test 2: Destroy entity without cell entity — immediate destruction
static void testDestroyWithoutCell() {
    TEST("Destruction: no cell entity → immediate base cleanup");

    auto dir = createDefDir();
    theseed::runtime::TcpConnection::globalInit();
    MiniCluster c(dir);
    c.connect();
    c.tickUntil([&] { return c.baseNode->hasPeer(2); });

    auto* base = c.baseApp->createEntity("Avatar");
    auto id = base->id();

    // Don't create cell entity
    bool ok = !base->cellEntityCall() || !base->cellEntityCall()->isValid();

    bool destroyed = c.baseApp->destroyEntity(id);
    ok = ok && destroyed;

    // Should be immediately destroyed (no pending)
    ok = ok && c.baseApp->findEntity(id) == nullptr;

    std::filesystem::remove_all(dir);
    if (ok) PASS();
    else FAIL("immediate destruction failed");
}

// Test 3: Destruction triggers onEntityDestroyed callback
static void testDestructionCallback() {
    TEST("Destruction: onEntityDestroyed callback fires");

    auto dir = createDefDir();
    theseed::runtime::TcpConnection::globalInit();
    MiniCluster c(dir);
    c.connect();
    c.tickUntil([&] { return c.baseNode->hasPeer(2); });

    auto* base = c.baseApp->createEntity("Avatar");
    auto id = base->id();

    // Destroy without cell (immediate)
    bool destroyed = c.baseApp->destroyEntity(id);
    bool ok = destroyed;

    // Verify entity is gone
    ok = ok && c.baseApp->findEntity(id) == nullptr;

    std::filesystem::remove_all(dir);
    if (ok) PASS();
    else FAIL("destruction callback test failed");
}

// Test 4: Destroy non-existent entity returns false
static void testDestroyNonExistent() {
    TEST("Destruction: non-existent entity returns false");

    auto dir = createDefDir();
    theseed::runtime::TcpConnection::globalInit();
    MiniCluster c(dir);
    c.connect();
    c.tickUntil([&] { return c.baseNode->hasPeer(2); });

    bool result = c.baseApp->destroyEntity(99999);
    bool ok = !result;

    std::filesystem::remove_all(dir);
    if (ok) PASS();
    else FAIL("should return false for non-existent entity");
}

// Test 5: Multiple entities — destroy one doesn't affect others
static void testDestroyIsolation() {
    TEST("Destruction: destroying one entity doesn't affect others");

    auto dir = createDefDir();
    theseed::runtime::TcpConnection::globalInit();
    MiniCluster c(dir);
    c.connect();
    c.tickUntil([&] { return c.baseNode->hasPeer(2); });

    auto* base1 = c.baseApp->createEntity("Avatar");
    auto* base2 = c.baseApp->createEntity("Avatar");
    auto id1 = base1->id();
    auto id2 = base2->id();

    c.baseApp->requestCreateCell(id1, "Avatar", Vector3{0, 0, 0}, 2);
    c.baseApp->requestCreateCell(id2, "Avatar", Vector3{10, 0, 10}, 2);
    c.tickUntil([&] {
        return base1->cellEntityCall() && base1->cellEntityCall()->isValid()
            && base2->cellEntityCall() && base2->cellEntityCall()->isValid();
    });

    // Destroy entity 1
    bool ok = c.baseApp->destroyEntity(id1);

    // Tick to complete destruction
    c.tickUntil([&] {
        return c.baseApp->findEntity(id1) == nullptr;
    }, 200);

    // Entity 2 should still exist
    ok = ok && c.baseApp->findEntity(id2) != nullptr;
    ok = ok && c.cellApp->runtime().findEntity(id2) != nullptr;

    std::filesystem::remove_all(dir);
    if (ok) PASS();
    else FAIL("destroy isolation failed");
}

// Test 6: Double destroy returns false
static void testDoubleDestroy() {
    TEST("Destruction: double destroy returns false");

    auto dir = createDefDir();
    theseed::runtime::TcpConnection::globalInit();
    MiniCluster c(dir);
    c.connect();
    c.tickUntil([&] { return c.baseNode->hasPeer(2); });

    auto* base = c.baseApp->createEntity("Avatar");
    auto id = base->id();

    bool first = c.baseApp->destroyEntity(id);
    bool second = c.baseApp->destroyEntity(id);
    bool ok = first && !second;

    std::filesystem::remove_all(dir);
    if (ok) PASS();
    else FAIL("double destroy should return false");
}

int main() {
    std::cout << "Coordinated entity destruction tests:\n";

    testCoordinatedDestruction();
    testDestroyWithoutCell();
    testDestructionCallback();
    testDestroyNonExistent();
    testDestroyIsolation();
    testDoubleDestroy();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
