// BaseApp 分支覆盖测试（第三批）：未 init 的 API 防御臂、下行回调对僵尸/
// 未知观察者的早退族、TCP 会话生命周期（hasPos=false 下行、销毁中实体的
// 动作与 flush 跳过、会话重绑、断连清理）、无 cellCall 的 Cell 动作、无属性
// Avatar 的 EnterGame 空快照路径。与 BaseAppTest 互补——那边是完整功能
// 回环，这边专收各防御/早退分支。
#include "theseed/core/BaseApp.h"
#include "theseed/core/IEntityStore.h"
#include "theseed/foundation/MemoryStream.h"
#include "theseed/login/ClientProtocol.h"
#include "theseed/login/ClientSession.h"
#include "theseed/login/LoginProtocol.h"
#include "theseed/login/SessionToken.h"
#include "theseed/runtime/Entity.h"
#include "theseed/runtime/RuntimeLoop.h"
#include "theseed/runtime/RuntimeTransport.h"
#include "theseed/runtime/RuntimeTypes.h"
#include "theseed/runtime/TcpConnection.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

using theseed::core::BaseApp;
using theseed::core::InMemoryEntityStore;
using theseed::login::ActionMsg;
using theseed::login::ClientMessageType;
using theseed::login::ClientSession;
using theseed::login::LoginProtocol;
using theseed::runtime::EntityId;
using theseed::runtime::InMemoryRuntimeTransport;
using theseed::runtime::RuntimeInvocation;
using theseed::runtime::TcpConnection;
using theseed::runtime::TickScheduler;
using theseed::runtime::Vector3;

static int testsPassed = 0;
static int testsFailed = 0;

#define TEST(name)                                           \
    do {                                                     \
        std::cout << "  " << (name) << "... " << std::flush; \
    } while (0)

#define PASS()               \
    do {                     \
        std::cout << "OK\n"; \
        ++testsPassed;       \
    } while (0)

#define FAIL(msg)                                 \
    do {                                          \
        std::cout << "FAILED: " << (msg) << "\n"; \
        ++testsFailed;                            \
    } while (0)

namespace {

bool writeFile(const std::string& path, const std::string& content) {
    std::ofstream f(path);
    if (!f.is_open()) return false;
    f << content;
    return true;
}

// Avatar：带属性 + Base/Cell 两个方法，覆盖 handleClientAction 的 side 分派。
std::string createDefDir() {
    std::string dir = "test_baseapp_branch_defs";
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
        <Method name="castSpell" side="Cell"/>
    </Methods>
</EntityDef>
)");
    return dir;
}

// 仅含 Base 标记属性的 Avatar：buildFullPropertySnapshot(Base) 排除它 →
// EnterGame 快照为空（handleEnterGame 的 !snapshot.empty() 否臂）。
// 不能用零属性 def——存储为 null 会让快照构造直接抛 invalid_argument。
std::string createBaseOnlyDefDir() {
    std::string dir = "test_baseapp_baseonly_defs";
    std::filesystem::create_directory(dir);
    writeFile(dir + "/Avatar.xml", R"(
<EntityDef name="Avatar">
    <Properties>
        <Property name="baseOnly" type="Int32" flags="Base"/>
    </Properties>
</EntityDef>
)");
    return dir;
}

std::unique_ptr<BaseApp> makeBaseApp(const std::string& defPath,
                                     std::shared_ptr<InMemoryRuntimeTransport> transport) {
    BaseApp::Config config;
    config.entityDefPath = defPath;
    config.componentId = 1;
    config.autoSaveInterval = {};
    config.clientListenPort = 0;
    return std::make_unique<BaseApp>(config, transport,
                                     std::make_shared<InMemoryEntityStore>());
}

// 裸帧客户端（照搬 BaseAppTest 的 TestClient）。
struct TestClient {
    std::shared_ptr<TcpConnection> conn;
    std::vector<std::byte> inbox;

