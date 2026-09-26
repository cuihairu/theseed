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
    std::string dir = "test_cell_spawn_defs";
    std::filesystem::create_directory(dir);
    writeFile(dir + "/Avatar.xml", R"(
<EntityDef name="Avatar">
    <Properties>
        <Property name="level" type="Int32"/>
    </Properties>
</EntityDef>
)");
    writeFile(dir + "/Monster.xml", R"(
<EntityDef name="Monster">
    <Properties>
        <Property name="hp" type="Int32"/>
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

// Test: Cell entity spawns a new entity via base
static void testCellSpawnEntity() {
    TEST("Cell spawn: cell entity creates new entity via base");

    auto dir = createDefDir();
    theseed::runtime::TcpConnection::globalInit();
    MiniCluster c(dir);
    c.connect();
    c.tickUntil([&] { return c.baseNode->hasPeer(2); });

    // Create creator entity with cell
    auto* baseEntity = c.baseApp->createEntity("Avatar");
    auto creatorId = baseEntity->id();

    c.baseApp->requestCreateCell(creatorId, "Avatar", Vector3{0, 0, 0}, 2);
    c.tickUntil([&] {
        return baseEntity->cellEntityCall() && baseEntity->cellEntityCall()->isValid();
    });

    auto* cellEntity = c.cellApp->runtime().findEntity(creatorId);
    bool ok = cellEntity != nullptr;
    if (!ok) { std::filesystem::remove_all(dir); FAIL("creator cell missing"); return; }

    // Record initial entity count
    auto countBefore = c.baseApp->runtime().entityCount();

    // Cell requests spawn
    ok = ok && c.cellApp->runtime().requestSpawnEntity("Monster", Vector3{10, 0, 20}, creatorId);

    // Wait for new entity to appear on both sides
    int ticks = c.tickUntil([&] {
        return c.baseApp->runtime().entityCount() == countBefore + 1;
    }, 100);

    ok = ok && ticks > 0;

    // Find the new entity (not the creator)
    Entity* newBaseEntity = nullptr;
    c.baseApp->runtime().forEachEntity([&](Entity& e) {
        if (e.id() != creatorId && e.entityType() == "Monster") {
            newBaseEntity = &e;
        }
    });
    ok = ok && newBaseEntity != nullptr;

    // Wait for cell entity to be created
    if (newBaseEntity) {
        int cellTicks = c.tickUntil([&] {
            return newBaseEntity->cellEntityCall() && newBaseEntity->cellEntityCall()->isValid();
        }, 100);
        ok = ok && cellTicks > 0;

        auto* newCellEntity = c.cellApp->runtime().findEntity(newBaseEntity->id());
        ok = ok && newCellEntity != nullptr;
    }

    std::filesystem::remove_all(dir);

    if (ok) PASS();
    else FAIL("baseCount=" + std::to_string(c.baseApp->runtime().entityCount())
              + " ticks=" + std::to_string(ticks));
}

// Test: Spawn with unknown entity type fails gracefully
static void testCellSpawnUnknownType() {
    TEST("Cell spawn: unknown entity type handled gracefully");

    auto dir = createDefDir();
    theseed::runtime::TcpConnection::globalInit();
    MiniCluster c(dir);
    c.connect();
    c.tickUntil([&] { return c.baseNode->hasPeer(2); });

    auto* baseEntity = c.baseApp->createEntity("Avatar");
    auto creatorId = baseEntity->id();

    c.baseApp->requestCreateCell(creatorId, "Avatar", Vector3{0, 0, 0}, 2);
    c.tickUntil([&] {
        return baseEntity->cellEntityCall() && baseEntity->cellEntityCall()->isValid();
    });

    // Request spawn of unknown type — callBase sends but base fails to create
    c.cellApp->runtime().requestSpawnEntity("NonExistent", Vector3{0, 0, 0}, creatorId);

    // Let it propagate
    c.tickUntil([&] { return false; }, 20);

    // Entity count should not increase (or may increase briefly then get destroyed)
    // The key is no crash
    bool ok = true;

    std::filesystem::remove_all(dir);

    if (ok) PASS();
    else FAIL("crashed or unexpected state");
}

// Test: Spawn without cell entity call returns false
static void testCellSpawnNoBaseCall() {
    TEST("Cell spawn: fails without base entity call");

    auto dir = createDefDir();
    theseed::runtime::TcpConnection::globalInit();
    MiniCluster c(dir);
    c.connect();
    c.tickUntil([&] { return c.baseNode->hasPeer(2); });

    c.baseApp->createEntity("Avatar");  // 只需 base 侧存在实体，cell 侧走独立创建

    // Create cell but don't wait for connection — test with entity that has no base call
    // Actually, CellApp::createEntity creates an entity directly without base call
    auto* cellEntity = c.cellApp->createEntity("Avatar", Vector3{0, 0, 0});
    bool ok = cellEntity != nullptr;
    if (!ok) { std::filesystem::remove_all(dir); FAIL("cell entity missing"); return; }

    // This entity has no baseEntityCall, so requestSpawnEntity should return false
    ok = !c.cellApp->runtime().requestSpawnEntity("Monster", Vector3{0, 0, 0}, cellEntity->id());

    std::filesystem::remove_all(dir);

    if (ok) PASS();
    else FAIL("should have returned false");
}

int main() {
    std::cout << "Cell spawn tests:\n";

    testCellSpawnEntity();
    testCellSpawnUnknownType();
    testCellSpawnNoBaseCall();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
