#include "theseed/login/ClientProtocol.h"
#include "theseed/login/LoginProtocol.h"
#include "theseed/login/SessionToken.h"

#include <cstdint>
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
    std::cout << "ClientProtocol tests:" << std::endl;

    TEST("encode and decode enter game request");
    {
        std::string token = SessionToken::issue("player1", "realm1");
        // Manually encode token as payload
        std::vector<std::byte> payload;
        auto len = static_cast<uint32_t>(token.size());
        payload.push_back(std::byte(len & 0xFF));
        payload.push_back(std::byte((len >> 8) & 0xFF));
        payload.push_back(std::byte((len >> 16) & 0xFF));
        payload.push_back(std::byte((len >> 24) & 0xFF));
        for (char c : token) payload.push_back(static_cast<std::byte>(c));

        auto framed = LoginProtocol::frameMessage(
            ClientMessageType::EnterGame,
            std::span<const std::byte>(payload.data(), payload.size()));

        ClientMessageType outType;
        std::span<const std::byte> outPayload;
        if (!LoginProtocol::parseFrame(
                std::span<const std::byte>(framed.data(), framed.size()),
                outType, outPayload))
            FAIL("parse failed");
        if (outType != ClientMessageType::EnterGame) FAIL("type mismatch");

        std::string outToken;
        if (!ClientProtocol::decodeEnterGame(outPayload, outToken)) FAIL("decode failed");
        if (outToken != token) FAIL("token mismatch");
    }
    PASS();

    TEST("encode enter game response success");
    {
        EnterGameResponse resp;
        resp.success = true;
        resp.entityId = 42;
        resp.entityType = "Player";

        auto data = ClientProtocol::encodeEnterGameResponse(resp);
        if (data.empty()) FAIL("empty response");

        ClientMessageType outType;
        std::span<const std::byte> outPayload;
        if (!LoginProtocol::parseFrame(
                std::span<const std::byte>(data.data(), data.size()),
                outType, outPayload))
            FAIL("parse failed");
        if (outType != ClientMessageType::EnterGameResponse) FAIL("wrong type");
    }
    PASS();

    TEST("encode enter game response failure");
    {
        EnterGameResponse resp;
        resp.success = false;
        resp.error = "invalid token";

        auto data = ClientProtocol::encodeEnterGameResponse(resp);
        if (data.empty()) FAIL("empty response");

        ClientMessageType outType;
        std::span<const std::byte> outPayload;
        if (!LoginProtocol::parseFrame(
                std::span<const std::byte>(data.data(), data.size()),
                outType, outPayload))
            FAIL("parse failed");
        if (outType != ClientMessageType::EnterGameResponse) FAIL("wrong type");
    }
    PASS();

    TEST("encode entity enter message");
    {
        EntityEnterMsg msg;
        msg.entityId = 100;
        msg.entityType = "Player";

        auto data = ClientProtocol::encodeEntityEnter(msg);
        if (data.empty()) FAIL("empty message");

        ClientMessageType outType;
        std::span<const std::byte> outPayload;
        if (!LoginProtocol::parseFrame(
                std::span<const std::byte>(data.data(), data.size()),
                outType, outPayload))
            FAIL("parse failed");
        if (outType != ClientMessageType::EntityEnter) FAIL("wrong type");
    }
    PASS();

    std::cout << "\nAll ClientProtocol tests passed!" << std::endl;
    return 0;
}
