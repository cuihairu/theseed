#include "theseed/core/BaseApp.h"
#include "theseed/core/CellApp.h"
#include "theseed/core/IEntityStore.h"
#include "theseed/foundation/MemoryStream.h"
#include "theseed/login/ClientProtocol.h"
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
using theseed::login::ActionMsg;

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

// Test 1: Client action routed to Cell-side method
static void testClientActionToCell() {
    TEST("Client action -> Cell-side method dispatch");

    std::string dir = "test_client_method_defs1";
    std::filesystem::create_directory(dir);
    writeFile(dir + "/Avatar.xml", R"(
<EntityDef name="Avatar">
    <Properties>
        <Property name="hp" type="Float32" defaultValue="100.0"/>
    </Properties>
    <Methods>
        <Method name="castSkill" side="Cell">
            <Arg name="skillId" type="Int32"/>
        </Method>
    </Methods>
</EntityDef>
)");

    theseed::runtime::TcpConnection::globalInit();
    MiniCluster c(dir);
    c.connect();
    c.tickUntil([&] { return c.baseNode->hasPeer(2); });

    std::int32_t receivedSkillId = 0;
    c.cellApp->runtime().setEntityFactoryHook([&](Entity& entity) {
        if (entity.entityType() != "Avatar") return;
        std::function<void(Entity&, std::int32_t)> handler =
            [&](Entity&, std::int32_t skillId) {
                receivedSkillId = skillId;
            };
        entity.bindTypedMethodHandler<std::int32_t>("castSkill", std::move(handler));
    });

    auto* base = c.baseApp->createEntity("Avatar");
    auto id = base->id();

    c.baseApp->requestCreateCell(id, "Avatar", Vector3{0, 0, 0}, 2);
    c.tickUntil([&] {
        return base->cellEntityCall() && base->cellEntityCall()->isValid();
    });

    // Simulate client action
    ActionMsg action;
    action.entityId = id;
    action.actionName = "castSkill";
    // Encode int32 arg into actionData
    theseed::foundation::MemoryStream ms;
    ms.writeInt32(std::int32_t(5));
    action.actionData.assign(
        reinterpret_cast<const std::byte*>(ms.data()),
        reinterpret_cast<const std::byte*>(ms.data()) + ms.size());

    c.baseApp->handleClientAction(action);

    c.tickUntil([&] { return receivedSkillId != 0; }, 200);

    bool ok = receivedSkillId == 5;

    std::filesystem::remove_all(dir);
    if (ok) PASS();
    else FAIL("client action -> cell method failed");
}

// Test 2: Client action routed to Base-side method
static void testClientActionToBase() {
    TEST("Client action -> Base-side method dispatch");

    std::string dir = "test_client_method_defs2";
    std::filesystem::create_directory(dir);
    writeFile(dir + "/Avatar.xml", R"(
<EntityDef name="Avatar">
    <Properties>
        <Property name="gold" type="Int32" defaultValue="0"/>
    </Properties>
    <Methods>
        <Method name="buyItem" side="Base">
            <Arg name="itemId" type="Int32"/>
            <Arg name="price" type="Int32"/>
        </Method>
    </Methods>
</EntityDef>
)");

    theseed::runtime::TcpConnection::globalInit();
    MiniCluster c(dir);
    c.connect();
    c.tickUntil([&] { return c.baseNode->hasPeer(2); });

    std::int32_t receivedItemId = 0;
    std::int32_t receivedPrice = 0;
    c.baseApp->runtime().setEntityFactoryHook([&](Entity& entity) {
        if (entity.entityType() != "Avatar") return;
        std::function<void(Entity&, std::int32_t, std::int32_t)> handler =
            [&](Entity& e, std::int32_t itemId, std::int32_t price) {
                receivedItemId = itemId;
                receivedPrice = price;
                auto* gold = e.findProperty<std::int32_t>("gold");
                if (gold) {
                    e.setProperty<std::int32_t>("gold", *gold - price);
                }
            };
        entity.bindTypedMethodHandler<std::int32_t, std::int32_t>("buyItem", std::move(handler));
    });

    auto* base = c.baseApp->createEntity("Avatar");
    auto id = base->id();

    c.baseApp->requestCreateCell(id, "Avatar", Vector3{0, 0, 0}, 2);
    c.tickUntil([&] {
        return base->cellEntityCall() && base->cellEntityCall()->isValid();
    });

    // Simulate client action
    ActionMsg action;
    action.entityId = id;
    action.actionName = "buyItem";
    theseed::foundation::MemoryStream ms;
    ms.writeInt32(std::int32_t(1001));  // itemId
    ms.writeInt32(std::int32_t(50));    // price
    action.actionData.assign(
        reinterpret_cast<const std::byte*>(ms.data()),
        reinterpret_cast<const std::byte*>(ms.data()) + ms.size());

    c.baseApp->handleClientAction(action);

    // Base-side dispatch is synchronous
    bool ok = receivedItemId == 1001;
    ok = ok && receivedPrice == 50;

    if (ok) {
        auto* gold = base->findProperty<std::int32_t>("gold");
        ok = gold != nullptr && *gold == -50;
    }

    std::filesystem::remove_all(dir);
    if (ok) PASS();
    else FAIL("client action -> base method failed");
}

