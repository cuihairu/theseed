#include "theseed/login/LoginApp.h"
#include "theseed/login/ClientSession.h"
#include "theseed/login/LoginProtocol.h"
#include "theseed/login/SessionToken.h"
#include "theseed/runtime/TcpConnection.h"

#include <algorithm>

namespace theseed::login {

LoginApp::LoginApp(LoginAppConfig config)
    : config_(std::move(config)) {
    listener_.setConnectionFactory([]() {
        return runtime::TcpConnection::create();
    });
}

LoginApp::~LoginApp() {
    stop();
}

void LoginApp::init() {
    if (!listener_.listen(config_.listenHost, config_.listenPort)) {
        return;
    }
}

void LoginApp::tick() {
    acceptConnections();
    for (auto& session : sessions_) {
        session->pump();
    }
    cleanupDisconnected();
}

void LoginApp::stop() {
    sessions_.clear();
    listener_.close();
}

const std::vector<RealmInfo>& LoginApp::realms() const {
    return config_.realms;
}

void LoginApp::acceptConnections() {
    while (auto conn = listener_.accept()) {
        conn->setOnReceived(nullptr);  // ClientSession manages callbacks
        auto pipe = std::shared_ptr<runtime::IBytePipe>(
            conn.get(), [conn](auto*) { /* shared ownership via conn */ });
        // Wrap the TcpConnection directly
        auto session = std::make_unique<ClientSession>(conn);

        auto* rawSession = session.get();
        session->setMessageCallback(
            [this, rawSession](ClientMessageType type, std::span<const std::byte> payload) {
                onClientMessage(rawSession, type, payload);
            });

        sessions_.push_back(std::move(session));
    }
}

void LoginApp::onClientMessage(ClientSession* session,
                               ClientMessageType type,
                               std::span<const std::byte> payload) {
    switch (type) {
        case ClientMessageType::Login: {
            std::string account, password;
            if (LoginProtocol::decodeLogin(payload, account, password)) {
                handleLogin(session, account, password);
            }
            break;
        }
        case ClientMessageType::QueryRealms:
            handleQueryRealms(session);
            break;
        case ClientMessageType::SelectRealm: {
            std::string realmId;
            if (LoginProtocol::decodeSelectRealm(payload, realmId)) {
                handleSelectRealm(session, realmId);
            }
            break;
        }
        default:
            break;
    }
}

void LoginApp::handleLogin(ClientSession* session,
                           const std::string& account,
                           const std::string& password) {
    // MVP: authType == "null" accepts everything, "password" checks non-empty
    LoginResponse resp;

    if (config_.authType == "null" || (!account.empty() && !password.empty())) {
        resp.success = true;
        resp.token = SessionToken::issue(account, "");
        resp.realms = config_.realms;
    } else {
        resp.success = false;
        resp.error = "invalid credentials";
    }

    auto data = LoginProtocol::encodeLoginResponse(resp);
    session->send(std::span<const std::byte>(data.data(), data.size()));
}

void LoginApp::handleQueryRealms(ClientSession* session) {
    auto data = LoginProtocol::encodeRealmList(config_.realms);
    session->send(std::span<const std::byte>(data.data(), data.size()));
}

void LoginApp::handleSelectRealm(ClientSession* session, const std::string& realmId) {
    SelectRealmResponse resp;

    const RealmInfo* found = nullptr;
    for (auto& r : config_.realms) {
        if (r.realmId == realmId) {
            found = &r;
            break;
        }
    }

    if (found) {
        resp.success = true;
        resp.host = found->host;
        resp.port = found->port;
        resp.token = SessionToken::issue("", found->realmId);
    } else {
        resp.success = false;
        resp.error = "realm not found";
    }

    auto data = LoginProtocol::encodeSelectRealmResponse(resp);
    session->send(std::span<const std::byte>(data.data(), data.size()));
}

void LoginApp::cleanupDisconnected() {
    sessions_.erase(
        std::remove_if(sessions_.begin(), sessions_.end(),
                       [](const std::unique_ptr<ClientSession>& s) {
                           return !s->isConnected();
                       }),
        sessions_.end());
}

}  // namespace theseed::login
