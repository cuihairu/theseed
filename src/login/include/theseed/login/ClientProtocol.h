#pragma once

#include "theseed/login/LoginProtocol.h"

#include <cstdint>
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
};

class ClientProtocol {
public:
    // Encode server -> client
    static std::vector<std::byte> encodeEnterGameResponse(const EnterGameResponse& resp);
    static std::vector<std::byte> encodeEntityEnter(const EntityEnterMsg& msg);

    // Decode client -> server
    static bool decodeEnterGame(std::span<const std::byte> payload, std::string& outToken);
};

}  // namespace theseed::login
