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

// Test 1: Property default values from XML
static void testDefaultValues() {
    std::string dir = "test_prop_flags_defs1";
    std::filesystem::create_directory(dir);
    writeFile(dir + "/Npc.xml", R"(
<EntityDef name="Npc">
    <Properties>
        <Property name="hp" type="Float32" defaultValue="100.0"/>
        <Property name="level" type="Int32" defaultValue="1"/>
        <Property name="name" type="String" defaultValue="Monster"/>
    </Properties>
</EntityDef>
)");

    theseed::runtime::TcpConnection::globalInit();
    auto cellNode = std::make_shared<NetworkNode>(NetworkNode::Config{.localComponent = 2, .listenPort = 0});
    auto baseNode = std::make_shared<NetworkNode>(NetworkNode::Config{.localComponent = 1, .listenPort = 0});
    auto store = std::make_shared<InMemoryEntityStore>();
    auto baseApp = std::make_unique<BaseApp>(BaseApp::Config{.entityDefPath = dir, .componentId = 1}, baseNode->hub(), store);
    baseApp->init();

    auto* entity = baseApp->createEntity("Npc");
    bool ok = entity != nullptr;

    if (ok) {
        auto* hp = entity->findProperty<float>("hp");
        ok = ok && hp != nullptr && *hp == 100.0f;
    }
    if (ok) {
        auto* level = entity->findProperty<std::int32_t>("level");
        ok = ok && level != nullptr && *level == 1;
    }
    if (ok) {
        auto name = entity->findString("name");
        ok = ok && name == "Monster";
    }

    std::filesystem::remove_all(dir);
    if (ok) PASS();
    else FAIL("default values incorrect");
}

// Test 2: Properties without default are zero-initialized
static void testNoDefaultZero() {
    std::string dir = "test_prop_flags_defs2";
    std::filesystem::create_directory(dir);
    writeFile(dir + "/Item.xml", R"(
<EntityDef name="Item">
    <Properties>
        <Property name="count" type="Int32"/>
        <Property name="weight" type="Float32"/>
    </Properties>
</EntityDef>
)");

    theseed::runtime::TcpConnection::globalInit();
    auto baseNode = std::make_shared<NetworkNode>(NetworkNode::Config{.localComponent = 1, .listenPort = 0});
    auto store = std::make_shared<InMemoryEntityStore>();
    auto baseApp = std::make_unique<BaseApp>(BaseApp::Config{.entityDefPath = dir, .componentId = 1}, baseNode->hub(), store);
    baseApp->init();

    auto* entity = baseApp->createEntity("Item");
    bool ok = entity != nullptr;

    if (ok) {
        auto* count = entity->findProperty<std::int32_t>("count");
        ok = ok && count != nullptr && *count == 0;
    }
    if (ok) {
        auto* weight = entity->findProperty<float>("weight");
        ok = ok && weight != nullptr && *weight == 0.0f;
    }

    std::filesystem::remove_all(dir);
    if (ok) PASS();
    else FAIL("zero-init incorrect");
}

// Test 3: Property flags parsed from XML
static void testPropertyFlags() {
    std::string dir = "test_prop_flags_defs3";
    std::filesystem::create_directory(dir);
    writeFile(dir + "/Player.xml", R"(
<EntityDef name="Player">
    <Properties>
        <Property name="hp" type="Float32" defaultValue="100.0" flags="Persistent,ClientSync"/>
        <Property name="adminLevel" type="Int32" flags="Base"/>
        <Property name="speed" type="Float32" flags="Cell"/>
        <Property name="name" type="String" defaultValue="Player"/>
    </Properties>
</EntityDef>
)");

    theseed::runtime::TcpConnection::globalInit();
    auto baseNode = std::make_shared<NetworkNode>(NetworkNode::Config{.localComponent = 1, .listenPort = 0});
    auto store = std::make_shared<InMemoryEntityStore>();
    auto baseApp = std::make_unique<BaseApp>(BaseApp::Config{.entityDefPath = dir, .componentId = 1}, baseNode->hub(), store);
    baseApp->init();

    auto* entity = baseApp->createEntity("Player");
    bool ok = entity != nullptr;

    // Check flags via propertyBlock
    if (ok) {
        auto& def = entity->propertyBlock().def();
        auto* hpDesc = def.findProperty("hp");
        ok = ok && hpDesc != nullptr;
        if (hpDesc) {
            ok = ok && theseed::runtime::hasFlag(hpDesc->flags, theseed::runtime::PropertyFlag::Persistent);
            ok = ok && theseed::runtime::hasFlag(hpDesc->flags, theseed::runtime::PropertyFlag::ClientSync);
        }
        auto* adminDesc = def.findProperty("adminLevel");
        ok = ok && adminDesc != nullptr;
        if (adminDesc) {
            ok = ok && theseed::runtime::hasFlag(adminDesc->flags, theseed::runtime::PropertyFlag::Base);
            ok = ok && !theseed::runtime::hasFlag(adminDesc->flags, theseed::runtime::PropertyFlag::ClientSync);
        }
    }

    // Verify default value
    if (ok) {
        auto* hp = entity->findProperty<float>("hp");
        ok = ok && hp != nullptr && *hp == 100.0f;
    }

    std::filesystem::remove_all(dir);
    if (ok) PASS();
    else FAIL("property flags incorrect");
}

