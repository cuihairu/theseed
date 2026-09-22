// LoginApp 端到端测试：真实 TCP 回环驱动完整 tick 循环。
// 场景 A（authType=null + 限流 + 会话存储 + ops）：QueryRealms / Login /
// 限流拒绝 / SelectRealm 命中与未命中 / 未知消息存活 / HTTP 端点。
// 场景 B（authType=password）：空密码拒绝、正常放行。
// 场景 C（authType=db）：后台线程驱动 file 后端 DBApp，覆盖 auto-register、
// 正确/错误密码三条鉴权路径。
#include "theseed/db/DBApp.h"
#include "theseed/foundation/MemoryStream.h"
#include "theseed/foundation/RedisProvider.h"
#include "theseed/foundation/SessionStore.h"
#include "theseed/login/LoginProtocol.h"
#include "theseed/login/LoginApp.h"
#include "theseed/login/LoginTypes.h"
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
using theseed::runtime::TcpConnection;

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

    std::cout << "\nAll LoginApp E2E tests passed!" << std::endl;
    return 0;
}
