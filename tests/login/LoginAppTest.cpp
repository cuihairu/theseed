#include "theseed/login/LoginApp.h"
#include "theseed/login/ClientSession.h"
#include "theseed/login/LoginProtocol.h"
#include "theseed/login/SessionToken.h"
#include "theseed/control/machine/MachineDaemon.h"
#include "theseed/db/DBProtocol.h"
#include "theseed/foundation/Metrics.h"
#include "theseed/runtime/InMemoryBytePipe.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <thread>
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
#ifndef _WIN32
#include <unistd.h>
#else
#include <windows.h>
#endif

#ifndef _WIN32
#define SLEEP_MS(ms) usleep((ms) * 1000)
#else
#define SLEEP_MS(ms) Sleep(ms)
#endif

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

// 模拟 DBApp 的 IRuntimeTransport：send 时按请求 method 同步入队应答，
// 让 dbRequest 的等待循环在单线程内立即拿到结果；Silent 模式吞掉请求
// 以触发超时，Closed 模式让 send 直接 NotConnected。method 路由：
// queryAccount 走 queryMode_，其余（createAccount 与监督探针 listTypes）
// 走 createMode_。alive/sends/probe 供 db 腿韧性监督测试：alive 模拟
// 链路活性（置假 = DBApp 侧重启断连，监督面经 isConnected 读到），
// sends 计数发出的请求，probe 捕获最近一条接通探针。
class FakeDbTransport final : public theseed::runtime::IRuntimeTransport {
public:
    enum class Mode { Canned, Silent, WrongMethod, Closed };

    FakeDbTransport(Mode queryMode, std::vector<std::byte> queryPayload,
                    Mode createMode = Mode::Canned,
                    std::vector<std::byte> createPayload = {})
        : queryMode_(queryMode), queryPayload_(std::move(queryPayload)),
          createMode_(createMode), createPayload_(std::move(createPayload)) {}

    bool isConnected() const override { return alive; }