// Test 4: Base-flagged properties excluded from client snapshot
static void testBaseFlagExcludesFromClient() {
    std::string dir = "test_prop_flags_defs4";
    std::filesystem::create_directory(dir);
    writeFile(dir + "/Hero.xml", R"(
<EntityDef name="Hero">
    <Properties>
        <Property name="hp" type="Float32" defaultValue="100.0"/>
        <Property name="secretKey" type="Int32" flags="Base"/>
    </Properties>
</EntityDef>
)");

    theseed::runtime::TcpConnection::globalInit();
    auto baseNode = std::make_shared<NetworkNode>(NetworkNode::Config{.localComponent = 1, .listenPort = 0});
    auto store = std::make_shared<InMemoryEntityStore>();
    auto baseApp = std::make_unique<BaseApp>(BaseApp::Config{.entityDefPath = dir, .componentId = 1}, baseNode->hub(), store);
    baseApp->init();

    auto* entity = baseApp->createEntity("Hero");
    bool ok = entity != nullptr;

    // Modify both properties
    if (ok) {
        entity->setProperty<float>("hp", 50.0f);
        entity->setProperty<std::int32_t>("secretKey", 42);
    }

    // Build client-visible delta (should exclude Base-flagged properties)
    if (ok) {
        auto deltas = entity->buildDirtyPropertyDelta(theseed::runtime::PropertyFlag::Base);
        ok = ok && deltas.size() == 1;  // Only hp, not secretKey
        if (!deltas.empty()) {
            auto& def = entity->propertyBlock().def();
            auto* hpDesc = def.findProperty("hp");
            ok = ok && deltas[0].propertyId == hpDesc->id;
        }
    }

    // Full snapshot should also exclude Base-flagged
    if (ok) {
        entity->clearDirtyFlags();
        auto snapshot = entity->buildFullPropertySnapshot(theseed::runtime::PropertyFlag::Base);
        ok = ok && snapshot.size() == 1;  // Only hp
    }

    std::filesystem::remove_all(dir);
    if (ok) PASS();
    else FAIL("Base flag exclusion incorrect");
}

// Test 5: Mixed flags - all non-Base properties included
static void testMixedFlagsSync() {
    std::string dir = "test_prop_flags_defs5";
    std::filesystem::create_directory(dir);
    writeFile(dir + "/Mob.xml", R"(
<EntityDef name="Mob">
    <Properties>
        <Property name="hp" type="Float32" defaultValue="50.0" flags="Persistent,ClientSync"/>
        <Property name="internalState" type="Int32" flags="Base"/>
        <Property name="speed" type="Float32" flags="Cell"/>
        <Property name="name" type="String" defaultValue="Goblin"/>
    </Properties>
</EntityDef>
)");

    theseed::runtime::TcpConnection::globalInit();
    auto baseNode = std::make_shared<NetworkNode>(NetworkNode::Config{.localComponent = 1, .listenPort = 0});
    auto store = std::make_shared<InMemoryEntityStore>();
    auto baseApp = std::make_unique<BaseApp>(BaseApp::Config{.entityDefPath = dir, .componentId = 1}, baseNode->hub(), store);
    baseApp->init();

    auto* entity = baseApp->createEntity("Mob");
    bool ok = entity != nullptr;

    // Client snapshot should exclude Base-flagged
    if (ok) {
        auto snapshot = entity->buildFullPropertySnapshot(theseed::runtime::PropertyFlag::Base);
        // Should include: hp (Persistent,ClientSync), speed (Cell), name (None)
        // Should exclude: internalState (Base)
        ok = ok && snapshot.size() == 3;
    }

    // Cell sync should exclude Base-flagged too
    if (ok) {
        entity->setProperty<float>("hp", 30.0f);
        entity->setProperty<std::int32_t>("internalState", 99);
        auto deltas = entity->buildDirtyPropertyDelta(theseed::runtime::PropertyFlag::Base);
        ok = ok && deltas.size() == 1;  // Only hp
    }

    std::filesystem::remove_all(dir);
    if (ok) PASS();
    else FAIL("mixed flags sync incorrect");
}

int main() {
    std::cout << "Property flags and default values tests:\n";

    testDefaultValues();
    testNoDefaultZero();
    testPropertyFlags();
    testBaseFlagExcludesFromClient();
    testMixedFlagsSync();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
