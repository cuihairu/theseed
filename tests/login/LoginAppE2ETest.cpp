// LoginApp 端到端测试：真实 TCP 回环驱动完整 tick 循环。
// 场景 A（authType=null + 限流 + 会话存储 + ops）：QueryRealms / Login /
// 限流拒绝 / SelectRealm 命中与未命中 / 未知消息存活 / HTTP 端点。
// 场景 B（authType=password）：空密码拒绝、正常放行。
// 场景 C（authType=db）：后台线程驱动 file 后端 DBApp，覆盖 auto-register、
// 正确/错误密码三条鉴权路径。
#include "theseed/control/machine/HostProbe.h"
#include "theseed/control/machine/MachineAgent.h"
#include "theseed/control/machine/MachineDaemon.h"
#include "theseed/control/machine/ProcessSupervisor.h"
#include "theseed/db/DBApp.h"
#include "theseed/foundation/MemoryStream.h"
#include "theseed/foundation/Metrics.h"
#include "theseed/foundation/RedisProvider.h"
#include "theseed/foundation/SessionStore.h"
#include "theseed/login/LoginProtocol.h"
#include "theseed/login/LoginApp.h"
#include "theseed/login/LoginTypes.h"
#include "theseed/runtime/NetworkTransport.h"
#include "theseed/runtime/TcpConnection.h"

#include <arpa/inet.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

using namespace theseed;
using theseed::db::DBApp;
using theseed::foundation::InMemoryRedisProvider;
using theseed::foundation::RateLimiter;
using theseed::foundation::SessionStore;
using theseed::foundation::StoredSession;
using theseed::login::ClientMessageType;
using theseed::login::LoginApp;
using theseed::login::LoginAppConfig;
using theseed::login::LoginProtocol;
using theseed::login::RealmInfo;
using theseed::runtime::ComponentId;
using theseed::runtime::NetworkTransport;
using theseed::runtime::RuntimeInvocation;
using theseed::runtime::SendResult;
using theseed::runtime::TcpConnection;
using theseed::control::machine::AccessRole;
using theseed::control::machine::LocalHostProbe;
using theseed::control::machine::LocalProcessSupervisor;
using theseed::control::machine::MachineAgent;
using theseed::control::machine::MachineDaemon;
namespace MachineMethod = theseed::control::machine::MachineMethod;

#define TEST(name)                            \
    do {                                      \
        std::cout << "  " << name << "... ";  \
    } while (0)
#define PASS() std::cout << "OK" << std::endl
#define FAIL(msg)                                    \
    do {                                             \
        std::cout << "FAILED: " << msg << std::endl; \
        return 1;                                    \
    } while (0)

namespace {

// 通知腿两端身份（与 LoginAppConfig 缺省一致）：LoginApp 20 / Machine 60。
constexpr ComponentId kLoginComponent = 20;
constexpr ComponentId kMachineComponent = 60;
// 运维客户端身份（NodeOpsPolicy 白名单 + Operator 角色绑定）。
constexpr ComponentId kOpsComponent = 5;

std::vector<std::byte> toBytes(const std::string& text) {
    std::vector<std::byte> bytes(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        bytes[i] = static_cast<std::byte>(text[i]);
    }
    return bytes;
}

std::uint16_t freePort() {
    int s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return 0;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(s);
        return 0;
    }
    socklen_t len = sizeof(addr);
    ::getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len);
    ::close(s);
    return ntohs(addr.sin_port);
}

// 裸帧客户端：直接收发 LoginProtocol 帧。
struct FrameClient {
    std::shared_ptr<TcpConnection> conn;
    std::vector<std::byte> inbox;
    int pumpCount = 0;

    bool connect(std::uint16_t port) {
        conn = TcpConnection::create();
        conn->setOnReceived([this](std::span<const std::byte> data) {
            inbox.insert(inbox.end(), data.begin(), data.end());
        });
        return conn->connect("127.0.0.1", port);
    }

    void send(ClientMessageType type, std::span<const std::byte> payload) {
        auto frame = LoginProtocol::frameMessage(type, payload);
        conn->write(std::span<const std::byte>(frame.data(), frame.size()));
        conn->pump();
    }

