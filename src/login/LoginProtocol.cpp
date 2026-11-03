#include "theseed/login/LoginProtocol.h"
#include "theseed/foundation/MemoryStream.h"

#include <cstddef>
#include <cstdint>
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

// --- Encode client -> server（bot / 真实客户端的请求构造面，与下方 decode 镜像） ---

std::vector<std::byte> LoginProtocol::encodeLogin(const std::string& account,
                                                  const std::string& password) {
    foundation::MemoryStream payload;
    writeString(payload, account);
    writeString(payload, password);
    auto raw = std::span<const std::byte>(payload.data(), payload.size());
    return frameMessage(ClientMessageType::Login, raw);
}

std::vector<std::byte> LoginProtocol::encodeSelectRealm(const std::string& realmId) {
    foundation::MemoryStream payload;
    writeString(payload, realmId);
    auto raw = std::span<const std::byte>(payload.data(), payload.size());
    return frameMessage(ClientMessageType::SelectRealm, raw);
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

// --- Decode server -> client（bot / 真实客户端的应答解析面，与上方 encode 镜像） ---

static bool readUint8FromSpan(std::span<const std::byte> data, std::size_t& offset,
                              std::uint8_t& out) {
    if (offset + 1 > data.size()) return false;
    out = static_cast<std::uint8_t>(data[offset]);
    offset += 1;
    return true;
}

static bool readUint16FromSpan(std::span<const std::byte> data, std::size_t& offset,
                               std::uint16_t& out) {
    if (offset + 2 > data.size()) return false;
    out = static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[offset])
                                   | (static_cast<std::uint16_t>(data[offset + 1]) << 8));
    offset += 2;
    return true;
}

static bool readUint32FromSpan(std::span<const std::byte> data, std::size_t& offset,
                               std::uint32_t& out) {
    if (offset + 4 > data.size()) return false;
    out = static_cast<std::uint32_t>(data[offset])
        | (static_cast<std::uint32_t>(data[offset + 1]) << 8)
        | (static_cast<std::uint32_t>(data[offset + 2]) << 16)
        | (static_cast<std::uint32_t>(data[offset + 3]) << 24);
    offset += 4;
    return true;
}

bool LoginProtocol::decodeLoginResponse(std::span<const std::byte> payload,
                                        LoginResponse& outResp) {
    std::size_t offset = 0;
    std::uint8_t success = 0;
    if (!readUint8FromSpan(payload, offset, success)) return false;
    outResp.success = success != 0;
    if (!readStringFromSpan(payload, offset, outResp.error)) return false;
    if (!readStringFromSpan(payload, offset, outResp.token)) return false;
    std::uint32_t realmCount = 0;
    if (!readUint32FromSpan(payload, offset, realmCount)) return false;
    outResp.realms.clear();
    outResp.realms.reserve(realmCount);
    for (std::uint32_t i = 0; i < realmCount; ++i) {
        RealmInfo realm;
        if (!readStringFromSpan(payload, offset, realm.realmId)) return false;
        if (!readStringFromSpan(payload, offset, realm.name)) return false;
        if (!readStringFromSpan(payload, offset, realm.status)) return false;
        if (!readStringFromSpan(payload, offset, realm.host)) return false;
        if (!readUint16FromSpan(payload, offset, realm.port)) return false;
        outResp.realms.push_back(std::move(realm));
    }
    return true;
}

bool LoginProtocol::decodeRealmList(std::span<const std::byte> payload,
                                    std::vector<RealmInfo>& outRealms) {
    std::size_t offset = 0;
    std::uint32_t realmCount = 0;
    if (!readUint32FromSpan(payload, offset, realmCount)) return false;
    outRealms.clear();
    outRealms.reserve(realmCount);
    for (std::uint32_t i = 0; i < realmCount; ++i) {
        RealmInfo realm;
        if (!readStringFromSpan(payload, offset, realm.realmId)) return false;
        if (!readStringFromSpan(payload, offset, realm.name)) return false;
        if (!readStringFromSpan(payload, offset, realm.status)) return false;
        if (!readStringFromSpan(payload, offset, realm.host)) return false;
        if (!readUint16FromSpan(payload, offset, realm.port)) return false;
        outRealms.push_back(std::move(realm));
    }
    return true;
}

bool LoginProtocol::decodeSelectRealmResponse(std::span<const std::byte> payload,
                                              SelectRealmResponse& outResp) {
    std::size_t offset = 0;
    std::uint8_t success = 0;
    if (!readUint8FromSpan(payload, offset, success)) return false;
    outResp.success = success != 0;
    if (!readStringFromSpan(payload, offset, outResp.error)) return false;
    if (!readStringFromSpan(payload, offset, outResp.host)) return false;
    if (!readUint16FromSpan(payload, offset, outResp.port)) return false;
    if (!readStringFromSpan(payload, offset, outResp.token)) return false;
    return true;
}

bool LoginProtocol::decodeError(std::span<const std::byte> payload,
                                std::string& outMessage) {
    std::size_t offset = 0;
    return readStringFromSpan(payload, offset, outMessage);
}

}  // namespace theseed::login
