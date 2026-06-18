#include "theseed/core/BaseApp.h"
#include "theseed/core/CellApp.h"
#include "theseed/core/IEntityStore.h"
#include "theseed/login/ClientProtocol.h"
#include "theseed/login/ClientSession.h"
#include "theseed/login/LoginProtocol.h"
#include "theseed/runtime/Entity.h"
#include "theseed/runtime/EntityDef.h"
#include "theseed/runtime/InMemoryBytePipe.h"
#include "theseed/runtime/NetworkNode.h"
#include "theseed/runtime/TcpConnection.h"
#include "theseed/runtime/TickScheduler.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using theseed::core::BaseApp;
using theseed::core::CellApp;
using theseed::core::InMemoryEntityStore;
using theseed::login::ActionMsg;
using theseed::login::ClientMessageType;
using theseed::login::ClientProtocol;
using theseed::login::LoginProtocol;
using theseed::runtime::Entity;
using theseed::runtime::EntityId;
using theseed::runtime::InMemoryBytePipe;
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
    std::string dir = "test_client_action_defs";
    std::filesystem::create_directory(dir);
    writeFile(dir + "/Avatar.xml", R"(
<EntityDef name="Avatar">
    <Properties>
        <Property name="level" type="Int32"/>
    </Properties>
    <Methods>
        <Method name="moveTo" side="Cell"/>
        <Method name="attack" side="Cell"/>
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

// Test: ActionMsg encode/decode round-trip
static void testActionEncodeDecode() {
    TEST("ActionMsg: encode/decode round-trip");

    ActionMsg orig;
    orig.entityId = 42;
    orig.actionName = "moveTo";
    std::float_t x = 10.0f, z = 20.0f;
    orig.actionData.resize(sizeof(float) * 2);
    std::memcpy(orig.actionData.data(), &x, sizeof(float));
    std::memcpy(orig.actionData.data() + sizeof(float), &z, sizeof(float));

    auto framed = ClientProtocol::encodeAction(orig);

    ClientMessageType type;
    std::span<const std::byte> payload;
    bool parsed = LoginProtocol::parseFrame(
        std::span<const std::byte>(framed.data(), framed.size()), type, payload);

    bool ok = parsed && type == ClientMessageType::Action;

    ActionMsg decoded;
    ok = ok && ClientProtocol::decodeAction(payload, decoded);
    ok = ok && decoded.entityId == 42;
    ok = ok && decoded.actionName == "moveTo";
    ok = ok && decoded.actionData.size() == sizeof(float) * 2;
    if (ok) {
        float dx = 0, dz = 0;
        std::memcpy(&dx, decoded.actionData.data(), sizeof(float));
        std::memcpy(&dz, decoded.actionData.data() + sizeof(float), sizeof(float));
        ok = ok && std::abs(dx - 10.0f) < 0.01f;
        ok = ok && std::abs(dz - 20.0f) < 0.01f;
    }

    if (ok) PASS();
    else FAIL("name=" + decoded.actionName + " dataLen=" + std::to_string(decoded.actionData.size()));
}

// Test: Client action reaches CellApp entity via BaseApp forwarding
static void testClientActionForwardToCell() {
    TEST("Client action: forwarded from BaseApp to CellApp entity");

    auto dir = createDefDir();
    theseed::runtime::TcpConnection::globalInit();
    MiniCluster c(dir);
    c.connect();
    c.tickUntil([&] { return c.baseNode->hasPeer(2); });

    auto* baseEntity = c.baseApp->createEntity("Avatar");
    auto entityId = baseEntity->id();

    c.baseApp->requestCreateCell(entityId, "Avatar", Vector3{0, 0, 0}, 2);
    c.tickUntil([&] {
        return baseEntity->cellEntityCall() && baseEntity->cellEntityCall()->isValid();
    });

    auto* cellEntity = c.cellApp->runtime().findEntity(entityId);
    bool ok = cellEntity != nullptr;
    if (!ok) { std::filesystem::remove_all(dir); FAIL("cell entity missing"); return; }

    // Set up method handler on cell entity
    std::string receivedAction;
    std::vector<std::byte> receivedData;
    cellEntity->bindMethodHandler("moveTo", [&](Entity& e, std::span<const std::byte> payload) {
        receivedAction = "moveTo";
        receivedData.assign(payload.begin(), payload.end());
    });

    // Simulate client sending action via BaseApp
    ActionMsg msg;
    msg.entityId = entityId;
    msg.actionName = "moveTo";
    float tx = 5.0f, tz = 10.0f;
    msg.actionData.resize(sizeof(float) * 2);
    std::memcpy(msg.actionData.data(), &tx, sizeof(float));
    std::memcpy(msg.actionData.data() + sizeof(float), &tz, sizeof(float));

    // BaseApp processes the action and forwards to CellApp
    c.baseApp->handleClientAction(msg);

    // Wait for the action to propagate through the network
    int ticks = c.tickUntil([&] {
        return !receivedAction.empty();
    }, 200);

    ok = ok && ticks > 0;
    ok = ok && receivedAction == "moveTo";
    if (ok && receivedData.size() >= sizeof(float) * 2) {
        float rx = 0, rz = 0;
        std::memcpy(&rx, receivedData.data(), sizeof(float));
        std::memcpy(&rz, receivedData.data() + sizeof(float), sizeof(float));
        ok = ok && std::abs(rx - 5.0f) < 0.01f;
        ok = ok && std::abs(rz - 10.0f) < 0.01f;
    }

    std::filesystem::remove_all(dir);

    if (ok) PASS();
    else FAIL("action=" + receivedAction + " ticks=" + std::to_string(ticks));
}

// Test: Action with empty payload
static void testClientActionEmptyPayload() {
    TEST("Client action: empty payload reaches cell entity");

    auto dir = createDefDir();
    theseed::runtime::TcpConnection::globalInit();
    MiniCluster c(dir);
    c.connect();
    c.tickUntil([&] { return c.baseNode->hasPeer(2); });

    auto* baseEntity = c.baseApp->createEntity("Avatar");
    auto entityId = baseEntity->id();

    c.baseApp->requestCreateCell(entityId, "Avatar", Vector3{0, 0, 0}, 2);
    c.tickUntil([&] {
        return baseEntity->cellEntityCall() && baseEntity->cellEntityCall()->isValid();
    });

    auto* cellEntity = c.cellApp->runtime().findEntity(entityId);
    bool ok = cellEntity != nullptr;
    if (!ok) { std::filesystem::remove_all(dir); FAIL("cell entity missing"); return; }

    std::string receivedAction;
    cellEntity->bindMethodHandler("attack", [&](Entity& e, std::span<const std::byte> payload) {
        receivedAction = "attack";
    });

    ActionMsg msg;
    msg.entityId = entityId;
    msg.actionName = "attack";

    c.baseApp->handleClientAction(msg);

    int ticks = c.tickUntil([&] {
        return !receivedAction.empty();
    }, 200);

    ok = ok && ticks > 0;
    ok = ok && receivedAction == "attack";

    std::filesystem::remove_all(dir);

    if (ok) PASS();
    else FAIL("action=" + receivedAction + " ticks=" + std::to_string(ticks));
}

// Test: Action for non-existent entity is ignored
static void testClientActionInvalidEntity() {
    TEST("Client action: ignored for non-existent entity");

    auto dir = createDefDir();
    theseed::runtime::TcpConnection::globalInit();
    MiniCluster c(dir);
    c.connect();
    c.tickUntil([&] { return c.baseNode->hasPeer(2); });

    ActionMsg msg;
    msg.entityId = 99999;
    msg.actionName = "moveTo";

    // Should not crash
    c.baseApp->handleClientAction(msg);
    c.tickUntil([&] { return false; }, 5);

    std::filesystem::remove_all(dir);
    PASS();
}

int main() {
    std::cout << "Client action tests:\n";

    testActionEncodeDecode();
    testClientActionForwardToCell();
    testClientActionEmptyPayload();
    testClientActionInvalidEntity();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
