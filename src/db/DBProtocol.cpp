#include "theseed/db/DBProtocol.h"
#include "theseed/foundation/MemoryStream.h"

#include <cstring>

namespace theseed::db {

static void writeString(foundation::MemoryStream& ms, const std::string& s) {
    ms.writeUint32(static_cast<std::uint32_t>(s.size()));
    if (!s.empty()) ms.writeBytes(reinterpret_cast<const std::byte*>(s.data()), s.size());
}

static bool readString(std::span<const std::byte> data, std::size_t& offset, std::string& out) {
    if (offset + 4 > data.size()) return false;
    auto len = static_cast<std::uint32_t>(data[offset])
             | (static_cast<std::uint32_t>(data[offset + 1]) << 8)
             | (static_cast<std::uint32_t>(data[offset + 2]) << 16)
             | (static_cast<std::uint32_t>(data[offset + 3]) << 24);
    offset += 4;
    if (offset + len > data.size()) return false;
    out.resize(len);
    if (len > 0) {
        std::memcpy(out.data(), data.data() + offset, len);
        offset += len;
    }
    return true;
}

// --- Request encoding ---

std::vector<std::byte> DBProtocol::encodeLoadRequest(core::EntityId id,
                                                      const std::string& entityType) {
    foundation::MemoryStream ms;
    ms.writeUint64(id);
    writeString(ms, entityType);
    return std::vector<std::byte>(ms.data(), ms.data() + ms.size());
}

std::vector<std::byte> DBProtocol::encodeSaveRequest(core::EntityId id,
                                                      const core::EntityData& data) {
    foundation::MemoryStream ms;
    ms.writeUint64(id);
    core::encodeEntityData(ms, data);
    return std::vector<std::byte>(ms.data(), ms.data() + ms.size());
}

std::vector<std::byte> DBProtocol::encodeRemoveRequest(core::EntityId id) {
    foundation::MemoryStream ms;
    ms.writeUint64(id);
    return std::vector<std::byte>(ms.data(), ms.data() + ms.size());
}

std::vector<std::byte> DBProtocol::encodeAllocIdRequest() {
    return {};
}

std::vector<std::byte> DBProtocol::encodeListIdsRequest(const std::string& entityType) {
    foundation::MemoryStream ms;
    writeString(ms, entityType);
    return std::vector<std::byte>(ms.data(), ms.data() + ms.size());
}

std::vector<std::byte> DBProtocol::encodeListTypesRequest() {
    return {};
}

// --- Response encoding ---

std::vector<std::byte> DBProtocol::encodeLoadResponse(bool success,
                                                       const core::EntityData& data) {
    foundation::MemoryStream ms;
    ms.writeUint8(success ? 1 : 0);
    if (success) {
        core::encodeEntityData(ms, data);
    }
    return std::vector<std::byte>(ms.data(), ms.data() + ms.size());
}

std::vector<std::byte> DBProtocol::encodeSaveResponse(bool success) {
    foundation::MemoryStream ms;
    ms.writeUint8(success ? 1 : 0);
    return std::vector<std::byte>(ms.data(), ms.data() + ms.size());
}

std::vector<std::byte> DBProtocol::encodeRemoveResponse(bool success) {
    foundation::MemoryStream ms;
    ms.writeUint8(success ? 1 : 0);
    return std::vector<std::byte>(ms.data(), ms.data() + ms.size());
}

std::vector<std::byte> DBProtocol::encodeAllocIdResponse(core::EntityId id) {
    foundation::MemoryStream ms;
    ms.writeUint64(id);
    return std::vector<std::byte>(ms.data(), ms.data() + ms.size());
}

std::vector<std::byte> DBProtocol::encodeListIdsResponse(const std::vector<core::EntityId>& ids) {
    foundation::MemoryStream ms;
    ms.writeUint32(static_cast<std::uint32_t>(ids.size()));
    for (auto id : ids) {
        ms.writeUint64(id);
    }
    return std::vector<std::byte>(ms.data(), ms.data() + ms.size());
}

std::vector<std::byte> DBProtocol::encodeListTypesResponse(const std::vector<std::string>& types) {
    foundation::MemoryStream ms;
    ms.writeUint32(static_cast<std::uint32_t>(types.size()));
    for (const auto& t : types) {
        writeString(ms, t);
    }
    return std::vector<std::byte>(ms.data(), ms.data() + ms.size());
}

// --- Request decoding ---

bool DBProtocol::decodeLoadRequest(std::span<const std::byte> payload,
                                    core::EntityId& outId,
                                    std::string& outEntityType) {
    if (payload.size() < 8) return false;
    std::size_t offset = 0;
    outId = static_cast<core::EntityId>(payload[0])
          | (static_cast<core::EntityId>(payload[1]) << 8)
          | (static_cast<core::EntityId>(payload[2]) << 16)
          | (static_cast<core::EntityId>(payload[3]) << 24)
          | (static_cast<core::EntityId>(payload[4]) << 32)
          | (static_cast<core::EntityId>(payload[5]) << 40)
          | (static_cast<core::EntityId>(payload[6]) << 48)
          | (static_cast<core::EntityId>(payload[7]) << 56);
    offset = 8;
    return readString(payload, offset, outEntityType);
}

bool DBProtocol::decodeSaveRequest(std::span<const std::byte> payload,
                                    core::EntityId& outId,
                                    core::EntityData& outData) {
    if (payload.size() < 8) return false;
    std::size_t offset = 0;
    outId = static_cast<core::EntityId>(payload[0])
          | (static_cast<core::EntityId>(payload[1]) << 8)
          | (static_cast<core::EntityId>(payload[2]) << 16)
          | (static_cast<core::EntityId>(payload[3]) << 24)
          | (static_cast<core::EntityId>(payload[4]) << 32)
          | (static_cast<core::EntityId>(payload[5]) << 40)
          | (static_cast<core::EntityId>(payload[6]) << 48)
          | (static_cast<core::EntityId>(payload[7]) << 56);
    offset = 8;
    foundation::MemoryStream ms;
    ms.writeBytes(payload.data() + offset, payload.size() - offset);
    ms.resetRead();
    return core::decodeEntityData(ms, outData);
}

bool DBProtocol::decodeRemoveRequest(std::span<const std::byte> payload,
                                      core::EntityId& outId) {
    if (payload.size() < 8) return false;
    outId = static_cast<core::EntityId>(payload[0])
          | (static_cast<core::EntityId>(payload[1]) << 8)
          | (static_cast<core::EntityId>(payload[2]) << 16)
          | (static_cast<core::EntityId>(payload[3]) << 24)
          | (static_cast<core::EntityId>(payload[4]) << 32)
          | (static_cast<core::EntityId>(payload[5]) << 40)
          | (static_cast<core::EntityId>(payload[6]) << 48)
          | (static_cast<core::EntityId>(payload[7]) << 56);
    return true;
}

// --- Response decoding ---

bool DBProtocol::decodeLoadResponse(std::span<const std::byte> payload,
                                     bool& outSuccess,
                                     core::EntityData& outData) {
    if (payload.empty()) return false;
    outSuccess = static_cast<std::uint8_t>(payload[0]) != 0;
    if (!outSuccess) return true;
    if (payload.size() < 2) return false;
    foundation::MemoryStream ms;
    ms.writeBytes(payload.data() + 1, payload.size() - 1);
    ms.resetRead();
    return core::decodeEntityData(ms, outData);
}

bool DBProtocol::decodeSaveResponse(std::span<const std::byte> payload, bool& outSuccess) {
    if (payload.empty()) return false;
    outSuccess = static_cast<std::uint8_t>(payload[0]) != 0;
    return true;
}

bool DBProtocol::decodeRemoveResponse(std::span<const std::byte> payload, bool& outSuccess) {
    if (payload.empty()) return false;
    outSuccess = static_cast<std::uint8_t>(payload[0]) != 0;
    return true;
}

bool DBProtocol::decodeAllocIdResponse(std::span<const std::byte> payload,
                                        core::EntityId& outId) {
    if (payload.size() < 8) return false;
    outId = static_cast<core::EntityId>(payload[0])
          | (static_cast<core::EntityId>(payload[1]) << 8)
          | (static_cast<core::EntityId>(payload[2]) << 16)
          | (static_cast<core::EntityId>(payload[3]) << 24)
          | (static_cast<core::EntityId>(payload[4]) << 32)
          | (static_cast<core::EntityId>(payload[5]) << 40)
          | (static_cast<core::EntityId>(payload[6]) << 48)
          | (static_cast<core::EntityId>(payload[7]) << 56);
    return true;
}

bool DBProtocol::decodeListIdsResponse(std::span<const std::byte> payload,
                                        std::vector<core::EntityId>& outIds) {
    if (payload.size() < 4) return false;
    std::size_t offset = 0;
    auto count = static_cast<std::uint32_t>(payload[0])
               | (static_cast<std::uint32_t>(payload[1]) << 8)
               | (static_cast<std::uint32_t>(payload[2]) << 16)
               | (static_cast<std::uint32_t>(payload[3]) << 24);
    offset = 4;
    outIds.clear();
    for (std::uint32_t i = 0; i < count; ++i) {
        if (offset + 8 > payload.size()) return false;
        auto id = static_cast<core::EntityId>(payload[offset])
                | (static_cast<core::EntityId>(payload[offset + 1]) << 8)
                | (static_cast<core::EntityId>(payload[offset + 2]) << 16)
                | (static_cast<core::EntityId>(payload[offset + 3]) << 24)
                | (static_cast<core::EntityId>(payload[offset + 4]) << 32)
                | (static_cast<core::EntityId>(payload[offset + 5]) << 40)
                | (static_cast<core::EntityId>(payload[offset + 6]) << 48)
                | (static_cast<core::EntityId>(payload[offset + 7]) << 56);
        offset += 8;
        outIds.push_back(id);
    }
    return true;
}

bool DBProtocol::decodeListTypesResponse(std::span<const std::byte> payload,
                                          std::vector<std::string>& outTypes) {
    if (payload.size() < 4) return false;
    std::size_t offset = 0;
    auto count = static_cast<std::uint32_t>(payload[0])
               | (static_cast<std::uint32_t>(payload[1]) << 8)
               | (static_cast<std::uint32_t>(payload[2]) << 16)
               | (static_cast<std::uint32_t>(payload[3]) << 24);
    offset = 4;
    outTypes.clear();
    for (std::uint32_t i = 0; i < count; ++i) {
        std::string t;
        if (!readString(payload, offset, t)) return false;
        outTypes.push_back(std::move(t));
    }
    return true;
}

}  // namespace theseed::db
