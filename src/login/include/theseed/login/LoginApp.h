#pragma once

#include "theseed/login/LoginProtocol.h"
#include "theseed/login/LoginTypes.h"
#include "theseed/runtime/TcpListener.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace theseed::login {

class ClientSession;

struct LoginAppConfig {
    std::string listenHost = "0.0.0.0";
    std::uint16_t listenPort = 20099;
    std::string authType = "password";  // "password" | "null"
    std::vector<RealmInfo> realms;
};

class LoginApp {
public:
    explicit LoginApp(LoginAppConfig config);
    ~LoginApp();

    LoginApp(const LoginApp&) = delete;
    LoginApp& operator=(const LoginApp&) = delete;

    void init();
    void tick();
    void stop();

    const std::vector<RealmInfo>& realms() const;

private:
    void acceptConnections();
    void onClientMessage(ClientSession* session,
                         ClientMessageType type,
                         std::span<const std::byte> payload);
    void handleLogin(ClientSession* session,
                     const std::string& account,
                     const std::string& password);
    void handleQueryRealms(ClientSession* session);
    void handleSelectRealm(ClientSession* session, const std::string& realmId);
    void cleanupDisconnected();

    LoginAppConfig config_;
    runtime::TcpListener listener_;
    std::vector<std::unique_ptr<ClientSession>> sessions_;
};

}  // namespace theseed::login
