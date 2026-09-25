#include "theseed/login/LoginProtocol.h"
#include "theseed/login/SessionToken.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>
#include <string>
#include <vector>

using namespace theseed::login;

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

int main() {
    std::cout << "LoginProtocol tests:" << std::endl;

    // --- SessionToken ---
    TEST("issue and validate token");
    {
        auto token = SessionToken::issue("player1", "realm_east");
        std::string accountId, realmId;
        if (!SessionToken::validate(token, accountId, realmId)) FAIL("validate failed");
        if (accountId != "player1") FAIL("accountId mismatch");
        if (realmId != "realm_east") FAIL("realmId mismatch");
    }
    PASS();

    TEST("reject invalid token");
    {
        std::string accountId, realmId;
        if (SessionToken::validate("garbage", accountId, realmId)) FAIL("should reject garbage");
        if (SessionToken::validate("", accountId, realmId)) FAIL("should reject empty");
    }
    PASS();

    TEST("token is unique per call");
    {
        auto t1 = SessionToken::issue("a", "r");
        auto t2 = SessionToken::issue("a", "r");
        if (t1 == t2) FAIL("tokens should be unique");
    }
    PASS();

    // --- LoginProtocol framing ---
    TEST("frame and parse message");
    {
        std::vector<std::byte> payload = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};
        auto framed = LoginProtocol::frameMessage(ClientMessageType::Login, payload);

        ClientMessageType outType;
        std::span<const std::byte> outPayload;
        if (!LoginProtocol::parseFrame(
                std::span<const std::byte>(framed.data(), framed.size()),
                outType, outPayload))
            FAIL("parse failed");
        if (outType != ClientMessageType::Login) FAIL("type mismatch");
        if (outPayload.size() != 3) FAIL("payload size mismatch");
        if (outPayload[0] != std::byte{0x01}) FAIL("payload data mismatch");
    }
    PASS();

    TEST("reject incomplete frame");
    {
        std::vector<std::byte> partial(3, std::byte{0});
        ClientMessageType outType;
        std::span<const std::byte> outPayload;
        if (LoginProtocol::parseFrame(
                std::span<const std::byte>(partial.data(), partial.size()),
                outType, outPayload))
            FAIL("should reject incomplete frame");
    }
    PASS();

    // --- encode/decode login ---
    TEST("encode and decode login request");
    {
        // Encode account + password using LoginProtocol's encoding format
        std::string account = "testuser";
        std::string password = "testpass";
        std::vector<std::byte> payload;

        auto appendStr = [&payload](const std::string& s) {
            auto len = static_cast<uint32_t>(s.size());
            payload.push_back(std::byte(len & 0xFF));
            payload.push_back(std::byte((len >> 8) & 0xFF));
            payload.push_back(std::byte((len >> 16) & 0xFF));
            payload.push_back(std::byte((len >> 24) & 0xFF));
            for (char c : s) payload.push_back(static_cast<std::byte>(c));
        };
        appendStr(account);
        appendStr(password);

        std::span<const std::byte> payloadSpan(payload.data(), payload.size());
        std::string outAccount, outPassword;
        if (!LoginProtocol::decodeLogin(payloadSpan, outAccount, outPassword))
            FAIL("decode failed");
        if (outAccount != "testuser") FAIL("account mismatch");
        if (outPassword != "testpass") FAIL("password mismatch");
    }
    PASS();

    // --- encode login response ---
    TEST("encode login response with realms");
    {
        LoginResponse resp;
        resp.success = true;
        resp.token = "abc123";
        RealmInfo r;
        r.realmId = "r1";
        r.name = "TestRealm";
        r.status = "smooth";
        r.host = "127.0.0.1";
        r.port = 20000;
        resp.realms.push_back(r);

        auto data = LoginProtocol::encodeLoginResponse(resp);
        if (data.empty()) FAIL("empty response");
        // Check message type
        if (static_cast<ClientMessageType>(data[4]) != ClientMessageType::LoginResponse)
            FAIL("wrong message type");
    }
    PASS();

    // --- encode select realm response ---
    TEST("encode select realm response");
    {
        SelectRealmResponse resp;
        resp.success = true;
        resp.host = "10.0.1.1";
        resp.port = 20000;
        resp.token = "tok123";

        auto data = LoginProtocol::encodeSelectRealmResponse(resp);
        if (data.empty()) FAIL("empty response");
    }
    PASS();

    // --- encode error ---
    TEST("encode error message");
    {
        auto data = LoginProtocol::encodeError("something went wrong");
        if (data.empty()) FAIL("empty error");
        if (static_cast<ClientMessageType>(data[4]) != ClientMessageType::ErrorResponse)
            FAIL("wrong message type");
    }
    PASS();

    // --- validate 分支矩阵：缺冒号 / 只有一个冒号 / 空 account / 空 realm ---
    TEST("validate colon branches");
    {
        std::string a, r;
        // "login:" 后无任何冒号 → col1 缺失。
        if (SessionToken::validate("bG9naW46", a, r)) FAIL("no-colon should fail");
        // "login:a" 只有一个冒号都没有……实际 0 个；补一个冒号变体 "login:a"→无。
        // "login:a" 无冒号段；"login:a:" 恰一个冒号 → col2 缺失。
        if (SessionToken::validate("bG9naW46YQ==", a, r)) FAIL("zero-colon should fail");
        if (SessionToken::validate("bG9naW46YTo=", a, r)) FAIL("one-colon should fail");
        // 空 account / 空 realm → 末尾校验假臂。
        if (SessionToken::validate("bG9naW46OmI=", a, r)) FAIL("empty account should fail");
        if (SessionToken::validate("bG9naW46YTo=", a, r)) FAIL("empty realm should fail");
        // 合法基线不受影响。
        auto token = SessionToken::issue("u", "r");
        if (!SessionToken::validate(token, a, r)) FAIL("baseline broken");
        // base64 padding 变体：明文长度 %3==1 → 尾 '=='；%3==2 → 尾 '='。
        auto tokPad2 = SessionToken::issue("a", "aa");   // 10 字节明文
        std::string realm2;
        if (!SessionToken::validate(tokPad2, a, realm2)) FAIL("pad2 roundtrip");
        if (realm2 != "aa") FAIL("pad2 realm mismatch");
        auto tokPad1 = SessionToken::issue("a", "aaa");  // 11 字节明文
        std::string realm3;
        if (!SessionToken::validate(tokPad1, a, realm3)) FAIL("pad1 roundtrip");
        if (realm3 != "aaa") FAIL("pad1 realm mismatch");
        // 非标准长度（%4==3 无 padding）：decode 走 n>=3 尾组臂，解出残缺串被拒。
        if (SessionToken::validate("bG9naW46YTp", a, r)) FAIL("non-padded residue should fail");
        // 解码后短于 "login:" 前缀 → 长度守卫真臂。
        if (SessionToken::validate("YQ==", a, r)) FAIL("short decoded payload should fail");
        // 表外字符（空格）→ decodeLookup 兜底 0，解出脏明文被拒。
        if (SessionToken::validate("bG9n aW46YQ==", a, r)) FAIL("table-outer char should fail");
    }
    PASS();

    // --- parseFrame / decodeLogin 截断矩阵 ---
    TEST("parseFrame and decodeLogin truncation branches");
    {
        auto appendLen = [](std::vector<std::byte>& v, std::uint32_t len) {
            v.push_back(std::byte(len & 0xFF));
            v.push_back(std::byte((len >> 8) & 0xFF));
            v.push_back(std::byte((len >> 16) & 0xFF));
            v.push_back(std::byte((len >> 24) & 0xFF));
        };

        // payloadLen 谎报大于实际 → data.size() < kHeaderSize + payloadLen（33 行真臂）。
        std::vector<std::byte> liar(5);
        liar[0] = std::byte{0xFF}; liar[1] = std::byte{0xFF};
        liar[2] = std::byte{0xFF}; liar[3] = std::byte{0xFF};
        liar[4] = std::byte{1};
        ClientMessageType t{};
        std::span<const std::byte> pl;
        if (LoginProtocol::parseFrame(std::span<const std::byte>(liar.data(), liar.size()), t, pl))
            FAIL("lying payloadLen should fail");

        // decodeLogin：第二串长度谎报 → offset+len 越界（106 行真臂）。
        {
            std::vector<std::byte> payload;
            appendLen(payload, 2);
            payload.push_back(std::byte{'a'});
            payload.push_back(std::byte{'b'});
            appendLen(payload, 99);   // 密码长度谎报
            std::span<const std::byte> raw(payload.data(), payload.size());
            std::string acc, pwd;
            if (LoginProtocol::decodeLogin(raw, acc, pwd)) FAIL("lying password len should fail");
        }
        // decodeLogin：末尾长度字段残缺 → offset+4 越界（100 行真臂）。
        {
            std::vector<std::byte> payload;
            appendLen(payload, 1);
            payload.push_back(std::byte{'a'});
            payload.push_back(std::byte{0}); payload.push_back(std::byte{0}); payload.push_back(std::byte{0});
            std::span<const std::byte> raw(payload.data(), payload.size());
            std::string acc, pwd;
            if (LoginProtocol::decodeLogin(raw, acc, pwd)) FAIL("short tail should fail");
        }
        // decodeSelectRealm：长度谎报（120 行短路假臂——第一串即失败）。
        {
            std::vector<std::byte> payload;
            appendLen(payload, 99);   // realm 长度谎报
            std::span<const std::byte> raw(payload.data(), payload.size());
            std::string realm;
            if (LoginProtocol::decodeSelectRealm(raw, realm)) FAIL("lying realm len should fail");
        }
    }
    PASS();

    std::cout << "\nAll LoginProtocol tests passed!" << std::endl;
    return 0;
}
