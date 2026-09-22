#pragma once

#include "theseed/login/LoginProtocol.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace theseed::login {

struct EnterGameResponse {
    bool success = false;
    std::uint64_t entityId = 0;
    std::string entityType;
    std::string error;
};

struct EntityEnterMsg {
    std::uint64_t entityId = 0;
    std::string entityType;
    float posX = 0;
    float posY = 0;
    float posZ = 0;
};

struct EntityLeaveMsg {
    std::uint64_t entityId = 0;
};

struct PropertySyncMsg {
    std::uint64_t entityId = 0;
    std::vector<std::byte> propertyData;
    bool hasPosition = false;
    float posX = 0;
    float posY = 0;
    float posZ = 0;
};

struct ActionMsg {
    std::uint64_t entityId = 0;
    std::string actionName;
    std::vector<std::byte> actionData;
};

struct EntityEventMsg {
    std::uint64_t entityId = 0;
    std::string eventName;
    std::vector<std::byte> eventData;
};

struct SpaceChangeMsg {
    std::uint64_t entityId = 0;
    std::uint32_t spaceId = 0;
    float posX = 0;
    float posY = 0;
    float posZ = 0;
};

class ClientProtocol {
public:
    static std::vector<std::byte> encodeEnterGameResponse(const EnterGameResponse& resp);
    static std::vector<std::byte> encodeEntityEnter(const EntityEnterMsg& msg);
    static std::vector<std::byte> encodeEntityLeave(const EntityLeaveMsg& msg);
    static std::vector<std::byte> encodePropertySync(const PropertySyncMsg& msg);
    static std::vector<std::byte> encodeEntityEvent(const EntityEventMsg& msg);
    static std::vector<std::byte> encodeSpaceChange(const SpaceChangeMsg& msg);

    static bool decodeEnterGame(std::span<const std::byte> payload, std::string& outToken);
    static bool decodeEntityLeave(std::span<const std::byte> payload, EntityLeaveMsg& msg);
    static bool decodePropertySync(std::span<const std::byte> payload, PropertySyncMsg& msg);

    // Client -> server action
    static std::vector<std::byte> encodeAction(const ActionMsg& msg);
    static bool decodeAction(std::span<const std::byte> payload, ActionMsg& msg);

    // Server -> cell forwarded action
    static std::vector<std::byte> encodeActionForward(const ActionMsg& msg);
    static bool decodeActionForward(std::span<const std::byte> payload, ActionMsg& msg);
};

}  // namespace theseed::login