    bool connect(std::uint16_t port) {
        conn = TcpConnection::create();
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
    inv.targetComponent = 1;
    inv.entityType = "Avatar";
    inv.method = method;
    inv.deliveryClass = theseed::runtime::DeliveryClass::ORDERED_RELIABLE;
    inv.payload = std::move(payload);
    return inv;
}

bool decodeEnterGameResponse(const std::vector<std::byte>& payload, bool& ok, EntityId& outId) {
    if (payload.size() < 9) return false;
    ok = payload[0] == std::byte{1};
    std::memcpy(&outId, payload.data() + 1, sizeof(outId));
    return true;
}

// ---- 下行 wire payload 构造（格式对齐 BaseRuntime 各 handler 的解码）-----

std::vector<std::byte> aoiEnterPayload(EntityId observer, EntityId target, bool hasPos) {
    theseed::foundation::MemoryStream ms;
    ms.writeUint64(static_cast<std::uint64_t>(observer));
    ms.writeUint64(static_cast<std::uint64_t>(target));
    ms.writeUint32(3);
    ms.writeBytes("NPC", 3);
    ms.writeUint8(hasPos ? 1 : 0);
    if (hasPos) {
        const Vector3 pos{1, 2, 3};
        ms.writeFloat(pos.x);
        ms.writeFloat(pos.y);
        ms.writeFloat(pos.z);
    }
    return std::vector<std::byte>(ms.data(), ms.data() + ms.size());
}

std::vector<std::byte> aoiLeavePayload(EntityId observer, EntityId target) {
    theseed::foundation::MemoryStream ms;
    ms.writeUint64(static_cast<std::uint64_t>(observer));
    ms.writeUint64(static_cast<std::uint64_t>(target));
    return std::vector<std::byte>(ms.data(), ms.data() + ms.size());
}

std::vector<std::byte> clientEventPayload(EntityId entityId) {
    theseed::foundation::MemoryStream ms;
    ms.writeUint64(static_cast<std::uint64_t>(entityId));
    ms.writeUint32(1);  // entry count
    ms.writeUint32(7);
    ms.writeBytes("levelUp", 7);
    ms.writeUint32(0);
    return std::vector<std::byte>(ms.data(), ms.data() + ms.size());
}

std::vector<std::byte> witnessSyncPayload(EntityId observer, EntityId target, bool hasPos) {
    theseed::foundation::MemoryStream ms;
    ms.writeUint64(static_cast<std::uint64_t>(observer));
    ms.writeUint32(1);  // entry count
    ms.writeUint64(static_cast<std::uint64_t>(target));
    ms.writeUint8(hasPos ? 1 : 0);
    if (hasPos) {
        const Vector3 pos{4, 5, 6};
        ms.writeFloat(pos.x);
        ms.writeFloat(pos.y);
        ms.writeFloat(pos.z);
    }
    ms.writeUint32(2);
    ms.writeBytes("\xAA\xBB", 2);
    return std::vector<std::byte>(ms.data(), ms.data() + ms.size());
}

std::vector<std::byte> spaceChangedPayload(EntityId entityId) {
    theseed::foundation::MemoryStream ms;
    ms.writeUint64(static_cast<std::uint64_t>(entityId));
    ms.writeUint64(77);
    const Vector3 pos{7, 8, 9};
    ms.writeFloat(pos.x);
    ms.writeFloat(pos.y);
    ms.writeFloat(pos.z);
    return std::vector<std::byte>(ms.data(), ms.data() + ms.size());
}

}  // namespace

