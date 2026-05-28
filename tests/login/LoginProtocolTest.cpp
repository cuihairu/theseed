#include "theseed/login/LoginProtocol.h"
#include "theseed/login/SessionToken.h"

#include <cassert>
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

    std::cout << "\nAll LoginProtocol tests passed!" << std::endl;
    return 0;
}