// Test 3: Undefined action name is ignored
static void testClientActionUndefined() {
    TEST("Client action with undefined method is ignored");

    std::string dir = "test_client_method_defs3";
    std::filesystem::create_directory(dir);
    writeFile(dir + "/Avatar.xml", R"(
<EntityDef name="Avatar">
    <Properties>
        <Property name="hp" type="Float32" defaultValue="100.0"/>
    </Properties>
    <Methods>
        <Method name="heal" side="Cell">
            <Arg name="amount" type="Float32"/>
        </Method>
    </Methods>
</EntityDef>
)");

    theseed::runtime::TcpConnection::globalInit();
    auto baseNode = std::make_shared<NetworkNode>(NetworkNode::Config{.localComponent = 1, .listenPort = 0});
    auto store = std::make_shared<InMemoryEntityStore>();
    auto baseApp = std::make_unique<BaseApp>(BaseApp::Config{.entityDefPath = dir, .componentId = 1}, baseNode->hub(), store);
    baseApp->init();

    auto* entity = baseApp->createEntity("Avatar");
    auto id = entity->id();

    ActionMsg action;
    action.entityId = id;
    action.actionName = "cheat";
    action.actionData = {};

    // Should not crash, just return
    baseApp->handleClientAction(action);

    // Entity should still exist and be unchanged
    bool ok = baseApp->findEntity(id) != nullptr;
    auto* hp = entity->findProperty<float>("hp");
    ok = ok && hp != nullptr && *hp == 100.0f;

    std::filesystem::remove_all(dir);
    if (ok) PASS();
    else FAIL("undefined action should be ignored");
}

