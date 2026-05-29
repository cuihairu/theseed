#include "theseed/core/EntityData.h"
#include "theseed/core/IEntityStore.h"
#include "theseed/db/DBApp.h"
#include "theseed/db/DBProtocol.h"
#include "theseed/db/RemoteEntityStore.h"
#include "theseed/runtime/PipedTransport.h"
#include "theseed/runtime/RuntimeTransport.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace theseed::db;
using namespace theseed::core;
using namespace theseed::runtime;

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

static bool writeFile(const std::string& path, const std::string& content) {
    std::ofstream f(path);
    if (!f.is_open()) return false;
    f << content;
    return true;
}

int main() {
    std::cout << "DBApp tests:" << std::endl;

    // Create temp store directory
    std::string storeDir = "test_dbapp_store";
    std::filesystem::remove_all(storeDir);
    std::filesystem::create_directory(storeDir);

    TEST("DBProtocol save/load round trip");
    {
        // Test full protocol encode → decode cycle
        EntityData data;
        data.id = 1;
        data.entityType = "Avatar";
        PropertyData prop;
        prop.id = 0;
        prop.name = "level";
        prop.type = DataType::Int32;
        prop.rawValue = {std::byte{42}, std::byte{0}, std::byte{0}, std::byte{0}};
        data.properties.push_back(prop);

        auto req = DBProtocol::encodeSaveRequest(1, data);
        EntityId outId;
        EntityData outData;
        if (!DBProtocol::decodeSaveRequest(
                std::span<const std::byte>(req.data(), req.size()),
                outId, outData))
            FAIL("decode save request failed");
        if (outId != 1) FAIL("id mismatch");
        if (outData.entityType != "Avatar") FAIL("type mismatch");

        auto resp = DBProtocol::encodeSaveResponse(true);
        bool success;
        if (!DBProtocol::decodeSaveResponse(
                std::span<const std::byte>(resp.data(), resp.size()),
                success))
            FAIL("decode save response failed");
        if (!success) FAIL("should be success");
    }
    PASS();

    TEST("DBProtocol encode/decode load request");
    {
        auto req = DBProtocol::encodeLoadRequest(42, "Avatar");
        EntityId outId;
        std::string outType;
        if (!DBProtocol::decodeLoadRequest(
                std::span<const std::byte>(req.data(), req.size()),
                outId, outType))
            FAIL("decode failed");
        if (outId != 42) FAIL("id mismatch");
        if (outType != "Avatar") FAIL("type mismatch");
    }
    PASS();

    TEST("DBProtocol encode/decode allocId response");
    {
        auto resp = DBProtocol::encodeAllocIdResponse(123);
        EntityId outId;
        if (!DBProtocol::decodeAllocIdResponse(
                std::span<const std::byte>(resp.data(), resp.size()),
                outId))
            FAIL("decode failed");
        if (outId != 123) FAIL("id mismatch");
    }
    PASS();

    TEST("DBProtocol encode/decode listIds response");
    {
        std::vector<EntityId> ids = {1, 2, 3};
        auto resp = DBProtocol::encodeListIdsResponse(ids);
        std::vector<EntityId> outIds;
        if (!DBProtocol::decodeListIdsResponse(
                std::span<const std::byte>(resp.data(), resp.size()),
                outIds))
            FAIL("decode failed");
        if (outIds.size() != 3) FAIL("count mismatch");
        if (outIds[0] != 1 || outIds[1] != 2 || outIds[2] != 3) FAIL("ids mismatch");
    }
    PASS();

    TEST("DBProtocol encode/decode listTypes response");
    {
        std::vector<std::string> types = {"Avatar", "Monster"};
        auto resp = DBProtocol::encodeListTypesResponse(types);
        std::vector<std::string> outTypes;
        if (!DBProtocol::decodeListTypesResponse(
                std::span<const std::byte>(resp.data(), resp.size()),
                outTypes))
            FAIL("decode failed");
        if (outTypes.size() != 2) FAIL("count mismatch");
        if (outTypes[0] != "Avatar") FAIL("type mismatch");
    }
    PASS();

    std::cout << "\nAll DBApp tests passed!" << std::endl;
    return 0;
}
