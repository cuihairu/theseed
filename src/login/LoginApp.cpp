#include "theseed/login/LoginApp.h"
#include "theseed/login/ClientSession.h"
#include "theseed/login/LoginProtocol.h"
#include "theseed/login/SessionToken.h"
#include "theseed/db/DBProtocol.h"
#include "theseed/foundation/Metrics.h"
#include "theseed/runtime/NetworkTransport.h"
#include "theseed/runtime/TcpConnection.h"

#include <algorithm>
#include <chrono>

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

    if (config_.authType == "db" && !config_.dbHost.empty()) {
        hub_ = std::make_shared<runtime::TransportHub>(config_.localComponentId);

        auto conn = runtime::TcpConnection::create();
        if (!conn->connect(config_.dbHost, config_.dbPort)) {
            return;
        }
        auto transport = std::make_shared<runtime::NetworkTransport>(conn);
        hub_->connectPeer(config_.dbComponentId, transport);
    }

    if (config_.ops.enabled) {
        ops::ProcessInfo info{};
        info.role = "LoginApp";
        info.version = "0.1.0";
        info.startTime = std::chrono::system_clock::now();
        info.componentId = config_.localComponentId;

        opsInspector_ = std::make_unique<ops::OpsInspector>(std::move(info), [this] {
            ops::RuntimeInfo rt{};
            rt.sessionCount = sessions_.size();
            if (hub_) {
                rt.transportStats = hub_->stats();
            }
            return rt;
        });

        ops::OpsServer::Config opsCfg{};
        opsCfg.host = config_.ops.host;
        opsCfg.port = config_.ops.port;
        opsCfg.maxConnections = config_.ops.maxConnections;
        opsServer_ = std::make_unique<ops::OpsServer>(opsCfg, *opsInspector_);
        opsServer_->start();
    }
}

void LoginApp::tick() {
    if (hub_) hub_->tick();
    acceptConnections();
    for (auto& session : sessions_) {
        session->pump();
    }
    cleanupDisconnected();

    // Phase B MVP metric: live login sessions.
    theseed::foundation::MetricsRegistry::instance()
        .gauge("login_pending_count", "active client sessions held by LoginApp")
        .set(static_cast<std::int64_t>(sessions_.size()));

    if (hub_) {
        transportStatsCollector_.collect(hub_->stats());
    }

    if (opsServer_) {
        opsServer_->tick();
    }
}

void LoginApp::stop() {
    sessions_.clear();
    hub_.reset();
    listener_.close();
}

const std::vector<RealmInfo>& LoginApp::realms() const {
    return config_.realms;
}

runtime::RuntimeInvocation LoginApp::dbRequest(const std::string& method,
                                                std::span<const std::byte> payload) {
    runtime::RuntimeInvocation inv;
    inv.sourceComponent = config_.localComponentId;
    inv.targetComponent = config_.dbComponentId;
    inv.entityId = 0;
    inv.method = method;
    inv.payload = std::vector<std::byte>(payload.begin(), payload.end());

    hub_->send(std::move(inv));
    hub_->flush();

    runtime::RuntimeInvocation resp;
    while (true) {
        hub_->tick();
        auto count = hub_->receive(config_.localComponentId, &resp, 1);
        if (count > 0) return resp;
    }
}

void LoginApp::acceptConnections() {
    while (auto conn = listener_.accept()) {
        conn->setOnReceived(nullptr);
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
    LoginResponse resp;

    if (config_.authType == "null") {
        resp.success = true;
        resp.token = SessionToken::issue(account, "");
        resp.realms = config_.realms;
    } else if (config_.authType == "db" && hub_) {
        // Query DBApp for account
        auto req = db::DBProtocol::encodeQueryAccountRequest(account);
        auto respPayload = dbRequest(db::DBMethod::kQueryAccount,
                                      std::span<const std::byte>(req.data(), req.size()));

        bool found = false;
        core::EntityId accountId = 0;
        std::string storedPassword;
        db::DBProtocol::decodeQueryAccountResponse(
            std::span<const std::byte>(respPayload.payload.data(), respPayload.payload.size()),
            found, accountId, storedPassword);

        if (found && storedPassword == password) {
            resp.success = true;
            resp.token = SessionToken::issue(account, "");
            resp.realms = config_.realms;
        } else if (!found) {
            // Auto-register: create new account
            auto createReq = db::DBProtocol::encodeCreateAccountRequest(account, password);
            auto createRespPayload = dbRequest(db::DBMethod::kCreateAccount,
                                                std::span<const std::byte>(createReq.data(), createReq.size()));
            bool created = false;
            db::DBProtocol::decodeCreateAccountResponse(
                std::span<const std::byte>(createRespPayload.payload.data(), createRespPayload.payload.size()),
                created, accountId);

            if (created) {
                resp.success = true;
                resp.token = SessionToken::issue(account, "");
                resp.realms = config_.realms;
            } else {
                resp.success = false;
                resp.error = "account creation failed";
                theseed::foundation::MetricsRegistry::instance()
                    .counter("challenge_failure_count",
                             "any login authentication failure (auth failures, account creation failures, fallback rejection)")
                    .increment();
            }
        } else {
            resp.success = false;
            resp.error = "invalid credentials";
            theseed::foundation::MetricsRegistry::instance()
                .counter("challenge_failure_count",
                         "any login authentication failure (auth failures, account creation failures, fallback rejection)")
                .increment();
        }
    } else {
        // Fallback "password" mode: check non-empty
        if (!account.empty() && !password.empty()) {
            resp.success = true;
            resp.token = SessionToken::issue(account, "");
            resp.realms = config_.realms;
        } else {
            resp.success = false;
            resp.error = "invalid credentials";
            theseed::foundation::MetricsRegistry::instance()
                .counter("challenge_failure_count",
                         "any login authentication failure (auth failures, account creation failures, fallback rejection)")
                .increment();
        }
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
