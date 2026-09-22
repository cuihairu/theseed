#include "theseed/core/BaseApp.h"
#include "theseed/core/CellApp.h"
#include "theseed/core/IEntityStore.h"
#include "theseed/runtime/Entity.h"
#include "theseed/runtime/EntityDef.h"
#include "theseed/runtime/NetworkNode.h"
#include "theseed/runtime/TcpConnection.h"
#include "theseed/runtime/TickScheduler.h"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <utility>

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

// Test 1: Typed method call from base to cell — float + int32 args
static void testTypedMethodBaseToCell() {
    TEST("Typed method: base->cell (float + int32)");

    std::string dir = "test_method_dispatch_defs1";
    std::filesystem::create_directory(dir);
    writeFile(dir + "/Avatar.xml", R"(
<EntityDef name="Avatar">
    <Properties>
        <Property name="hp" type="Float32" defaultValue="100.0"/>
    </Properties>
    <Methods>
        <Method name="takeDamage" side="Cell">
            <Arg name="damage" type="Float32"/>
            <Arg name="sourceId" type="Int32"/>
        </Method>
    </Methods>
</EntityDef>
)");

    theseed::runtime::TcpConnection::globalInit();
    MiniCluster c(dir);
    c.connect();
    c.tickUntil([&] { return c.baseNode->hasPeer(2); });

    float receivedDamage = 0;
    std::int32_t receivedSourceId = 0;
    c.cellApp->runtime().setEntityFactoryHook([&](Entity& entity) {
        if (entity.entityType() != "Avatar") return;
        std::function<void(Entity&, float, std::int32_t)> handler =
            [&](Entity& e, float damage, std::int32_t sourceId) {
                receivedDamage = damage;
                receivedSourceId = sourceId;
                e.setProperty<float>("hp", *e.findProperty<float>("hp") - damage);
            };
        entity.bindTypedMethodHandler<float, std::int32_t>("takeDamage", std::move(handler));
    });

    auto* base = c.baseApp->createEntity("Avatar");
    auto id = base->id();

    c.baseApp->requestCreateCell(id, "Avatar", Vector3{0, 0, 0}, 2);
    c.tickUntil([&] {
        return base->cellEntityCall() && base->cellEntityCall()->isValid();
    });

    auto result = base->callCellWith<float, std::int32_t>("takeDamage", 25.5f, std::int32_t(42));
    bool ok = result == theseed::runtime::SendResult::Accepted;

    int ticks = c.tickUntil([&] { return receivedDamage > 0; }, 200);
    ok = ok && ticks > 0;
    ok = ok && receivedDamage == 25.5f;
    ok = ok && receivedSourceId == 42;

    std::filesystem::remove_all(dir);
    if (ok) PASS();
    else FAIL("typed method base->cell failed");
}

// Test 2: Typed method call from cell to base — string + int32 args
static void testTypedMethodCellToBase() {
    TEST("Typed method: cell->base (string + int32)");

    std::string dir = "test_method_dispatch_defs2";
    std::filesystem::create_directory(dir);
    writeFile(dir + "/Avatar.xml", R"(
<EntityDef name="Avatar">
    <Properties>
        <Property name="score" type="Int32" defaultValue="0"/>
    </Properties>
    <Methods>
        <Method name="addScore" side="Base">
            <Arg name="reason" type="String"/>
            <Arg name="points" type="Int32"/>
        </Method>
    </Methods>
</EntityDef>
)");

    theseed::runtime::TcpConnection::globalInit();
    MiniCluster c(dir);
    c.connect();
    c.tickUntil([&] { return c.baseNode->hasPeer(2); });

    std::string receivedReason;
    std::int32_t receivedPoints = 0;
    c.baseApp->runtime().setEntityFactoryHook([&](Entity& entity) {
        if (entity.entityType() != "Avatar") return;
        std::function<void(Entity&, std::string, std::int32_t)> handler =
            [&](Entity& e, std::string reason, std::int32_t points) {
                receivedReason = std::move(reason);
                receivedPoints = points;
                e.setProperty<std::int32_t>("score", *e.findProperty<std::int32_t>("score") + points);
            };
        entity.bindTypedMethodHandler<std::string, std::int32_t>("addScore", std::move(handler));
    });

    auto* base = c.baseApp->createEntity("Avatar");
    auto id = base->id();

    c.baseApp->requestCreateCell(id, "Avatar", Vector3{0, 0, 0}, 2);
    c.tickUntil([&] {
        return base->cellEntityCall() && base->cellEntityCall()->isValid();
    });

    auto* cell = c.cellApp->runtime().findEntity(id);
    bool ok = cell != nullptr;
    if (ok) {
        auto result = cell->callBaseWith<std::string, std::int32_t>("addScore", "kill_enemy", std::int32_t(100));
        ok = result == theseed::runtime::SendResult::Accepted;
    }

    c.tickUntil([&] { return receivedPoints > 0; }, 200);

    ok = ok && receivedReason == "kill_enemy";
    ok = ok && receivedPoints == 100;

    if (ok) {
        auto* score = base->findProperty<std::int32_t>("score");
        ok = score != nullptr && *score == 100;
    }

    std::filesystem::remove_all(dir);
    if (ok) PASS();
    else FAIL("typed method cell->base failed");
}

