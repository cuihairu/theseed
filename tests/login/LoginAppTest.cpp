#include "theseed/login/LoginApp.h"
#include "theseed/login/ClientSession.h"
#include "theseed/login/LoginProtocol.h"
#include "theseed/login/SessionToken.h"
#include "theseed/db/DBProtocol.h"
#include "theseed/foundation/Metrics.h"
#include "theseed/runtime/InMemoryBytePipe.h"

#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace theseed::login;
using namespace theseed::runtime;
using namespace theseed::db;
namespace db = theseed::db;

#define TEST(name)                                      \
    do {                                                \
        std::cout << "  " << name << "... ";            \
    } while (0)

#define PASS() std::cout << "OK" << std::endl
#define FAIL(msg)                           \
    do {                                    \
        std::cout << "FAILED: " << msg << std::endl; \
        return 1;                           \
    } while (0)

// Helper: create a pipe pair simulating a connected client
struct MockClient {
    std::shared_ptr<InMemoryBytePipe> clientPipe;
    std::shared_ptr<InMemoryBytePipe> serverPipe;
    std::vector<std::byte> receivedData;

    MockClient() {
        auto pair = InMemoryBytePipe::createPair();
        clientPipe = pair.first;
        serverPipe = pair.second;

        clientPipe->setOnReceived([this](std::span<const std::byte> data) {
            receivedData.insert(receivedData.end(), data.begin(), data.end());
        });
    }

    void sendToServer(ClientMessageType type, std::span<const std::byte> payload) {
        auto frame = LoginProtocol::frameMessage(type, payload);
        // Client writes to clientPipe → data arrives at serverPipe
        clientPipe->write(std::span<const std::byte>(frame.data(), frame.size()));
    }

    void pump() {
        // Pump both directions
        clientPipe->pump();
        serverPipe->pump();
    }

    bool parseResponse(ClientMessageType& outType, std::span<const std::byte>& outPayload) {
        if (receivedData.size() < LoginProtocol::kHeaderSize) return false;
        return LoginProtocol::parseFrame(
            std::span<const std::byte>(receivedData.data(), receivedData.size()),
            outType, outPayload);
    }

    void clearReceived() {
        receivedData.clear();
    }
};

// Manually encode a login payload
static std::vector<std::byte> encodeLoginPayload(const std::string& account,
                                                   const std::string& password) {
    std::vector<std::byte> out;
    auto appendStr = [&out](const std::string& s) {
        auto len = static_cast<uint32_t>(s.size());
        out.push_back(std::byte(len & 0xFF));
        out.push_back(std::byte((len >> 8) & 0xFF));
        out.push_back(std::byte((len >> 16) & 0xFF));
        out.push_back(std::byte((len >> 24) & 0xFF));
        for (char c : s) out.push_back(static_cast<std::byte>(c));
    };
    appendStr(account);
    appendStr(password);
    return out;
}

static std::vector<std::byte> encodeRealmId(const std::string& realmId) {
    std::vector<std::byte> out;
    auto len = static_cast<uint32_t>(realmId.size());
    out.push_back(std::byte(len & 0xFF));
    out.push_back(std::byte((len >> 8) & 0xFF));
    out.push_back(std::byte((len >> 16) & 0xFF));
    out.push_back(std::byte((len >> 24) & 0xFF));
    for (char c : realmId) out.push_back(static_cast<std::byte>(c));
    return out;
}

// 模拟 DBApp 的 IRuntimeTransport：send 时按请求 method 同步入队应答，
// 让 dbRequest 的等待循环在单线程内立即拿到结果；Silent 模式吞掉请求
// 以触发超时，Closed 模式让 send 直接 NotConnected。
class FakeDbTransport final : public theseed::runtime::IRuntimeTransport {
public:
    enum class Mode { Canned, Silent, WrongMethod, Closed };

    FakeDbTransport(Mode queryMode, std::vector<std::byte> queryPayload,
                    Mode createMode = Mode::Canned,
                    std::vector<std::byte> createPayload = {})
        : queryMode_(queryMode), queryPayload_(std::move(queryPayload)),
          createMode_(createMode), createPayload_(std::move(createPayload)) {}

