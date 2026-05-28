#pragma once

#include <string>

namespace theseed::login {

class SessionToken {
public:
    static std::string issue(const std::string& accountId, const std::string& realmId);
    static bool validate(const std::string& token,
                         std::string& outAccountId,
                         std::string& outRealmId);
};

}  // namespace theseed::login