// Test 3: callDefMethod routes to correct side automatically
static void testCallDefMethod() {
    TEST("callDefMethod: auto-routes to correct side");

    std::string dir = "test_method_dispatch_defs3";
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
    MiniCluster c(dir);
    c.connect();
    c.tickUntil([&] { return c.baseNode->hasPeer(2); });

    float healAmount = 0;
    c.cellApp->runtime().setEntityFactoryHook([&](Entity& entity) {
        if (entity.entityType() != "Avatar") return;
        std::function<void(Entity&, float)> handler = [&](Entity&, float amount) {
            healAmount = amount;
        };
        entity.bindTypedMethodHandler<float>("heal", std::move(handler));
    });

    auto* base = c.baseApp->createEntity("Avatar");
    auto id = base->id();

    c.baseApp->requestCreateCell(id, "Avatar", Vector3{0, 0, 0}, 2);
    c.tickUntil([&] {
        return base->cellEntityCall() && base->cellEntityCall()->isValid();
    });

    auto result = base->callDefMethod<float>("heal", 50.0f);
    bool ok = result == theseed::runtime::SendResult::Accepted;

    c.tickUntil([&] { return healAmount > 0; }, 200);

    ok = ok && healAmount == 50.0f;

    std::filesystem::remove_all(dir);
    if (ok) PASS();
    else FAIL("callDefMethod routing failed");
}

// Test 4: Zero-arg method via bindMethodHandler
static void testTypedMethodNoArgs() {
    TEST("Zero-arg method dispatch via bindMethodHandler");

    std::string dir = "test_method_dispatch_defs4";
    std::filesystem::create_directory(dir);
    writeFile(dir + "/Avatar.xml", R"(
<EntityDef name="Avatar">
    <Properties>
        <Property name="hp" type="Float32" defaultValue="100.0"/>
    </Properties>
    <Methods>
        <Method name="respawn" side="Cell"/>
    </Methods>
</EntityDef>
)");

    theseed::runtime::TcpConnection::globalInit();
    MiniCluster c(dir);
    c.connect();
    c.tickUntil([&] { return c.baseNode->hasPeer(2); });

    bool respawnCalled = false;
    c.cellApp->runtime().setEntityFactoryHook([&](Entity& entity) {
        if (entity.entityType() != "Avatar") return;
        entity.bindMethodHandler("respawn", [&](Entity& e, std::span<const std::byte>) {
            respawnCalled = true;
            e.setProperty<float>("hp", 100.0f);
        });
    });

    auto* base = c.baseApp->createEntity("Avatar");
    auto id = base->id();

    c.baseApp->requestCreateCell(id, "Avatar", Vector3{0, 0, 0}, 2);
    c.tickUntil([&] {
        return base->cellEntityCall() && base->cellEntityCall()->isValid();
    });

    auto result = base->callCell("respawn");
    bool ok = result == theseed::runtime::SendResult::Accepted;

    c.tickUntil([&] { return respawnCalled; }, 200);

    ok = ok && respawnCalled;

    if (ok) {
        auto* cell = c.cellApp->runtime().findEntity(id);
        ok = cell != nullptr;
        if (ok) {
            auto* hp = cell->findProperty<float>("hp");
            ok = hp != nullptr && *hp == 100.0f;
        }
    }

    std::filesystem::remove_all(dir);
    if (ok) PASS();
    else FAIL("zero-arg method dispatch failed");
}

