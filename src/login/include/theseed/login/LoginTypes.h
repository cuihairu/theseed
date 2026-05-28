#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace theseed::login {

struct CharacterSummary {
    std::string name;
    std::uint32_t level = 0;
    std::string className;
};

struct RealmInfo {
    std::string realmId;
    std::string name;
    std::string status;  // "smooth" / "busy" / "full"
    std::string host;
    std::uint16_t port = 0;
};

struct LoginResult {
    bool success = false;
    std::string accountId;
    std::string error;
};

struct LoginResponse {
    bool success = false;
    std::string error;
    std::string token;
    std::vector<RealmInfo> realms;
};

struct SelectRealmResponse {
    bool success = false;
    std::string error;
    std::string host;
    std::uint16_t port = 0;
    std::string token;
};

}  // namespace theseed::login
