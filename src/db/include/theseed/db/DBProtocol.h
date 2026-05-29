#pragma once

#include "theseed/core/EntityData.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace theseed::db {

// DB method names (carried in RuntimeInvocation::method)
namespace DBMethod {
inline constexpr const char* kLoad = "db.load";
inline constexpr const char* kLoadOk = "db.load.ok";
inline constexpr const char* kSave = "db.save";
inline constexpr const char* kSaveOk = "db.save.ok";
inline constexpr const char* kRemove = "db.remove";
inline constexpr const char* kRemoveOk = "db.remove.ok";
inline constexpr const char* kAllocId = "db.allocId";
inline constexpr const char* kAllocIdOk = "db.allocId.ok";
inline constexpr const char* kListIds = "db.listIds";
inline constexpr const char* kListIdsOk = "db.listIds.ok";
inline constexpr const char* kListTypes = "db.listTypes";
inline constexpr const char* kListTypesOk = "db.listTypes.ok";
}  // namespace DBMethod

class DBProtocol {
public:
    // Request encoding
    static std::vector<std::byte> encodeLoadRequest(core::EntityId id,
                                                     const std::string& entityType);
    static std::vector<std::byte> encodeSaveRequest(core::EntityId id,
                                                     const core::EntityData& data);
    static std::vector<std::byte> encodeRemoveRequest(core::EntityId id);
    static std::vector<std::byte> encodeAllocIdRequest();
    static std::vector<std::byte> encodeListIdsRequest(const std::string& entityType);
    static std::vector<std::byte> encodeListTypesRequest();

    // Response encoding
    static std::vector<std::byte> encodeLoadResponse(bool success,
                                                      const core::EntityData& data);
    static std::vector<std::byte> encodeSaveResponse(bool success);
    static std::vector<std::byte> encodeRemoveResponse(bool success);
    static std::vector<std::byte> encodeAllocIdResponse(core::EntityId id);
    static std::vector<std::byte> encodeListIdsResponse(const std::vector<core::EntityId>& ids);
    static std::vector<std::byte> encodeListTypesResponse(const std::vector<std::string>& types);

    // Request decoding
    static bool decodeLoadRequest(std::span<const std::byte> payload,
                                   core::EntityId& outId,
                                   std::string& outEntityType);
    static bool decodeSaveRequest(std::span<const std::byte> payload,
                                   core::EntityId& outId,
                                   core::EntityData& outData);
    static bool decodeRemoveRequest(std::span<const std::byte> payload,
                                     core::EntityId& outId);

    // Response decoding
    static bool decodeLoadResponse(std::span<const std::byte> payload,
                                    bool& outSuccess,
                                    core::EntityData& outData);
    static bool decodeSaveResponse(std::span<const std::byte> payload, bool& outSuccess);
    static bool decodeRemoveResponse(std::span<const std::byte> payload, bool& outSuccess);
    static bool decodeAllocIdResponse(std::span<const std::byte> payload,
                                       core::EntityId& outId);
    static bool decodeListIdsResponse(std::span<const std::byte> payload,
                                       std::vector<core::EntityId>& outIds);
    static bool decodeListTypesResponse(std::span<const std::byte> payload,
                                         std::vector<std::string>& outTypes);
};

}  // namespace theseed::db
