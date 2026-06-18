#pragma once

#include "theseed/login/LoginProtocol.h"
#include "theseed/login/LoginTypes.h"
#include "theseed/ops/OpsInspector.h"
#include "theseed/ops/OpsServer.h"
#include "theseed/runtime/RuntimeTransport.h"
#include "theseed/runtime/TcpListener.h"
#include "theseed/runtime/TransportHub.h"
#include "theseed/runtime/TransportStatsCollector.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace theseed::login {

class ClientSession;

struct LoginAppOpsConfig {
    bool enabled = false;
    std::string host = "127.0.0.1";
    std::uint16_t port = 20099 + 100;  // avoid colliding with listen port
    std::size_t maxConnections = 8;
};

struct LoginAppConfig {
    std::string listenHost = "0.0.0.0";
    std::uint16_t listenPort = 20099;
    std::string authType = "password";  // "password" | "null" | "db"
    std::vector<RealmInfo> realms;
    std::string dbHost;
    std::uint16_t dbPort = 20003;
    runtime::ComponentId dbComponentId = 10;
    runtime::ComponentId localComponentId = 20;
    LoginAppOpsConfig ops;
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

    runtime::RuntimeInvocation dbRequest(const std::string& method,
                                          std::span<const std::byte> payload);

    LoginAppConfig config_;
    runtime::TcpListener listener_;
    std::shared_ptr<runtime::TransportHub> hub_;
    std::vector<std::unique_ptr<ClientSession>> sessions_;
    runtime::TransportStatsCollector transportStatsCollector_;

    std::unique_ptr<ops::OpsInspector> opsInspector_;
    std::unique_ptr<ops::OpsServer> opsServer_;
};

}  // namespace theseed::login
