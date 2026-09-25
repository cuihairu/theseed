#include "theseed/login/ClientProtocol.h"
#include "theseed/login/LoginProtocol.h"
#include "theseed/login/SessionToken.h"

#include <cstddef>
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

    TEST("encode and decode property sync with position");
    {
        PropertySyncMsg msg;
        msg.entityId = 77;
        msg.hasPosition = true;
        msg.posX = 1.5F;
        msg.posY = 2.5F;
        msg.posZ = 3.5F;
        msg.propertyData = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};

        auto data = ClientProtocol::encodePropertySync(msg);
        ClientMessageType outType;
        std::span<const std::byte> outPayload;
        if (!LoginProtocol::parseFrame(
                std::span<const std::byte>(data.data(), data.size()),
                outType, outPayload))
            FAIL("parse failed");
        if (outType != ClientMessageType::PropertySync) FAIL("wrong type");

        PropertySyncMsg out;
        if (!ClientProtocol::decodePropertySync(outPayload, out)) FAIL("decode failed");
        if (out.entityId != 77 || !out.hasPosition) FAIL("header mismatch");
        if (out.posX != 1.5F || out.posY != 2.5F || out.posZ != 3.5F) FAIL("position mismatch");
        if (out.propertyData.size() != 3 || out.propertyData[2] != std::byte{0x03})
            FAIL("property data mismatch");
    }
    PASS();

    TEST("encode and decode property sync without position");
    {
        PropertySyncMsg msg;
        msg.entityId = 9;
        msg.propertyData = {std::byte{0xAB}};

        auto data = ClientProtocol::encodePropertySync(msg);
        ClientMessageType outType;
        std::span<const std::byte> outPayload;
        if (!LoginProtocol::parseFrame(
                std::span<const std::byte>(data.data(), data.size()),
                outType, outPayload))
            FAIL("parse failed");

        PropertySyncMsg out;
        if (!ClientProtocol::decodePropertySync(outPayload, out)) FAIL("decode failed");
        if (out.hasPosition) FAIL("should have no position");
    }
    PASS();

    TEST("decode property sync truncation branches");
    {
        PropertySyncMsg msg;
        msg.entityId = 5;
        msg.hasPosition = true;  // 带 position 但把 payload 截掉浮点数
        auto data = ClientProtocol::encodePropertySync(msg);
        ClientMessageType outType;
        std::span<const std::byte> outPayload;
        if (!LoginProtocol::parseFrame(
                std::span<const std::byte>(data.data(), data.size()),
                outType, outPayload))
            FAIL("parse failed");
        // 只保留 id+hasPos（9 字节）后再加 3 字节：浮点区越界
        PropertySyncMsg out;
        if (ClientProtocol::decodePropertySync(
                outPayload.first(12), out))
            FAIL("truncated floats should fail");

        // 再截掉长度字段之后的部分：dataLen 越界
        if (ClientProtocol::decodePropertySync(
                outPayload.first(outPayload.size() - 1), out))
            FAIL("truncated property data should fail");

        if (ClientProtocol::decodePropertySync(outPayload.first(4), out))
            FAIL("too short should fail");
    }
    PASS();

    TEST("encode and decode entity leave");
    {
        EntityLeaveMsg msg;
        msg.entityId = 0xDEADBEEF;
        auto data = ClientProtocol::encodeEntityLeave(msg);
        ClientMessageType outType;
        std::span<const std::byte> outPayload;
        if (!LoginProtocol::parseFrame(
                std::span<const std::byte>(data.data(), data.size()),
                outType, outPayload))
            FAIL("parse failed");
        if (outType != ClientMessageType::EntityLeave) FAIL("wrong type");

        EntityLeaveMsg out;
        if (!ClientProtocol::decodeEntityLeave(outPayload, out)) FAIL("decode failed");
        if (out.entityId != 0xDEADBEEF) FAIL("id mismatch");
        if (ClientProtocol::decodeEntityLeave(outPayload.first(4), out))
            FAIL("short payload should fail");
    }
    PASS();

    TEST("encode and decode action forward");
    {
        ActionMsg msg;
        msg.entityId = 31;
        msg.actionName = "MoveTo";
        msg.actionData = {std::byte{0x10}, std::byte{0x20}};

        auto data = ClientProtocol::encodeActionForward(msg);
        ClientMessageType outType;
        std::span<const std::byte> outPayload;
        if (!LoginProtocol::parseFrame(
                std::span<const std::byte>(data.data(), data.size()),
                outType, outPayload))
            FAIL("parse failed");
        if (outType != ClientMessageType::ActionForward) FAIL("wrong type");

        ActionMsg out;
        if (!ClientProtocol::decodeActionForward(outPayload, out)) FAIL("decode failed");
        if (out.entityId != 31 || out.actionName != "MoveTo") FAIL("header mismatch");
        if (out.actionData.size() != 2 || out.actionData[1] != std::byte{0x20})
            FAIL("action data mismatch");

        // 空 actionData 的编解码
        ActionMsg empty;
        empty.entityId = 1;
        empty.actionName = "";
        auto data2 = ClientProtocol::encodeActionForward(empty);
        std::span<const std::byte> payload2;
        if (!LoginProtocol::parseFrame(
                std::span<const std::byte>(data2.data(), data2.size()),
                outType, payload2))
            FAIL("parse failed");
        ActionMsg out2;
        if (!ClientProtocol::decodeActionForward(payload2, out2)) FAIL("decode failed");
        if (!out2.actionData.empty() || !out2.actionName.empty()) FAIL("should be empty");

        // 截断分支
        if (ClientProtocol::decodeActionForward(payload2.first(3), out2))
            FAIL("truncated should fail");
        if (ClientProtocol::decodeActionForward(payload2.first(payload2.size() - 1), out2))
            FAIL("truncated data should fail");
    }
    PASS();

    // --- 截断分支矩阵：actionName 长度谎报 / actionData 长度谎报 / 空 actionData、eventData ---
    TEST("action and event truncation branches");
    {
        auto appendLen = [](std::vector<std::byte>& v, std::uint32_t len) {
            v.push_back(std::byte(len & 0xFF));
            v.push_back(std::byte((len >> 8) & 0xFF));
            v.push_back(std::byte((len >> 16) & 0xFF));
            v.push_back(std::byte((len >> 24) & 0xFF));
        };
        auto u64 = [](std::vector<std::byte>& v, std::uint64_t x) {
            for (int i = 0; i < 8; ++i) v.push_back(std::byte((x >> (8 * i)) & 0xFF));
        };

        ActionMsg out;
        // actionName 长度谎报 → readStringFromSpan offset+len 越界（23/151 行真臂）。
        {
            std::vector<std::byte> v;
            u64(v, 7);
            appendLen(v, 999);   // nameLen 谎报
            if (ClientProtocol::decodeAction(std::span<const std::byte>(v.data(), v.size()), out))
                FAIL("lying nameLen should fail");
        }
        // actionData 长度谎报 → 157 行真臂。
        {
            std::vector<std::byte> v;
            u64(v, 7);
            appendLen(v, 2);
            v.push_back(std::byte{'h'}); v.push_back(std::byte{'i'});
            appendLen(v, 500);   // dataLen 谎报
            if (ClientProtocol::decodeAction(std::span<const std::byte>(v.data(), v.size()), out))
                FAIL("lying dataLen should fail");
        }
        // 空 actionData round-trip（125/136 空臂）。
        {
            ActionMsg m;
            m.entityId = 9;
            m.actionName = "idle";
            auto frame = ClientProtocol::encodeAction(m);   // actionData 为空 → 136 空臂
            ClientMessageType t{};
            std::span<const std::byte> pl;
            if (!LoginProtocol::parseFrame(std::span<const std::byte>(frame.data(), frame.size()), t, pl))
                FAIL("parse empty-action failed");
            if (!ClientProtocol::decodeAction(pl, out)) FAIL("decode empty-action failed");
            if (!out.actionData.empty() || out.actionName != "idle") FAIL("empty-action content");
        }
        // 空 eventData round-trip（186 空臂）：encode + parse 即可（无独立 decodeEntityEvent）。
        {
            EntityEventMsg m;
            m.entityId = 3;
            m.eventName = "tick";
            auto frame = ClientProtocol::encodeEntityEvent(m);   // eventData 为空 → 186 空臂
            ClientMessageType t{};
            std::span<const std::byte> pl;
            if (!LoginProtocol::parseFrame(std::span<const std::byte>(frame.data(), frame.size()), t, pl))
                FAIL("parse empty-event failed");
            if (pl.size() != 8 + 4 + 4 + 4) FAIL("empty-event payload size");
        }
        // propertySync：dataLen 完整但谎报（123 真臂）与非空 propertyData（125 真臂）。
        {
            // 手工拼：entityId(8) + hasPos(1) + pos(12) + dataLen(4, 谎报 500)。
            // hasPos 必须为 1：为 0 时 pos 12 字节不消费，dataLen 读到 pos 前字节（0）即合法返回。
            std::vector<std::byte> v;
            for (int i = 0; i < 8; ++i) v.push_back(std::byte{0});
            v.push_back(std::byte{1});
            for (int i = 0; i < 12; ++i) v.push_back(std::byte{0});
            appendLen(v, 500);
            PropertySyncMsg ps;
            if (ClientProtocol::decodePropertySync(std::span<const std::byte>(v.data(), v.size()), ps))
                FAIL("lying property dataLen should fail");
        }
        {
            PropertySyncMsg m;
            m.entityId = 8;
            m.propertyData = {std::byte{0xAA}, std::byte{0xBB}};
            auto frame = ClientProtocol::encodePropertySync(m);
            ClientMessageType t{};
            std::span<const std::byte> pl;
            if (!LoginProtocol::parseFrame(std::span<const std::byte>(frame.data(), frame.size()), t, pl))
                FAIL("parse psync failed");
            PropertySyncMsg out;
            if (!ClientProtocol::decodePropertySync(pl, out)) FAIL("decode psync failed");
            if (out.propertyData.size() != 2 || out.propertyData[0] != std::byte{0xAA})
                FAIL("psync content");
        }
    }
    PASS();

    std::cout << "\nAll ClientProtocol tests passed!" << std::endl;
    return 0;
}
