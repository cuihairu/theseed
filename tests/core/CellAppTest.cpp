#include "theseed/core/CellApp.h"
#include "theseed/runtime/Entity.h"
#include "theseed/runtime/EntityDef.h"
#include "theseed/runtime/RuntimeTransport.h"
#include "theseed/runtime/RuntimeTypes.h"
#include "theseed/runtime/TcpConnection.h"
#include "theseed/runtime/TickScheduler.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

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

// entityDefPath 为空：init 跳过 loadDirectory 直接起默认空间（L20 假臂），registry 保持空。
static void testInitWithoutDefPath() {
    TEST("init without entityDefPath skips definition loading");

    CellApp::Config config;
    config.componentId = 3;
    CellApp app(config, std::make_shared<InMemoryRuntimeTransport>());
    bool ok = app.init();
    ok = ok && app.registry().defCount() == 0;
    ok = ok && !app.registry().hasDef("Avatar");

    if (ok) PASS(); else FAIL("init without def path failed");
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

static void testEdgeBranches() {
    TEST("constructor throw / ops server / const accessors / destroy fallbacks");

    auto dir = createDefDir();

    // null transport → invalid_argument
    bool threw = false;
    try {
        CellApp bad(CellApp::Config{}, nullptr);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    if (!threw) { FAIL("null transport accepted"); std::filesystem::remove_all(dir); return; }

    // ops.enabled：init 建 OpsServer，tick 驱动，HTTP /inspect 走 inspector 回调
    auto transport = std::make_shared<InMemoryRuntimeTransport>();
    CellApp::Config config;
    config.entityDefPath = dir;
    config.componentId = 2;
    config.ops.enabled = true;
    config.ops.host = "127.0.0.1";
    config.ops.port = 0;  // ephemeral

    CellApp app(config, transport);
    bool ok = app.init();
    ok = ok && app.opsListenPort() != 0;
    app.tick();

    auto opsConn = theseed::runtime::TcpConnection::create();
    std::vector<std::byte> rx;
    opsConn->setOnReceived([&rx](std::span<const std::byte> data) {
        rx.insert(rx.end(), data.begin(), data.end());
    });
    ok = ok && opsConn->connect("127.0.0.1", app.opsListenPort());
    if (ok) {
        static const std::string req = "GET /inspect HTTP/1.1\r\nHost: localhost\r\n\r\n";
        opsConn->write(std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(req.data()), req.size()));
        for (int i = 0; i < 2000 && rx.empty(); ++i) {
            app.tick();
            opsConn->pump();
        }
        std::string body(reinterpret_cast<const char*>(rx.data()), rx.size());
        ok = ok && body.find("\"role\":\"CellApp\"") != std::string::npos;
    }
    opsConn->close();

    // const 访问器
    const auto& constApp = app;
    ok = ok && &constApp.runtime() == &app.runtime();  // const/non-const 访问同一对象
    ok = ok && constApp.registry().hasDef("Avatar");

    // 未知类型 createEntity → nullptr
    ok = ok && app.createEntity("NoSuchType", Vector3{0, 0, 0}) == nullptr;

    // destroyEntity：未知 id → false
    ok = ok && !app.destroyEntity(99999);

    if (ok) PASS(); else FAIL("edge branches failed");
    std::filesystem::remove_all(dir);
}

static void testDestroyBeforeInit() {
    TEST("destroyEntity before init returns false");

    auto transport = std::make_shared<InMemoryRuntimeTransport>();
    CellApp app(CellApp::Config{}, transport);  // 未 init → runtime_ 为空
    if (!app.destroyEntity(1)) PASS(); else FAIL("destroy should fail before init");
}

// 未 init 的防御臂：tick / attach / detach / findEntity / opsListenPort 全部安全跳过。
static void testUninitializedDefensiveArms() {
    TEST("uninitialized app defensive arms");

    auto transport = std::make_shared<InMemoryRuntimeTransport>();
    CellApp app(CellApp::Config{}, transport);  // runtime_ / opsServer_ 均为空

    bool ok = true;
    ok = ok && app.findEntity(1) == nullptr;      // runtime_ 空臂 → nullptr
    ok = ok && app.opsListenPort() == 0;          // opsServer_ 空臂 → 0

    theseed::runtime::TickScheduler scheduler;
    app.attach(scheduler);                        // runtime_ 空臂 → 不注册
    app.tick();                                   // runtime_ / transport_ / opsServer_ 空臂全部走假臂
    app.detach(scheduler);                        // runtime_ 空臂 → 不注销

    // init 后同一定时器对象 attach/detach 才走真臂；这里只需验证未 init 全程 no-throw。
    if (ok) PASS(); else FAIL("uninitialized defensive arms failed");
}

int main() {
    std::cout << "CellApp tests:\n";

    testInit();
    testInitWithoutDefPath();
    testCreateEntity();
    testFindAndDestroy();
    testSetProperty();
    testMultipleEntities();
    testOnDestroyFires();
    testOnEnterLeaveSpace();
    testEdgeBranches();
    testDestroyBeforeInit();
    testUninitializedDefensiveArms();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
