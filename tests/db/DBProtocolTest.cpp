// DBProtocol 直测：DBApp 与各存储客户端（RemoteEntityStore/LoginApp）共享的
// DB 帧编解码。既有 DBAppTest 只从 DBApp 服务视角顺带驱动编解码，这里按协议
// 本身锚定：全部消息 encode→decode 往返（64 位极值 id、空串、空列表、多属
// 性 EntityData 与二进制安全载荷），每个解码器的逐前缀截断拒绝矩阵（任何严
// 格前缀都必须返回 false，锁死「截断不半解码」的边界口径），以及失败/未命
// 中响应的短路与哨兵臂（success=false 不携带载荷、not-found 置零）。
#include "theseed/db/DBProtocol.h"
#include "theseed/core/EntityData.h"
#include "theseed/foundation/MemoryStream.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <vector>

using namespace theseed::core;
using namespace theseed::db;

#define TEST(name)                            \
    do {                                      \
        std::cout << "  " << name << "... ";  \
    } while (0)
#define PASS() std::cout << "OK" << std::endl
#define FAIL(msg)                                   \
    do {                                            \
        std::cout << "FAILED: " << msg << std::endl; \
        return 1;                                   \
    } while (0)

namespace {

// 样本：三属性 EntityData——定宽 int、变长 string、二进制 blob（含
// 0x00/0xFF），覆盖定宽与变长两条编码路径和二进制安全性。
EntityData sampleEntity(EntityId id) {
    EntityData data;
    data.id = id;
    data.entityType = "Avatar";

    PropertyData level;
    level.id = 0;
    level.name = "level";
    level.type = DataType::Int32;
    level.rawValue = {std::byte{42}, std::byte{0}, std::byte{0}, std::byte{0}};
    data.properties.push_back(level);

    PropertyData name;
    name.id = 1;
    name.name = "name";
    name.type = DataType::String;
    name.rawValue = {std::byte{'a'}, std::byte{'v'}, std::byte{'a'}};
    data.properties.push_back(name);

    PropertyData blob;
    blob.id = 2;
    blob.name = "raw";
    blob.type = DataType::Blob;
    blob.rawValue = {std::byte{0x00}, std::byte{0xFF}, std::byte{0x7F}, std::byte{0x80}};
    data.properties.push_back(blob);

    return data;
}

// 全字段一致性判定：false = 已打印失配明细，调用方沿 main 的 FAIL 路径退出。
bool expectSameEntity(const EntityData& expected, const EntityData& actual,
                      const char* what) {
    if (actual.id != expected.id) {
        std::cout << "FAILED: " << what << ": id mismatch" << std::endl;
        return false;
    }
    if (actual.entityType != expected.entityType) {
        std::cout << "FAILED: " << what << ": entityType mismatch" << std::endl;
        return false;
    }
    if (actual.properties.size() != expected.properties.size()) {
        std::cout << "FAILED: " << what << ": property count mismatch" << std::endl;
        return false;
    }
    for (std::size_t i = 0; i < expected.properties.size(); ++i) {
        const auto& e = expected.properties[i];
        const auto& a = actual.properties[i];
        if (a.id != e.id || a.name != e.name || a.type != e.type ||
            a.rawValue != e.rawValue) {
            std::cout << "FAILED: " << what << ": property " << i
                      << " mismatch" << std::endl;
            return false;
        }
    }
    return true;
}

// 截断拒绝矩阵：合法载荷的每个严格前缀，解码都必须返回 false。一个循环
// 同时锁住定长头、计数与 readString 的全部截断臂。false = 某前缀被接受
// （已打印明细）。
template <typename Decode>
bool expectPrefixesRejected(Decode&& decode,
                            const std::vector<std::byte>& payload,
                            const char* what) {
    for (std::size_t cut = 0; cut < payload.size(); ++cut) {
        if (decode(std::span<const std::byte>(payload.data(), cut))) {
            std::cout << "FAILED: " << what << ": accepted a truncated prefix of "
                      << cut << "/" << payload.size() << " bytes" << std::endl;
            return false;
        }
    }
    return true;
}

}  // namespace