    theseed::runtime::SendResult send(theseed::runtime::RuntimeInvocation inv) override {
        ++sends;
        if (inv.method == theseed::db::DBMethod::kListTypes) {
            probe = inv;  // db 腿接通探针（监督面重连时重发的那条）
        }
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

    // 韧性测试观测面：probe = 最近一条接通探针，sends = 发出的请求数，
    // alive = 链路活性（置假模拟 DBApp 重启断连，监督面经 isConnected
    // 读到）。与 FakeMachineTransport 同款。
    theseed::runtime::RuntimeInvocation probe;
    std::size_t sends = 0;
    bool alive = true;

private:
    Mode modeFor(const std::string& method) const {
        // 探针（kListTypes）走 createMode_ 维：韧性臂用 createMode 编排
        // 探针的应答形态（Canned=应答 / Silent=吞 / Closed=发不出）。
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

// 模拟 MachineDaemon 通知腿的 IRuntimeTransport：捕获注册探针并按模式
// 入队应答（Ack = snapshot.ok，Reject = machine.error，Silent = 吞掉，
// Closed = send 拒绝）；测试可主动 enqueue 任意入站推送。单线程假设与
// FakeDbTransport 同。alive 模拟链路活性（置假 = daemon 侧重启断连），
// 监督面经 IRuntimeTransport::isConnected 读到它；sends 计数发出的探针。
class FakeMachineTransport final : public theseed::runtime::IRuntimeTransport {
public:
    enum class Mode { Ack, Reject, Silent, Closed };

    explicit FakeMachineTransport(Mode mode) : mode_(mode) {}

    bool isConnected() const override { return alive; }

    theseed::runtime::SendResult send(
        theseed::runtime::RuntimeInvocation inv) override {
        probe = inv;  // LoginApp 通知腿只会发注册探针
        ++sends;
        if (mode_ == Mode::Closed) {
            return theseed::runtime::SendResult::NotConnected;
        }
        if (mode_ == Mode::Silent) {
            return theseed::runtime::SendResult::Accepted;
        }
        theseed::runtime::RuntimeInvocation resp;
        resp.sourceComponent = inv.targetComponent;
        resp.targetComponent = inv.sourceComponent;
        if (mode_ == Mode::Reject) {
            resp.method = theseed::control::machine::MachineMethod::kError;
            const std::string reason = "machine not trusted";
            resp.payload.reserve(reason.size());
            for (const char ch : reason) {
                resp.payload.push_back(static_cast<std::byte>(ch));
            }
        } else {
            resp.method =
                theseed::control::machine::MachineMethod::kSnapshotOk;
        }
        inbox_.push_back(std::move(resp));
        return theseed::runtime::SendResult::Accepted;
    }

    void enqueue(theseed::runtime::RuntimeInvocation inv) {
        inbox_.push_back(std::move(inv));
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

    theseed::runtime::RuntimeInvocation probe;
    std::size_t sends = 0;
    bool alive = true;

private:
    Mode mode_;
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

    // --- §8 Phase 2 踢人联动：吊销通知（account+realm）关本地登录连接 ---
    TEST("session revocation linkage closes matching live logins");
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

        // 单字符串载荷（SelectRealm 的 realmId，长度前缀小端——与
        // encodeLoginPayload 的 appendStr 同格式）。
        auto stringPayload = [](const std::string& s) {
            std::vector<std::byte> out;
            const auto len = static_cast<std::uint32_t>(s.size());
            out.push_back(std::byte(len & 0xFF));
            out.push_back(std::byte((len >> 8) & 0xFF));
            out.push_back(std::byte((len >> 16) & 0xFF));
            out.push_back(std::byte((len >> 24) & 0xFF));
            for (const char c : s) out.push_back(static_cast<std::byte>(c));
            return out;
        };
        auto driveLogin = [&app](MockClient& client, ClientSession& session,
                                 const std::string& account) {
            session.setMessageCallback(
                [&app, &session](ClientMessageType type,
                                 std::span<const std::byte> payload) {
                    app.handleClientMessage(&session, type, payload);
                });
            auto payload = encodeLoginPayload(account, "pw");
            client.clearReceived();
            client.sendToServer(
                ClientMessageType::Login,
                std::span<const std::byte>(payload.data(), payload.size()));
            client.pump();
            session.pump();
            client.pump();
            ClientMessageType type{};
            std::span<const std::byte> body;
            return client.parseResponse(type, body) &&
                   type == ClientMessageType::LoginResponse &&
                   !body.empty() && std::to_integer<std::uint8_t>(body[0]) != 0;
        };

        // 块级存活的两个登录连接（绑定表键的生命周期与块一致）
        MockClient carlClient;
        ClientSession carlSession(carlClient.serverPipe);
        if (!driveLogin(carlClient, carlSession, "carl"))
            FAIL("carl login failed");
        MockClient amyClient;
        ClientSession amySession(amyClient.serverPipe);
        if (!driveLogin(amyClient, amySession, "amy"))
            FAIL("amy login failed");

        // carl 选领域 → 绑定领域补齐；amy 不选（绑定领域保持空串）
        auto realmPayload = stringPayload("default");
        carlClient.clearReceived();
        carlClient.sendToServer(
            ClientMessageType::SelectRealm,
            std::span<const std::byte>(realmPayload.data(),
                                       realmPayload.size()));
        carlClient.pump();
        carlSession.pump();
        carlClient.pump();
        ClientMessageType realmType{};
        std::span<const std::byte> realmBody;
        if (!carlClient.parseResponse(realmType, realmBody) ||
            realmType != ClientMessageType::SelectRealmResponse)
            FAIL("carl select realm failed");

        auto& revokedCounter = theseed::foundation::MetricsRegistry::instance()
            .counter("login_session_revoked_count");
        const auto revoked0 = revokedCounter.value();

        // 账号+领域精确命中：carl@default 关一条
        if (app.handleSessionRevoked("carl", "default") != 1)
            FAIL("carl@default must close exactly one live login");
        if (carlSession.isConnected()) FAIL("carl session must be closed");
        if (!amySession.isConnected()) FAIL("amy must stay connected");
        // 空领域命中未选领域的 amy
        if (app.handleSessionRevoked("amy", "") != 1)
            FAIL("amy with empty realm must close");
        // 已断/无匹配不再计数
        if (app.handleSessionRevoked("carl", "default") != 0)
            FAIL("already-closed login must not recount");
        if (app.handleSessionRevoked("nobody", "") != 0)
            FAIL("no-match must close nothing");
        // 领域不匹配不应命中
        if (app.handleSessionRevoked("carl", "other") != 0)
            FAIL("realm mismatch must not close");

        if (revokedCounter.value() != revoked0 + 2)
            FAIL("revocation counter must be +2");
        // 绑定随连接清扫出表的真臂在 E2E 测试（真实 TCP 生命周期）覆盖：
        // 本桩测试的会话不经 acceptConnections 进 sessions_。
    }
    PASS();

    // --- §8 通知腿生产接线的桩面：注册探针、推送分发、畸形/未知臂 ---
    TEST("machine notify leg: probe, dispatch, malformed and unknown arms");
    {
        namespace MachineMethod = theseed::control::machine::MachineMethod;
        // 通知腿两端身份（LoginAppConfig 的缺省组件 id）：探针与推送
        // 帧都按这两个 id 收发。
        constexpr theseed::runtime::ComponentId kLocalComponent = 20;
        constexpr theseed::runtime::ComponentId kMachineComponent = 60;
        auto toBytes = [](const std::string& text) {
            std::vector<std::byte> out(text.size());
            for (std::size_t i = 0; i < text.size(); ++i)
                out[i] = static_cast<std::byte>(text[i]);
            return out;
        };
        auto makeMachineConfig = [](
            std::function<std::shared_ptr<theseed::runtime::IRuntimeTransport>(
                const std::string&, std::uint16_t)> factory) {
            LoginAppConfig config;
            config.listenHost = "127.0.0.1";
            config.listenPort = 0;
            config.authType = "null";
            config.machineHost = "fake-machine";
            config.machinePort = 7777;
            config.machineTransportFactory = std::move(factory);
            return config;
        };

        // --- 注册探针四臂 + 工厂空臂 ---
        {
            auto transport = std::make_shared<FakeMachineTransport>(
                FakeMachineTransport::Mode::Ack);
            std::string seenHost;
            std::uint16_t seenPort = 0;
            LoginApp app(makeMachineConfig(
                [&seenHost, &seenPort, transport](const std::string& host,
                                                  std::uint16_t port)
                    -> std::shared_ptr<theseed::runtime::IRuntimeTransport> {
                    seenHost = host;
                    seenPort = port;
                    return transport;
                }));
            app.init();
            if (seenHost != "fake-machine" || seenPort != 7777)
                FAIL("machine factory must receive host/port");
            if (transport->probe.method != MachineMethod::kSnapshot ||
                transport->probe.sourceComponent != kLocalComponent ||
                transport->probe.targetComponent != kMachineComponent)
                FAIL("registration probe must be machine.snapshot from self to daemon");
            if (transport->probe.requestId == 0)
                FAIL("registration probe must carry a minted request id (04 §6.2)");
            app.tick();  // ack 应答排空（link ready 路由）
            if (transport->pendingCount() != 0)
                FAIL("snapshot ack must be drained by tick");
        }
        {
            auto transport = std::make_shared<FakeMachineTransport>(
                FakeMachineTransport::Mode::Reject);
            LoginApp app(makeMachineConfig(
                [transport](const std::string&, std::uint16_t)
                    -> std::shared_ptr<theseed::runtime::IRuntimeTransport> {
                    return transport;
                }));
            app.init();
            app.tick();  // machine.error 应答排空（告警路由）
            if (transport->pendingCount() != 0)
                FAIL("probe rejection must be drained by tick");
        }
        {
            auto transport = std::make_shared<FakeMachineTransport>(
                FakeMachineTransport::Mode::Silent);
            LoginApp app(makeMachineConfig(
                [transport](const std::string&, std::uint16_t)
                    -> std::shared_ptr<theseed::runtime::IRuntimeTransport> {
                    return transport;
                }));
            app.init();
            app.tick();  // 吞掉探针：无应答可排空
        }
        {
            auto transport = std::make_shared<FakeMachineTransport>(
                FakeMachineTransport::Mode::Closed);
            LoginApp app(makeMachineConfig(
                [transport](const std::string&, std::uint16_t)
                    -> std::shared_ptr<theseed::runtime::IRuntimeTransport> {
                    return transport;
                }));
            app.init();  // send 拒绝 → 探针未发出臂
            app.tick();
        }
        {
            LoginApp app(makeMachineConfig(
                [](const std::string&, std::uint16_t)
                    -> std::shared_ptr<theseed::runtime::IRuntimeTransport> {
                    return nullptr;  // 跳过通知腿臂
                }));
            app.init();
            app.tick();
        }

        // --- 推送分发：全转义字符集载荷 → 转义解析 → 命中关闭活登录 ---
        auto& revokedCounter = theseed::foundation::MetricsRegistry::instance()
            .counter("login_session_revoked_count");
        auto& malformedCounter = theseed::foundation::MetricsRegistry::instance()
            .counter("login_session_notify_malformed_count");
        auto& unknownCounter = theseed::foundation::MetricsRegistry::instance()
            .counter("login_unknown_invocation_count");
        const auto revoked0 = revokedCounter.value();
        const auto malformed0 = malformedCounter.value();
        const auto unknown0 = unknownCounter.value();

        {
            auto transport = std::make_shared<FakeMachineTransport>(
                FakeMachineTransport::Mode::Ack);
            LoginApp app(makeMachineConfig(
                [transport](const std::string&, std::uint16_t)
                    -> std::shared_ptr<theseed::runtime::IRuntimeTransport> {
                    return transport;
                }));
            app.init();

            // 登录账号带全转义字符集（LF TAB CR 反斜杠 引号）：控制面
            // escapeJsonString 会转义它们，消费侧必须解析还原后精确匹配。
            const std::string weird = "we\n\t\r\\d\"q";
            MockClient client;
            ClientSession session(client.serverPipe);
            session.setMessageCallback(
                [&app, &session](ClientMessageType type,
                                 std::span<const std::byte> payload) {
                    app.handleClientMessage(&session, type, payload);
                });
            auto payload = encodeLoginPayload(weird, "pw");
            client.clearReceived();
            client.sendToServer(
                ClientMessageType::Login,
                std::span<const std::byte>(payload.data(), payload.size()));
            client.pump();
            session.pump();
            client.pump();
            ClientMessageType type{};
            std::span<const std::byte> body;
            if (!client.parseResponse(type, body) ||
                type != ClientMessageType::LoginResponse ||
                body.empty() || std::to_integer<std::uint8_t>(body[0]) == 0)
                FAIL("weird-account login must succeed");

            // 与 daemon notifySessionRevoked 同形的推送（escapeJsonString
            // 口径：LF TAB CR 反斜杠 引号 → \n \t \r \\ \"）
            const std::string notice =
                std::string("{\"account\":\"we\\n\\t\\r\\\\d\\\"q\","
                            "\"realm\":\"\","
                            "\"session\":\"session(len=8)\","
                            "\"reason\":\"operator.kick\"}");
            theseed::runtime::RuntimeInvocation push;
            push.sourceComponent = kMachineComponent;
            push.targetComponent = kLocalComponent;
            push.method = MachineMethod::kSessionRevoked;
            push.payload = toBytes(notice);
            transport->enqueue(std::move(push));
            app.tick();
            if (session.isConnected())
                FAIL("revocation push must close the matching login");
            if (revokedCounter.value() != revoked0 + 1)
                FAIL("revocation counter must be +1");
        }

        // --- 畸形载荷五臂 + 未知 method 臂（不静默，计数可断言）---
        {
            auto transport = std::make_shared<FakeMachineTransport>(
                FakeMachineTransport::Mode::Ack);
            LoginApp app(makeMachineConfig(
                [transport](const std::string&, std::uint16_t)
                    -> std::shared_ptr<theseed::runtime::IRuntimeTransport> {
                    return transport;
                }));
            app.init();
            const std::string bads[] = {
                "{\"realm\":\"\",\"session\":\"session(len=3)\","
                "\"reason\":\"operator.kick\"}",  // account 缺失
                "{\"account\":\"x\"}",            // realm 缺失
                "{\"account\":\"x\\q\",\"realm\":\"\"}",  // 越集转义
                "{\"account\":\"x",                // 未闭合
                "{\"account\":\"x\\",              // 转义悬空
            };
            for (const auto& bad : bads) {
                theseed::runtime::RuntimeInvocation push;
                push.sourceComponent = kMachineComponent;
                push.targetComponent = kLocalComponent;
                push.method = MachineMethod::kSessionRevoked;
                push.payload = toBytes(bad);
                transport->enqueue(std::move(push));
            }
            theseed::runtime::RuntimeInvocation stray;
            stray.sourceComponent = kMachineComponent;
            stray.targetComponent = kLocalComponent;
            stray.method = "machine.bogus";
            transport->enqueue(std::move(stray));
            app.tick();
            if (malformedCounter.value() != malformed0 + 5)
                FAIL("malformed pushes must each count, got " +
                     std::to_string(malformedCounter.value() - malformed0));
            if (unknownCounter.value() != unknown0 + 1)
                FAIL("unknown method must count once");
        }
    }
    PASS();

    // --- §8 通知腿运行期韧性的桩面：断链检测、退避重连、探针重发 ---
    TEST("machine notify leg resilience: drop, backoff, reconnect, re-probe");
    {
        namespace MachineMethod = theseed::control::machine::MachineMethod;
        using FMode = FakeMachineTransport::Mode;
        constexpr theseed::runtime::ComponentId kLocalComponent = 20;
        constexpr theseed::runtime::ComponentId kMachineComponent = 60;
        auto& downCounter = theseed::foundation::MetricsRegistry::instance()
            .counter("login_machine_link_down_count");
        auto& upCounter = theseed::foundation::MetricsRegistry::instance()
            .counter("login_machine_link_up_count");
        // 接口缺省活性：内存 transport 无"断开"概念，恒真（监督面的
        // 判定基线——只有 TCP 实现会按 socket 实况回答假）。
        theseed::runtime::InMemoryRuntimeTransport alwaysLive;
        if (!alwaysLive.isConnected()) FAIL("default transport liveness must be optimistic");
        auto toBytes = [](const std::string& text) {
            std::vector<std::byte> out(text.size());
            for (std::size_t i = 0; i < text.size(); ++i)
                out[i] = static_cast<std::byte>(text[i]);
            return out;
        };

        // 铸造桩 transport 的记录工厂：按尝试序取模式脚本（末尾项重复
        // 使用）；nullCalls 里的尝试序号返回 nullptr（seam 返空臂）。
        struct Recording {
            std::vector<std::shared_ptr<FakeMachineTransport>> made;
            std::vector<FMode> modes;
            std::vector<int> nullCalls;
            int calls = 0;
            std::function<std::shared_ptr<theseed::runtime::IRuntimeTransport>(
                const std::string&, std::uint16_t)> factory() {
                return [this](const std::string&, std::uint16_t)
                           -> std::shared_ptr<theseed::runtime::IRuntimeTransport> {
                    const int idx = calls++;
                    const auto mode = modes[std::min<std::size_t>(
                        static_cast<std::size_t>(idx), modes.size() - 1)];
                    if (std::find(nullCalls.begin(), nullCalls.end(), idx) !=
                        nullCalls.end()) {
                        return nullptr;
                    }
                    auto t = std::make_shared<FakeMachineTransport>(mode);
                    made.push_back(t);
                    return t;
                };
            }
        };
        auto resilienceConfig = [](std::function<
            std::shared_ptr<theseed::runtime::IRuntimeTransport>(
                const std::string&, std::uint16_t)> factory,
            std::chrono::milliseconds ackTimeout) {
            LoginAppConfig config;
            config.listenHost = "127.0.0.1";
            config.listenPort = 0;
            config.authType = "null";
            config.machineHost = "fake-machine";
            config.machinePort = 7777;
            config.machineTransportFactory = std::move(factory);
            // 短退避：测试里毫秒级收敛（真实默认 1s/30s 会拖慢单测）。
            config.machineReconnectBaseDelay = std::chrono::milliseconds{5};
            config.machineReconnectMaxDelay = std::chrono::milliseconds{40};
            config.machineProbeAckTimeout = ackTimeout;
            return config;
        };
        // 泵 tick 直到谓词成立（有界，防呆死）。
        auto pumpUntil = [](LoginApp& app,
                            const std::function<bool()>& done) {
            for (int i = 0; i < 1000; ++i) {
                if (done()) return true;
                app.tick();
                SLEEP_MS(2);
            }
            return done();
        };

        // --- 臂 1：Up 活性转假 → 断链计数 → 退避到点重连 → 探针重发 →
        //     应答恢复 Up；恢复的链路不再重连。 ---
        {
            Recording rec;
            rec.modes = {FMode::Ack};
            const auto down0 = downCounter.value();
            const auto up0 = upCounter.value();
            LoginApp app(resilienceConfig(rec.factory(),
                                          std::chrono::milliseconds{500}));
            app.init();
            app.tick();  // 排空首个探针应答 → Up
            if (rec.calls != 1) FAIL("initial attempt must be exactly one call");
            if (upCounter.value() != up0 + 1) FAIL("initial ack must count one link up");
            rec.made[0]->alive = false;  // 模拟 daemon 侧重启断连
            const bool recovered = pumpUntil(app, [&] {
                return upCounter.value() >= up0 + 2;
            });
            if (!recovered) FAIL("dead link must reconnect and recover");
            if (rec.calls != 2) FAIL("recovery must take exactly one retry once acked");
            if (downCounter.value() != down0 + 1) FAIL("the drop must count one link down");
            if (rec.made[1]->sends != 1 ||
                rec.made[1]->probe.method != MachineMethod::kSnapshot)
                FAIL("reconnect must re-send the registration probe");
            if (rec.made[1]->probe.sourceComponent != kLocalComponent ||
                rec.made[1]->probe.targetComponent != kMachineComponent)
                FAIL("re-registration probe must carry the same identity");
            // 链路已 Up：继续泵不再触发尝试。
            for (int i = 0; i < 20; ++i) app.tick();
            if (rec.calls != 2) FAIL("an up link must not keep reconnecting");
        }

        // --- 臂 2：探针始终无应答（transport 活性真）→ ack 超时断链，
        //     退避重连仍无应答 → 再计一次 down（超时臂）。 ---
        {
            Recording rec;
            rec.modes = {FMode::Silent};
            const auto down0 = downCounter.value();
            const auto up0 = upCounter.value();
            LoginApp app(resilienceConfig(rec.factory(),
                                          std::chrono::milliseconds{20}));
            app.init();
            app.tick();  // PendingAck：无应答可排空、宽限未到
            if (rec.calls != 1) FAIL("PendingAck must not retry before the deadline");
            const bool timedOut = pumpUntil(app, [&] {
                return downCounter.value() >= down0 + 2;
            });
            if (!timedOut) FAIL("probe without ack must time out and retry");
            if (rec.calls < 2) FAIL("ack timeout must drive a reconnect attempt");
            if (upCounter.value() != up0) FAIL("no inbound must not claim link up");
        }

        // --- 臂 3：断链后首次重连 seam 返空 → 告警退避（窗口翻倍）→
        //     再试接通恢复。失败的尝试不是第二次断链。 ---
        {
            Recording rec;
            rec.modes = {FMode::Ack};
            rec.nullCalls = {1};
            const auto down0 = downCounter.value();
            const auto up0 = upCounter.value();
            LoginApp app(resilienceConfig(rec.factory(),
                                          std::chrono::milliseconds{500}));
            app.init();
            app.tick();
            rec.made[0]->alive = false;
            const bool recovered = pumpUntil(app, [&] {
                return upCounter.value() >= up0 + 2;
            });
            if (!recovered) FAIL("reconnect must recover after a failed attempt");
            if (rec.calls != 3) FAIL("expected exactly: link, null, recovery");
            if (downCounter.value() != down0 + 1)
                FAIL("a failed attempt is not a second link down");
        }

        // --- 臂 4：重连拿到发不出的 transport（Closed）→ 探针未发出 →
        //     摘除死 peer 退避，下一次尝试接通恢复（不残留僵尸注册）。 ---
        {
            Recording rec;
            rec.modes = {FMode::Ack, FMode::Closed, FMode::Ack};
            const auto up0 = upCounter.value();
            LoginApp app(resilienceConfig(rec.factory(),
                                          std::chrono::milliseconds{500}));
            app.init();
            app.tick();
            rec.made[0]->alive = false;
            const bool recovered = pumpUntil(app, [&] {
                return upCounter.value() >= up0 + 2;
            });
            if (!recovered) FAIL("closed transport retry must fall back and recover");
            if (rec.calls != 3) FAIL("expected exactly: link, closed, recovery");
        }

        // --- 臂 5：断链期间的 revoked 推送经恢复后的新 transport 到达，
        //     联动落点照常工作（恢复的是功能面不是计数面）。 ---
        {
            Recording rec;
            rec.modes = {FMode::Ack};
            const auto up0 = upCounter.value();
            const auto revoked0 = theseed::foundation::MetricsRegistry::instance()
                                      .counter("login_session_revoked_count")
                                      .value();
            LoginApp app(resilienceConfig(rec.factory(),
                                          std::chrono::milliseconds{500}));
            app.init();
            app.tick();

            // 先登记一个活登录（绑定表定位面）。
            MockClient client;
            ClientSession session(client.serverPipe);
            session.setMessageCallback([&app, &session](ClientMessageType type,
                                                         std::span<const std::byte> payload) {
                app.handleClientMessage(&session, type, payload);
            });
            auto payload = encodeLoginPayload("zoe", "pw");
            client.sendToServer(ClientMessageType::Login,
                                std::span<const std::byte>(payload.data(), payload.size()));
            client.pump();
            session.pump();
            client.pump();

            rec.made[0]->alive = false;
            if (!pumpUntil(app, [&] { return upCounter.value() >= up0 + 2; }))
                FAIL("link must recover before the push test");

            theseed::runtime::RuntimeInvocation push;
            push.sourceComponent = kMachineComponent;
            push.targetComponent = kLocalComponent;
            push.method = MachineMethod::kSessionRevoked;
            const std::string notice =
                R"json({"account":"zoe","realm":"","session":"session(len=3)","reason":"operator.kick"})json";
            push.payload = toBytes(notice);
            rec.made[1]->enqueue(std::move(push));
            app.tick();
            if (session.isConnected()) FAIL("push after recovery must still close the login");
            if (theseed::foundation::MetricsRegistry::instance()
                    .counter("login_session_revoked_count")
                    .value() != revoked0 + 1)
                FAIL("recovered delivery must count once");
        }
    }
    PASS();

    TEST("db leg resilience: drop, backoff, reconnect, re-probe, query restored");
    {
        // db 腿韧性臂与通知腿同款编排：探针经 createMode 维路由
        // （modeFor：非 queryAccount 一律 createMode_），Canned = 回
        // db.listTypes.ok（接通证实），Silent = 吞探针（ack 超时臂），
        // Closed = 探针发不出（摘死 peer 重试臂）。
        using DMode = FakeDbTransport::Mode;
        constexpr theseed::runtime::ComponentId kLocalComponent = 20;
        constexpr theseed::runtime::ComponentId kDbComponent = 10;
        auto& downCounter = theseed::foundation::MetricsRegistry::instance()
            .counter("login_db_link_down_count");
        auto& upCounter = theseed::foundation::MetricsRegistry::instance()
            .counter("login_db_link_up_count");

        struct DbRecording {
            std::vector<std::shared_ptr<FakeDbTransport>> made;
            std::vector<DMode> probeModes;  // 末尾项重复使用
            std::vector<int> nullCalls;     // 这些尝试序号返回 nullptr
            DMode queryMode = DMode::Canned;
            std::vector<std::byte> queryPayload;  // 功能臂的登录查询落点
            int calls = 0;
            std::function<std::shared_ptr<theseed::runtime::IRuntimeTransport>(
                const std::string&, std::uint16_t)> factory() {
                return [this](const std::string&, std::uint16_t)
                           -> std::shared_ptr<theseed::runtime::IRuntimeTransport> {
                    const int idx = calls++;
                    const auto mode = probeModes[std::min<std::size_t>(
                        static_cast<std::size_t>(idx), probeModes.size() - 1)];
                    if (std::find(nullCalls.begin(), nullCalls.end(), idx) !=
                        nullCalls.end()) {
                        return nullptr;
                    }
                    auto t = std::make_shared<FakeDbTransport>(
                        queryMode, queryPayload, mode);
                    made.push_back(t);
                    return t;
                };
            }
        };
        auto dbResilienceConfig = [](
            std::function<std::shared_ptr<theseed::runtime::IRuntimeTransport>(
                const std::string&, std::uint16_t)> factory,
            std::chrono::milliseconds ackTimeout) {
            LoginAppConfig config;
            config.listenHost = "127.0.0.1";
            config.listenPort = 0;
            config.authType = "db";
            config.dbHost = "fake-db";  // factory 注入时不真正 connect
            config.dbPort = 7777;
            config.dbRequestTimeout = std::chrono::milliseconds{20};
            config.dbTransportFactory = std::move(factory);
            // 短退避：测试里毫秒级收敛（真实默认 1s/30s 会拖慢单测）。
            config.dbReconnectBaseDelay = std::chrono::milliseconds{5};
            config.dbReconnectMaxDelay = std::chrono::milliseconds{40};
            config.dbProbeAckTimeout = ackTimeout;
            return config;
        };
        auto pumpUntil = [](LoginApp& app, const std::function<bool()>& done) {
            for (int i = 0; i < 1000; ++i) {
                if (done()) return true;
                app.tick();
                SLEEP_MS(2);
            }
            return done();
        };

        // --- 臂 1：Up 活性转假 → 断链计数 → 退避到点重连 → 探针重发 →
        //     应答恢复 Up；恢复的链路不再重连。 ---
        {
            DbRecording rec;
            rec.probeModes = {DMode::Canned};
            const auto down0 = downCounter.value();
            const auto up0 = upCounter.value();
            LoginApp app(dbResilienceConfig(rec.factory(),
                                            std::chrono::milliseconds{500}));
            app.init();
            app.tick();  // 排空首个探针应答 → Up
            if (rec.calls != 1) FAIL("initial attempt must be exactly one call");
            if (upCounter.value() != up0 + 1) FAIL("initial ack must count one link up");
            rec.made[0]->alive = false;  // 模拟 DBApp 侧重启断连
            const bool recovered = pumpUntil(app, [&] {
                return upCounter.value() >= up0 + 2;
            });
            if (!recovered) FAIL("dead db link must reconnect and recover");
            if (rec.calls != 2) FAIL("recovery must take exactly one retry once acked");
            if (downCounter.value() != down0 + 1) FAIL("the drop must count one link down");
            if (rec.made[1]->sends != 1 ||
                rec.made[1]->probe.method != db::DBMethod::kListTypes)
                FAIL("reconnect must re-send the liveness probe");
            if (rec.made[1]->probe.sourceComponent != kLocalComponent ||
                rec.made[1]->probe.targetComponent != kDbComponent)
                FAIL("re-registration probe must carry the same identity");
            for (int i = 0; i < 20; ++i) app.tick();
            if (rec.calls != 2) FAIL("an up db link must not keep reconnecting");
        }

        // --- 臂 2：探针始终无应答（活性真）→ ack 超时断链，退避重连
        //     仍无应答 → 再计一次 down（超时臂）。 ---
        {
            DbRecording rec;
            rec.probeModes = {DMode::Silent};
            const auto down0 = downCounter.value();
            const auto up0 = upCounter.value();
            LoginApp app(dbResilienceConfig(rec.factory(),
                                            std::chrono::milliseconds{20}));
            app.init();
            app.tick();  // PendingAck：无应答可排空、宽限未到
            if (rec.calls != 1) FAIL("PendingAck must not retry before the deadline");
            const bool timedOut = pumpUntil(app, [&] {
                return downCounter.value() >= down0 + 2;
            });
            if (!timedOut) FAIL("probe without ack must time out and retry");
            if (rec.calls < 2) FAIL("ack timeout must drive a reconnect attempt");
            if (upCounter.value() != up0) FAIL("no inbound must not claim link up");
        }

        // --- 臂 3：断链后首次重连 seam 返空 → 告警退避（窗口翻倍）→
        //     再试接通恢复。失败的尝试不是第二次断链。 ---
        {
            DbRecording rec;
            rec.probeModes = {DMode::Canned};
            rec.nullCalls = {1};
            const auto down0 = downCounter.value();
            const auto up0 = upCounter.value();
            LoginApp app(dbResilienceConfig(rec.factory(),
                                            std::chrono::milliseconds{500}));
            app.init();
            app.tick();
            rec.made[0]->alive = false;
            const bool recovered = pumpUntil(app, [&] {
                return upCounter.value() >= up0 + 2;
            });
            if (!recovered) FAIL("db reconnect must recover after a failed attempt");
            if (rec.calls != 3) FAIL("expected exactly: link, null, recovery");
            if (downCounter.value() != down0 + 1)
                FAIL("a failed attempt is not a second link down");
        }

        // --- 臂 4：重连拿到发不出的 transport（Closed）→ 探针未发出 →
        //     摘除死 peer 退避，下一次尝试接通恢复（不残留空注册）。 ---
        {
            DbRecording rec;
            rec.probeModes = {DMode::Canned, DMode::Closed, DMode::Canned};
            const auto up0 = upCounter.value();
            LoginApp app(dbResilienceConfig(rec.factory(),
                                            std::chrono::milliseconds{500}));
            app.init();
            app.tick();
            rec.made[0]->alive = false;
            const bool recovered = pumpUntil(app, [&] {
                return upCounter.value() >= up0 + 2;
            });
            if (!recovered) FAIL("closed transport retry must fall back and recover");
            if (rec.calls != 3) FAIL("expected exactly: link, closed, recovery");
        }

        // --- 臂 5：恢复后的链路功能面照常——登录查询经重连后的新
        //     transport 得到应答（验收是查询恢复，不是计数）。 ---
        {
            DbRecording rec;
            rec.probeModes = {DMode::Canned};
            rec.queryMode = DMode::Canned;
            rec.queryPayload = db::DBProtocol::encodeQueryAccountResponse(true, 77, "pw");
            const auto up0 = upCounter.value();
            LoginApp app(dbResilienceConfig(rec.factory(),
                                            std::chrono::milliseconds{500}));
            app.init();
            app.tick();
            rec.made[0]->alive = false;
            if (!pumpUntil(app, [&] { return upCounter.value() >= up0 + 2; }))
                FAIL("db link must recover before the query test");
            bool success = false;
            std::string error, token;
            if (!runLogin(app, "iron", "pw", success, error, token))
                FAIL("no login response after recovery");
            if (!success || token.empty())
                FAIL("query over recovered link must log in, error=" + error);
            if (rec.made[1]->sends < 2)
                FAIL("recovered transport must serve both probe and login query");
        }
    }
    PASS();

    // --- 选领域把领域写回会话行（踢人联动的存储侧口径）+ 过期行不复活 ---
    TEST("realm selection records the realm in the session row");
    {
        auto redis = std::make_shared<theseed::foundation::InMemoryRedisProvider>();
        auto store = std::make_shared<theseed::foundation::SessionStore>(redis);

        LoginAppConfig config;
        config.listenHost = "127.0.0.1";
        config.listenPort = 0;
        config.authType = "null";
        config.sessionStore = store;
        config.sessionTtl = std::chrono::seconds(60);
        RealmInfo r;
        r.realmId = "default";
        r.name = "Default";
        r.status = "smooth";
        r.host = "127.0.0.1";
        r.port = 20000;
        config.realms.push_back(r);
        LoginApp app(std::move(config));
        app.init();

        auto stringPayload = [](const std::string& s) {
            std::vector<std::byte> out;
            const auto len = static_cast<std::uint32_t>(s.size());
            out.push_back(std::byte(len & 0xFF));
            out.push_back(std::byte((len >> 8) & 0xFF));
            out.push_back(std::byte((len >> 16) & 0xFF));
            out.push_back(std::byte((len >> 24) & 0xFF));
            for (const char c : s) out.push_back(static_cast<std::byte>(c));
            return out;
        };
        auto driveLogin = [&app](MockClient& client, ClientSession& session,
                                 const std::string& account, std::string& token) {
            session.setMessageCallback(
                [&app, &session](ClientMessageType type,
                                 std::span<const std::byte> payload) {
                    app.handleClientMessage(&session, type, payload);
                });
            auto payload = encodeLoginPayload(account, "pw");
            client.clearReceived();
            client.sendToServer(
                ClientMessageType::Login,
                std::span<const std::byte>(payload.data(), payload.size()));
            client.pump();
            session.pump();
            client.pump();
            ClientMessageType type{};
            std::span<const std::byte> body;
            if (!client.parseResponse(type, body) ||
                type != ClientMessageType::LoginResponse ||
                body.empty() || std::to_integer<std::uint8_t>(body[0]) == 0)
                return false;
            // token 是响应尾串：[u8][u32 errLen][error][u32 tokLen][token]
            std::size_t off = 1;
            std::uint32_t errLen = 0;
            for (int i = 0; i < 4; ++i)
                errLen |= static_cast<std::uint32_t>(
                              std::to_integer<std::uint8_t>(body[off + i])) << (8 * i);
            off += 4 + errLen;
            std::uint32_t tokLen = 0;
            for (int i = 0; i < 4; ++i)
                tokLen |= static_cast<std::uint32_t>(
                              std::to_integer<std::uint8_t>(body[off + i])) << (8 * i);
            off += 4;
            token.assign(reinterpret_cast<const char*>(body.data() + off), tokLen);
            return true;
        };
        auto driveSelectRealm = [&stringPayload](MockClient& client, ClientSession& session,
                                                const std::string& realmId) {
            auto payload = stringPayload(realmId);
            client.clearReceived();
            client.sendToServer(
                ClientMessageType::SelectRealm,
                std::span<const std::byte>(payload.data(), payload.size()));
            client.pump();
            session.pump();
            client.pump();
            ClientMessageType type{};
            std::span<const std::byte> body;
            return client.parseResponse(type, body) &&
                   type == ClientMessageType::SelectRealmResponse &&
                   !body.empty() && std::to_integer<std::uint8_t>(body[0]) != 0;
        };

        // 登录即写基础会话行（领域为空）；选领域后领域补进同一行。
        MockClient daveClient;
        ClientSession daveSession(daveClient.serverPipe);
        std::string daveToken;
        if (!driveLogin(daveClient, daveSession, "dave", daveToken))
            FAIL("dave login failed");
        const auto loginRow = store->load(daveToken);
        if (!loginRow || loginRow->accountId != "dave" || !loginRow->realmId.empty())
            FAIL("login must store the base session with an empty realm");
        if (!driveSelectRealm(daveClient, daveSession, "default"))
            FAIL("dave select realm failed");
        const auto realmRow = store->load(daveToken);
        if (!realmRow || realmRow->realmId != "default" || realmRow->accountId != "dave")
            FAIL("realm selection must be written back to the session row");
        // 枚举面同口径：list-sessions 的 realm 字段不再是空壳
        const auto rows = store->listSessions();
        if (rows.size() != 1 || rows[0].realmId != "default")
            FAIL("list-sessions must expose the recorded realm");

        // 登录后未选领域的连接：选领域同样补写（绑定存在，行为一致）
        MockClient erinClient;
        ClientSession erinSession(erinClient.serverPipe);
        std::string erinToken;
        if (!driveLogin(erinClient, erinSession, "erin", erinToken))
            FAIL("erin login failed");
        if (!driveSelectRealm(erinClient, erinSession, "default"))
            FAIL("erin select realm failed");
        if (!store->load(erinToken) || store->load(erinToken)->realmId != "default")
            FAIL("erin session row must carry the realm");

        // 未登录的连接选领域：不建绑定、不碰存储（无 token 可写）
        MockClient strangerClient;
        ClientSession strangerSession(strangerClient.serverPipe);
        strangerSession.setMessageCallback(
            [&app, &strangerSession](ClientMessageType type,
                                     std::span<const std::byte> payload) {
                app.handleClientMessage(&strangerSession, type, payload);
            });
        if (!driveSelectRealm(strangerClient, strangerSession, "default"))
            FAIL("stranger select realm should still answer ok");
        if (store->listSessions().size() != 2)
            FAIL("an unauthenticated realm select must not create a session row");

        // 过期行不因选领域复活：TTL 走完后 load 落空 → 跳过改写。
        MockClient ghostClient;
        ClientSession ghostSession(ghostClient.serverPipe);
        std::string ghostToken;
        if (!driveLogin(ghostClient, ghostSession, "ghost", ghostToken))
            FAIL("ghost login failed");
        redis->advanceClock(std::chrono::seconds(120));
        if (store->load(ghostToken))
            FAIL("the session row should have expired before the realm select");
        if (!driveSelectRealm(ghostClient, ghostSession, "default"))
            FAIL("ghost select realm should still answer ok");
        if (store->load(ghostToken))
            FAIL("an expired session row must not be resurrected by realm select");
    }
    PASS();

    std::cout << "\nAll LoginApp tests passed!" << std::endl;
    return 0;
}
