#include "theseed/core/CellApp.h"
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

using theseed::core::CellApp;
using theseed::runtime::Entity;
using theseed::runtime::EntitySide;
using theseed::runtime::EntityState;
using theseed::runtime::InMemoryRuntimeTransport;
using theseed::runtime::MethodSide;
using theseed::runtime::PropertyType;
using theseed::runtime::SpaceId;
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
    std::string dir = "test_cellapp_defs";
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

    writeFile(dir + "/Monster.xml", R"(
<EntityDef name="Monster">
    <Properties>
        <Property name="hp" type="Int32"/>
    </Properties>
</EntityDef>
)");

    return dir;
}

static void testInit() {
    TEST("init loads definitions");

    auto dir = createDefDir();
    auto transport = std::make_shared<InMemoryRuntimeTransport>();

    CellApp::Config config;
    config.entityDefPath = dir;
    config.componentId = 2;

    CellApp app(config, transport);
    bool ok = app.init();
    ok = ok && app.registry().defCount() == 2;
    ok = ok && app.registry().hasDef("Avatar");
    ok = ok && app.registry().hasDef("Monster");

    std::filesystem::remove_all(dir);
    if (ok) PASS(); else FAIL("init failed");
}

static void testCreateEntity() {
    TEST("create entity with position");

    auto dir = createDefDir();
    auto transport = std::make_shared<InMemoryRuntimeTransport>();

    CellApp::Config config;
    config.entityDefPath = dir;
    config.componentId = 2;

    CellApp app(config, transport);
    app.init();

    auto* entity = app.createEntity("Avatar", Vector3{10.0f, 0.0f, 20.0f});
    bool ok = entity != nullptr;
    ok = ok && entity->entityType() == "Avatar";
    ok = ok && entity->side() == EntitySide::Cell;
    ok = ok && entity->state() == EntityState::Active;

    if (ok) PASS(); else FAIL("create entity failed");
    std::filesystem::remove_all(dir);
}

static void testFindAndDestroy() {
    TEST("find and destroy entity");

    auto dir = createDefDir();
    auto transport = std::make_shared<InMemoryRuntimeTransport>();

    CellApp::Config config;
    config.entityDefPath = dir;
    config.componentId = 2;

    CellApp app(config, transport);
    app.init();

    auto* e1 = app.createEntity("Avatar", Vector3{0, 0, 0});
    auto* e2 = app.createEntity("Monster", Vector3{10, 0, 0});
    auto id1 = e1->id();
    auto id2 = e2->id();

    bool ok = app.findEntity(id1) == e1;
    ok = ok && app.findEntity(id2) == e2;
    ok = ok && app.findEntity(99999) == nullptr;

    app.destroyEntity(id1);
    ok = ok && app.findEntity(id1) == nullptr;
    ok = ok && app.findEntity(id2) != nullptr;

    if (ok) PASS(); else FAIL("find/destroy failed");
    std::filesystem::remove_all(dir);
}

static void testSetProperty() {
    TEST("set and get property on cell entity");

    auto dir = createDefDir();
    auto transport = std::make_shared<InMemoryRuntimeTransport>();

    CellApp::Config config;
    config.entityDefPath = dir;
    config.componentId = 2;

    CellApp app(config, transport);
    app.init();

    auto* entity = app.createEntity("Avatar", Vector3{0, 0, 0});
    entity->setProperty<std::int32_t>(0, 42);
    entity->setProperty<float>(1, 99.5f);

    bool ok = entity->getProperty<std::int32_t>(0) == 42;
    ok = ok && entity->getProperty<float>(1) == 99.5f;

    if (ok) PASS(); else FAIL("property get/set failed");
    std::filesystem::remove_all(dir);
}

static void testMultipleEntities() {
    TEST("create multiple entities in space");

    auto dir = createDefDir();
    auto transport = std::make_shared<InMemoryRuntimeTransport>();

    CellApp::Config config;
    config.entityDefPath = dir;
    config.componentId = 2;

    CellApp app(config, transport);
    app.init();

    auto* e1 = app.createEntity("Avatar", Vector3{0, 0, 0});
    auto* e2 = app.createEntity("Monster", Vector3{10, 0, 0});
    auto* e3 = app.createEntity("Monster", Vector3{20, 0, 0});

    bool ok = e1 != nullptr && e2 != nullptr && e3 != nullptr;
    ok = ok && e1->id() != e2->id() && e2->id() != e3->id();

    if (ok) PASS(); else FAIL("multiple entities failed");
    std::filesystem::remove_all(dir);
}

static void testOnDestroyFires() {
    TEST("destroyEntity triggers onDestroy and cleans up");

    auto dir = createDefDir();
    auto transport = std::make_shared<InMemoryRuntimeTransport>();

    CellApp::Config config;
    config.entityDefPath = dir;
    config.componentId = 2;

    CellApp app(config, transport);
    app.init();

    auto* entity = app.createEntity("Avatar", Vector3{0, 0, 0});
    auto id = entity->id();

    bool destroyed = false;
    entity->setOnDestroy([&destroyed](Entity&) {
        destroyed = true;
    });

    // Verify entity state before destroy
    bool ok = entity->state() == EntityState::Active;

    app.destroyEntity(id);
    ok = ok && destroyed;
    ok = ok && app.findEntity(id) == nullptr;

    std::filesystem::remove_all(dir);
    if (ok) PASS(); else FAIL("destroy cleanup failed");
}

static void testOnEnterLeaveSpace() {
    TEST("entity receives enterSpace/leaveSpace callbacks");

    auto dir = createDefDir();
    auto transport = std::make_shared<InMemoryRuntimeTransport>();

    CellApp::Config config;
    config.entityDefPath = dir;
    config.componentId = 2;

    CellApp app(config, transport);
    app.init();

    bool entered = false;
    bool left = false;

    // Set callbacks after creation, test manual space events
    auto* entity = app.createEntity("Avatar", Vector3{5, 0, 0});

    // notifyEnterSpace is called during addEntity (in createEntity flow)
    // Callbacks set after creation won't catch the initial enterSpace.
    // Instead, verify by manually calling notifyEnterSpace/notifyLeaveSpace
    entity->setOnEnterSpace([&entered](Entity&, SpaceId) {
        entered = true;
    });
    entity->setOnLeaveSpace([&left](Entity&, SpaceId) {
        left = true;
    });

    // Manually trigger to verify callback wiring
    entity->notifyEnterSpace(1);
    entity->notifyLeaveSpace(1);

    bool ok = entered && left;

    std::filesystem::remove_all(dir);
    if (ok) PASS(); else FAIL("entered=" + std::string(entered ? "T" : "F")
                               + " left=" + std::string(left ? "T" : "F"));
}

int main() {
    std::cout << "CellApp tests:\n";

    testInit();
    testCreateEntity();
    testFindAndDestroy();
    testSetProperty();
    testMultipleEntities();
    testOnDestroyFires();
    testOnEnterLeaveSpace();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
