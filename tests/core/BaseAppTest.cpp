#include "theseed/core/BaseApp.h"
#include "theseed/core/IEntityStore.h"
#include "theseed/foundation/MemoryStream.h"
#include "theseed/login/ClientProtocol.h"
#include "theseed/login/LoginProtocol.h"
#include "theseed/login/SessionToken.h"
#include "theseed/runtime/Entity.h"
#include "theseed/runtime/EntityDef.h"
#include "theseed/runtime/RuntimeLoop.h"
#include "theseed/runtime/RuntimeTransport.h"
#include "theseed/runtime/RuntimeTypes.h"
#include "theseed/runtime/TcpConnection.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <vector>

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

    bool ok = app->registry().hasDef("Avatar");
    ok = ok && app->runtime().entityCount() == 0;  // init 未建实体

    const auto& constApp = *app;
    ok = ok && &constApp.runtime() == &app->runtime();  // const/non-const 访问同一对象
    ok = ok && constApp.registry().hasDef("Avatar");
    ok = ok && constApp.runtime().entityCount() == 0;

    if (ok) PASS(); else FAIL("runtime access failed");
    std::filesystem::remove_all(dir);
}

// ---- 客户端 TCP 回环测试支持 -------------------------------------------

namespace {

using theseed::login::ClientMessageType;
using theseed::login::ClientProtocol;
using theseed::login::LoginProtocol;
using theseed::runtime::RuntimeInvocation;

// 裸帧客户端：项目封装的 TcpConnection，无 POSIX 裸 socket，跨平台。
struct TestClient {
    std::shared_ptr<theseed::runtime::TcpConnection> conn;
    std::vector<std::byte> inbox;

    bool connect(std::uint16_t port) {
        conn = theseed::runtime::TcpConnection::create();
        conn->setOnReceived([this](std::span<const std::byte> data) {
            inbox.insert(inbox.end(), data.begin(), data.end());
        });
        return conn->connect("127.0.0.1", port);
    }

    void sendFrame(ClientMessageType type, const std::vector<std::byte>& payload) {
        auto frame = LoginProtocol::frameMessage(type, payload);
        conn->write(std::span<const std::byte>(frame.data(), frame.size()));
        conn->pump();
    }

    // 泵 app tick 与客户端 socket，直到收齐一帧或耗尽重试。
    // 保留 inbox 剩余字节（服务端可能一次发多帧，如响应+初始快照）。
    bool recvFrame(const std::function<void()>& appTick,
                   ClientMessageType& outType, std::vector<std::byte>& outPayload) {
        for (int i = 0; i < 20000; ++i) {
            appTick();
            conn->pump();
            ClientMessageType type{};
            std::span<const std::byte> payload;
            const auto* base = inbox.data();
            if (LoginProtocol::parseFrame(std::span<const std::byte>(base, inbox.size()),
                                          type, payload)) {
                outType = type;
                outPayload.assign(payload.begin(), payload.end());
                const auto frameLen = static_cast<std::size_t>(payload.data() - base)
                                    + payload.size();
                inbox.erase(inbox.begin(),
                            inbox.begin() + static_cast<std::ptrdiff_t>(frameLen));
                return true;
            }
        }
        return false;
    }

    void close() { conn->close(); }
};

std::vector<std::byte> tokenPayload(const std::string& token) {
    theseed::foundation::MemoryStream ms;
    ms.writeString(token);
    return std::vector<std::byte>(ms.data(), ms.data() + ms.size());
}

std::vector<std::byte> actionPayload(EntityId entityId, const std::string& action,
                                     const std::string& data = "") {
    theseed::foundation::MemoryStream ms;
    ms.writeUint64(static_cast<std::uint64_t>(entityId));
    ms.writeString(action);
    ms.writeUint32(static_cast<std::uint32_t>(data.size()));
    ms.writeBytes(data.data(), data.size());
    return std::vector<std::byte>(ms.data(), ms.data() + ms.size());
}

RuntimeInvocation makeInv(EntityId entityId, const std::string& method,
                          std::vector<std::byte> payload) {
    RuntimeInvocation inv;
    inv.entityId = entityId;
    inv.targetComponent = 1;  // BaseApp componentId
    inv.entityType = "Avatar";
    inv.method = method;
    inv.deliveryClass = theseed::runtime::DeliveryClass::ORDERED_RELIABLE;
    inv.payload = std::move(payload);
    return inv;
}

// EnterGameResponse payload: success(1) + entityId(8) + entityType + error
bool decodeEnterGameResponse(const std::vector<std::byte>& payload, bool& ok, EntityId& outId) {
    if (payload.size() < 9) return false;
    ok = payload[0] == std::byte{1};
    std::memcpy(&outId, payload.data() + 1, sizeof(outId));
    return true;
}

}  // namespace