    theseed::runtime::SendResult send(theseed::runtime::RuntimeInvocation inv) override {
        Mode mode = modeFor(inv.method);
        if (mode == Mode::Closed) {
            return theseed::runtime::SendResult::NotConnected;
        }
        if (mode == Mode::Silent) {
            return theseed::runtime::SendResult::Accepted;  // 吞掉，永不回
        }
        theseed::runtime::RuntimeInvocation resp;
        resp.sourceComponent = inv.targetComponent;
        resp.targetComponent = inv.sourceComponent;
        if (mode == Mode::WrongMethod) {
            resp.method = "db.bogus.ok";  // 杂散应答：等待方必须丢弃
        } else {
            resp.method = inv.method + ".ok";
        }
        resp.payload = payloadFor(inv.method);
        inbox_.push_back(std::move(resp));
        return theseed::runtime::SendResult::Accepted;
    }

    std::size_t receive(theseed::runtime::ComponentId,
                        theseed::runtime::RuntimeInvocation* out,
                        std::size_t) override {
        if (inbox_.empty()) return 0;
        *out = inbox_.front();
        inbox_.pop_front();
        return 1;
    }

    std::size_t pendingCount() const override { return inbox_.size(); }
    void flush() override {}
    theseed::runtime::TransportStats stats() const override { return {}; }
    void tick() override {}

private:
    Mode modeFor(const std::string& method) const {
        return method == theseed::db::DBMethod::kQueryAccount ? queryMode_ : createMode_;
    }
    const std::vector<std::byte>& payloadFor(const std::string& method) const {
        return method == theseed::db::DBMethod::kQueryAccount ? queryPayload_ : createPayload_;
    }

    Mode queryMode_;
    std::vector<std::byte> queryPayload_;
    Mode createMode_;
    std::vector<std::byte> createPayload_;
    std::deque<theseed::runtime::RuntimeInvocation> inbox_;
};

// db auth 场景的公共 config：port 0 让 init() 的 listen 必成功（随机端口），
// db 走注入的 FakeDbTransport 而不是真实 TCP。
static LoginAppConfig makeDbConfig(
    std::function<std::shared_ptr<theseed::runtime::IRuntimeTransport>(
        const std::string&, std::uint16_t)> factory) {
    LoginAppConfig config;
    config.listenHost = "127.0.0.1";
    config.listenPort = 0;
    config.authType = "db";
    config.dbHost = "fake";  // 非空即可：factory 注入时不真正 connect
    config.dbRequestTimeout = std::chrono::milliseconds{20};
    config.dbTransportFactory = std::move(factory);
    return config;
}

// 驱动一次登录请求，解析 LoginResponse（[u8 success][u32 errLen][error][u32 tokLen][token]）。
static bool runLogin(LoginApp& app, const std::string& account, const std::string& password,
                     bool& outSuccess, std::string& outError, std::string& outToken) {
    MockClient client;
    ClientSession session(client.serverPipe);
    session.setMessageCallback([&app, &session](ClientMessageType type,
                                                std::span<const std::byte> payload) {
        app.handleClientMessage(&session, type, payload);
    });

    auto payload = encodeLoginPayload(account, password);
    client.sendToServer(ClientMessageType::Login,
                        std::span<const std::byte>(payload.data(), payload.size()));
    client.pump();
    session.pump();
    client.pump();

    ClientMessageType respType;
    std::span<const std::byte> respPayload;
    if (!client.parseResponse(respType, respPayload)) return false;
    if (respType != ClientMessageType::LoginResponse) return false;

    auto readU32 = [&respPayload](std::size_t& off) {
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            v |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(respPayload[off + i]))
                 << (8 * i);
        }
        off += 4;
        return v;
    };

    std::size_t off = 0;
    outSuccess = std::to_integer<std::uint8_t>(respPayload[off]) != 0;
    ++off;
    std::uint32_t errLen = readU32(off);
    outError.assign(reinterpret_cast<const char*>(respPayload.data() + off), errLen);
    off += errLen;
    std::uint32_t tokLen = readU32(off);
    // readU32 内部已前移 off，这里不再重复 +4（重复会错位吞入尾部 NUL）
    outToken.assign(reinterpret_cast<const char*>(respPayload.data() + off), tokLen);
    return true;
}

