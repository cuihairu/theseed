#include "theseed/login/LoginProtocol.h"
#include "theseed/foundation/MemoryStream.h"

#include <cstring>

namespace theseed::login {

std::vector<std::byte> LoginProtocol::frameMessage(ClientMessageType type,
                                                    std::span<const std::byte> payload) {
    foundation::MemoryStream ms;
    ms.writeUint32(static_cast<std::uint32_t>(payload.size()));
    ms.writeUint8(static_cast<std::uint8_t>(type));
    if (!payload.empty()) {
        ms.writeBytes(payload.data(), payload.size());
    }
    return std::vector<std::byte>(ms.data(), ms.data() + ms.size());
}

bool LoginProtocol::parseFrame(std::span<const std::byte> data,
                               ClientMessageType& outType,
                               std::span<const std::byte>& outPayload) {
    if (data.size() < kHeaderSize) return false;

    // Parse header manually
    auto payloadLen = static_cast<std::uint32_t>(data[0])
                    | (static_cast<std::uint32_t>(data[1]) << 8)
                    | (static_cast<std::uint32_t>(data[2]) << 16)
                    | (static_cast<std::uint32_t>(data[3]) << 24);
    auto typeVal = static_cast<std::uint8_t>(data[4]);

    if (data.size() < kHeaderSize + payloadLen) return false;

    outType = static_cast<ClientMessageType>(typeVal);
    outPayload = data.subspan(kHeaderSize, payloadLen);
    return true;
}

// --- Encode server -> client ---

static void writeString(foundation::MemoryStream& ms, const std::string& s) {
    ms.writeUint32(static_cast<std::uint32_t>(s.size()));
    if (!s.empty()) ms.writeBytes(reinterpret_cast<const std::byte*>(s.data()), s.size());
}

std::vector<std::byte> LoginProtocol::encodeLoginResponse(const LoginResponse& resp) {
    foundation::MemoryStream payload;
    payload.writeUint8(resp.success ? 1 : 0);
    writeString(payload, resp.error);
    writeString(payload, resp.token);
    payload.writeUint32(static_cast<std::uint32_t>(resp.realms.size()));
    for (const auto& r : resp.realms) {
        writeString(payload, r.realmId);
        writeString(payload, r.name);
        writeString(payload, r.status);
        writeString(payload, r.host);
        payload.writeUint16(r.port);
    }
    auto raw = std::span<const std::byte>(payload.data(), payload.size());
    return frameMessage(ClientMessageType::LoginResponse, raw);
}

std::vector<std::byte> LoginProtocol::encodeRealmList(const std::vector<RealmInfo>& realms) {
    foundation::MemoryStream payload;
    payload.writeUint32(static_cast<std::uint32_t>(realms.size()));
    for (const auto& r : realms) {
        writeString(payload, r.realmId);
        writeString(payload, r.name);
        writeString(payload, r.status);
        writeString(payload, r.host);
        payload.writeUint16(r.port);
    }
    auto raw = std::span<const std::byte>(payload.data(), payload.size());
    return frameMessage(ClientMessageType::QueryRealmsResponse, raw);
}

std::vector<std::byte> LoginProtocol::encodeSelectRealmResponse(const SelectRealmResponse& resp) {
    foundation::MemoryStream payload;
    payload.writeUint8(resp.success ? 1 : 0);
    writeString(payload, resp.error);
    writeString(payload, resp.host);
    payload.writeUint16(resp.port);
    writeString(payload, resp.token);
    auto raw = std::span<const std::byte>(payload.data(), payload.size());
    return frameMessage(ClientMessageType::SelectRealmResponse, raw);
}

std::vector<std::byte> LoginProtocol::encodeError(const std::string& message) {
    foundation::MemoryStream payload;
    writeString(payload, message);
    auto raw = std::span<const std::byte>(payload.data(), payload.size());
    return frameMessage(ClientMessageType::ErrorResponse, raw);
}

// --- Decode client -> server ---

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

bool LoginProtocol::decodeLogin(std::span<const std::byte> payload,
                                std::string& outAccount,
                                std::string& outPassword) {
    std::size_t offset = 0;
    return readStringFromSpan(payload, offset, outAccount)
        && readStringFromSpan(payload, offset, outPassword);
}

bool LoginProtocol::decodeSelectRealm(std::span<const std::byte> payload,
                                      std::string& outRealmId) {
    std::size_t offset = 0;
    return readStringFromSpan(payload, offset, outRealmId);
}

}  // namespace theseed::login
