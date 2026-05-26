#include "theseed/core/BaseApp.h"
#include "theseed/core/CellApp.h"
#include "theseed/core/IEntityStore.h"
#include "theseed/runtime/Entity.h"
#include "theseed/runtime/EntityDef.h"
#include "theseed/runtime/RuntimeTransport.h"
#include "theseed/runtime/RuntimeTypes.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

using theseed::core::BaseApp;
using theseed::core::CellApp;
using theseed::core::InMemoryEntityStore;
using theseed::runtime::Entity;
using theseed::runtime::EntityId;
using theseed::runtime::EntitySide;
using theseed::runtime::InMemoryRuntimeTransport;
using theseed::runtime::MethodSide;
using theseed::runtime::PropertyType;
using theseed::runtime::RuntimeInvocation;
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
    std::string dir = "test_integration_defs";
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

static void testBaseToCellCall() {
    TEST("base -> cell entity call");

    auto dir = createDefDir();
    auto transport = std::make_shared<InMemoryRuntimeTransport>();

    BaseApp::Config baseConfig;
    baseConfig.entityDefPath = dir;
    baseConfig.componentId = 1;

    auto store = std::make_shared<InMemoryEntityStore>();
    BaseApp baseApp(baseConfig, transport, store);
    baseApp.init();

    CellApp::Config cellConfig;
    cellConfig.entityDefPath = dir;
    cellConfig.componentId = 2;

    CellApp cellApp(cellConfig, transport);
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

    std::int32_t damage = 25;
    std::vector<std::byte> damagePayload(sizeof(damage));
    std::memcpy(damagePayload.data(), &damage, sizeof(damage));

    baseEntity->cellEntityCall()->call(*transport, "onDamage", damagePayload);

    cellApp.runtime().pumpInbound();

    bool ok = receivedDamage == 25;

    std::filesystem::remove_all(dir);
    if (ok) PASS(); else FAIL("base->cell call failed, got " + std::to_string(receivedDamage));
}

static void testCellToBaseCall() {
    TEST("cell -> base entity call");

    auto dir = createDefDir();
    auto transport = std::make_shared<InMemoryRuntimeTransport>();

    BaseApp::Config baseConfig;
    baseConfig.entityDefPath = dir;
    baseConfig.componentId = 1;

    auto store = std::make_shared<InMemoryEntityStore>();
    BaseApp baseApp(baseConfig, transport, store);
    baseApp.init();

    CellApp::Config cellConfig;
    cellConfig.entityDefPath = dir;
    cellConfig.componentId = 2;

    CellApp cellApp(cellConfig, transport);
    cellApp.init();

    auto* baseEntity = baseApp.createEntity("Avatar");
    auto entityId = baseEntity->id();
    auto* cellEntity = cellApp.createEntity("Avatar", Vector3{0, 0, 0}, entityId);

    baseEntity->bindCellEntityCall(2);
    cellEntity->bindBaseEntityCall(1);

    float receivedHeal = 0;
    baseEntity->bindMethodHandler("onHeal", [&receivedHeal](Entity& e, std::span<const std::byte> payload) {
        static_cast<void>(e);
        if (payload.size() >= sizeof(float)) {
            std::memcpy(&receivedHeal, payload.data(), sizeof(float));
        }
    });

    float heal = 50.5f;
    std::vector<std::byte> healPayload(sizeof(heal));
    std::memcpy(healPayload.data(), &heal, sizeof(heal));

    cellEntity->baseEntityCall()->call(*transport, "onHeal", healPayload);

    baseApp.runtime().pumpInbound();

    bool ok = receivedHeal == 50.5f;

    std::filesystem::remove_all(dir);
    if (ok) PASS(); else FAIL("cell->base call failed");
}

static void testDualBodyPropertySync() {
    TEST("dual body: base saves, cell has spatial properties");

    auto dir = createDefDir();
    auto transport = std::make_shared<InMemoryRuntimeTransport>();

    BaseApp::Config baseConfig;
    baseConfig.entityDefPath = dir;
    baseConfig.componentId = 1;

    auto store = std::make_shared<InMemoryEntityStore>();
    BaseApp baseApp(baseConfig, transport, store);
    baseApp.init();

    CellApp::Config cellConfig;
    cellConfig.entityDefPath = dir;
    cellConfig.componentId = 2;

    CellApp cellApp(cellConfig, transport);
    cellApp.init();

    auto* baseEntity = baseApp.createEntity("Avatar");
    baseEntity->setProperty<std::int32_t>(0, 10);
    baseEntity->setProperty<float>(1, 100.0f);

    baseApp.runtime().saveEntity(baseEntity->id());

    auto* cellEntity = cellApp.createEntity("Avatar", Vector3{5.0f, 0.0f, 10.0f});

    bool ok = baseEntity->getProperty<std::int32_t>(0) == 10;
    ok = ok && cellEntity->entityType() == "Avatar";
    ok = ok && cellEntity->side() == EntitySide::Cell;
    ok = ok && store->exists(baseEntity->id());

    std::filesystem::remove_all(dir);
    if (ok) PASS(); else FAIL("dual body sync failed");
}

static void testBidirectionalPingPong() {
    TEST("bidirectional: base calls cell, cell calls back");

    auto dir = createDefDir();
    auto transport = std::make_shared<InMemoryRuntimeTransport>();

    BaseApp::Config baseConfig;
    baseConfig.entityDefPath = dir;
    baseConfig.componentId = 1;

    auto store = std::make_shared<InMemoryEntityStore>();
    BaseApp baseApp(baseConfig, transport, store);
    baseApp.init();

    CellApp::Config cellConfig;
    cellConfig.entityDefPath = dir;
    cellConfig.componentId = 2;

    CellApp cellApp(cellConfig, transport);
    cellApp.init();

    auto* baseEntity = baseApp.createEntity("Avatar");
    auto entityId = baseEntity->id();
    auto* cellEntity = cellApp.createEntity("Avatar", Vector3{0, 0, 0}, entityId);

    baseEntity->bindCellEntityCall(2);
    cellEntity->bindBaseEntityCall(1);

    int callCount = 0;
    cellEntity->bindMethodHandler("onDamage", [&callCount, &transport](Entity& e, std::span<const std::byte> payload) {
        static_cast<void>(payload);
        ++callCount;
        e.baseEntityCall()->call(*transport, "onHeal", {});
    });

    baseEntity->bindMethodHandler("onHeal", [&callCount](Entity& e, std::span<const std::byte> payload) {
        static_cast<void>(e);
        static_cast<void>(payload);
        ++callCount;
    });

    baseEntity->cellEntityCall()->call(*transport, "onDamage", {});

    cellApp.runtime().pumpInbound();
    baseApp.runtime().pumpInbound();

    bool ok = callCount == 2;

    std::filesystem::remove_all(dir);
    if (ok) PASS(); else FAIL("ping-pong failed, count=" + std::to_string(callCount));
}

int main() {
    std::cout << "Integration tests:\n";

    testBaseToCellCall();
    testCellToBaseCall();
    testDualBodyPropertySync();
    testBidirectionalPingPong();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