// 未 init 的 BaseApp：所有转发 API 走 runtime_ 空防御臂。
static void testUninitializedApiGuards() {
    TEST("uninitialized app: forwarding APIs hit null-runtime guards");

    auto app = makeBaseApp("", std::make_shared<InMemoryRuntimeTransport>());

    bool ok = app->opsListenPort() == 0;
    ok = ok && app->createEntity("Avatar") == nullptr;
    ok = ok && app->findEntity(1) == nullptr;
    ok = ok && !app->destroyEntity(1);
    ok = ok && app->restoreEntities("Avatar") == 0;
    ok = ok && !app->requestCreateCell(1, "Avatar", Vector3{}, 2);
    ok = ok && !app->requestDestroyCell(1, 2);
    ok = ok && !app->requestTeleport(1, 0, Vector3{});

    TickScheduler scheduler(std::chrono::milliseconds{100});
    app->attach(scheduler);  // runtime_ null：跳过 runtime 挂接，仅注册 clientPump_
    app->detach(scheduler);
    app->tick();  // listener/ops 未启动，空转安全

    if (ok) PASS(); else FAIL("uninitialized guards failed");
}

// 断连（僵尸）会话与未知观察者：五类下行回调的早退臂。
static void testCallbacksEarlyReturn() {
    TEST("downstream callbacks early-return for zombie/unknown observers");

    auto dir = createDefDir();
    auto transport = std::make_shared<InMemoryRuntimeTransport>();
    auto app = makeBaseApp(dir, transport);
    bool ok = app->init();
    TickScheduler scheduler(std::chrono::milliseconds{100});
    app->attach(scheduler);

    // 僵尸会话：从未连接的 TcpConnection，经公共 API 注入会话表并绑定 7777。
    auto zombie = std::make_unique<ClientSession>(TcpConnection::create());
    auto* zombieRaw = zombie.get();
    app->takeClientSession(std::move(zombie));
    app->bindSessionToEntity(zombieRaw, 7777);

    // 对 7777（断连会话）与 8888（从未绑定）各推五类下行消息 → 回调一律早退。
    auto push = [&](EntityId observer, const std::string& method,
                    std::vector<std::byte> payload) {
        static_cast<void>(transport->send(makeInv(observer, method, std::move(payload))));
    };
    for (const auto observer : {EntityId{7777}, EntityId{8888}}) {
        push(observer, "aoi.enter", aoiEnterPayload(observer, 900, true));
        push(observer, "aoi.leave", aoiLeavePayload(observer, 900));
        push(observer, "entity.clientEvent", clientEventPayload(observer));
        push(observer, "witness.propertySync", witnessSyncPayload(observer, 900, true));
        push(observer, "entity.spaceChanged", spaceChangedPayload(observer));
    }

    // 一轮 runOnce：Network 泵入站触发五类回调早退；Flush 里 flushClient
    // 跳过断连会话、cleanupClients 清掉僵尸及其映射（findEntity null 臂）。
    scheduler.runOnce();
    app->detach(scheduler);

    if (ok) PASS(); else FAIL("callback early-return failed");
    std::filesystem::remove_all(dir);
}

