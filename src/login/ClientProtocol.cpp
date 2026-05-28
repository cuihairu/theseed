#include "theseed/login/ClientProtocol.h"
#include "theseed/foundation/MemoryStream.h"

#include <cstring>

namespace theseed::login {

static void writeString(foundation::MemoryStream& ms, const std::string& s) {
    ms.writeUint32(static_cast<std::uint32_t>(s.size()));
    if (!s.empty()) ms.writeBytes(reinterpret_cast<const std::byte*>(s.data()), s.size());
}

static bool readStringFromSpan(std::span<const std::byte> data, std::size_t& offset,
                               std::string& out) {
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

std::vector<std::byte> ClientProtocol::encodeEnterGameResponse(const EnterGameResponse& resp) {
    foundation::MemoryStream payload;
    payload.writeUint8(resp.success ? 1 : 0);
    payload.writeUint64(resp.entityId);
    writeString(payload, resp.entityType);
    writeString(payload, resp.error);
    auto raw = std::span<const std::byte>(payload.data(), payload.size());
    return LoginProtocol::frameMessage(ClientMessageType::EnterGameResponse, raw);
}

std::vector<std::byte> ClientProtocol::encodeEntityEnter(const EntityEnterMsg& msg) {
    foundation::MemoryStream payload;
    payload.writeUint64(msg.entityId);
    writeString(payload, msg.entityType);
    auto raw = std::span<const std::byte>(payload.data(), payload.size());
    return LoginProtocol::frameMessage(ClientMessageType::EntityEnter, raw);
}

bool ClientProtocol::decodeEnterGame(std::span<const std::byte> payload, std::string& outToken) {
    std::size_t offset = 0;
    return readStringFromSpan(payload, offset, outToken);
}

}  // namespace theseed::login
