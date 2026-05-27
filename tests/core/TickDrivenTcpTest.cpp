#include "theseed/core/BaseApp.h"
#include "theseed/core/CellApp.h"
#include "theseed/core/IEntityStore.h"
#include "theseed/runtime/Entity.h"
#include "theseed/runtime/EntityDef.h"
#include "theseed/runtime/NetworkNode.h"
#include "theseed/runtime/RuntimeTypes.h"
#include "theseed/runtime/TcpConnection.h"
#include "theseed/runtime/TickScheduler.h"
#include "theseed/runtime/TcpListener.h"
#include "theseed/runtime/TransportHub.h"
#include "theseed/runtime/NetworkTransport.h"

#include <chrono>
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
using theseed::runtime::Entity;
using theseed::runtime::EntityId;
using theseed::runtime::EntitySide;
using theseed::runtime::IRuntimeTransport;
using theseed::runtime::NetworkNode;
using theseed::runtime::NetworkTransport;
using theseed::runtime::RuntimeInvocation;
using theseed::runtime::SendResult;
using theseed::runtime::TcpConnection;
using theseed::runtime::TcpListener;
using theseed::runtime::TickScheduler;
using theseed::runtime::TransportHub;
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
    std::string dir = "test_tick_driven_defs";
    std::filesystem::create_directory(dir);

    writeFile(dir + "/Avatar.xml", R"(
<EntityDef name="Avatar">
    <Properties>
        <Property name="level" type="Int32"/>
        <Property name="hp" type="Float32"/>
    </Properties>
    <Methods>
        <Method name="onDamage" side="Cell"/>
        <Method name="onHeal" side="Base"/>
    </Methods>
</EntityDef>
)");

    return dir;
}

// Manually wire TCP connections + hubs (NetworkNode doesn't track accepted transports
// for tick-driven inbound on the server side yet, so we use lower-level setup).
struct TickTcpStack {
    TcpListener listener;
    std::shared_ptr<TcpConnection> clientConn;
    std::shared_ptr<TcpConnection> serverConn;
    std::shared_ptr<NetworkTransport> clientTransport;
    std::shared_ptr<NetworkTransport> serverTransport;
    std::shared_ptr<TransportHub> baseHub;
    std::shared_ptr<TransportHub> cellHub;

    TickTcpStack() {
        listener.listen("127.0.0.1", 0);
        auto port = listener.localPort();

        clientConn = TcpConnection::create();
        clientConn->connect("127.0.0.1", port);
        std::this_thread::sleep_for(std::chrono::milliseconds{50});

        serverConn = listener.accept();
        std::this_thread::sleep_for(std::chrono::milliseconds{50});

        clientTransport = std::make_shared<NetworkTransport>(clientConn);
        serverTransport = std::make_shared<NetworkTransport>(serverConn);

        baseHub = std::make_shared<TransportHub>(1);
        cellHub = std::make_shared<TransportHub>(2);

        baseHub->connectPeer(2, clientTransport);
        cellHub->connectPeer(1, serverTransport);
    }

    void close() {
        listener.close();
    }
};

// Tickable that pumps both transports each tick
class TransportPump final : public theseed::runtime::ITickable {
public:
    TransportPump(std::shared_ptr<NetworkTransport> a,
                  std::shared_ptr<NetworkTransport> b)
        : a_(std::move(a)), b_(std::move(b)) {}

    void tick(theseed::runtime::TickContext& context) override {
        static_cast<void>(context);
        a_->tick();
        b_->tick();
    }

private:
    std::shared_ptr<NetworkTransport> a_;
    std::shared_ptr<NetworkTransport> b_;
};

