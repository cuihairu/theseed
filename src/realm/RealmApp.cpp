#include "theseed/realm/RealmApp.h"
#include "theseed/login/ClientSession.h"
#include "theseed/login/LoginProtocol.h"
#include "theseed/runtime/TcpConnection.h"

#include <algorithm>

namespace theseed::realm {

RealmApp::RealmApp(RealmAppConfig config)
    : config_(std::move(config)) {
    listener_.setConnectionFactory([]() {
        return runtime::TcpConnection::create();
    });
}

RealmApp::~RealmApp() {
    stop();
}

void RealmApp::init() {
    listener_.listen(config_.listenHost, config_.listenPort);
}

void RealmApp::tick() {
    acceptConnections();
    for (auto& session : sessions_) {
        session->pump();
    }
    cleanupDisconnected();
}

void RealmApp::stop() {
    sessions_.clear();
    listener_.close();
}

const std::vector<login::RealmInfo>& RealmApp::realms() const {
    return config_.realms;
}

void RealmApp::acceptConnections() {
    while (auto conn = listener_.accept()) {
        auto session = std::make_unique<login::ClientSession>(conn);
        auto* rawSession = session.get();
        session->setMessageCallback(
            [this, rawSession](login::ClientMessageType type,
                               std::span<const std::byte> payload) {
                onClientMessage(rawSession, type, payload);
            });
        sessions_.push_back(std::move(session));
    }
}

void RealmApp::onClientMessage(login::ClientSession* session,
                               login::ClientMessageType type,
                               std::span<const std::byte> payload) {
    switch (type) {
        case login::ClientMessageType::QueryRealms:
            handleQueryRealms(session);
            break;
        default:
            break;
    }
}

void RealmApp::handleQueryRealms(login::ClientSession* session) {
    auto data = login::LoginProtocol::encodeRealmList(config_.realms);
    session->send(std::span<const std::byte>(data.data(), data.size()));
}

void RealmApp::cleanupDisconnected() {
    sessions_.erase(
        std::remove_if(sessions_.begin(), sessions_.end(),
                       [](const std::unique_ptr<login::ClientSession>& s) {
                           return !s->isConnected();
                       }),
        sessions_.end());
}

}  // namespace theseed::realm