int main() {
    std::cout << "LoginApp tests:" << std::endl;

    TEST("login with null auth succeeds");
    {
        LoginAppConfig config;
        config.authType = "null";
        RealmInfo r;
        r.realmId = "default";
        r.name = "Default";
        r.status = "smooth";
        r.host = "127.0.0.1";
        r.port = 20000;
        config.realms.push_back(r);

        LoginApp app(std::move(config));

        // Skip TCP, test protocol logic directly via InMemoryBytePipe
        MockClient client;
        ClientSession session(client.serverPipe);

        // Wire session to LoginApp
        session.setMessageCallback([&app, &session](ClientMessageType type,
                                                      std::span<const std::byte> payload) {
            // Replicate LoginApp's message dispatch
            if (type == ClientMessageType::Login) {
                std::string account, password;
                if (LoginProtocol::decodeLogin(payload, account, password)) {
                    // Direct test: encode login response manually
                    LoginResponse resp;
                    resp.success = true;
                    resp.token = SessionToken::issue(account, "");
                    resp.realms = app.realms();
                    auto data = LoginProtocol::encodeLoginResponse(resp);
                    session.send(std::span<const std::byte>(data.data(), data.size()));
                }
            }
        });

        auto loginPayload = encodeLoginPayload("testuser", "testpass");
        client.sendToServer(ClientMessageType::Login,
                            std::span<const std::byte>(loginPayload.data(), loginPayload.size()));
        client.pump();
        session.pump();
        client.pump();

        ClientMessageType respType;
        std::span<const std::byte> respPayload;
        if (!client.parseResponse(respType, respPayload)) FAIL("no response");
        if (respType != ClientMessageType::LoginResponse) FAIL("wrong response type");
    }
    PASS();

    TEST("session token validates correctly");
    {
        auto token = SessionToken::issue("user1", "realm1");
        std::string accountId, realmId;
        if (!SessionToken::validate(token, accountId, realmId)) FAIL("token invalid");
        if (accountId != "user1") FAIL("accountId mismatch");
        if (realmId != "realm1") FAIL("realmId mismatch");
    }
    PASS();

    TEST("realms config is accessible");
    {
        LoginAppConfig config;
        RealmInfo r1;
        r1.realmId = "east";
        r1.name = "East";
        r1.status = "smooth";
        r1.host = "10.0.1.1";
        r1.port = 20000;
        RealmInfo r2;
        r2.realmId = "south";
        r2.name = "South";
        r2.status = "busy";
        r2.host = "10.0.2.1";
        r2.port = 20000;
        config.realms.push_back(r1);
        config.realms.push_back(r2);

        LoginApp app(std::move(config));
        if (app.realms().size() != 2) FAIL("realm count");
        if (app.realms()[0].realmId != "east") FAIL("first realm id");
        if (app.realms()[1].realmId != "south") FAIL("second realm id");
    }
    PASS();

    // === db auth 场景组：FakeDbTransport 注入 + dbRequest 超时语义 ===

    TEST("db auth: query hit with matching password logs in");
    {
        auto fake = std::make_shared<FakeDbTransport>(
            FakeDbTransport::Mode::Canned,
            db::DBProtocol::encodeQueryAccountResponse(true, 42, "pw"));
        LoginApp app(makeDbConfig([fake](const std::string&, std::uint16_t) {
            return std::shared_ptr<theseed::runtime::IRuntimeTransport>(fake);
        }));
        app.init();

        bool success = false;
        std::string error, token;
        if (!runLogin(app, "alice", "pw", success, error, token)) FAIL("no login response");
        if (!success) FAIL("login should succeed via canned query hit, error=" + error);
        if (token.empty()) FAIL("token should be non-empty");
    }
    PASS();

    TEST("db auth: unknown account auto-registers");
    {
        auto fake = std::make_shared<FakeDbTransport>(
            FakeDbTransport::Mode::Canned,
            db::DBProtocol::encodeQueryAccountResponse(false, 0, ""),
            FakeDbTransport::Mode::Canned,
            db::DBProtocol::encodeCreateAccountResponse(true, 7));
        LoginApp app(makeDbConfig([fake](const std::string&, std::uint16_t) {
            return std::shared_ptr<theseed::runtime::IRuntimeTransport>(fake);
        }));
        app.init();

        bool success = false;
        std::string error, token;
        if (!runLogin(app, "newbie", "pw", success, error, token)) FAIL("no login response");
        if (!success) FAIL("auto-register should succeed, error=" + error);
        if (token.empty()) FAIL("token should be non-empty");
    }
    PASS();

    TEST("db auth: failed creation reports account creation failed");
    {
        // query 回 not-found、create 永不应答：dbRequest 超时 → 落入
        // "account creation failed" 分支（challenge_failure_count）。
        auto fake = std::make_shared<FakeDbTransport>(
            FakeDbTransport::Mode::Canned,
            db::DBProtocol::encodeQueryAccountResponse(false, 0, ""),
            FakeDbTransport::Mode::Silent);
        LoginApp app(makeDbConfig([fake](const std::string&, std::uint16_t) {
            return std::shared_ptr<theseed::runtime::IRuntimeTransport>(fake);
        }));
        app.init();

        auto& challenge = theseed::foundation::MetricsRegistry::instance()
                              .counter("challenge_failure_count", "login auth failures");
        auto before = challenge.value();

        bool success = true;
        std::string error, token;
        if (!runLogin(app, "ghost", "pw", success, error, token)) FAIL("no login response");
        if (success) FAIL("login must fail when creation times out");
        if (error != "account creation failed") FAIL("unexpected error: " + error);
        if (challenge.value() != before + 1) FAIL("challenge_failure_count should +1");
    }
    PASS();

    TEST("db auth: silent DBApp times out as database unavailable");
    {
        auto fake = std::make_shared<FakeDbTransport>(
            FakeDbTransport::Mode::Silent, std::vector<std::byte>{});
        LoginApp app(makeDbConfig([fake](const std::string&, std::uint16_t) {
            return std::shared_ptr<theseed::runtime::IRuntimeTransport>(fake);
        }));
        app.init();

        auto& dbDown = theseed::foundation::MetricsRegistry::instance()
                           .counter("login_db_unavailable_count",
                                    "login attempts aborted because DBApp did not answer in time");
        auto before = dbDown.value();

        bool success = true;
        std::string error, token;
        if (!runLogin(app, "alice", "pw", success, error, token)) FAIL("no login response");
        if (success) FAIL("login must fail when DBApp is silent");
        if (error != "database unavailable") FAIL("unexpected error: " + error);
        if (dbDown.value() != before + 1) FAIL("login_db_unavailable_count should +1");
    }
    PASS();

    TEST("db auth: stray-method responses are discarded until timeout");
    {
        auto fake = std::make_shared<FakeDbTransport>(
            FakeDbTransport::Mode::WrongMethod, std::vector<std::byte>{});
        LoginApp app(makeDbConfig([fake](const std::string&, std::uint16_t) {
            return std::shared_ptr<theseed::runtime::IRuntimeTransport>(fake);
        }));
        app.init();

        bool success = true;
        std::string error, token;
        if (!runLogin(app, "alice", "pw", success, error, token)) FAIL("no login response");
        if (success) FAIL("stray responses must not satisfy the request");
        if (error != "database unavailable") FAIL("unexpected error: " + error);
    }
    PASS();

    TEST("db auth: closed DBApp fails fast without waiting for timeout");
    {
        auto fake = std::make_shared<FakeDbTransport>(
            FakeDbTransport::Mode::Closed, std::vector<std::byte>{});
        LoginApp app(makeDbConfig([fake](const std::string&, std::uint16_t) {
            return std::shared_ptr<theseed::runtime::IRuntimeTransport>(fake);
        }));
        app.init();

        bool success = true;
        std::string error, token;
        if (!runLogin(app, "alice", "pw", success, error, token)) FAIL("no login response");
        if (success) FAIL("login must fail when DBApp connection is closed");
        if (error != "database unavailable") FAIL("unexpected error: " + error);
    }
    PASS();

    TEST("db auth: null factory transport behaves as NotConnected");
    {
        LoginApp app(makeDbConfig([](const std::string&, std::uint16_t) {
            return std::shared_ptr<theseed::runtime::IRuntimeTransport>{};
        }));
        app.init();

        bool success = true;
        std::string error, token;
        if (!runLogin(app, "alice", "pw", success, error, token)) FAIL("no login response");
        if (success) FAIL("login must fail without a db transport");
        if (error != "database unavailable") FAIL("unexpected error: " + error);
    }
    PASS();

    // --- fallback password 模式：非空账密成功；空密码走假臂失败 ---
    TEST("password fallback auth branches");
    {
        LoginAppConfig config;
        config.listenHost = "127.0.0.1";
        config.listenPort = 0;
        config.authType = "password";   // 非 "db"：init 里 db 分支整体跳过
        LoginApp app(config);
        app.init();

        bool success = false;
        std::string error, token;
        if (!runLogin(app, "bob", "pw", success, error, token)) FAIL("no login response");
        if (!success || token.empty()) FAIL("password auth should succeed");

        // 空密码 → fallback 校验假臂，登录失败且无 token。
        bool ok2 = false;
        std::string error2, token2;
        if (!runLogin(app, "bob", "", ok2, error2, token2)) FAIL("no response for empty pw");
        if (ok2 || !token2.empty()) FAIL("empty password should fail");

        // 空 account → 限流守卫与 fallback 校验的 account 空假臂，登录失败。
        bool ok3 = false;
        std::string error3, token3;
        if (!runLogin(app, "", "pw", ok3, error3, token3)) FAIL("no response for empty account");
        if (ok3 || !token3.empty()) FAIL("empty account should fail");
    }
    PASS();

    // --- 限流与会话存储：capacity=1 耗尽后第二次登录被拒；成功登录写入 session store ---
    TEST("rate limiter and session store branches");
    {
        auto redis = std::make_shared<theseed::foundation::InMemoryRedisProvider>();
        auto limiter = std::make_shared<theseed::foundation::RateLimiter>(redis);
        theseed::foundation::RateLimiter::Config lc;
        lc.capacity = 1;
        lc.refillInterval = std::chrono::hours{1};
        auto store = std::make_shared<theseed::foundation::SessionStore>(redis);

        LoginAppConfig config;
        config.listenHost = "127.0.0.1";
        config.listenPort = 0;
        config.authType = "password";
        config.rateLimiter = limiter;
        config.rateLimitConfig = lc;
        config.sessionStore = store;
        LoginApp app(config);
        app.init();

        bool success = false;
        std::string error, token;
        if (!runLogin(app, "carl", "pw", success, error, token)) FAIL("no login response");
        if (!success) FAIL("first login should pass limiter");
        if (token.empty()) FAIL("token required for session store");
        // token 已写入 store（persistSession 真臂）。
        auto stored = store->load(token);
        if (!stored.has_value() || stored->accountId != "carl") FAIL("session not stored");

        // 第二次：桶已空 → rate limited（203/204 真臂）。
        bool ok2 = true;
        std::string error2, token2;
        if (!runLogin(app, "carl", "pw", ok2, error2, token2)) FAIL("no response for limited");
        if (ok2) FAIL("second login should be rate limited");
        if (error2 != "rate limited") FAIL("unexpected error: " + error2);

        // 空 account 在限流守卫第二段短路（203 假臂）：先于 tryConsume 返回，不查桶。
        bool ok3 = true;
        std::string error3, token3;
        if (!runLogin(app, "", "pw", ok3, error3, token3)) FAIL("no response for empty acct w/ limiter");
        if (ok3) FAIL("empty account should fail before limiter");
    }
    PASS();

    // --- 坏 payload：decode 失败被静默丢弃（177/187 假臂），连接保持 ---
    TEST("malformed payloads are dropped silently");
    {
        LoginAppConfig config;
        config.listenHost = "127.0.0.1";
        config.listenPort = 0;
        config.authType = "password";
        LoginApp app(config);
        app.init();

        MockClient client;
        ClientSession session(client.serverPipe);
        session.setMessageCallback([&app, &session](ClientMessageType type,
                                                    std::span<const std::byte> payload) {
            app.handleClientMessage(&session, type, payload);
        });

        // Login payload：account 长度谎报。
        std::vector<std::byte> bad{std::byte{0xFF}, std::byte{0xFF}, std::byte{0}, std::byte{0}};
        client.sendToServer(ClientMessageType::Login,
                            std::span<const std::byte>(bad.data(), bad.size()));
        client.pump();
        session.pump();
        client.pump();
        ClientMessageType t1{};
        std::span<const std::byte> p1;
        if (client.parseResponse(t1, p1)) FAIL("bad login payload must not respond");

        // SelectRealm payload：长度谎报。
        client.clearReceived();
        client.sendToServer(ClientMessageType::SelectRealm,
                            std::span<const std::byte>(bad.data(), bad.size()));
        client.pump();
        session.pump();
        client.pump();
        if (client.parseResponse(t1, p1)) FAIL("bad selectRealm payload must not respond");
    }
    PASS();

    // --- authType=db 但 dbHost 为空：init 跳过 hub；handleLogin 走 fallback 分支 ---
    TEST("db auth without dbHost falls back to password check");
    {
        LoginAppConfig config;
        config.listenHost = "127.0.0.1";
        config.listenPort = 0;
        config.authType = "db";
        config.dbHost = "";   // 空 host：36 行短路假臂，不建 hub
        LoginApp app(config);
        app.init();

        bool success = false;
        std::string error, token;
        if (!runLogin(app, "dave", "pw", success, error, token)) FAIL("no login response");
        // 233 行短路假臂 → fallback password 检查 → 成功。
        if (!success || token.empty()) FAIL("fallback after empty dbHost should succeed");
    }
    PASS();

    // --- ops 面板开启：init 起 OpsServer（55/70 段），stop 收尾 ---
    TEST("ops enabled init and stop");
    {
        LoginAppConfig config;
        config.listenHost = "127.0.0.1";
        config.listenPort = 0;
        config.authType = "password";
        config.ops.enabled = true;
        config.ops.port = 0;   // ephemeral，避免端口冲突
        LoginApp app(config);
        app.init();
        app.tick();
        app.stop();
    }
    PASS();

    // --- ClientSession 防御臂：无管道会话全程 no-throw；空回调；半帧缓冲 ---
    TEST("client session defensive arms");
    {
        // 无管道：send/close/isConnected/pump/析构全部安全。
        {
            ClientSession bare(nullptr);
            bare.send(std::span<const std::byte>{});
            bool ok = !bare.isConnected();
            bare.pump();
            bare.close();
            ok = ok && !bare.isConnected();   // 仍为假
            if (!ok) FAIL("null-pipe session misbehaved");
        }

        // 关闭后的会话：send 走 isConnected 假臂静默丢弃，重复 close 幂等。
        {
            MockClient client;
            ClientSession session(client.serverPipe);
            session.close();
            auto payload = encodeLoginPayload("a", "b");
            session.send(std::span<const std::byte>(payload.data(), payload.size()));
            if (session.isConnected()) FAIL("closed session must report disconnected");
            session.close();
        }

        // 不设置消息回调：完整帧被消费但不触发任何回调（76 假臂）。
        {
            MockClient client;
            ClientSession session(client.serverPipe);
            auto payload = encodeLoginPayload("a", "b");
            client.sendToServer(ClientMessageType::Login,
                                std::span<const std::byte>(payload.data(), payload.size()));
            client.pump();
            session.pump();
            client.pump();
        }

        // 半帧：前半到达不回调，补齐后回调一次（70 真臂）。
        {
            MockClient client;
            int messages = 0;
            ClientSession session(client.serverPipe);
            session.setMessageCallback([&messages](ClientMessageType, std::span<const std::byte>) {
                ++messages;
            });

            auto payload = encodeLoginPayload("a", "b");
            auto frame = LoginProtocol::frameMessage(ClientMessageType::Login,
                                                     std::span<const std::byte>(payload.data(), payload.size()));
            const auto half = frame.size() / 2;
            client.clientPipe->write(std::span<const std::byte>(frame.data(), half));
            client.pump();
            session.pump();
            client.pump();
            if (messages != 0) FAIL("half frame must not dispatch");

            client.clientPipe->write(std::span<const std::byte>(frame.data() + half, frame.size() - half));
            client.pump();
            session.pump();
            client.pump();
            if (messages != 1) FAIL("completed frame must dispatch once");
        }
    }
    PASS();

    std::cout << "\nAll LoginApp tests passed!" << std::endl;
    return 0;
}
