#include "theseed/core/BaseApp.h"
#include "theseed/core/CellApp.h"
#include "theseed/core/IEntityStore.h"
#include "theseed/runtime/Entity.h"
#include "theseed/runtime/EntityDef.h"
#include "theseed/runtime/NetworkNode.h"
#include "theseed/runtime/TcpConnection.h"
#include "theseed/runtime/TickScheduler.h"

#include <cmath>
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
    std::string dir = "test_entity_query_defs";
    std::filesystem::create_directory(dir);
    writeFile(dir + "/Avatar.xml", R"(
<EntityDef name="Avatar">
    <Properties>
        <Property name="hp" type="Float32" defaultValue="100.0"/>
        <Property name="level" type="Int32" defaultValue="1"/>
    </Properties>
</EntityDef>
)");
    writeFile(dir + "/NPC.xml", R"(
<EntityDef name="NPC">
    <Properties>
        <Property name="hp" type="Float32" defaultValue="50.0"/>
        <Property name="shopType" type="Int32" defaultValue="0"/>
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

// Test 1: BaseRuntime findEntitiesByTag
static void testBaseFindEntitiesByTag() {
    TEST("BaseRuntime::findEntitiesByTag");

    auto dir = createDefDir();
    theseed::runtime::TcpConnection::globalInit();
    MiniCluster c(dir);
    c.connect();
    c.tickUntil([&] { return c.baseNode->hasPeer(2); });

    auto* e1 = c.baseApp->createEntity("Avatar");
    auto* e2 = c.baseApp->createEntity("Avatar");
    auto* e3 = c.baseApp->createEntity("NPC");

    e1->addTag("player");
    e2->addTag("player");
    e3->addTag("vendor");

    auto players = c.baseApp->runtime().findEntitiesByTag("player");
    bool ok = players.size() == 2;

    auto vendors = c.baseApp->runtime().findEntitiesByTag("vendor");
    ok = ok && vendors.size() == 1;
    ok = ok && vendors[0]->id() == e3->id();

    auto none = c.baseApp->runtime().findEntitiesByTag("nonexistent");
    ok = ok && none.empty();

    std::filesystem::remove_all(dir);
    if (ok) PASS();
    else FAIL("tag count mismatch");
}

// Test 2: BaseRuntime queryEntities with property predicate
static void testBaseQueryEntities() {
    TEST("BaseRuntime::queryEntities with property predicate");

    auto dir = createDefDir();
    theseed::runtime::TcpConnection::globalInit();
    MiniCluster c(dir);
    c.connect();
    c.tickUntil([&] { return c.baseNode->hasPeer(2); });

    auto* e1 = c.baseApp->createEntity("Avatar");
    auto* e2 = c.baseApp->createEntity("Avatar");
    auto* e3 = c.baseApp->createEntity("Avatar");

    e1->setProperty<std::int32_t>("level", 5);
    e2->setProperty<std::int32_t>("level", 10);
    e3->setProperty<std::int32_t>("level", 15);

    auto highLevel = c.baseApp->runtime().queryEntities([](const Entity& e) {
        auto* lvl = e.findProperty<std::int32_t>("level");
        return lvl && *lvl >= 10;
    });

    bool ok = highLevel.size() == 2;

    // Verify correct entities matched
    if (ok) {
        bool foundE2 = false, foundE3 = false;
        for (auto* e : highLevel) {
            if (e->id() == e2->id()) foundE2 = true;
            if (e->id() == e3->id()) foundE3 = true;
        }
        ok = foundE2 && foundE3;
    }

    std::filesystem::remove_all(dir);
    if (ok) PASS();
    else FAIL("query result count=" + std::to_string(highLevel.size()));
}

// Test 3: CellRuntime findEntitiesByTag
static void testCellFindEntitiesByTag() {
    TEST("CellRuntime::findEntitiesByTag");

    auto dir = createDefDir();
    theseed::runtime::TcpConnection::globalInit();
    MiniCluster c(dir);
    c.connect();
    c.tickUntil([&] { return c.baseNode->hasPeer(2); });

    auto* b1 = c.baseApp->createEntity("Avatar");
    auto* b2 = c.baseApp->createEntity("Avatar");
    auto* b3 = c.baseApp->createEntity("NPC");

    // Tag base entities; hooks will propagate to cell entities
    b1->addTag("player");
    b2->addTag("player");
    b3->addTag("vendor");

    auto id1 = b1->id(), id2 = b2->id(), id3 = b3->id();

    // Use factory hook to tag cell entities
    c.cellApp->runtime().setEntityFactoryHook([&](Entity& entity) {
        if (entity.id() == id1 || entity.id() == id2) {
            entity.addTag("player");
        } else if (entity.id() == id3) {
            entity.addTag("vendor");
        }
    });

    c.baseApp->requestCreateCell(id1, "Avatar", Vector3{0, 0, 0}, 2);
    c.baseApp->requestCreateCell(id2, "Avatar", Vector3{10, 0, 0}, 2);
    c.baseApp->requestCreateCell(id3, "NPC", Vector3{20, 0, 0}, 2);

    c.tickUntil([&] {
        return c.cellApp->runtime().findEntity(id1)
            && c.cellApp->runtime().findEntity(id2)
            && c.cellApp->runtime().findEntity(id3);
    });

    auto players = c.cellApp->runtime().findEntitiesByTag("player");
    bool ok = players.size() == 2;

    auto vendors = c.cellApp->runtime().findEntitiesByTag("vendor");
    ok = ok && vendors.size() == 1;

    std::filesystem::remove_all(dir);
    if (ok) PASS();
    else FAIL("cell tag count: players=" + std::to_string(players.size()) + " vendors=" + std::to_string(vendors.size()));
}

// Test 4: CellRuntime queryEntities with type predicate
static void testCellQueryEntitiesByType() {
    TEST("CellRuntime::queryEntities with type predicate");

    auto dir = createDefDir();
    theseed::runtime::TcpConnection::globalInit();
    MiniCluster c(dir);
    c.connect();
    c.tickUntil([&] { return c.baseNode->hasPeer(2); });

    auto* b1 = c.baseApp->createEntity("Avatar");
    auto* b2 = c.baseApp->createEntity("Avatar");
    auto* b3 = c.baseApp->createEntity("NPC");

    auto id1 = b1->id(), id2 = b2->id(), id3 = b3->id();

    c.baseApp->requestCreateCell(id1, "Avatar", Vector3{0, 0, 0}, 2);
    c.baseApp->requestCreateCell(id2, "Avatar", Vector3{10, 0, 0}, 2);
    c.baseApp->requestCreateCell(id3, "NPC", Vector3{20, 0, 0}, 2);

    c.tickUntil([&] {
        return c.cellApp->runtime().findEntity(id1)
            && c.cellApp->runtime().findEntity(id2)
            && c.cellApp->runtime().findEntity(id3);
    });

    auto avatars = c.cellApp->runtime().queryEntities([](const Entity& e) {
        return e.entityType() == "Avatar";
    });
    bool ok = avatars.size() == 2;

    auto npcs = c.cellApp->runtime().queryEntities([](const Entity& e) {
        return e.entityType() == "NPC";
    });
    ok = ok && npcs.size() == 1;

    std::filesystem::remove_all(dir);
    if (ok) PASS();
    else FAIL("cell query: avatars=" + std::to_string(avatars.size()) + " npcs=" + std::to_string(npcs.size()));
}

// Test 5: Combined tag + property query
static void testCombinedTagPropertyQuery() {
    TEST("Combined tag + property query");

    auto dir = createDefDir();
    theseed::runtime::TcpConnection::globalInit();
    MiniCluster c(dir);
    c.connect();
    c.tickUntil([&] { return c.baseNode->hasPeer(2); });

    auto* e1 = c.baseApp->createEntity("Avatar");
    auto* e2 = c.baseApp->createEntity("Avatar");
    auto* e3 = c.baseApp->createEntity("Avatar");

    e1->addTag("warrior");
    e2->addTag("warrior");
    e3->addTag("mage");

    e1->setProperty<std::int32_t>("level", 5);
    e2->setProperty<std::int32_t>("level", 20);
    e3->setProperty<std::int32_t>("level", 15);

    // Find high-level warriors
    auto result = c.baseApp->runtime().queryEntities([](const Entity& e) {
        if (!e.hasTag("warrior")) return false;
        auto* lvl = e.findProperty<std::int32_t>("level");
        return lvl && *lvl >= 15;
    });

    bool ok = result.size() == 1;
    ok = ok && result[0]->id() == e2->id();

    std::filesystem::remove_all(dir);
    if (ok) PASS();
    else FAIL("combined query count=" + std::to_string(result.size()));
}

// Test 6: forEachEntity iteration
static void testForEachEntity() {
    TEST("CellRuntime::forEachEntity iteration");

    auto dir = createDefDir();
    theseed::runtime::TcpConnection::globalInit();
    MiniCluster c(dir);
    c.connect();
    c.tickUntil([&] { return c.baseNode->hasPeer(2); });

    auto* b1 = c.baseApp->createEntity("Avatar");
    auto* b2 = c.baseApp->createEntity("NPC");

    auto id1 = b1->id(), id2 = b2->id();

    c.baseApp->requestCreateCell(id1, "Avatar", Vector3{0, 0, 0}, 2);
    c.baseApp->requestCreateCell(id2, "NPC", Vector3{10, 0, 0}, 2);

    c.tickUntil([&] {
        return c.cellApp->runtime().findEntity(id1)
            && c.cellApp->runtime().findEntity(id2);
    });

    std::vector<EntityId> ids;
    c.cellApp->runtime().forEachEntity([&](Entity& e) {
        ids.push_back(e.id());
    });

    bool ok = ids.size() == 2;
    bool hasId1 = false, hasId2 = false;
    for (auto id : ids) {
        if (id == id1) hasId1 = true;
        if (id == id2) hasId2 = true;
    }
    ok = ok && hasId1 && hasId2;

    std::filesystem::remove_all(dir);
    if (ok) PASS();
    else FAIL("forEach count=" + std::to_string(ids.size()));
}

// Test 7: Entity tag add/remove
static void testTagAddRemove() {
    TEST("Entity tag add/remove lifecycle");

    auto dir = createDefDir();
    theseed::runtime::TcpConnection::globalInit();
    MiniCluster c(dir);
    c.connect();
    c.tickUntil([&] { return c.baseNode->hasPeer(2); });

    auto* e = c.baseApp->createEntity("Avatar");

    e->addTag("active");
    bool ok = e->hasTag("active");

    e->removeTag("active");
    ok = ok && !e->hasTag("active");

    // Removing non-existent tag should be safe
    e->removeTag("nonexistent");
    ok = ok && !e->hasTag("nonexistent");

    // Multiple tags
    e->addTag("tag1");
    e->addTag("tag2");
    e->addTag("tag3");
    ok = ok && e->hasTag("tag1") && e->hasTag("tag2") && e->hasTag("tag3");

    auto tagged = c.baseApp->runtime().findEntitiesByTag("tag2");
    ok = ok && tagged.size() == 1;

    std::filesystem::remove_all(dir);
    if (ok) PASS();
    else FAIL("tag lifecycle failed");
}

// Test 8: Space-level spatial query
static void testSpaceSpatialQuery() {
    TEST("Space::queryRange spatial query");

    auto dir = createDefDir();
    theseed::runtime::TcpConnection::globalInit();
    MiniCluster c(dir);
    c.connect();
    c.tickUntil([&] { return c.baseNode->hasPeer(2); });

    auto* b1 = c.baseApp->createEntity("Avatar");
    auto* b2 = c.baseApp->createEntity("Avatar");
    auto* b3 = c.baseApp->createEntity("Avatar");

    auto id1 = b1->id(), id2 = b2->id(), id3 = b3->id();

    c.baseApp->requestCreateCell(id1, "Avatar", Vector3{0, 0, 0}, 2);
    c.baseApp->requestCreateCell(id2, "Avatar", Vector3{5, 0, 0}, 2);
    c.baseApp->requestCreateCell(id3, "Avatar", Vector3{100, 0, 0}, 2);

    c.tickUntil([&] {
        return c.cellApp->runtime().findEntity(id1)
            && c.cellApp->runtime().findEntity(id2)
            && c.cellApp->runtime().findEntity(id3);
    });

    // Query range 10 from origin: should find id1 (0,0,0) and id2 (5,0,0)
    auto& space = c.cellApp->runtime().spaceRuntime().space();
    auto nearby = space.queryRange(Vector3{0, 0, 0}, 10.0f);

    bool ok = nearby.size() == 2;
    bool found1 = false, found2 = false;
    for (auto* e : nearby) {
        if (e->id() == id1) found1 = true;
        if (e->id() == id2) found2 = true;
    }
    ok = ok && found1 && found2;

    std::filesystem::remove_all(dir);
    if (ok) PASS();
    else FAIL("spatial query count=" + std::to_string(nearby.size()));
}

// Test 9: Space findEntitiesByType
static void testSpaceFindEntitiesByType() {
    TEST("Space::findEntitiesByType");

    auto dir = createDefDir();
    theseed::runtime::TcpConnection::globalInit();
    MiniCluster c(dir);
    c.connect();
    c.tickUntil([&] { return c.baseNode->hasPeer(2); });

    auto* b1 = c.baseApp->createEntity("Avatar");
    auto* b2 = c.baseApp->createEntity("Avatar");
    auto* b3 = c.baseApp->createEntity("NPC");

    auto id1 = b1->id(), id2 = b2->id(), id3 = b3->id();

    c.baseApp->requestCreateCell(id1, "Avatar", Vector3{0, 0, 0}, 2);
    c.baseApp->requestCreateCell(id2, "Avatar", Vector3{10, 0, 0}, 2);
    c.baseApp->requestCreateCell(id3, "NPC", Vector3{20, 0, 0}, 2);

    c.tickUntil([&] {
        return c.cellApp->runtime().findEntity(id1)
            && c.cellApp->runtime().findEntity(id2)
            && c.cellApp->runtime().findEntity(id3);
    });

    auto& space = c.cellApp->runtime().spaceRuntime().space();
    auto avatars = space.findEntitiesByType("Avatar");
    bool ok = avatars.size() == 2;

    auto npcs = space.findEntitiesByType("NPC");
    ok = ok && npcs.size() == 1;

    auto none = space.findEntitiesByType("Monster");
    ok = ok && none.empty();

    std::filesystem::remove_all(dir);
    if (ok) PASS();
    else FAIL("space type query: avatars=" + std::to_string(avatars.size()));
}

int main() {
    std::cout << "Entity query tests:\n";

    testBaseFindEntitiesByTag();
    testBaseQueryEntities();
    testCellFindEntitiesByTag();
    testCellQueryEntitiesByType();
    testCombinedTagPropertyQuery();
    testForEachEntity();
    testTagAddRemove();
    testSpaceSpatialQuery();
    testSpaceFindEntitiesByType();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