// Test: purely tick-driven base -> cell call
static void testTickDrivenBaseToCell() {
    TEST("tick-driven: base -> cell entity call");

    auto dir = createDefDir();
    TcpConnection::globalInit();
    TickTcpStack stack;

    auto store = std::make_shared<InMemoryEntityStore>();

    BaseApp::Config baseConfig;
    baseConfig.entityDefPath = dir;
    baseConfig.componentId = 1;
    BaseApp baseApp(baseConfig, stack.baseHub, store);
    baseApp.init();

    CellApp::Config cellConfig;
    cellConfig.entityDefPath = dir;
    cellConfig.componentId = 2;
    CellApp cellApp(cellConfig, stack.cellHub);
    cellApp.init();

    auto* baseEntity = baseApp.createEntity("Avatar");
    auto entityId = baseEntity->id();
    auto* cellEntity = cellApp.createEntity("Avatar", Vector3{0, 0, 0}, entityId);

    baseEntity->bindCellEntityCall(2);
    cellEntity->bindBaseEntityCall(1);

    std::int32_t receivedDamage = 0;
    cellEntity->bindMethodHandler("onDamage", [&receivedDamage](Entity& e, std::span<const std::byte> payload) {
        static_cast<void>(e);
        if (payload.size() >= sizeof(std::int32_t)) {
            std::memcpy(&receivedDamage, payload.data(), sizeof(std::int32_t));
        }
    });

    // Setup tick scheduler
    TickScheduler scheduler(std::chrono::milliseconds{16});
    baseApp.attach(scheduler);
    cellApp.attach(scheduler);

    auto pump = std::make_shared<TransportPump>(stack.clientTransport, stack.serverTransport);
    scheduler.registerTickable(theseed::runtime::TickPhase::Network, *pump);

    // Send call
    std::int32_t damage = 25;
    std::vector<std::byte> payload(sizeof(damage));
    std::memcpy(payload.data(), &damage, sizeof(damage));
    baseEntity->cellEntityCall()->call(*stack.baseHub, "onDamage", payload);

    // Run ticks until received or timeout
    for (int i = 0; i < 100 && receivedDamage == 0; ++i) {
        scheduler.runOnce();
        if (receivedDamage == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }
    }

    bool ok = receivedDamage == 25;

    scheduler.unregisterTickable(theseed::runtime::TickPhase::Network, *pump);
    baseApp.detach(scheduler);
    cellApp.detach(scheduler);
    stack.close();
    TcpConnection::globalShutdown();
    std::filesystem::remove_all(dir);

    if (ok) PASS();
    else FAIL("receivedDamage=" + std::to_string(receivedDamage));
}

// Test: tick-driven bidirectional ping-pong
static void testTickDrivenPingPong() {
    TEST("tick-driven: bidirectional ping-pong");

    auto dir = createDefDir();
    TcpConnection::globalInit();
    TickTcpStack stack;

    auto store = std::make_shared<InMemoryEntityStore>();

    BaseApp::Config baseConfig;
    baseConfig.entityDefPath = dir;
    baseConfig.componentId = 1;
    BaseApp baseApp(baseConfig, stack.baseHub, store);
    baseApp.init();

    CellApp::Config cellConfig;
    cellConfig.entityDefPath = dir;
    cellConfig.componentId = 2;
    CellApp cellApp(cellConfig, stack.cellHub);
    cellApp.init();

    auto* baseEntity = baseApp.createEntity("Avatar");
    auto entityId = baseEntity->id();
    auto* cellEntity = cellApp.createEntity("Avatar", Vector3{0, 0, 0}, entityId);

    baseEntity->bindCellEntityCall(2);
    cellEntity->bindBaseEntityCall(1);

    int callCount = 0;
    cellEntity->bindMethodHandler("onDamage", [&callCount, &stack](Entity& e, std::span<const std::byte> payload) {
        static_cast<void>(payload);
        ++callCount;
        e.baseEntityCall()->call(*stack.cellHub, "onHeal", {});
    });

    baseEntity->bindMethodHandler("onHeal", [&callCount](Entity& e, std::span<const std::byte> payload) {
        static_cast<void>(e);
        static_cast<void>(payload);
        ++callCount;
    });

    // Setup tick scheduler
    TickScheduler scheduler(std::chrono::milliseconds{16});
    baseApp.attach(scheduler);
    cellApp.attach(scheduler);

    auto pump = std::make_shared<TransportPump>(stack.clientTransport, stack.serverTransport);
    scheduler.registerTickable(theseed::runtime::TickPhase::Network, *pump);

    // Trigger
    baseEntity->cellEntityCall()->call(*stack.baseHub, "onDamage", {});

    // Run ticks until both calls complete or timeout
    for (int i = 0; i < 200 && callCount < 2; ++i) {
        scheduler.runOnce();
        if (callCount < 2) {
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }
    }

    bool ok = callCount == 2;

    scheduler.unregisterTickable(theseed::runtime::TickPhase::Network, *pump);
    baseApp.detach(scheduler);
    cellApp.detach(scheduler);
    stack.close();
    TcpConnection::globalShutdown();
    std::filesystem::remove_all(dir);

    if (ok) PASS();
    else FAIL("callCount=" + std::to_string(callCount));
}