// TCP 会话生命周期：hasPos=false 下行、销毁中实体的动作/flush 跳过、
// 会话重绑到不存在实体、断连清理的空实体臂。
static void testClientLifecycleBranches() {
    TEST("tcp lifecycle: hasPos=false, destroying skip, rebind, cleanup");

    auto dir = createDefDir();
    auto transport = std::make_shared<InMemoryRuntimeTransport>();
    auto app = makeBaseApp(dir, transport);
    bool ok = app->init();
    TickScheduler scheduler(std::chrono::milliseconds{100});
    app->attach(scheduler);
    auto tick = [&] { scheduler.runOnce(); };

    TestClient client;
    ok = ok && client.connect(app->clientListenPort());
    for (int i = 0; i < 10; ++i) app->tick();  // acceptClientConnections

    auto token = theseed::login::SessionToken::issue("acc", "realm");
    client.sendFrame(ClientMessageType::EnterGame, tokenPayload(token));
    ClientMessageType type{};
    std::vector<std::byte> payload;
    ok = ok && client.recvFrame(tick, type, payload);
    ok = ok && type == ClientMessageType::EnterGameResponse;
    bool okResp = false;
    EntityId entityId = 0;
    ok = ok && decodeEnterGameResponse(payload, okResp, entityId) && okResp;
    ok = ok && client.recvFrame(tick, type, payload);  // 初始属性快照
    ok = ok && type == ClientMessageType::PropertySync;

    // hasPos=false 的下行：EntityEnter / PropertySync 不携带位置。
    static_cast<void>(transport->send(
        makeInv(entityId, "aoi.enter", aoiEnterPayload(entityId, 900, false))));
    static_cast<void>(transport->send(
        makeInv(entityId, "witness.propertySync", witnessSyncPayload(entityId, 900, false))));
    ok = ok && client.recvFrame(tick, type, payload);
    ok = ok && type == ClientMessageType::EntityEnter;
    ok = ok && client.recvFrame(tick, type, payload);
    ok = ok && type == ClientMessageType::PropertySync;

    // 会话重绑到不存在实体：flush 找不到实体 → continue。
    auto* session = app->findSessionByEntity(entityId);
    ok = ok && session != nullptr;
    app->bindSessionToEntity(session, 424242);
    for (int i = 0; i < 5; ++i) tick();

    // 造一个延迟销毁实体（显式绑 cellCall 后 destroy → Destroying 挂起）：
    // 其 Action 命中 !isActive 早退，flush 命中 !isActive continue。
    auto* dying = app->createEntity("Avatar");
    ok = ok && dying != nullptr;
    if (!dying) {
        FAIL("create dying entity failed");
        std::filesystem::remove_all(dir);
        return;
    }
    const auto dyingId = dying->id();  // destroy 后原指针失效，先存 id
    ok = ok && app->runtime().setCellEntityCall(dyingId, 2);
    ok = ok && app->destroyEntity(dyingId);
    client.sendFrame(ClientMessageType::Action, actionPayload(dyingId, "onDamage", "x"));
    app->bindSessionToEntity(session, dyingId);
    for (int i = 0; i < 50; ++i) tick();
    client.conn->pump();

    // 断连前重绑到不存在实体：cleanup 走 findEntity null 臂，只清映射。
    app->bindSessionToEntity(session, 555555);
    client.close();
    for (int i = 0; i < 50; ++i) app->tick();  // cleanupClients

    app->detach(scheduler);

    if (ok) PASS(); else FAIL("tcp lifecycle branches failed");
    std::filesystem::remove_all(dir);
}

// 直连会话未建立时的 Cell/Base 动作分派：无 cellCall 的早退与绑定后的转发。
static void testCellActionWithoutCellBinding() {
    TEST("cell-side action without cell binding skips forwarding");

    auto dir = createDefDir();
    auto app = makeBaseApp(dir, std::make_shared<InMemoryRuntimeTransport>());
    bool ok = app->init();

    // 直接 createEntity（不走 EnterGame）→ 无 cellCall。
    auto* entity = app->createEntity("Avatar");
    ok = ok && entity != nullptr;
    if (!entity) {
        FAIL("create entity failed");
        std::filesystem::remove_all(dir);
        return;
    }
    const auto id = entity->id();

    ActionMsg cellMsg{id, "castSpell", {std::byte{1}}};
    app->handleClientAction(cellMsg);  // cellCall 为空 → 跳过转发

    ok = ok && app->runtime().setCellEntityCall(id, 2);
    app->handleClientAction(cellMsg);  // 绑定后走 callCell

    ActionMsg baseMsg{id, "onDamage", {}};
    app->handleClientAction(baseMsg);  // Base 侧直接分发

    ActionMsg unknownMethod{id, "nope", {}};
    app->handleClientAction(unknownMethod);  // def 查无此方法 → 早退

    ActionMsg unknownEntity{999999, "onDamage", {}};
    app->handleClientAction(unknownEntity);  // 实体不存在 → 早退

    if (ok) PASS(); else FAIL("cell action binding failed");
    std::filesystem::remove_all(dir);
}