    // 泵应用 tick 与客户端 socket，直到收齐一帧或超时。
    bool recvFrame(const std::function<void()>& appTick,
                   ClientMessageType& outType, std::vector<std::byte>& outPayload) {
        for (int i = 0; i < 4000; ++i) {
            if (++pumpCount > 40000) {
                std::cout << "FAILED: pump guard tripped" << std::endl;
                std::exit(1);
            }
            appTick();
            conn->pump();
            ClientMessageType type;
            std::span<const std::byte> payload;
            if (LoginProtocol::parseFrame(std::span<const std::byte>(inbox.data(), inbox.size()),
                                          type, payload)) {
                outType = type;
                outPayload.assign(payload.begin(), payload.end());
                inbox.clear();
                return true;
            }
            usleep(500);
        }
        return false;
    }

    // 组合：发送 + 等响应。
    bool request(const std::function<void()>& appTick, ClientMessageType type,
                 std::span<const std::byte> payload,
                 ClientMessageType& outType, std::vector<std::byte>& outPayload) {
        send(type, payload);
        return recvFrame(appTick, outType, outPayload);
    }
};

// --- payload 编解码 helpers（对照 LoginProtocol.cpp 的 writeString 布局）---

std::vector<std::byte> encodeLoginRequest(const std::string& account,
                                          const std::string& password) {
    foundation::MemoryStream ms;
    ms.writeString(account);
    ms.writeString(password);
    return std::vector<std::byte>(ms.data(), ms.data() + ms.size());
}

std::vector<std::byte> encodeSelectRealmRequest(const std::string& realmId) {
    foundation::MemoryStream ms;
    ms.writeString(realmId);
    return std::vector<std::byte>(ms.data(), ms.data() + ms.size());
}

std::string readStr(foundation::MemoryStream& ms) { return ms.readString(); }

struct ParsedLoginResponse {
    bool success = false;
    std::string error;
    std::string token;
    std::uint32_t realmCount = 0;
};

bool decodeLoginResponse(std::span<const std::byte> payload, ParsedLoginResponse& out) {
    foundation::MemoryStream ms;
    ms.writeBytes(payload.data(), payload.size());
    ms.resetRead();
    out.success = ms.readUint8() != 0;
    out.error = readStr(ms);
    out.token = readStr(ms);
    out.realmCount = ms.readUint32();
    return true;
}

struct ParsedSelectRealmResponse {
    bool success = false;
    std::string error;
    std::string host;
    std::uint16_t port = 0;
    std::string token;
};

bool decodeSelectRealmResponse(std::span<const std::byte> payload,
                               ParsedSelectRealmResponse& out) {
    foundation::MemoryStream ms;
    ms.writeBytes(payload.data(), payload.size());
    ms.resetRead();
    out.success = ms.readUint8() != 0;
    out.error = readStr(ms);
    out.host = readStr(ms);
    out.port = ms.readUint16();
    out.token = readStr(ms);
    return true;
}

// HTTP 探针：非阻塞轮询 recv，泵应用 tick 驱动 accept/响应。
bool httpProbe(std::uint16_t port, const std::string& requestLine,
               std::string& firstLine, const std::function<void()>& pump) {
    int s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(s);
        return false;
    }
    std::string req = requestLine + " HTTP/1.0\r\n\r\n";
    if (::send(s, req.data(), req.size(), 0) < 0) {
        ::close(s);
        return false;
    }
    char buf[4096] = {};
    ssize_t n = -1;
    for (int i = 0; i < 4000; ++i) {
        if (pump) pump();
        n = ::recv(s, buf, sizeof(buf) - 1, MSG_DONTWAIT);
        if (n > 0) break;
        usleep(500);
    }
    ::close(s);
    if (n <= 0) return false;
    firstLine = std::string(buf, static_cast<std::size_t>(n));
    return true;
}

// 控制面裸客户端：直连 MachineDaemon 监听端口收发 RuntimeInvocation——
// 测试里代表“运维控制台”这一侧（机器方法请求-应答）。
struct ControlClient {
    std::shared_ptr<TcpConnection> conn;
    std::shared_ptr<NetworkTransport> transport;

    bool connect(std::uint16_t port) {
        conn = TcpConnection::create();
        if (!conn->connect("127.0.0.1", port)) return false;
        transport = std::make_shared<NetworkTransport>(conn);
        return true;
    }