static void testConstructorValidation() {
    TEST("constructor rejects null transport / store");

    BaseApp::Config config;
    bool ok = false;
    try {
        BaseApp bad(config, nullptr, std::make_shared<InMemoryEntityStore>());
    } catch (const std::invalid_argument&) {
        ok = true;
    }

    bool ok2 = false;
    try {
        BaseApp bad(config, std::make_shared<InMemoryRuntimeTransport>(), nullptr);
    } catch (const std::invalid_argument&) {
        ok2 = true;
    }

    if (ok && ok2) PASS(); else FAIL("constructor validation failed");
}

static void testOpsEnabled() {
    TEST("init starts ops server when enabled");

    auto dir = createDefDir();
    BaseApp::Config config;
    config.entityDefPath = dir;
    config.componentId = 1;
    config.clientListenPort = 0;  // 并行测试下固定端口可能被占，改自动分配
    config.ops.enabled = true;
    config.ops.host = "127.0.0.1";
    config.ops.port = 0;  // ephemeral

    auto app = std::make_unique<BaseApp>(config,
                                         std::make_shared<InMemoryRuntimeTransport>(),
                                         std::make_shared<InMemoryEntityStore>());
    bool ok = app->init();
    ok = ok && app->clientListenPort() != 0;
    app->tick();  // opsServer_->tick branch

    // HTTP GET /inspect → OpsInspector 回调（RuntimeInfo 快照）→ 响应 JSON
    auto opsConn = theseed::runtime::TcpConnection::create();
    std::vector<std::byte> rx;
    opsConn->setOnReceived([&rx](std::span<const std::byte> data) {
        rx.insert(rx.end(), data.begin(), data.end());
    });
    ok = ok && opsConn->connect("127.0.0.1", app->opsListenPort());
    if (ok) {
        static const std::string req = "GET /inspect HTTP/1.1\r\nHost: localhost\r\n\r\n";
        opsConn->write(std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(req.data()), req.size()));
        for (int i = 0; i < 2000 && rx.empty(); ++i) {
            app->tick();
            opsConn->pump();
        }
        std::string body(reinterpret_cast<const char*>(rx.data()), rx.size());
        ok = ok && body.find("\"role\":\"BaseApp\"") != std::string::npos;
        ok = ok && body.find("\"entity_count\"") != std::string::npos;
    }
    opsConn->close();

    if (ok) PASS(); else FAIL("ops-enabled init failed");
    std::filesystem::remove_all(dir);
}

static void testClientEnterGameFailures() {
    TEST("enter game: invalid token / factory missing");

    // 场景一：无效 token → EnterGameResponse(success=false)
    BaseApp::Config config;
    config.componentId = 1;
    config.clientListenPort = 0;
    auto transport = std::make_shared<InMemoryRuntimeTransport>();
    auto app = std::make_unique<BaseApp>(config, transport, std::make_shared<InMemoryEntityStore>());
    if (!app->init()) { FAIL("init failed"); return; }

    TestClient client;
    if (!client.connect(app->clientListenPort())) { FAIL("connect failed"); return; }
    {
        ClientMessageType type{};
        std::vector<std::byte> payload;
        auto tick = [&] { app->tick(); };
        client.sendFrame(ClientMessageType::EnterGame, tokenPayload("not-a-token"));
        if (!client.recvFrame(tick, type, payload)) { FAIL("no response"); return; }
        bool okResp = false;
        EntityId id = 0;
        if (type != ClientMessageType::EnterGameResponse ||
            !decodeEnterGameResponse(payload, okResp, id) || okResp) {
            FAIL("expected failure response"); return;
        }
    }

    // 场景二：无 def 注册 → createEntity 失败 → EnterGameResponse(success=false)
    auto token = theseed::login::SessionToken::issue("acc", "realm");
    {
        ClientMessageType type{};
        std::vector<std::byte> payload;
        auto tick = [&] { app->tick(); };
        client.sendFrame(ClientMessageType::EnterGame, tokenPayload(token));
        if (!client.recvFrame(tick, type, payload)) { FAIL("no response 2"); return; }
        bool okResp = false;
        EntityId id = 0;
        if (type != ClientMessageType::EnterGameResponse ||
            !decodeEnterGameResponse(payload, okResp, id) || okResp) {
            FAIL("expected factory-missing failure"); return;
        }
    }

    // 场景三：decode 失败与未知消息类型（onClientMessage 的 default / else 分支）
    {
        client.sendFrame(ClientMessageType::EnterGame, {std::byte{0xFF}, std::byte{0xFF}});
        client.sendFrame(ClientMessageType::Action, {std::byte{0x01}});
        client.sendFrame(ClientMessageType::QueryRealms, {});
        for (int i = 0; i < 50; ++i) app->tick();
        client.conn->pump();
    }

    client.close();
    for (int i = 0; i < 50; ++i) app->tick();  // cleanupClients 清断连 session
    PASS();
}

