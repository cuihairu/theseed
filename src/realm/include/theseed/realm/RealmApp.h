#pragma once

#include "theseed/login/LoginProtocol.h"
#include "theseed/login/LoginTypes.h"
#include "theseed/ops/OpsInspector.h"
#include "theseed/ops/OpsServer.h"
#include "theseed/runtime/TcpListener.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace theseed::login {
class ClientSession;
}

namespace theseed::realm {

struct RealmAppOpsConfig {
    bool enabled = false;
    std::string host = "127.0.0.1";
    std::uint16_t port = 20098 + 100;  // 避免与 listen 端口冲突
    std::size_t maxConnections = 8;
};

struct RealmAppConfig {
    std::string listenHost = "0.0.0.0";
    std::uint16_t listenPort = 20098;
    std::vector<login::RealmInfo> realms;
    RealmAppOpsConfig ops;
};

class RealmApp {
public:
    explicit RealmApp(RealmAppConfig config);
    ~RealmApp();

    RealmApp(const RealmApp&) = delete;
    RealmApp& operator=(const RealmApp&) = delete;

    void init();
    void tick();
    void stop();

    const std::vector<login::RealmInfo>& realms() const;

private:
    void acceptConnections();
    void onClientMessage(login::ClientSession* session,
                         login::ClientMessageType type,
                         std::span<const std::byte> payload);
    void handleQueryRealms(login::ClientSession* session);
    void cleanupDisconnected();

    RealmAppConfig config_;
    runtime::TcpListener listener_;
    std::vector<std::unique_ptr<login::ClientSession>> sessions_;

    std::unique_ptr<ops::OpsInspector> opsInspector_;
    std::unique_ptr<ops::OpsServer> opsServer_;
};

}  // namespace theseed::realm
