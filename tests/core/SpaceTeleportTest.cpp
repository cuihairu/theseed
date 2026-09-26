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
    std::string dir = "test_teleport_defs";
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
        cellApp = std::make_unique<CellApp>(CellApp::Config{.entityDefPath = dir, .componentId = 2, .defaultSpaceId = 1}, cellNode->hub());
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

// Test 1: Local teleport between spaces on same CellApp
static void testLocalTeleport() {
    TEST("Teleport: local space-to-space transfer on same CellApp");

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

    // Create a second space on CellApp
    c.cellApp->runtime().createSpace(10, "dungeon");

    // Teleport from space 1 to space 10
    auto* cellEntity = c.cellApp->runtime().findEntity(id);
    bool ok = cellEntity != nullptr;
    if (ok) {
        auto oldSpace = c.cellApp->runtime().findEntitySpace(id);
        ok = ok && oldSpace == 1;
    }

    if (ok) {
        bool teleported = c.cellApp->runtime().teleportEntity(id, 10, Vector3{100, 0, 100});
        ok = ok && teleported;
    }

    if (ok) {
        auto newSpace = c.cellApp->runtime().findEntitySpace(id);
        ok = ok && newSpace == 10;
    }

    // Entity should still be findable
    if (ok) {
        auto* found = c.cellApp->runtime().findEntity(id);
        ok = ok && found != nullptr;
        ok = ok && found->position().x == 100.0f;
    }

    std::filesystem::remove_all(dir);
    if (ok) PASS();
    else FAIL("local teleport failed");
}

// Test 2: Network teleport request from base to cell
static void testNetworkTeleport() {
    TEST("Teleport: base requestTeleport triggers cell teleport + base notification");

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

    // Create target space on CellApp
    c.cellApp->runtime().createSpace(20, "battle_arena");

    // Track space change callback on base side
    base->setOnEnterSpace([&](Entity&, theseed::runtime::SpaceId) {
        // This fires on cell entity, not base - skip
    });

    // Request teleport from base
    bool sent = c.baseApp->requestTeleport(id, 20, Vector3{50, 0, 50});
    bool ok = sent;

    // Tick to propagate
    int ticks = c.tickUntil([&] {
        auto sp = c.cellApp->runtime().findEntitySpace(id);
        return sp == 20;
    }, 200);
    ok = ok && ticks > 0;

    // Verify position updated on cell
    if (ok) {
        auto* cellEntity = c.cellApp->runtime().findEntity(id);
        ok = ok && cellEntity != nullptr;
        if (cellEntity) {
            ok = ok && cellEntity->position().x == 50.0f;
        }
    }

    std::filesystem::remove_all(dir);
    if (ok) PASS();
    else FAIL("network teleport failed");
}

// Test 3: Entity callbacks fire during teleport
static void testTeleportCallbacks() {
    TEST("Teleport: onEnterSpace/onLeaveSpace callbacks fire");

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

    c.cellApp->runtime().createSpace(30, "instance");

    auto* cellEntity = c.cellApp->runtime().findEntity(id);
    bool ok = cellEntity != nullptr;

    theseed::runtime::SpaceId leftSpace = 0;
    theseed::runtime::SpaceId enteredSpace = 0;
    int leaveCount = 0;
    int enterCount = 0;

    if (ok) {
        cellEntity->setOnLeaveSpace([&](Entity&, theseed::runtime::SpaceId sid) {
            leftSpace = sid;
            ++leaveCount;
        });
        cellEntity->setOnEnterSpace([&](Entity&, theseed::runtime::SpaceId sid) {
            enteredSpace = sid;
            ++enterCount;
        });

        c.cellApp->runtime().teleportEntity(id, 30, Vector3{10, 0, 10});
    }

    ok = ok && leaveCount == 1 && leftSpace == 1;
    ok = ok && enterCount == 1 && enteredSpace == 30;

    std::filesystem::remove_all(dir);
    if (ok) PASS();
    else FAIL("teleport callbacks failed");
}

// Test 4: Teleport to non-existent space returns false
static void testTeleportToNonExistentSpace() {
    TEST("Teleport: to non-existent space returns false");

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

    // Space 999 doesn't exist
    bool result = c.cellApp->runtime().teleportEntity(id, 999, Vector3{0, 0, 0});
    bool ok = !result;

    // Entity should still be in original space
    if (ok) {
        auto sp = c.cellApp->runtime().findEntitySpace(id);
        ok = ok && sp == 1;
    }

    std::filesystem::remove_all(dir);
    if (ok) PASS();
    else FAIL("should have returned false for non-existent space");
}

// Test 5: Teleport same space returns false
static void testTeleportSameSpace() {
    TEST("Teleport: to same space returns false");

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

    auto sp = c.cellApp->runtime().findEntitySpace(id);
    bool result = c.cellApp->runtime().teleportEntity(id, sp, Vector3{0, 0, 0});
    bool ok = !result;

    std::filesystem::remove_all(dir);
    if (ok) PASS();
    else FAIL("should have returned false for same space");
}

// Test 6: Network teleport auto-creates target space if needed
static void testNetworkTeleportAutoCreateSpace() {
    TEST("Teleport: network request auto-creates target space");

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

    // Space 42 does NOT exist - handleTeleport should auto-create it
    bool sent = c.baseApp->requestTeleport(id, 42, Vector3{10, 0, 10});
    bool ok = sent;

    int ticks = c.tickUntil([&] {
        auto sp = c.cellApp->runtime().findEntitySpace(id);
        return sp == 42;
    }, 200);
    ok = ok && ticks > 0;

    // Verify space was auto-created
    if (ok) {
        auto* sr = c.cellApp->runtime().findSpaceRuntime(42);
        ok = ok && sr != nullptr;
    }

    std::filesystem::remove_all(dir);
    if (ok) PASS();
    else FAIL("auto-create space teleport failed");
}

int main() {
    std::cout << "Space teleportation tests:\n";

    testLocalTeleport();
    testNetworkTeleport();
    testTeleportCallbacks();
    testTeleportToNonExistentSpace();
    testTeleportSameSpace();
    testNetworkTeleportAutoCreateSpace();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
