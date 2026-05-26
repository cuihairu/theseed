#include "theseed/core/BaseApp.h"
#include "theseed/core/IEntityStore.h"
#include "theseed/runtime/Entity.h"
#include "theseed/runtime/EntityDef.h"
#include "theseed/runtime/RuntimeLoop.h"
#include "theseed/runtime/RuntimeTransport.h"
#include "theseed/runtime/RuntimeTypes.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

using theseed::core::BaseApp;
using theseed::core::BaseRuntime;
using theseed::core::EntityDefRegistry;
using theseed::core::InMemoryEntityStore;
using theseed::runtime::Entity;
using theseed::runtime::EntityDef;
using theseed::runtime::EntityId;
using theseed::runtime::EntitySide;
using theseed::runtime::EntityState;
using theseed::runtime::InMemoryIORuntime;
using theseed::runtime::InMemoryRuntimeTransport;
using theseed::runtime::PropertyType;
using theseed::runtime::ServiceApp;
using theseed::runtime::TickScheduler;

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

static std::unique_ptr<BaseApp> makeBaseApp(
    const std::string& defPath = "",
    std::shared_ptr<InMemoryRuntimeTransport> transport = nullptr,
    std::shared_ptr<InMemoryEntityStore> store = nullptr) {
    if (!transport) {
        transport = std::make_shared<InMemoryRuntimeTransport>();
    }
    if (!store) {
        store = std::make_shared<InMemoryEntityStore>();
    }
    BaseApp::Config config;
    config.entityDefPath = defPath;
    config.componentId = 1;
    config.autoSaveInterval = {};
    return std::make_unique<BaseApp>(config, transport, store);
}

static std::string createDefDir() {
    std::string dir = "test_baseapp_defs";
    std::filesystem::create_directory(dir);

    writeFile(dir + "/Avatar.xml", R"(
<EntityDef name="Avatar">
    <Properties>
        <Property name="level" type="Int32"/>
        <Property name="hp" type="Float32"/>
        <Property name="alive" type="Bool"/>
    </Properties>
    <Methods>
        <Method name="onDamage" side="Base"/>
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
    TEST("init loads definitions and registers factories");

    auto dir = createDefDir();
    auto app = makeBaseApp(dir);

    bool ok = app->init();
    ok = ok && app->registry().defCount() == 2;
    ok = ok && app->registry().hasDef("Avatar");
    ok = ok && app->registry().hasDef("Monster");

    std::filesystem::remove_all(dir);

    if (ok) PASS(); else FAIL("init failed");
}

static void testCreateEntity() {
    TEST("create entity through BaseApp");

    auto dir = createDefDir();
    auto app = makeBaseApp(dir);
    app->init();

    auto* entity = app->createEntity("Avatar");
    bool ok = entity != nullptr;
    ok = ok && entity->entityType() == "Avatar";
    ok = ok && entity->side() == EntitySide::Base;
    ok = ok && entity->state() == EntityState::Active;

    if (ok) PASS(); else FAIL("create entity failed");
    std::filesystem::remove_all(dir);
}

static void testFindAndDestroy() {
    TEST("find and destroy entity");

    auto dir = createDefDir();
    auto app = makeBaseApp(dir);
    app->init();

    auto* e1 = app->createEntity("Avatar");
    auto* e2 = app->createEntity("Monster");
    auto id1 = e1->id();
    auto id2 = e2->id();

    bool ok = app->findEntity(id1) == e1;
    ok = ok && app->findEntity(id2) == e2;
    ok = ok && app->findEntity(99999) == nullptr;

    ok = ok && app->destroyEntity(id1);
    ok = ok && app->findEntity(id1) == nullptr;
    ok = ok && app->findEntity(id2) != nullptr;

    if (ok) PASS(); else FAIL("find/destroy failed");
    std::filesystem::remove_all(dir);
}

static void testAttachDetach() {
    TEST("attach to TickScheduler and run ticks");

    auto dir = createDefDir();
    auto store = std::make_shared<InMemoryEntityStore>();
    auto transport = std::make_shared<InMemoryRuntimeTransport>();

    BaseApp::Config config;
    config.entityDefPath = dir;
    config.componentId = 1;
    config.autoSaveInterval = std::chrono::milliseconds{50};

    auto app = std::make_unique<BaseApp>(config, transport, store);
    app->init();

    auto* entity = app->createEntity("Avatar");
    auto id = entity->id();
    entity->setProperty<std::int32_t>(0, 42);

    TickScheduler scheduler(std::chrono::milliseconds{100});
    app->attach(scheduler);

    using namespace std::chrono;
    theseed::runtime::TickContext ctx;
    ctx.deltaTime = milliseconds{100};

    scheduler.runOnce();
    scheduler.runOnce();

    bool ok = store->exists(id);

    app->detach(scheduler);
    std::filesystem::remove_all(dir);

    if (ok) PASS(); else FAIL("attach/detach tick failed");
}

static void testPersistence() {
    TEST("create save destroy load cycle");

    auto dir = createDefDir();
    auto store = std::make_shared<InMemoryEntityStore>();
    auto app = makeBaseApp(dir, nullptr, store);
    app->init();

    auto* entity = app->createEntity("Avatar");
    auto id = entity->id();

    entity->setProperty<std::int32_t>(0, 42);
    entity->setProperty<float>(1, 99.5f);
    entity->setProperty<bool>(2, true);

    app->runtime().saveEntity(id);
    app->destroyEntity(id);

    auto* loaded = app->runtime().loadEntity(id, "Avatar");
    bool ok = loaded != nullptr;
    ok = ok && loaded->id() == id;
    ok = ok && loaded->getProperty<std::int32_t>(0) == 42;
    ok = ok && loaded->getProperty<float>(1) == 99.5f;
    ok = ok && loaded->getProperty<bool>(2) == true;

    if (ok) PASS(); else FAIL("persistence cycle failed");
    std::filesystem::remove_all(dir);
}

static void testRuntimeAccess() {
    TEST("access BaseRuntime and EntityDefRegistry");

    auto dir = createDefDir();
    auto app = makeBaseApp(dir);
    app->init();

    bool ok = &app->runtime() != nullptr;
    ok = ok && &app->registry() != nullptr;
    ok = ok && app->registry().hasDef("Avatar");

    const auto& constApp = *app;
    ok = ok && &constApp.registry() != nullptr;

    if (ok) PASS(); else FAIL("runtime access failed");
    std::filesystem::remove_all(dir);
}

int main() {
    std::cout << "BaseApp tests:\n";

    testInit();
    testCreateEntity();
    testFindAndDestroy();
    testAttachDetach();
    testPersistence();
    testRuntimeAccess();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