// Test: tick-driven with multiple sequential calls
static void testTickDrivenMultipleCalls() {
    TEST("tick-driven: multiple sequential calls on same connection");

    auto dir = createDefDir();
    TcpConnection::globalInit();
    TickTcpStack stack;

    auto store = std::make_shared<InMemoryEntityStore>();

    BaseApp::Config baseConfig;
    baseConfig.entityDefPath = dir;
    baseConfig.componentId = 1;
    BaseApp baseApp(baseConfig, stack.baseHub, store);
    baseApp.init();

    CellApp::Config cellConfig;
    cellConfig.entityDefPath = dir;
    cellConfig.componentId = 2;
    CellApp cellApp(cellConfig, stack.cellHub);
    cellApp.init();

    auto* baseEntity = baseApp.createEntity("Avatar");
    auto entityId = baseEntity->id();
    auto* cellEntity = cellApp.createEntity("Avatar", Vector3{0, 0, 0}, entityId);

    baseEntity->bindCellEntityCall(2);
    cellEntity->bindBaseEntityCall(1);

    std::int32_t totalDamage = 0;
    cellEntity->bindMethodHandler("onDamage", [&totalDamage](Entity& e, std::span<const std::byte> payload) {
        static_cast<void>(e);
        if (payload.size() >= sizeof(std::int32_t)) {
            std::int32_t dmg = 0;
            std::memcpy(&dmg, payload.data(), sizeof(std::int32_t));
            totalDamage += dmg;
        }
    });

    // Setup tick scheduler
    TickScheduler scheduler(std::chrono::milliseconds{16});
    baseApp.attach(scheduler);
    cellApp.attach(scheduler);

    auto pump = std::make_shared<TransportPump>(stack.clientTransport, stack.serverTransport);
    scheduler.registerTickable(theseed::runtime::TickPhase::Network, *pump);

    // Send 5 damage calls
    for (std::int32_t i = 1; i <= 5; ++i) {
        std::vector<std::byte> payload(sizeof(i));
        std::memcpy(payload.data(), &i, sizeof(i));
        baseEntity->cellEntityCall()->call(*stack.baseHub, "onDamage", payload);
    }

    // Run ticks
    for (int i = 0; i < 200 && totalDamage < 15; ++i) {
        scheduler.runOnce();
        if (totalDamage < 15) {
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }
    }

    bool ok = totalDamage == 15;  // 1+2+3+4+5

    scheduler.unregisterTickable(theseed::runtime::TickPhase::Network, *pump);
    baseApp.detach(scheduler);
    cellApp.detach(scheduler);
    stack.close();
    TcpConnection::globalShutdown();
    std::filesystem::remove_all(dir);

    if (ok) PASS();
    else FAIL("totalDamage=" + std::to_string(totalDamage));
}

int main() {
    std::cout << "Tick-driven TCP tests:\n";

    testTickDrivenBaseToCell();
    testTickDrivenPingPong();
    testTickDrivenMultipleCalls();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
