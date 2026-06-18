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
    std::byte fbuf[sizeof(float)];
    std::memcpy(fbuf, &msg.posX, sizeof(float));
    payload.writeBytes(fbuf, sizeof(float));
    std::memcpy(fbuf, &msg.posY, sizeof(float));
    payload.writeBytes(fbuf, sizeof(float));
    std::memcpy(fbuf, &msg.posZ, sizeof(float));
    payload.writeBytes(fbuf, sizeof(float));
    auto raw = std::span<const std::byte>(payload.data(), payload.size());
    return LoginProtocol::frameMessage(ClientMessageType::EntityEnter, raw);
}

std::vector<std::byte> ClientProtocol::encodeEntityLeave(const EntityLeaveMsg& msg) {
    foundation::MemoryStream payload;
    payload.writeUint64(msg.entityId);
    auto raw = std::span<const std::byte>(payload.data(), payload.size());
    return LoginProtocol::frameMessage(ClientMessageType::EntityLeave, raw);
}

std::vector<std::byte> ClientProtocol::encodePropertySync(const PropertySyncMsg& msg) {
    foundation::MemoryStream payload;
    payload.writeUint64(msg.entityId);
    payload.writeUint8(msg.hasPosition ? 1 : 0);
    if (msg.hasPosition) {
        std::byte fbuf[sizeof(float)];
        std::memcpy(fbuf, &msg.posX, sizeof(float));
        payload.writeBytes(fbuf, sizeof(float));
        std::memcpy(fbuf, &msg.posY, sizeof(float));
        payload.writeBytes(fbuf, sizeof(float));
        std::memcpy(fbuf, &msg.posZ, sizeof(float));
        payload.writeBytes(fbuf, sizeof(float));
    }
    payload.writeUint32(static_cast<std::uint32_t>(msg.propertyData.size()));
    if (!msg.propertyData.empty()) {
        payload.writeBytes(msg.propertyData.data(), msg.propertyData.size());
    }
    auto raw = std::span<const std::byte>(payload.data(), payload.size());
    return LoginProtocol::frameMessage(ClientMessageType::PropertySync, raw);
}

bool ClientProtocol::decodeEnterGame(std::span<const std::byte> payload, std::string& outToken) {
    std::size_t offset = 0;
    return readStringFromSpan(payload, offset, outToken);
}

bool ClientProtocol::decodeEntityLeave(std::span<const std::byte> payload, EntityLeaveMsg& msg) {
    if (payload.size() < 8) return false;
    std::uint64_t id = 0;
    std::memcpy(&id, payload.data(), 8);
    msg.entityId = id;
    return true;
}

bool ClientProtocol::decodePropertySync(std::span<const std::byte> payload, PropertySyncMsg& msg) {
    if (payload.size() < 9) return false;
    std::size_t offset = 0;
    std::uint64_t entityId = 0;
    std::memcpy(&entityId, payload.data() + offset, 8);
    offset += 8;
    msg.entityId = entityId;

    std::uint8_t hasPos = 0;
    std::memcpy(&hasPos, payload.data() + offset, 1);
    offset += 1;
    msg.hasPosition = hasPos != 0;

    if (msg.hasPosition) {
        if (offset + sizeof(float) * 3 > payload.size()) return false;
        std::memcpy(&msg.posX, payload.data() + offset, sizeof(float)); offset += sizeof(float);
        std::memcpy(&msg.posY, payload.data() + offset, sizeof(float)); offset += sizeof(float);
        std::memcpy(&msg.posZ, payload.data() + offset, sizeof(float)); offset += sizeof(float);
    }

    if (offset + 4 > payload.size()) return false;
    std::uint32_t dataLen = 0;
    std::memcpy(&dataLen, payload.data() + offset, 4);
    offset += 4;

    if (offset + dataLen > payload.size()) return false;
    msg.propertyData.resize(dataLen);
    if (dataLen > 0) {
        std::memcpy(msg.propertyData.data(), payload.data() + offset, dataLen);
    }
    return true;
}

std::vector<std::byte> ClientProtocol::encodeAction(const ActionMsg& msg) {
    foundation::MemoryStream payload;
    payload.writeUint64(msg.entityId);
    writeString(payload, msg.actionName);
    payload.writeUint32(static_cast<std::uint32_t>(msg.actionData.size()));
    if (!msg.actionData.empty()) {
        payload.writeBytes(msg.actionData.data(), msg.actionData.size());
    }
    auto raw = std::span<const std::byte>(payload.data(), payload.size());
    return LoginProtocol::frameMessage(ClientMessageType::Action, raw);
}

bool ClientProtocol::decodeAction(std::span<const std::byte> payload, ActionMsg& msg) {
    std::size_t offset = 0;
    if (offset + 8 > payload.size()) return false;
    std::uint64_t entityId = 0;
    std::memcpy(&entityId, payload.data() + offset, 8);
    offset += 8;
    msg.entityId = entityId;

    if (!readStringFromSpan(payload, offset, msg.actionName)) return false;

    if (offset + 4 > payload.size()) return false;
    std::uint32_t dataLen = 0;
    std::memcpy(&dataLen, payload.data() + offset, 4);
    offset += 4;
    if (offset + dataLen > payload.size()) return false;
    msg.actionData.resize(dataLen);
    if (dataLen > 0) {
        std::memcpy(msg.actionData.data(), payload.data() + offset, dataLen);
    }
    return true;
}

std::vector<std::byte> ClientProtocol::encodeActionForward(const ActionMsg& msg) {
    foundation::MemoryStream payload;
    payload.writeUint64(msg.entityId);
    writeString(payload, msg.actionName);
    payload.writeUint32(static_cast<std::uint32_t>(msg.actionData.size()));
    if (!msg.actionData.empty()) {
        payload.writeBytes(msg.actionData.data(), msg.actionData.size());
    }
    auto raw = std::span<const std::byte>(payload.data(), payload.size());
    return LoginProtocol::frameMessage(ClientMessageType::ActionForward, raw);
}

bool ClientProtocol::decodeActionForward(std::span<const std::byte> payload, ActionMsg& msg) {
    return decodeAction(payload, msg);
}

std::vector<std::byte> ClientProtocol::encodeEntityEvent(const EntityEventMsg& msg) {
    foundation::MemoryStream payload;
    payload.writeUint64(msg.entityId);
    writeString(payload, msg.eventName);
    payload.writeUint32(static_cast<std::uint32_t>(msg.eventData.size()));
    if (!msg.eventData.empty()) {
        payload.writeBytes(msg.eventData.data(), msg.eventData.size());
    }
    auto raw = std::span<const std::byte>(payload.data(), payload.size());
    return LoginProtocol::frameMessage(ClientMessageType::EntityEvent, raw);
}

std::vector<std::byte> ClientProtocol::encodeSpaceChange(const SpaceChangeMsg& msg) {
    foundation::MemoryStream payload;
    payload.writeUint64(msg.entityId);
    payload.writeUint32(msg.spaceId);
    std::byte fbuf[sizeof(float)];
    std::memcpy(fbuf, &msg.posX, sizeof(float));
    payload.writeBytes(fbuf, sizeof(float));
    std::memcpy(fbuf, &msg.posY, sizeof(float));
    payload.writeBytes(fbuf, sizeof(float));
    std::memcpy(fbuf, &msg.posZ, sizeof(float));
    payload.writeBytes(fbuf, sizeof(float));
    auto raw = std::span<const std::byte>(payload.data(), payload.size());
    return LoginProtocol::frameMessage(ClientMessageType::SpaceChange, raw);
}

}  // namespace theseed::login