static void testClientFullLoop() {
    TEST("client loop: enter / action / replication / cell events / cleanup");

    auto dir = createDefDir();
    // 追加 Cell/Client side 方法，覆盖 handleClientAction 的分支
    writeFile(dir + "/Avatar.xml", R"(
<EntityDef name="Avatar">
    <Properties>
        <Property name="level" type="Int32"/>
        <Property name="hp" type="Float32"/>
        <Property name="alive" type="Bool"/>
    </Properties>
    <Methods>
        <Method name="onDamage" side="Base"/>
        <Method name="castSpell" side="Cell"/>
        <Method name="announce" side="Client"/>
    </Methods>
</EntityDef>
)");

    BaseApp::Config config;
    config.entityDefPath = dir;
    config.componentId = 1;
    config.clientListenPort = 0;
    auto transport = std::make_shared<InMemoryRuntimeTransport>();
    auto app = std::make_unique<BaseApp>(config, transport, std::make_shared<InMemoryEntityStore>());
    if (!app->init()) { FAIL("init failed"); return; }
    // 经 TickScheduler 驱动完整 tick 循环：Network phase 泵 runtime ingress
    // （wire 消息分发），Flush phase 泵 BaseApp::tick（客户端会话收发）。
    theseed::runtime::TickScheduler scheduler(std::chrono::milliseconds{100});
    app->attach(scheduler);
    auto tick = [&] { scheduler.runOnce(); };

    TestClient client;
    if (!client.connect(app->clientListenPort())) { FAIL("connect failed"); return; }
    for (int i = 0; i < 10; ++i) app->tick();  // acceptClientConnections

    // EnterGame：合法 token → success + 初始属性快照
    auto token = theseed::login::SessionToken::issue("acc1", "realm1");
    client.sendFrame(ClientMessageType::EnterGame, tokenPayload(token));
    {
        ClientMessageType type{};
        std::vector<std::byte> payload;
        if (!client.recvFrame(tick, type, payload)) { FAIL("no enter response"); return; }
        bool okResp = false;
        EntityId entityId = 0;
        if (type != ClientMessageType::EnterGameResponse ||
            !decodeEnterGameResponse(payload, okResp, entityId) || !okResp || entityId == 0) {
            FAIL("enter game failed"); return;
        }

        auto* entity = app->findEntity(entityId);
        if (!entity) { FAIL("entity missing"); return; }

        // 初始快照（handleEnterGame 的 PropertySync 段）
        if (!client.recvFrame(tick, type, payload)) { FAIL("no snapshot"); return; }
        if (type != ClientMessageType::PropertySync) { FAIL("expected snapshot"); return; }

        // requestCreateCell 已发往 CellApp（targetComponent=2）
        std::vector<RuntimeInvocation> drained(16);
        auto n = transport->receive(2, drained.data(), drained.size());
        bool sawCellRequest = false;
        for (std::size_t i = 0; i < n; ++i) {
            if (drained[i].method == "entity.createCell") sawCellRequest = true;
        }
        if (!sawCellRequest) { FAIL("no createCell"); return; }

        // handleClientAction：未知实体 / 未知方法 / Base / Client side
        client.sendFrame(ClientMessageType::Action, actionPayload(9999, "onDamage"));
        client.sendFrame(ClientMessageType::Action, actionPayload(entityId, "nope"));
        client.sendFrame(ClientMessageType::Action, actionPayload(entityId, "onDamage", "dmg"));
        client.sendFrame(ClientMessageType::Action, actionPayload(entityId, "announce", "hi"));
        // Cell side：cellEntityCall 未设置 → 只覆盖无效分支
        client.sendFrame(ClientMessageType::Action, actionPayload(entityId, "castSpell", "fx"));
        for (int i = 0; i < 50; ++i) app->tick();
        client.conn->pump();

        // Cell side：设置 cellEntityCall 后走 callCell 分支
        if (!app->runtime().setCellEntityCall(entityId, 2)) { FAIL("set cell call"); return; }
        client.sendFrame(ClientMessageType::Action, actionPayload(entityId, "castSpell", "fx2"));
        for (int i = 0; i < 50; ++i) app->tick();
        client.conn->pump();

        // flushClientPropertyUpdates：属性 dirty → tick → PropertySync
        entity->setProperty<std::int32_t>(0, 7);
        if (!client.recvFrame(tick, type, payload)) { FAIL("no property sync"); return; }
        if (type != ClientMessageType::PropertySync) { FAIL("expected property sync"); return; }

        // 模拟 CellApp 下发的事件流（BaseRuntime wire handlers → BaseApp 回调 → 客户端）
        auto pushInv = [&](const RuntimeInvocation& inv) {
            static_cast<void>(transport->send(inv));
        };

        // aoi.enter（含位置）
        {
            theseed::foundation::MemoryStream ms;
            ms.writeUint64(static_cast<std::uint64_t>(entityId));
            ms.writeUint64(900);
            ms.writeString("Monster");
            ms.writeUint8(1);
            const theseed::runtime::Vector3 pos{1, 2, 3};
            ms.writeFloat(pos.x);
            ms.writeFloat(pos.y);
            ms.writeFloat(pos.z);
            pushInv(makeInv(entityId, "aoi.enter",
                            std::vector<std::byte>(ms.data(), ms.data() + ms.size())));
            if (!client.recvFrame(tick, type, payload) || type != ClientMessageType::EntityEnter) {
                FAIL("no entity enter"); return;
            }
        }
        // aoi.leave
        {
            theseed::foundation::MemoryStream ms;
            ms.writeUint64(static_cast<std::uint64_t>(entityId));
            ms.writeUint64(900);
            pushInv(makeInv(entityId, "aoi.leave",
                            std::vector<std::byte>(ms.data(), ms.data() + ms.size())));
            if (!client.recvFrame(tick, type, payload) || type != ClientMessageType::EntityLeave) {
                FAIL("no entity leave"); return;
            }
        }
        // entity.clientEvent
        {
            theseed::foundation::MemoryStream ms;
            ms.writeUint64(static_cast<std::uint64_t>(entityId));
            ms.writeUint32(1);
            ms.writeString("levelUp");
            ms.writeUint32(2);
            ms.writeBytes("\x01\x02", 2);
            pushInv(makeInv(entityId, "entity.clientEvent",
                            std::vector<std::byte>(ms.data(), ms.data() + ms.size())));
            if (!client.recvFrame(tick, type, payload) || type != ClientMessageType::EntityEvent) {
                FAIL("no entity event"); return;
            }
        }
        // witness.propertySync（含位置）
        {
            theseed::foundation::MemoryStream ms;
            ms.writeUint64(static_cast<std::uint64_t>(entityId));
            ms.writeUint32(1);
            ms.writeUint64(900);
            ms.writeUint8(1);
            const theseed::runtime::Vector3 pos{4, 5, 6};
            ms.writeFloat(pos.x);
            ms.writeFloat(pos.y);
            ms.writeFloat(pos.z);
            ms.writeUint32(3);
            ms.writeBytes("\xAA\xBB\xCC", 3);
            pushInv(makeInv(entityId, "witness.propertySync",
                            std::vector<std::byte>(ms.data(), ms.data() + ms.size())));
            if (!client.recvFrame(tick, type, payload) || type != ClientMessageType::PropertySync) {
                FAIL("no witness sync"); return;
            }
        }
        // entity.spaceChanged
        {
            theseed::foundation::MemoryStream ms;
            ms.writeUint64(static_cast<std::uint64_t>(entityId));
            ms.writeUint64(77);
            const theseed::runtime::Vector3 pos{7, 8, 9};
            ms.writeFloat(pos.x);
            ms.writeFloat(pos.y);
            ms.writeFloat(pos.z);
            pushInv(makeInv(entityId, "entity.spaceChanged",
                            std::vector<std::byte>(ms.data(), ms.data() + ms.size())));
            if (!client.recvFrame(tick, type, payload) || type != ClientMessageType::SpaceChange) {
                FAIL("no space change"); return;
            }
        }

        // destroyEntity（有 cell 绑定 → 进入延迟销毁）→ 模拟 CellApp 确认
        // entity.cellDestroyed → completeBaseDestruction → onEntityDestroyed → EntityLeave
        if (!app->destroyEntity(entityId)) { FAIL("destroy failed"); return; }
        {
            theseed::foundation::MemoryStream ms;
            ms.writeUint64(static_cast<std::uint64_t>(entityId));
            pushInv(makeInv(entityId, "entity.cellDestroyed",
                            std::vector<std::byte>(ms.data(), ms.data() + ms.size())));
        }
        if (!client.recvFrame(tick, type, payload) || type != ClientMessageType::EntityLeave) {
            FAIL("no destroy leave"); return;
        }

        // 实体已销毁：断连 cleanup 只清映射（findEntity null 分支）
        client.close();
        for (int i = 0; i < 50; ++i) app->tick();
    }

    PASS();
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
    testConstructorValidation();
    testOpsEnabled();
    testClientEnterGameFailures();
    testClientFullLoop();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
