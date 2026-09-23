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

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <utility>
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
    std::string dir = "test_aoi_client_defs";
    std::filesystem::create_directory(dir);
    writeFile(dir + "/Avatar.xml", R"(
<EntityDef name="Avatar">
    <Properties>
        <Property name="level" type="Int32"/>
        <Property name="hp" type="Float32"/>
    </Properties>
    <Methods>
        <Method name="onDamage" side="Cell"/>
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

// Parse EntityEnter messages from raw byte buffer
struct ParsedEntityEnter {
    std::uint64_t entityId = 0;
    std::string entityType;
    float posX = 0, posY = 0, posZ = 0;
};

static std::vector<ParsedEntityEnter> extractEntityEnterMessages(
    const std::vector<std::byte>& buffer) {
    std::vector<ParsedEntityEnter> results;
    std::size_t offset = 0;

    while (offset + LoginProtocol::kHeaderSize <= buffer.size()) {
        auto payloadLen = static_cast<std::uint32_t>(buffer[offset])
                        | (static_cast<std::uint32_t>(buffer[offset + 1]) << 8)
                        | (static_cast<std::uint32_t>(buffer[offset + 2]) << 16)
                        | (static_cast<std::uint32_t>(buffer[offset + 3]) << 24);
        auto typeVal = static_cast<std::uint8_t>(buffer[offset + 4]);
        auto frameSize = LoginProtocol::kHeaderSize + payloadLen;

        if (offset + frameSize > buffer.size()) break;

        auto type = static_cast<ClientMessageType>(typeVal);
        if (type == ClientMessageType::EntityEnter) {
            auto payload = buffer.data() + offset + LoginProtocol::kHeaderSize;
            std::size_t poff = 0;

            ParsedEntityEnter msg;
            std::memcpy(&msg.entityId, payload + poff, 8);
            poff += 8;

            std::uint32_t typeLen = 0;
            std::memcpy(&typeLen, payload + poff, 4);
            poff += 4;
            msg.entityType.resize(typeLen);
            if (typeLen > 0) {
                std::memcpy(msg.entityType.data(), payload + poff, typeLen);
                poff += typeLen;
            }

            if (poff + sizeof(float) * 3 <= payloadLen) {
                std::memcpy(&msg.posX, payload + poff, sizeof(float)); poff += sizeof(float);
                std::memcpy(&msg.posY, payload + poff, sizeof(float)); poff += sizeof(float);
                std::memcpy(&msg.posZ, payload + poff, sizeof(float)); poff += sizeof(float);
            }

            results.push_back(std::move(msg));
        }

        offset += frameSize;
    }
    return results;
}

static std::vector<std::uint64_t> extractEntityLeaveMessages(
    const std::vector<std::byte>& buffer) {
    std::vector<std::uint64_t> results;
    std::size_t offset = 0;

    while (offset + LoginProtocol::kHeaderSize <= buffer.size()) {
        auto payloadLen = static_cast<std::uint32_t>(buffer[offset])
                        | (static_cast<std::uint32_t>(buffer[offset + 1]) << 8)
                        | (static_cast<std::uint32_t>(buffer[offset + 2]) << 16)
                        | (static_cast<std::uint32_t>(buffer[offset + 3]) << 24);
        auto typeVal = static_cast<std::uint8_t>(buffer[offset + 4]);
        auto frameSize = LoginProtocol::kHeaderSize + payloadLen;

        if (offset + frameSize > buffer.size()) break;

        auto type = static_cast<ClientMessageType>(typeVal);
        if (type == ClientMessageType::EntityLeave) {
            auto payload = buffer.data() + offset + LoginProtocol::kHeaderSize;
            std::uint64_t entityId = 0;
            if (payloadLen >= 8) {
                std::memcpy(&entityId, payload, 8);
            }
            results.push_back(entityId);
        }

        offset += frameSize;
    }
    return results;
}

struct CollectorPipe {
    std::shared_ptr<InMemoryBytePipe> clientEnd;
    std::shared_ptr<InMemoryBytePipe> serverEnd;
    std::vector<std::byte> received;

    static std::unique_ptr<CollectorPipe> create() {
        auto cp = std::make_unique<CollectorPipe>();
        auto [a, b] = InMemoryBytePipe::createPair();
        cp->serverEnd = a;
        cp->clientEnd = b;
        auto* raw = cp.get();
        cp->clientEnd->setOnReceived([raw](std::span<const std::byte> data) {
            raw->received.insert(raw->received.end(), data.begin(), data.end());
        });
        return cp;
    }

    void pump() {
        clientEnd->pump();
        serverEnd->pump();
    }
};

// Test: AOI enter sends EntityEnter to observer's client
static void testAoIEnterSendsEntityEnter() {
    TEST("AOI enter: sends EntityEnter to observer client");

    auto dir = createDefDir();
    theseed::runtime::TcpConnection::globalInit();  // NOLINT
    MiniCluster c(dir);
    c.connect();
    c.tickUntil([&] { return c.baseNode->hasPeer(2); });

    // Create two entities: observer and target
    auto* baseObserver = c.baseApp->createEntity("Avatar");
    auto* baseTarget = c.baseApp->createEntity("Avatar");
    auto observerId = baseObserver->id();
    auto targetId = baseTarget->id();

    c.baseApp->requestCreateCell(observerId, "Avatar", Vector3{0, 0, 0}, 2);
    c.baseApp->requestCreateCell(targetId, "Avatar", Vector3{5, 0, 5}, 2);
    c.tickUntil([&] {
        return baseObserver->cellEntityCall() && baseObserver->cellEntityCall()->isValid()
            && baseTarget->cellEntityCall() && baseTarget->cellEntityCall()->isValid();
    });

    auto* cellObserver = c.cellApp->runtime().findEntity(observerId);
    auto* cellTarget = c.cellApp->runtime().findEntity(targetId);
    bool ok = cellObserver != nullptr && cellTarget != nullptr;
    if (!ok) { std::filesystem::remove_all(dir); FAIL("cell entities missing"); return; }

    // Wire up a mock client for the observer BEFORE establishing witness
    auto pipe = CollectorPipe::create();
    auto session = std::make_unique<theseed::login::ClientSession>(pipe->serverEnd);
    auto* rawSession = session.get();

    c.baseApp->takeClientSession(std::move(session));
    c.baseApp->bindSessionToEntity(rawSession, observerId);

    // Set up witness on observer with range covering target
    c.cellApp->runtime().spaceRuntime().ensureWitness(*cellObserver, 10.0f);

    // Tick to trigger AOI scan, flush events, and propagate to client
    int ticks = c.tickUntil([&] {
        pipe->pump();
        auto enters = extractEntityEnterMessages(pipe->received);
        for (auto& e : enters) {
            if (e.entityId == targetId) return true;
        }
        return false;
    }, 200);

    pipe->pump();
    auto enters = extractEntityEnterMessages(pipe->received);
    bool found = false;
    for (auto& e : enters) {
        if (e.entityId == targetId) {
            found = true;
            ok = ok && e.entityType == "Avatar";
            // Position should be approximately (5, 0, 5)
            ok = ok && std::abs(e.posX - 5.0f) < 0.1f;
            ok = ok && std::abs(e.posZ - 5.0f) < 0.1f;
        }
    }
    ok = ok && found;

    /* TcpConnection::globalShutdown() */;
    std::filesystem::remove_all(dir);

    if (ok) PASS();
    else FAIL("ticks=" + std::to_string(ticks) + " enters=" + std::to_string(enters.size()));
}

// Test: AOI leave sends EntityLeave to observer's client
static void testAoILeaveSendsEntityLeave() {
    TEST("AOI leave: sends EntityLeave when target moves out of range");

    auto dir = createDefDir();
    theseed::runtime::TcpConnection::globalInit();  // NOLINT
    MiniCluster c(dir);
    c.connect();
    c.tickUntil([&] { return c.baseNode->hasPeer(2); });

    auto* baseObserver = c.baseApp->createEntity("Avatar");
    auto* baseTarget = c.baseApp->createEntity("Avatar");
    auto observerId = baseObserver->id();
    auto targetId = baseTarget->id();

    c.baseApp->requestCreateCell(observerId, "Avatar", Vector3{0, 0, 0}, 2);
    c.baseApp->requestCreateCell(targetId, "Avatar", Vector3{5, 0, 5}, 2);
    c.tickUntil([&] {
        return baseObserver->cellEntityCall() && baseObserver->cellEntityCall()->isValid()
            && baseTarget->cellEntityCall() && baseTarget->cellEntityCall()->isValid();
    });

    auto* cellObserver = c.cellApp->runtime().findEntity(observerId);
    auto* cellTarget = c.cellApp->runtime().findEntity(targetId);
    bool ok = cellObserver != nullptr && cellTarget != nullptr;
    if (!ok) { /* TcpConnection::globalShutdown() */; std::filesystem::remove_all(dir); FAIL("cell entities missing"); return; }

    // Set up witness on observer with range 10
    c.cellApp->runtime().spaceRuntime().ensureWitness(*cellObserver, 10.0f);

    // Wire up a mock client for the observer
    auto pipe = CollectorPipe::create();
    auto session = std::make_unique<theseed::login::ClientSession>(pipe->serverEnd);
    c.baseApp->bindSessionToEntity(session.get(), observerId);
    c.baseApp->takeClientSession(std::move(session));

    // Wait for AOI enter to settle
    c.tickUntil([&] {
        pipe->pump();
        auto enters = extractEntityEnterMessages(pipe->received);
        for (auto& e : enters) {
            if (e.entityId == targetId) return true;
        }
        return false;
    }, 200);

    // Move target far away (beyond 10 units)
    c.cellApp->runtime().spaceRuntime().space().updateEntityPosition(targetId, Vector3{100, 0, 100});

    // Tick to trigger AOI leave detection
    int ticks = c.tickUntil([&] {
        pipe->pump();
        auto leaves = extractEntityLeaveMessages(pipe->received);
        for (auto id : leaves) {
            if (id == targetId) return true;
        }
        return false;
    }, 200);

    pipe->pump();
    auto leaves = extractEntityLeaveMessages(pipe->received);
    bool found = false;
    for (auto id : leaves) {
        if (id == targetId) found = true;
    }
    ok = ok && found;

    /* TcpConnection::globalShutdown() */;
    std::filesystem::remove_all(dir);

    if (ok) PASS();
    else FAIL("ticks=" + std::to_string(ticks) + " leaves=" + std::to_string(leaves.size()));
}

// Test: no EntityEnter when entity is out of range
static void testAoINoEnterWhenOutOfRange() {
    TEST("AOI: no EntityEnter when target is out of range");

    auto dir = createDefDir();
    theseed::runtime::TcpConnection::globalInit();  // NOLINT
    MiniCluster c(dir);
    c.connect();
    c.tickUntil([&] { return c.baseNode->hasPeer(2); });

    auto* baseObserver = c.baseApp->createEntity("Avatar");
    auto* baseTarget = c.baseApp->createEntity("Avatar");
    auto observerId = baseObserver->id();
    auto targetId = baseTarget->id();

    // Observer at origin, target far away
    c.baseApp->requestCreateCell(observerId, "Avatar", Vector3{0, 0, 0}, 2);
    c.baseApp->requestCreateCell(targetId, "Avatar", Vector3{100, 0, 100}, 2);
    c.tickUntil([&] {
        return baseObserver->cellEntityCall() && baseObserver->cellEntityCall()->isValid()
            && baseTarget->cellEntityCall() && baseTarget->cellEntityCall()->isValid();
    });

    auto* cellObserver = c.cellApp->runtime().findEntity(observerId);
    bool ok = cellObserver != nullptr;
    if (!ok) { /* TcpConnection::globalShutdown() */; std::filesystem::remove_all(dir); FAIL("cell entities missing"); return; }

    // Witness with small range (5 units)
    c.cellApp->runtime().spaceRuntime().ensureWitness(*cellObserver, 5.0f);

    // Wire up a mock client
    auto pipe = CollectorPipe::create();
    auto session = std::make_unique<theseed::login::ClientSession>(pipe->serverEnd);
    c.baseApp->bindSessionToEntity(session.get(), observerId);
    c.baseApp->takeClientSession(std::move(session));

    // Tick several times - target should NOT appear
    for (int i = 0; i < 30; ++i) {
        c.scheduler.runOnce();
        pipe->pump();
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }

    auto enters = extractEntityEnterMessages(pipe->received);
    bool foundOutOfRange = false;
    for (auto& e : enters) {
        if (e.entityId == targetId) foundOutOfRange = true;
    }
    ok = ok && !foundOutOfRange;

    /* TcpConnection::globalShutdown() */;
    std::filesystem::remove_all(dir);

    if (ok) PASS();
    else FAIL("enters=" + std::to_string(enters.size()));
}

int main() {
    std::cout << "AOI client notification tests:\n";

    testAoIEnterSendsEntityEnter();
    testAoILeaveSendsEntityLeave();
    testAoINoEnterWhenOutOfRange();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