// 仅含 Base 标记属性的 Avatar：EnterGame 成功但不发送初始属性快照。
static void testEnterGameBaseOnlyAvatar() {
    TEST("enter game on base-only avatar sends no snapshot");

    auto dir = createBaseOnlyDefDir();
    auto app = makeBaseApp(dir, std::make_shared<InMemoryRuntimeTransport>());
    bool ok = app->init();
    TickScheduler scheduler(std::chrono::milliseconds{100});
    app->attach(scheduler);
    auto tick = [&] { scheduler.runOnce(); };

    TestClient client;
    ok = ok && client.connect(app->clientListenPort());
    for (int i = 0; i < 10; ++i) app->tick();

    auto token = theseed::login::SessionToken::issue("acc", "realm");
    client.sendFrame(ClientMessageType::EnterGame, tokenPayload(token));
    ClientMessageType type{};
    std::vector<std::byte> payload;
    ok = ok && client.recvFrame(tick, type, payload);
    ok = ok && type == ClientMessageType::EnterGameResponse;
    bool okResp = false;
    EntityId entityId = 0;
    ok = ok && decodeEnterGameResponse(payload, okResp, entityId) && okResp;

    // 快照为空：后续不再有 PropertySync 帧。
    for (int i = 0; i < 50; ++i) tick();
    client.conn->pump();
    ok = ok && client.inbox.empty();

    client.close();
    for (int i = 0; i < 50; ++i) app->tick();
    app->detach(scheduler);

    if (ok) PASS(); else FAIL("bare avatar snapshot failed");
    std::filesystem::remove_all(dir);
}

// init 全景：store 存量 Avatar 恢复（factory 真臂 / restoreEntities 分派）、
// ops.enabled 段与 /inspect 触发 inspector lambda 的 runtime/transport 真臂。
static void testInitRestoresStoredEntitiesWithOps() {
    TEST("init restores stored entities and serves /inspect with ops enabled");

    auto dir = createDefDir();
    auto transport = std::make_shared<InMemoryRuntimeTransport>();
    auto store = std::make_shared<InMemoryEntityStore>();

    // 预置一条存量 Avatar：level=7（Int32 定长 4 字节小端）。
    theseed::core::EntityData stored;
    stored.id = 77;
    stored.entityType = "Avatar";
    theseed::core::PropertyData level;
    level.id = 0;
    level.name = "level";
    level.type = theseed::core::DataType::Int32;
    std::int32_t levelVal = 7;
    level.rawValue.resize(4);
    std::memcpy(level.rawValue.data(), &levelVal, 4);
    stored.properties.push_back(level);
    const bool stored_ = store->save(77, stored);

    BaseApp::Config config;
    config.entityDefPath = dir;
    config.componentId = 1;
    config.autoSaveInterval = {};
    config.clientListenPort = 0;
    config.ops.enabled = true;
    config.ops.host = "127.0.0.1";
    config.ops.port = 0;  // ephemeral
    auto app = std::make_unique<BaseApp>(config, transport, store);

    bool ok = stored_;
    ok = ok && app->init();
    // 存量数据恢复：Avatar 在册且属性还原。
    auto* restored = app->findEntity(77);
    ok = ok && restored != nullptr && restored->entityType() == "Avatar";
    ok = ok && restored->getProperty<std::int32_t>(0) == 7;

    // ops 服务已起，/inspect 触发 inspector lambda（entityCount / transportStats 真臂）。
    ok = ok && app->opsListenPort() != 0;
    auto opsConn = TcpConnection::create();
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
        ok = ok && body.find("\"entity_count\":1") != std::string::npos;
    }
    opsConn->close();

    if (ok) PASS(); else FAIL("init restore + ops inspect failed");
    std::filesystem::remove_all(dir);
}

int main() {
    std::cout << "BaseApp branch tests:\n";

    testUninitializedApiGuards();
    testCallbacksEarlyReturn();
    testClientLifecycleBranches();
    testCellActionWithoutCellBinding();
    testEnterGameBaseOnlyAvatar();
    testInitRestoresStoredEntitiesWithOps();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
