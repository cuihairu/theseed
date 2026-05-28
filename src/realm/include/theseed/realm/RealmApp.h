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
}

namespace theseed::realm {

struct RealmAppConfig {
    std::string listenHost = "0.0.0.0";
    std::uint16_t listenPort = 20098;
    std::vector<login::RealmInfo> realms;
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
};

}  // namespace theseed::realm