int main() {
    std::cout << "DBProtocolTest:" << std::endl;

    // --- 请求编解码 ---
    TEST("load request round-trips id and entityType");
    {
        const auto payload =
            DBProtocol::encodeLoadRequest(0xDEADBEEFCAFEBABEull, "Avatar");
        EntityId id = 0;
        std::string type;
        if (!DBProtocol::decodeLoadRequest(payload, id, type))
            FAIL("decodeLoadRequest failed");
        if (id != 0xDEADBEEFCAFEBABEull || type != "Avatar")
            FAIL("load request round-trip mismatch");
        if (!expectPrefixesRejected(
                [](std::span<const std::byte> p) {
                    EntityId id = 0;
                    std::string type;
                    return DBProtocol::decodeLoadRequest(p, id, type);
                },
                payload, "decodeLoadRequest"))
            return 1;
        PASS();
    }

    TEST("save request round-trips id and multi-property entity data");
    {
        const EntityId id = 0xFFFFFFFFFFFFFFFFull;
        const auto data = sampleEntity(id);
        const auto payload = DBProtocol::encodeSaveRequest(id, data);
        EntityId outId = 0;
        EntityData outData;
        if (!DBProtocol::decodeSaveRequest(payload, outId, outData))
            FAIL("decodeSaveRequest failed");
        if (outId != id) FAIL("save request id mismatch");
        if (!expectSameEntity(data, outData, "decodeSaveRequest")) return 1;
        if (!expectPrefixesRejected(
                [](std::span<const std::byte> p) {
                    EntityId id = 0;
                    EntityData data;
                    return DBProtocol::decodeSaveRequest(p, id, data);
                },
                payload, "decodeSaveRequest"))
            return 1;
        PASS();
    }

    TEST("remove request round-trips extreme id");
    {
        const auto payload = DBProtocol::encodeRemoveRequest(0xFFFFFFFFFFFFFFFFull);
        EntityId id = 0;
        if (!DBProtocol::decodeRemoveRequest(payload, id))
            FAIL("decodeRemoveRequest failed");
        if (id != 0xFFFFFFFFFFFFFFFFull) FAIL("remove request id mismatch");
        if (!expectPrefixesRejected(
                [](std::span<const std::byte> p) {
                    EntityId id = 0;
                    return DBProtocol::decodeRemoveRequest(p, id);
                },
                payload, "decodeRemoveRequest"))
            return 1;
        PASS();
    }

    TEST("allocId request contract is an empty payload");
    {
        // 无参请求：载荷恒空，消费方（DBApp）不解码——契约就是「空」本身。
        if (!DBProtocol::encodeAllocIdRequest().empty())
            FAIL("allocId request should carry no payload");
        PASS();
    }

    TEST("listIds request carries a length-prefixed entityType");
    {
        // listIds 请求没有专用解码器（DBApp 直接读流）：布局是
        // u32 长度前缀 + 字节，用 MemoryStream 按消费方同款读法往返。
        const auto payload = DBProtocol::encodeListIdsRequest("Avatar");
        theseed::foundation::MemoryStream ms;
        ms.writeBytes(payload.data(), payload.size());
        ms.resetRead();
        if (ms.readString() != "Avatar")
            FAIL("listIds request entityType mismatch");

        const auto emptyPayload = DBProtocol::encodeListIdsRequest("");
        theseed::foundation::MemoryStream empty;
        empty.writeBytes(emptyPayload.data(), emptyPayload.size());
        empty.resetRead();
        if (!empty.readString().empty())
            FAIL("empty entityType round-trip mismatch");
        PASS();
    }

    TEST("listTypes request contract is an empty payload");
    {
        if (!DBProtocol::encodeListTypesRequest().empty())
            FAIL("listTypes request should carry no payload");
        PASS();
    }

    // --- 响应编解码 ---
    TEST("load response round-trips entity data and rejects truncation");
    {
        const auto data = sampleEntity(88);
        const auto ok = DBProtocol::encodeLoadResponse(true, data);
        bool success = false;
        EntityData outData;
        if (!DBProtocol::decodeLoadResponse(ok, success, outData))
            FAIL("decodeLoadResponse failed");
        if (!success) FAIL("load response success flag lost");
        if (!expectSameEntity(data, outData, "decodeLoadResponse")) return 1;
        if (!expectPrefixesRejected(
                [](std::span<const std::byte> p) {
                    bool success = false;
                    EntityData data;
                    return DBProtocol::decodeLoadResponse(p, success, data);
                },
                ok, "decodeLoadResponse"))
            return 1;
        PASS();
    }

    TEST("load response failure arm is a bare flag with no payload");
    {
        const auto failed =
            DBProtocol::encodeLoadResponse(false, sampleEntity(1));
        if (failed.size() != 1) FAIL("failure response should carry one byte");
        bool success = true;
        EntityData outData;
        if (!DBProtocol::decodeLoadResponse(failed, success, outData))
            FAIL("decodeLoadResponse failed on failure arm");
        if (success) FAIL("failure arm must report success=false");
        PASS();
    }

    TEST("save/remove response round-trip both arms and reject truncation");
    {
        for (const bool success : {true, false}) {
            const auto savePayload = DBProtocol::encodeSaveResponse(success);
            const auto removePayload = DBProtocol::encodeRemoveResponse(success);
            bool saveFlag = !success;
            bool removeFlag = !success;
            if (!DBProtocol::decodeSaveResponse(savePayload, saveFlag) ||
                !DBProtocol::decodeRemoveResponse(removePayload, removeFlag))
                FAIL("bool response decode failed");
            if (saveFlag != success || removeFlag != success)
                FAIL("bool response flag mismatch");
        }
        if (!expectPrefixesRejected(
                [](std::span<const std::byte> p) {
                    bool flag = false;
                    return DBProtocol::decodeSaveResponse(p, flag);
                },
                DBProtocol::encodeSaveResponse(true), "decodeSaveResponse"))
            return 1;
        if (!expectPrefixesRejected(
                [](std::span<const std::byte> p) {
                    bool flag = false;
                    return DBProtocol::decodeRemoveResponse(p, flag);
                },
                DBProtocol::encodeRemoveResponse(true), "decodeRemoveResponse"))
            return 1;
        PASS();
    }

    TEST("allocId response round-trips extreme id and rejects truncation");
    {
        const auto payload = DBProtocol::encodeAllocIdResponse(0x8000000000000001ull);
        EntityId id = 0;
        if (!DBProtocol::decodeAllocIdResponse(payload, id))
            FAIL("decodeAllocIdResponse failed");
        if (id != 0x8000000000000001ull) FAIL("allocId response id mismatch");
        if (!expectPrefixesRejected(
                [](std::span<const std::byte> p) {
                    EntityId id = 0;
                    return DBProtocol::decodeAllocIdResponse(p, id);
                },
                payload, "decodeAllocIdResponse"))
            return 1;
        PASS();
    }

    TEST("listIds response round-trips empty and multi-element lists");
    {
        const std::vector<EntityId> ids = {0, 1, 0xFFFFFFFFFFFFFFFFull};
        const auto payload = DBProtocol::encodeListIdsResponse(ids);
        std::vector<EntityId> outIds;
        if (!DBProtocol::decodeListIdsResponse(payload, outIds))
            FAIL("decodeListIdsResponse failed");
        if (outIds != ids) FAIL("listIds response round-trip mismatch");

        std::vector<EntityId> emptyIds = {123};
        if (!DBProtocol::decodeListIdsResponse(
                DBProtocol::encodeListIdsResponse({}), emptyIds))
            FAIL("decodeListIdsResponse failed on empty list");
        if (!emptyIds.empty()) FAIL("empty list round-trip mismatch");

        if (!expectPrefixesRejected(
                [](std::span<const std::byte> p) {
                    std::vector<EntityId> ids;
                    return DBProtocol::decodeListIdsResponse(p, ids);
                },
                payload, "decodeListIdsResponse"))
            return 1;
        PASS();
    }

    TEST("listTypes response round-trips strings including empty entries");
    {
        const std::vector<std::string> types = {"Avatar", "", "Account"};
        const auto payload = DBProtocol::encodeListTypesResponse(types);
        std::vector<std::string> outTypes;
        if (!DBProtocol::decodeListTypesResponse(payload, outTypes))
            FAIL("decodeListTypesResponse failed");
        if (outTypes != types) FAIL("listTypes response round-trip mismatch");

        if (!expectPrefixesRejected(
                [](std::span<const std::byte> p) {
                    std::vector<std::string> types;
                    return DBProtocol::decodeListTypesResponse(p, types);
                },
                payload, "decodeListTypesResponse"))
            return 1;
        PASS();
    }

    // --- Account 协议 ---
    TEST("queryAccount request round-trips username");
    {
        const auto payload = DBProtocol::encodeQueryAccountRequest("alice");
        std::string username = "stale";
        if (!DBProtocol::decodeQueryAccountRequest(payload, username))
            FAIL("decodeQueryAccountRequest failed");
        if (username != "alice") FAIL("queryAccount username mismatch");
        if (!expectPrefixesRejected(
                [](std::span<const std::byte> p) {
                    std::string username;
                    return DBProtocol::decodeQueryAccountRequest(p, username);
                },
                payload, "decodeQueryAccountRequest"))
            return 1;
        PASS();
    }

    TEST("queryAccount response round-trips found arm");
    {
        const auto payload =
            DBProtocol::encodeQueryAccountResponse(true, 0x1122334455667788ull,
                                                   "s3cret");
        bool found = false;
        EntityId entityId = 0;
        std::string password;
        if (!DBProtocol::decodeQueryAccountResponse(payload, found, entityId,
                                                    password))
            FAIL("decodeQueryAccountResponse failed");
        if (!found || entityId != 0x1122334455667788ull || password != "s3cret")
            FAIL("queryAccount found arm mismatch");
        if (!expectPrefixesRejected(
                [](std::span<const std::byte> p) {
                    bool found = false;
                    EntityId entityId = 0;
                    std::string password;
                    return DBProtocol::decodeQueryAccountResponse(p, found,
                                                                  entityId,
                                                                  password);
                },
                payload, "decodeQueryAccountResponse"))
            return 1;
        PASS();
    }

    TEST("queryAccount response not-found arm zeroes id and password");
    {
        const auto payload =
            DBProtocol::encodeQueryAccountResponse(false, 0, "");
        bool found = true;
        EntityId entityId = 999;
        std::string password = "stale";
        if (!DBProtocol::decodeQueryAccountResponse(payload, found, entityId,
                                                    password))
            FAIL("decodeQueryAccountResponse failed on not-found arm");
        if (found) FAIL("not-found arm must report found=false");
        if (entityId != 0 || !password.empty())
            FAIL("not-found arm must zero id and password");
        PASS();
    }

    TEST("createAccount request round-trips both credentials");
    {
        const auto payload =
            DBProtocol::encodeCreateAccountRequest("bob", "pass-**word!");
        std::string username;
        std::string password;
        if (!DBProtocol::decodeCreateAccountRequest(payload, username, password))
            FAIL("decodeCreateAccountRequest failed");
        if (username != "bob" || password != "pass-**word!")
            FAIL("createAccount credentials mismatch");
        if (!expectPrefixesRejected(
                [](std::span<const std::byte> p) {
                    std::string username;
                    std::string password;
                    return DBProtocol::decodeCreateAccountRequest(p, username,
                                                                  password);
                },
                payload, "decodeCreateAccountRequest"))
            return 1;
        PASS();
    }

    TEST("createAccount response round-trips success and failure arms");
    {
        const auto ok = DBProtocol::encodeCreateAccountResponse(true, 42);
        bool success = false;
        EntityId entityId = 0;
        if (!DBProtocol::decodeCreateAccountResponse(ok, success, entityId))
            FAIL("decodeCreateAccountResponse failed");
        if (!success || entityId != 42) FAIL("createAccount success arm mismatch");

        const auto failed = DBProtocol::encodeCreateAccountResponse(false, 0);
        success = true;
        entityId = 777;
        if (!DBProtocol::decodeCreateAccountResponse(failed, success, entityId))
            FAIL("decodeCreateAccountResponse failed on failure arm");
        if (success || entityId != 0)
            FAIL("createAccount failure arm must zero id");

        if (!expectPrefixesRejected(
                [](std::span<const std::byte> p) {
                    bool success = false;
                    EntityId entityId = 0;
                    return DBProtocol::decodeCreateAccountResponse(p, success,
                                                                   entityId);
                },
                ok, "decodeCreateAccountResponse"))
            return 1;
        PASS();
    }

    std::cout << "\nAll DBProtocol tests passed!" << std::endl;
    return 0;
}
