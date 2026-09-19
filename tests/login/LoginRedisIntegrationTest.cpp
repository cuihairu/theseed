#include "theseed/foundation/RedisProvider.h"
#include "theseed/foundation/RateLimiter.h"
#include "theseed/foundation/SessionStore.h"
#include "theseed/login/LoginApp.h"
#include "theseed/login/ClientSession.h"
#include "theseed/login/LoginProtocol.h"
#include "theseed/runtime/InMemoryBytePipe.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <vector>

using namespace theseed::login;
using namespace theseed::foundation;
using namespace theseed::runtime;

#define PASS() std::cout << "OK" << std::endl
#define FAIL(msg)                                       \
    do {                                                \
        std::cout << "FAILED: " << msg << std::endl;    \
        return 1;                                       \
    } while (0)

// 编码一个 Login 请求 payload（account + password，两个长度前缀串）。
static std::vector<std::byte> encodeLoginPayload(const std::string& account,
                                                  const std::string& password) {
    std::vector<std::byte> payload;
    auto appendStr = [&](const std::string& s) {
        std::uint32_t len = static_cast<std::uint32_t>(s.size());
        for (int i = 0; i < 4; ++i)
            payload.push_back(static_cast<std::byte>((len >> (8 * i)) & 0xFF));
        payload.insert(payload.end(),
                       reinterpret_cast<const std::byte*>(s.data()),
                       reinterpret_cast<const std::byte*>(s.data()) + s.size());
    };
    appendStr(account);
    appendStr(password);
    // frameMessage 加上 [u32 len][u8 type] 头
    return LoginProtocol::frameMessage(
        ClientMessageType::Login,
        std::span<const std::byte>(payload.data(), payload.size()));
}

// 模拟客户端：持有双向 pipe，捕获服务端回包。
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

    void sendLogin(const std::string& account, const std::string& password) {
        auto payload = encodeLoginPayload(account, password);
        clientPipe->write(std::span<const std::byte>(payload.data(), payload.size()));
    }

    void pump() {
        clientPipe->pump();
        serverPipe->pump();
    }

    // 解析第一个回包。返回 success 与 token。
    bool parseLoginResponse(bool& outSuccess, std::string& outToken) {
        if (receivedData.size() < LoginProtocol::kHeaderSize) return false;
        // 跳过 [u32 len][u8 type]
        std::size_t off = 5;
        if (off >= receivedData.size()) return false;
        outSuccess = static_cast<std::uint8_t>(receivedData[off]) != 0;
        ++off;
        if (off + 4 > receivedData.size()) return false;
        std::uint32_t errLen = 0;
        for (int i = 0; i < 4; ++i)
            errLen |= static_cast<std::uint32_t>(receivedData[off + i]) << (8 * i);
        off += 4 + errLen;  // 跳过 error string
        if (off + 4 > receivedData.size()) return false;
        std::uint32_t tokLen = 0;
        for (int i = 0; i < 4; ++i)
            tokLen |= static_cast<std::uint32_t>(receivedData[off + i]) << (8 * i);
        off += 4;
        if (off + tokLen > receivedData.size()) return false;
        outToken.assign(reinterpret_cast<const char*>(receivedData.data() + off), tokLen);
        return true;
    }
};

int main() {
    std::cout << "LoginApp Redis integration tests:" << std::endl;

    // --- 会话存储：登录成功后 token 被持久化 ---
    std::cout << "  login persists session to SessionStore... ";
    {
        auto redis = std::make_shared<InMemoryRedisProvider>();
        LoginAppConfig config;
        config.authType = "null";
        config.redis = redis;
        config.sessionStore = std::make_shared<SessionStore>(redis);
        config.rateLimiter = nullptr;  // 本用例不测限流
        RealmInfo r;
        r.realmId = "default"; r.name = "Default"; r.status = "smooth";
        r.host = "127.0.0.1"; r.port = 20000;
        config.realms.push_back(r);

        LoginApp app(std::move(config));

        MockClient client;
        ClientSession session(client.serverPipe);
        session.setMessageCallback([&app, &session](ClientMessageType type,
                                                     std::span<const std::byte> payload) {
            app.handleClientMessage(&session, type, payload);
        });

        client.sendLogin("alice", "pw");
        client.pump();
        session.pump();
        client.pump();

        bool success = false;
        std::string token;
        if (!client.parseLoginResponse(success, token)) FAIL("no login response");
        if (!success) FAIL("login should succeed");
        if (token.empty()) FAIL("token should be non-empty");

        // 验证 SessionStore 里有这个 token。
        // config 已被 move 进 app（config.sessionStore 现为 null），
        // 通过共享的 redis provider 直接查 session: 前缀。
        bool found = false;
        auto val = redis->get("session:" + token);
        if (val && val->find("alice") != std::string::npos) found = true;
        if (!found) FAIL("session not persisted in redis under token key");
    }
    PASS();

    // --- 限流：超出容量后登录被拒绝 ---
    std::cout << "  rate limiter rejects excess login attempts... ";
    {
        auto redis = std::make_shared<InMemoryRedisProvider>();
        LoginAppConfig config;
        config.authType = "null";
        config.redis = redis;
        config.rateLimiter = std::make_shared<RateLimiter>(redis);
        config.rateLimitConfig.capacity = 3;             // 最多 3 次
        config.rateLimitConfig.refillInterval = std::chrono::seconds(60);  // 测试内基本不补充
        RealmInfo r;
        r.realmId = "default"; r.name = "Default"; r.status = "smooth";
        r.host = "127.0.0.1"; r.port = 20000;
        config.realms.push_back(r);

        LoginApp app(std::move(config));

        int successCount = 0;
        int rateLimitedCount = 0;
        for (int i = 0; i < 5; ++i) {
            MockClient client;
            ClientSession session(client.serverPipe);
            session.setMessageCallback([&app, &session](ClientMessageType type,
                                                         std::span<const std::byte> payload) {
                app.handleClientMessage(&session, type, payload);
            });
            client.sendLogin("bob", "pw");
            client.pump();
            session.pump();
            client.pump();

            bool success = false;
            std::string token;
            client.parseLoginResponse(success, token);
            if (success) ++successCount;
            else ++rateLimitedCount;
        }

        if (successCount != 3) {
            std::cout << "FAILED: expected 3 successful logins, got " << successCount << std::endl;
            return 1;
        }
        if (rateLimitedCount != 2) {
            std::cout << "FAILED: expected 2 rate-limited, got " << rateLimitedCount << std::endl;
            return 1;
        }
    }
    PASS();

    // --- 无 Redis 配置时退化为旧行为 ---
    std::cout << "  no-redis config degrades gracefully... ";
    {
        LoginAppConfig config;
        config.authType = "null";
        // sessionStore / rateLimiter 均为 nullptr
        RealmInfo r;
        r.realmId = "default"; r.name = "Default"; r.status = "smooth";
        r.host = "127.0.0.1"; r.port = 20000;
        config.realms.push_back(r);

        LoginApp app(std::move(config));

        MockClient client;
        ClientSession session(client.serverPipe);
        session.setMessageCallback([&app, &session](ClientMessageType type,
                                                     std::span<const std::byte> payload) {
            app.handleClientMessage(&session, type, payload);
        });
        client.sendLogin("carol", "pw");
        client.pump();
        session.pump();
        client.pump();

        bool success = false;
        std::string token;
        if (!client.parseLoginResponse(success, token)) FAIL("no response");
        if (!success) FAIL("should succeed without redis");
    }
    PASS();

    std::cout << "\nLoginApp Redis integration: all passed\n";
    return 0;
}