    void settle(const std::function<void()>& tick) {
        for (int i = 0; i < 40; ++i) {
            tick();
            transport->tick();
            usleep(2000);
        }
    }

    bool request(ComponentId source, const std::string& method,
                 std::vector<std::byte> payload, const std::function<void()>& tick,
                 RuntimeInvocation& out) {
        RuntimeInvocation inv;
        inv.sourceComponent = source;
        inv.targetComponent = kMachineComponent;
        inv.method = method;
        inv.payload = std::move(payload);
        if (transport->send(std::move(inv)) != SendResult::Accepted) return false;
        transport->flush();
        for (int i = 0; i < 4000; ++i) {
            tick();
            transport->tick();
            usleep(500);
            if (transport->receive(source, &out, 1) > 0) return true;
        }
        return false;
    }
};

}  // namespace

int main() {
    std::cout << "LoginAppE2ETest:" << std::endl;

    // ------------------------------------------------------------------
    // 场景 A：authType=null + 限流 + 会话存储 + ops
    // ------------------------------------------------------------------
    {
        const std::uint16_t listenPort = freePort();
        const std::uint16_t opsPort = freePort();
        if (listenPort == 0 || opsPort == 0) FAIL("cannot find free ports");

        LoginAppConfig cfg;
        cfg.listenHost = "127.0.0.1";
        cfg.listenPort = listenPort;
        cfg.authType = "null";
        cfg.realms.push_back(RealmInfo{"realm1", "一区", "smooth", "127.0.0.1", 30001});
        cfg.realms.push_back(RealmInfo{"realm2", "二区", "busy", "127.0.0.1", 30002});
        cfg.ops.enabled = true;
        cfg.ops.port = opsPort;

        auto redis = std::make_shared<InMemoryRedisProvider>();
        auto store = std::make_shared<SessionStore>(redis);
        auto limiter = std::make_shared<RateLimiter>(redis);
        cfg.sessionStore = store;
        cfg.rateLimiter = limiter;
        cfg.rateLimitConfig.capacity = 2;  // 同一账号第 3 次登录被限流

        LoginApp app(std::move(cfg));
        TEST("init (authType=null + ops + redis-backed limiter)");
        app.init();
        PASS();

        FrameClient client;
        if (!client.connect(listenPort)) FAIL("client connect failed");
        auto appTick = [&app] { app.tick(); };
        for (int i = 0; i < 40; ++i) {  // 等 accept
            appTick();
            client.conn->pump();
            usleep(2000);
        }

        ClientMessageType type;
        std::vector<std::byte> payload;

        TEST("QueryRealms round trip returns both realms");
        if (!client.request(appTick, ClientMessageType::QueryRealms, {}, type, payload))
            FAIL("no response to QueryRealms");
        if (type != ClientMessageType::QueryRealmsResponse)
            FAIL("wrong response type");
        {
            foundation::MemoryStream ms;
            ms.writeBytes(payload.data(), payload.size());
            ms.resetRead();
            if (ms.readUint32() != 2) FAIL("realm count mismatch");
        }
        PASS();

        TEST("Login (null auth) succeeds and persists session");
        if (!client.request(appTick, ClientMessageType::Login,
                            encodeLoginRequest("alice", "any"), type, payload))
            FAIL("no response to Login");
        ParsedLoginResponse lr;
        if (!decodeLoginResponse(payload, lr)) FAIL("decode LoginResponse failed");
        if (!lr.success || lr.token.empty() || lr.realmCount != 2)
            FAIL("login response mismatch: success=" + std::to_string(lr.success) +
                 " token='" + lr.token + "'");
        auto stored = store->load(lr.token);
        if (!stored || stored->accountId != "alice")
            FAIL("session not persisted in SessionStore");
        PASS();

        TEST("rate limiter rejects third login burst on same account");
        for (int i = 0; i < 2; ++i) {  // capacity=2：前两次放行
            client.send(ClientMessageType::Login, encodeLoginRequest("bob", "x"));
            if (!client.recvFrame(appTick, type, payload))
                FAIL("no response to burst login #" + std::to_string(i + 1));
            if (!decodeLoginResponse(payload, lr) || !lr.success)
                FAIL("burst login #" + std::to_string(i + 1) + " should pass: " + lr.error);
        }
        client.send(ClientMessageType::Login, encodeLoginRequest("bob", "x"));
        if (!client.recvFrame(appTick, type, payload)) FAIL("no 3rd login response");
        if (!decodeLoginResponse(payload, lr) || lr.success || lr.error != "rate limited")
            FAIL("third burst should be rate limited, got error='" + lr.error + "'");
        PASS();

        TEST("SelectRealm hit returns host/port/token");
        if (!client.request(appTick, ClientMessageType::SelectRealm,
                            encodeSelectRealmRequest("realm1"), type, payload))
            FAIL("no response to SelectRealm");
        ParsedSelectRealmResponse sr;
        if (!decodeSelectRealmResponse(payload, sr)) FAIL("decode failed");
        if (!sr.success || sr.host != "127.0.0.1" || sr.port != 30001 || sr.token.empty())
            FAIL("select realm mismatch");
        PASS();

        TEST("SelectRealm miss reports realm not found");
        if (!client.request(appTick, ClientMessageType::SelectRealm,
                            encodeSelectRealmRequest("nope"), type, payload))
            FAIL("no response to SelectRealm(nope)");
        if (!decodeSelectRealmResponse(payload, sr) || sr.success || sr.error != "realm not found")
            FAIL("expected realm-not-found, got success=" +
                 std::to_string(sr.success) + " error='" + sr.error + "'");
        PASS();

        TEST("unknown message type does not kill server");
        std::vector<std::byte> junk(4, std::byte{0});
        client.send(static_cast<ClientMessageType>(77), junk);
        if (!client.request(appTick, ClientMessageType::QueryRealms, {}, type, payload))
            FAIL("server unresponsive after junk frame");
        PASS();

        TEST("ops /health and /inspect return 200");
        {
            auto opsTick = [&app] { app.tick(); };
            for (const char* path : {"/health", "/inspect"}) {
                std::string respLine;
                if (!httpProbe(opsPort, "GET " + std::string(path), respLine, opsTick))
                    FAIL(std::string("no response from ") + path);
                if (respLine.find(" 200 ") == std::string::npos)
                    FAIL(std::string(path) + " -> " + respLine.substr(0, 30));
            }
        }
        PASS();
    }

    // ------------------------------------------------------------------
    // 场景 B：authType=password（fallback 模式）
    // ------------------------------------------------------------------
    {
        const std::uint16_t listenPort = freePort();
        LoginAppConfig cfg;
        cfg.listenHost = "127.0.0.1";
        cfg.listenPort = listenPort;
        cfg.authType = "password";  // 非 null 非 db 且无 hub → fallback 校验非空

        LoginApp app(std::move(cfg));
        TEST("init (password fallback)");
        app.init();
        PASS();

        FrameClient client;
        if (!client.connect(listenPort)) FAIL("client connect failed");
        auto appTick = [&app] { app.tick(); };
        for (int i = 0; i < 40; ++i) {
            appTick();
            client.conn->pump();
            usleep(2000);
        }

        ClientMessageType type;
        std::vector<std::byte> payload;
        ParsedLoginResponse lr;

        TEST("empty password rejected");
        if (!client.request(appTick, ClientMessageType::Login,
                            encodeLoginRequest("carol", ""), type, payload))
            FAIL("no response");
        if (!decodeLoginResponse(payload, lr) || lr.success || lr.error != "invalid credentials")
            FAIL("empty password should be rejected");
        PASS();

        TEST("non-empty credentials accepted");
        if (!client.request(appTick, ClientMessageType::Login,
                            encodeLoginRequest("carol", "pw"), type, payload))
            FAIL("no response");
        if (!decodeLoginResponse(payload, lr) || !lr.success || lr.token.empty())
            FAIL("valid fallback login should succeed");
        PASS();
    }

    // ------------------------------------------------------------------
    // 场景 C：authType=db，后台线程驱动 DBApp（file 后端）
    // ------------------------------------------------------------------
    {
        const std::uint16_t dbListenPort = freePort();
        const std::string storeDir = "test_login_e2e_store";
        std::filesystem::remove_all(storeDir);

        DBApp::Config dbCfg;
        dbCfg.listenPort = dbListenPort;
        dbCfg.storePath = storeDir;
        dbCfg.storeBackend = "file";
        DBApp dbApp(std::move(dbCfg));
        TEST("DBApp boots for db-auth section");
        if (!dbApp.init()) FAIL("DBApp init failed");
        PASS();

        std::atomic<bool> dbRunning{true};
        std::thread dbThread([&dbApp, &dbRunning] {
            while (dbRunning.load()) {
                dbApp.tick();
                usleep(1000);
            }
        });

        const std::uint16_t listenPort = freePort();
        const std::uint16_t opsPort = freePort();
        LoginAppConfig cfg;
        cfg.listenHost = "127.0.0.1";
        cfg.listenPort = listenPort;
        cfg.authType = "db";
        cfg.dbHost = "127.0.0.1";
        cfg.dbPort = dbListenPort;
        cfg.ops.enabled = true;
        cfg.ops.port = opsPort;

        LoginApp app(std::move(cfg));
        TEST("init (authType=db connects to DBApp)");
        app.init();
        PASS();

        FrameClient client;
        if (!client.connect(listenPort)) FAIL("client connect failed");
        auto appTick = [&app] { app.tick(); };
        for (int i = 0; i < 40; ++i) {
            appTick();
            client.conn->pump();
            usleep(2000);
        }

        ClientMessageType type;
        std::vector<std::byte> payload;
        ParsedLoginResponse lr;

        TEST("db login auto-registers new account");
        if (!client.request(appTick, ClientMessageType::Login,
                            encodeLoginRequest("dave", "secret"), type, payload))
            FAIL("no response to db login");
        if (!decodeLoginResponse(payload, lr) || !lr.success || lr.token.empty())
            FAIL("auto-register login failed: " + lr.error);
        PASS();

        TEST("db login with correct password succeeds");
        if (!client.request(appTick, ClientMessageType::Login,
                            encodeLoginRequest("dave", "secret"), type, payload))
            FAIL("no response");
        if (!decodeLoginResponse(payload, lr) || !lr.success)
            FAIL("correct-password login failed: " + lr.error);
        PASS();

        TEST("db login with wrong password rejected");
        if (!client.request(appTick, ClientMessageType::Login,
                            encodeLoginRequest("dave", "wrong"), type, payload))
            FAIL("no response");
        if (!decodeLoginResponse(payload, lr) || lr.success || lr.error != "invalid credentials")
            FAIL("wrong password should be rejected, got error='" + lr.error + "'");
        PASS();

        TEST("ops /inspect in db mode reports transport stats");
        {
            auto opsTick = [&app] { app.tick(); };
            std::string respLine;
            if (!httpProbe(opsPort, "GET /inspect", respLine, opsTick))
                FAIL("no response from ops /inspect");
            if (respLine.find(" 200 ") == std::string::npos)
                FAIL("/inspect -> " + respLine.substr(0, 30));
        }
        PASS();

        TEST("second LoginApp on the same port cannot take over");
        {
            LoginAppConfig cfg2;
            cfg2.listenHost = "127.0.0.1";
            cfg2.listenPort = listenPort;  // 已被 app 监听
            cfg2.authType = "db";
            cfg2.dbHost = "127.0.0.1";
            cfg2.dbPort = dbListenPort;
            LoginApp app2(std::move(cfg2));
            app2.init();  // listen 失败 → 直接返回，不影响 app
        }
        // 原 app 仍然可用
        if (!client.request(appTick, ClientMessageType::QueryRealms, {}, type, payload))
            FAIL("first LoginApp unresponsive after takeover attempt");
        PASS();

        dbRunning.store(false);
        dbThread.join();
        dbApp.stop();
    }

    // ------------------------------------------------------------------
    // 场景 E：踢人联动（04 §8 Phase 2）——真实 TCP 生命周期：吊销关掉
    // 活登录，tick 清扫把连接与绑定一起出表（不留悬垂绑定键）
    // ------------------------------------------------------------------
    {
        const std::uint16_t listenPort = freePort();
        if (listenPort == 0) FAIL("cannot find a free port");

        LoginAppConfig cfg;
        cfg.listenHost = "127.0.0.1";
        cfg.listenPort = listenPort;
        cfg.authType = "null";

        LoginApp app(std::move(cfg));
        TEST("init (revocation linkage)");
        app.init();
        PASS();

        FrameClient client;
        if (!client.connect(listenPort)) FAIL("client connect failed");
        auto appTick = [&app] { app.tick(); };
        for (int i = 0; i < 40; ++i) {
            appTick();
            client.conn->pump();
            usleep(2000);
        }

        ClientMessageType type;
        std::vector<std::byte> payload;
        TEST("linkage setup: real-tcp login");
        if (!client.request(appTick, ClientMessageType::Login,
                            encodeLoginRequest("link", "x"), type, payload))
            FAIL("no response to Login");
        ParsedLoginResponse lr;
        if (!decodeLoginResponse(payload, lr) || !lr.success)
            FAIL("linkage login failed: " + lr.error);
        PASS();

        TEST("revocation closes the live login and cleanup drops the binding");
        if (app.handleSessionRevoked("link", "") != 1)
            FAIL("revocation must close the live login");
        for (int i = 0; i < 40; ++i) {  // 等 close 传播 + 清扫收敛
            appTick();
            client.conn->pump();
            usleep(2000);
        }
        if (app.handleSessionRevoked("link", "") != 0)
            FAIL("cleaned-up binding must not match again");
        PASS();
    }

    // ------------------------------------------------------------------
    // 场景 F：踢人联动生产接线（04 §8 Phase 2）——真实 MachineDaemon 吊销
    // 会话 → machine.session.revoked 推送沿真实 TCP 送达 LoginApp 的入站
    // 分发面 → 匹配的活跃登录连接被关闭、计数递增；不匹配者不受影响。
    // ------------------------------------------------------------------
    {
        const std::uint16_t machinePort = freePort();
        const std::uint16_t listenPort = freePort();
        if (machinePort == 0 || listenPort == 0) FAIL("cannot find free ports");

        auto redis = std::make_shared<InMemoryRedisProvider>();
        auto store = std::make_shared<SessionStore>(redis);

        MachineAgent agent(std::make_unique<LocalHostProbe>(),
                           std::make_unique<LocalProcessSupervisor>());
        MachineDaemon::Config mcfg;
        mcfg.listenHost = "127.0.0.1";
        mcfg.listenPort = machinePort;
        mcfg.sessionStore = store.get();
        mcfg.sessionNotifyComponent = kLoginComponent;  // 通知目标 = LoginApp
        mcfg.nodeOpsPolicy.trustedComponents = {kOpsComponent};
        // LoginApp 的注册探针走 inspect 面（ReadOnly 及以上）
        mcfg.roleBindings = {{kOpsComponent, AccessRole::Operator},
                             {kLoginComponent, AccessRole::ReadOnly}};
        MachineDaemon daemon(mcfg, agent);
        TEST("MachineDaemon starts with the notify target configured");
        if (!daemon.start()) FAIL("machine daemon start failed");
        if (daemon.localPort() == 0) FAIL("daemon has no local port");
        PASS();

        LoginAppConfig cfg;
        cfg.listenHost = "127.0.0.1";
        cfg.listenPort = listenPort;
        cfg.authType = "null";
        cfg.sessionStore = store;
        cfg.realms.push_back(RealmInfo{"realm1", "一区", "smooth", "127.0.0.1", 30001});
        cfg.localComponentId = kLoginComponent;
        cfg.machineHost = "127.0.0.1";
        cfg.machinePort = machinePort;
        cfg.machineComponentId = kMachineComponent;

        LoginApp app(std::move(cfg));
        TEST("LoginApp init wires the notify leg (outbound link + probe)");
        app.init();
        PASS();

        // 混合泵：通知是双向的，两个进程都要 tick。
        auto appTick = [&app, &daemon] {
            app.tick();
            daemon.tick();
        };

        FrameClient erin;
        if (!erin.connect(listenPort)) FAIL("erin connect failed");
        FrameClient frank;
        if (!frank.connect(listenPort)) FAIL("frank connect failed");
        for (int i = 0; i < 40; ++i) {  // 等 accept + 通知腿注册探针握手
            appTick();
            erin.conn->pump();
            frank.conn->pump();
            usleep(2000);
        }

        ClientMessageType type;
        std::vector<std::byte> payload;
        ParsedLoginResponse lr;
        ParsedSelectRealmResponse sr;
        std::string erinToken;
        std::string frankToken;

        TEST("two live logins (erin/frank) select the same realm");
        struct LoginCase {
            FrameClient* client;
            const char* account;
            std::string* token;
        };
        const LoginCase cases[] = {{&erin, "erin", &erinToken},
                                   {&frank, "frank", &frankToken}};
        for (const auto& one : cases) {
            if (!one.client->request(appTick, ClientMessageType::Login,
                                     encodeLoginRequest(one.account, "pw"), type, payload))
                FAIL(std::string("no response to Login(") + one.account + ")");
            if (!decodeLoginResponse(payload, lr) || !lr.success)
                FAIL(std::string(one.account) + " login failed: " + lr.error);
            *one.token = lr.token;
            if (!one.client->request(appTick, ClientMessageType::SelectRealm,
                                     encodeSelectRealmRequest("realm1"), type, payload))
                FAIL(std::string("no response to SelectRealm(") + one.account + ")");
            if (!decodeSelectRealmResponse(payload, sr) || !sr.success)
                FAIL(std::string(one.account) + " select realm failed: " + sr.error);
        }
        // 选领域把领域写回会话行（联动配对的存储侧口径）
        const auto erinRow = store->load(erinToken);
        if (!erinRow || erinRow->accountId != "erin" || erinRow->realmId != "realm1")
            FAIL("realm selection must be recorded in the session row");
        PASS();

        ControlClient ops;
        if (!ops.connect(daemon.localPort())) FAIL("ops connect failed");
        ops.settle(appTick);

        auto& revokedCounter = foundation::MetricsRegistry::instance().counter(
            "login_session_revoked_count");
        auto& notifyCounter = foundation::MetricsRegistry::instance().counter(
            "machine_session_notify_count");
        const auto revoked0 = revokedCounter.value();
        const auto notify0 = notifyCounter.value();

        RuntimeInvocation kickResp;
        TEST("operator kick pushes the revocation to LoginApp and closes the login");
        if (!ops.request(kOpsComponent, MachineMethod::kKickSession,
                         toBytes(erinToken), appTick, kickResp))
            FAIL("no response to machine.kick-session");
        if (kickResp.method != MachineMethod::kKickSessionOk ||
            kickResp.payload != std::vector<std::byte>{std::byte{0x01}})
            FAIL("kick must be accepted, got method=" + kickResp.method);
        if (notifyCounter.value() != notify0 + 1)
            FAIL("daemon must count the delivered notification");
        for (int i = 0; i < 200 && erin.conn->isConnected(); ++i) {
            appTick();
            erin.conn->pump();
            frank.conn->pump();
            usleep(2000);
        }
        if (erin.conn->isConnected())
            FAIL("the revoked account's live login must be closed by the push");
        if (!frank.conn->isConnected())
            FAIL("a non-matching account must stay connected");
        if (revokedCounter.value() != revoked0 + 1)
            FAIL("LoginApp must count exactly one closed login");
        // 存储行已被吊销（通知是提示，吊销事实以存储为准）
        if (store->load(erinToken))
            FAIL("the session row must be gone after the kick");
        PASS();

        TEST("kick of an unknown token is rejected and closes nothing");
        if (!ops.request(kOpsComponent, MachineMethod::kKickSession,
                         toBytes("no-such-token"), appTick, kickResp))
            FAIL("no response to the unknown-token kick");
        if (kickResp.method != MachineMethod::kError)
            FAIL("unknown token must be rejected, got " + kickResp.method);
        if (revokedCounter.value() != revoked0 + 1)
            FAIL("a rejected kick must not close logins");
        if (!frank.conn->isConnected())
            FAIL("a rejected kick must leave other logins alone");
        PASS();

        TEST("cleanup drops the closed binding (no stale key after close)");
        for (int i = 0; i < 40; ++i) {
            appTick();
            usleep(2000);
        }
        if (app.handleSessionRevoked("erin", "realm1") != 0)
            FAIL("the cleaned-up binding must not match again");
        PASS();

        daemon.stop();
    }

    std::cout << "\nAll LoginApp E2E tests passed!" << std::endl;
    return 0;
}