// Test 5: Unknown method returns false on dispatch
static void testUnknownMethodDispatch() {
    TEST("Unknown method dispatch returns false");

    std::string dir = "test_method_dispatch_defs5";
    std::filesystem::create_directory(dir);
    writeFile(dir + "/Avatar.xml", R"(
<EntityDef name="Avatar">
    <Properties>
        <Property name="hp" type="Float32" defaultValue="100.0"/>
    </Properties>
    <Methods>
        <Method name="heal" side="Base">
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
    bool ok = entity != nullptr;

    // Dispatch unregistered method should fail (no handler bound)
    if (ok) {
        ok = !entity->dispatchMethod("heal", {});
    }

    // Bind handler for Base-side method
    if (ok) {
        ok = entity->bindMethodHandler("heal", [](Entity&, std::span<const std::byte>) {});
        ok = ok && entity->hasMethodHandler("heal");
        ok = ok && !entity->hasMethodHandler("unknown");
    }

    // Dispatch of method not in EntityDef should fail
    if (ok) {
        ok = !entity->dispatchMethod("nonExistent", {});
    }

    std::filesystem::remove_all(dir);
    if (ok) PASS();
    else FAIL("unknown method dispatch test failed");
}

// Test 6: Multiple typed args — int32 + float + string
static void testMultipleTypedArgs() {
    TEST("Multiple typed args: int32 + float + string");

    std::string dir = "test_method_dispatch_defs6";
    std::filesystem::create_directory(dir);
    writeFile(dir + "/Avatar.xml", R"(
<EntityDef name="Avatar">
    <Properties>
        <Property name="hp" type="Float32" defaultValue="100.0"/>
    </Properties>
    <Methods>
        <Method name="applyEffect" side="Cell">
            <Arg name="effectId" type="Int32"/>
            <Arg name="duration" type="Float32"/>
            <Arg name="effectName" type="String"/>
        </Method>
    </Methods>
</EntityDef>
)");

    theseed::runtime::TcpConnection::globalInit();
    MiniCluster c(dir);
    c.connect();
    c.tickUntil([&] { return c.baseNode->hasPeer(2); });

    std::int32_t effectId = 0;
    float duration = 0;
    std::string effectName;
    c.cellApp->runtime().setEntityFactoryHook([&](Entity& entity) {
        if (entity.entityType() != "Avatar") return;
        std::function<void(Entity&, std::int32_t, float, std::string)> handler =
            [&](Entity&, std::int32_t id, float dur, std::string name) {
                effectId = id;
                duration = dur;
                effectName = std::move(name);
            };
        entity.bindTypedMethodHandler<std::int32_t, float, std::string>(
            "applyEffect", std::move(handler));
    });

    auto* base = c.baseApp->createEntity("Avatar");
    auto id = base->id();

    c.baseApp->requestCreateCell(id, "Avatar", Vector3{0, 0, 0}, 2);
    c.tickUntil([&] {
        return base->cellEntityCall() && base->cellEntityCall()->isValid();
    });

    auto result = base->callCellWith<std::int32_t, float, std::string>(
        "applyEffect", std::int32_t(7), 3.5f, "poison");
    bool ok = result == theseed::runtime::SendResult::Accepted;

    c.tickUntil([&] { return effectId != 0; }, 200);

    ok = ok && effectId == 7;
    ok = ok && duration == 3.5f;
    ok = ok && effectName == "poison";

    std::filesystem::remove_all(dir);
    if (ok) PASS();
    else FAIL("multiple typed args failed");
}

// Test 7: Method side enforcement — Base method can't bind on Cell entity
static void testMethodSideEnforcement() {
    TEST("Method side enforcement");

    std::string dir = "test_method_dispatch_defs7";
    std::filesystem::create_directory(dir);
    writeFile(dir + "/Avatar.xml", R"(
<EntityDef name="Avatar">
    <Properties>
        <Property name="hp" type="Float32" defaultValue="100.0"/>
    </Properties>
    <Methods>
        <Method name="saveData" side="Base"/>
        <Method name="update" side="Cell"/>
    </Methods>
</EntityDef>
)");

    theseed::runtime::TcpConnection::globalInit();
    auto baseNode = std::make_shared<NetworkNode>(NetworkNode::Config{.localComponent = 1, .listenPort = 0});
    auto store = std::make_shared<InMemoryEntityStore>();
    auto baseApp = std::make_unique<BaseApp>(BaseApp::Config{.entityDefPath = dir, .componentId = 1}, baseNode->hub(), store);
    baseApp->init();

    auto* entity = baseApp->createEntity("Avatar");
    bool ok = entity != nullptr;

    // Base entity should accept Base-side method
    if (ok) {
        ok = entity->bindMethodHandler("saveData", [](Entity&, std::span<const std::byte>) {});
    }

    // Base entity should reject Cell-side method
    if (ok) {
        ok = !entity->bindMethodHandler("update", [](Entity&, std::span<const std::byte>) {});
    }

    // Verify hasMethodHandler works correctly
    if (ok) {
        ok = entity->hasMethodHandler("saveData");
        ok = ok && !entity->hasMethodHandler("update");
    }

    std::filesystem::remove_all(dir);
    if (ok) PASS();
    else FAIL("method side enforcement failed");
}

int main() {
    std::cout << "Entity method dispatch tests:\n";

    testTypedMethodBaseToCell();
    testTypedMethodCellToBase();
    testCallDefMethod();
    testTypedMethodNoArgs();
    testUnknownMethodDispatch();
    testMultipleTypedArgs();
    testMethodSideEnforcement();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