// Test 4: Client method call updates property and syncs to cell
static void testClientMethodPropertySync() {
    TEST("Client base method updates property -> syncs to cell");

    std::string dir = "test_client_method_defs4";
    std::filesystem::create_directory(dir);
    writeFile(dir + "/Avatar.xml", R"(
<EntityDef name="Avatar">
    <Properties>
        <Property name="hp" type="Float32" defaultValue="100.0"/>
        <Property name="defense" type="Int32" defaultValue="10"/>
    </Properties>
    <Methods>
        <Method name="equipArmor" side="Base">
            <Arg name="defenseBonus" type="Int32"/>
        </Method>
    </Methods>
</EntityDef>
)");

    theseed::runtime::TcpConnection::globalInit();
    MiniCluster c(dir);
    c.connect();
    c.tickUntil([&] { return c.baseNode->hasPeer(2); });

    c.baseApp->runtime().setEntityFactoryHook([&](Entity& entity) {
        if (entity.entityType() != "Avatar") return;
        std::function<void(Entity&, std::int32_t)> handler =
            [&](Entity& e, std::int32_t defenseBonus) {
                auto* defense = e.findProperty<std::int32_t>("defense");
                if (defense) {
                    e.setProperty<std::int32_t>("defense", *defense + defenseBonus);
                }
            };
        entity.bindTypedMethodHandler<std::int32_t>("equipArmor", std::move(handler));
    });

    auto* base = c.baseApp->createEntity("Avatar");
    auto id = base->id();

    c.baseApp->requestCreateCell(id, "Avatar", Vector3{0, 0, 0}, 2);
    c.tickUntil([&] {
        return base->cellEntityCall() && base->cellEntityCall()->isValid();
    });

    // Simulate client calling equipArmor
    ActionMsg action;
    action.entityId = id;
    action.actionName = "equipArmor";
    theseed::foundation::MemoryStream ms;
    ms.writeInt32(std::int32_t(15));
    action.actionData.assign(
        reinterpret_cast<const std::byte*>(ms.data()),
        reinterpret_cast<const std::byte*>(ms.data()) + ms.size());

    c.baseApp->handleClientAction(action);

    // Base property should be updated immediately
    bool ok = *base->findProperty<std::int32_t>("defense") == 25;

    // Property should sync to cell
    c.tickUntil([&] {
        auto* cell = c.cellApp->runtime().findEntity(id);
        if (!cell) return false;
        auto* defense = cell->findProperty<std::int32_t>("defense");
        return defense && *defense == 25;
    }, 200);

    std::filesystem::remove_all(dir);
    if (ok) PASS();
    else FAIL("client method property sync failed");
}

// Test 5: Cell-side client method with multiple typed args
static void testClientCellMethodMultiArgs() {
    TEST("Client cell method with float + float + string args");

    std::string dir = "test_client_method_defs5";
    std::filesystem::create_directory(dir);
    writeFile(dir + "/Avatar.xml", R"(
<EntityDef name="Avatar">
    <Properties>
        <Property name="hp" type="Float32" defaultValue="100.0"/>
    </Properties>
    <Methods>
        <Method name="moveTo" side="Cell">
            <Arg name="x" type="Float32"/>
            <Arg name="y" type="Float32"/>
        </Method>
    </Methods>
</EntityDef>
)");

    theseed::runtime::TcpConnection::globalInit();
    MiniCluster c(dir);
    c.connect();
    c.tickUntil([&] { return c.baseNode->hasPeer(2); });

    float targetX = 0, targetY = 0;
    c.cellApp->runtime().setEntityFactoryHook([&](Entity& entity) {
        if (entity.entityType() != "Avatar") return;
        std::function<void(Entity&, float, float)> handler =
            [&](Entity&, float x, float y) {
                targetX = x;
                targetY = y;
            };
        entity.bindTypedMethodHandler<float, float>("moveTo", std::move(handler));
    });

    auto* base = c.baseApp->createEntity("Avatar");
    auto id = base->id();

    c.baseApp->requestCreateCell(id, "Avatar", Vector3{0, 0, 0}, 2);
    c.tickUntil([&] {
        return base->cellEntityCall() && base->cellEntityCall()->isValid();
    });

    ActionMsg action;
    action.entityId = id;
    action.actionName = "moveTo";
    theseed::foundation::MemoryStream ms;
    ms.writeFloat(10.5f);
    ms.writeFloat(20.3f);
    action.actionData.assign(
        reinterpret_cast<const std::byte*>(ms.data()),
        reinterpret_cast<const std::byte*>(ms.data()) + ms.size());

    c.baseApp->handleClientAction(action);

    c.tickUntil([&] { return targetX > 0; }, 200);

    bool ok = std::abs(targetX - 10.5f) < 0.01f;
    ok = ok && std::abs(targetY - 20.3f) < 0.01f;

    std::filesystem::remove_all(dir);
    if (ok) PASS();
    else FAIL("client cell method multi-args failed");
}

int main() {
    std::cout << "Client method routing tests:\n";

    testClientActionToCell();
    testClientActionToBase();
    testClientActionUndefined();
    testClientMethodPropertySync();
    testClientCellMethodMultiArgs();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
