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
    std::string dir = "test_client_disconnect_defs";
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

// Test: Disconnecting session removes base entity
static void testDisconnectDestroysBaseEntity() {
    TEST("Disconnect: base entity removed on disconnect");

    auto dir = createDefDir();
    theseed::runtime::TcpConnection::globalInit();
    MiniCluster c(dir);
    c.connect();
    c.tickUntil([&] { return c.baseNode->hasPeer(2); });

    auto* baseEntity = c.baseApp->createEntity("Avatar");
    auto entityId = baseEntity->id();

    auto [clientPipe, serverPipe] = InMemoryBytePipe::createPair();
    auto serverPipeRef = serverPipe;  // keep ref to close from server side
    auto session = std::make_unique<theseed::login::ClientSession>(std::move(serverPipe));
    auto* rawSession = session.get();

    c.baseApp->bindSessionToEntity(rawSession, entityId);
    c.baseApp->takeClientSession(std::move(session));

    bool ok = c.baseApp->findEntity(entityId) != nullptr;

    // Disconnect: close server-side pipe to simulate network drop
    serverPipeRef->close();
    int ticks = c.tickUntil([&] {
        return c.baseApp->findEntity(entityId) == nullptr;
    }, 50);

    ok = ok && ticks > 0;
    ok = ok && c.baseApp->findSessionByEntity(entityId) == nullptr;

    std::filesystem::remove_all(dir);

    if (ok) PASS();
    else FAIL("entity still exists or session mapping remains, ticks=" + std::to_string(ticks));
}

// Test: Disconnecting session destroys cell entity too
static void testDisconnectDestroysCellEntity() {
    TEST("Disconnect: cell entity destroyed on disconnect");

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

    auto [clientPipe, serverPipe] = InMemoryBytePipe::createPair();
    auto serverPipeRef = serverPipe;
    auto session = std::make_unique<theseed::login::ClientSession>(std::move(serverPipe));
    auto* rawSession = session.get();

    c.baseApp->bindSessionToEntity(rawSession, entityId);
    c.baseApp->takeClientSession(std::move(session));

    // Disconnect and wait for cell entity destruction
    serverPipeRef->close();
    int ticks = c.tickUntil([&] {
        return c.cellApp->runtime().findEntity(entityId) == nullptr
               && c.baseApp->findEntity(entityId) == nullptr;
    }, 100);

    ok = ok && ticks > 0;

    std::filesystem::remove_all(dir);

    if (ok) PASS();
    else FAIL("cell ticks=" + std::to_string(ticks));
}

// Test: Multiple sessions, only disconnected one is cleaned up
static void testDisconnectOnlyOneSession() {
    TEST("Disconnect: only disconnected session cleaned up");

    auto dir = createDefDir();
    theseed::runtime::TcpConnection::globalInit();
    MiniCluster c(dir);
    c.connect();
    c.tickUntil([&] { return c.baseNode->hasPeer(2); });

    auto* entity1 = c.baseApp->createEntity("Avatar");
    auto* entity2 = c.baseApp->createEntity("Avatar");
    auto id1 = entity1->id();
    auto id2 = entity2->id();

    auto [pipe1, server1] = InMemoryBytePipe::createPair();
    auto server1Ref = server1;
    auto session1 = std::make_unique<theseed::login::ClientSession>(std::move(server1));
    auto* raw1 = session1.get();
    c.baseApp->bindSessionToEntity(raw1, id1);
    c.baseApp->takeClientSession(std::move(session1));

    auto [pipe2, server2] = InMemoryBytePipe::createPair();
    auto session2 = std::make_unique<theseed::login::ClientSession>(std::move(server2));
    auto* raw2 = session2.get();
    c.baseApp->bindSessionToEntity(raw2, id2);
    c.baseApp->takeClientSession(std::move(session2));

    // Disconnect session1 only
    server1Ref->close();
    int ticks = c.tickUntil([&] {
        return c.baseApp->findEntity(id1) == nullptr;
    }, 50);

    bool ok = ticks > 0;
    ok = ok && c.baseApp->findEntity(id2) != nullptr;
    ok = ok && c.baseApp->findSessionByEntity(id2) == raw2;

    std::filesystem::remove_all(dir);

    if (ok) PASS();
    else FAIL("wrong cleanup, ticks=" + std::to_string(ticks));
}

// Test: Disconnect with no session binding (no-op)
static void testDisconnectNoBinding() {
    TEST("Disconnect: unbound session cleanup is safe");

    auto dir = createDefDir();
    theseed::runtime::TcpConnection::globalInit();
    MiniCluster c(dir);
    c.connect();
    c.tickUntil([&] { return c.baseNode->hasPeer(2); });

    auto* entity = c.baseApp->createEntity("Avatar");
    auto entityId = entity->id();

    auto [pipe, serverPipe] = InMemoryBytePipe::createPair();
    auto serverPipeRef = serverPipe;
    auto session = std::make_unique<theseed::login::ClientSession>(std::move(serverPipe));

    c.baseApp->takeClientSession(std::move(session));

    serverPipeRef->close();
    // 注意：不要在谓词里解引用 rawSession —— session 一旦被 cleanupClients
    // 回收，裸指针立即悬垂（MSVC 上恰好读到旧值，libstdc++ 上直接段错误）。
    // session 被析构时会经由共享的 pipe_ 调 close()，因此观察 serverPipeRef。
    c.tickUntil([&] { return !serverPipeRef->isConnected(); }, 50);
    c.scheduler.runOnce();

    // Entity still exists (not bound to session)
    bool ok = c.baseApp->findEntity(entityId) != nullptr;

    std::filesystem::remove_all(dir);

    if (ok) PASS();
    else FAIL("entity should still exist");
}

int main() {
    std::cout << "Client disconnect tests:\n";

    testDisconnectDestroysBaseEntity();
    testDisconnectDestroysCellEntity();
    testDisconnectOnlyOneSession();
    testDisconnectNoBinding();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
