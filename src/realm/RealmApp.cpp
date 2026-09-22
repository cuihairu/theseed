#include "theseed/realm/RealmApp.h"
#include "theseed/foundation/Metrics.h"
#include "theseed/login/ClientSession.h"
#include "theseed/login/LoginProtocol.h"
#include "theseed/runtime/TcpConnection.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

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

    if (config_.ops.enabled) {
        ops::ProcessInfo info{};
        info.role = "RealmApp";
        info.version = "0.1.0";
        info.startTime = std::chrono::system_clock::now();

        opsInspector_ = std::make_unique<ops::OpsInspector>(std::move(info), [this] {
            ops::RuntimeInfo rt{};
            rt.sessionCount = sessions_.size();
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

void RealmApp::tick() {
    const auto tickStart = std::chrono::steady_clock::now();
    acceptConnections();
    for (auto& session : sessions_) {
        session->pump();
    }
    cleanupDisconnected();

    // Phase B MVP metrics：活动会话数 + tick 耗时。
    theseed::foundation::MetricsRegistry::instance()
        .gauge("realm_session_count", "active client sessions held by RealmApp")
        .set(static_cast<std::int64_t>(sessions_.size()));

    if (opsServer_) {
        opsServer_->tick();
    }

    const auto elapsed = std::chrono::steady_clock::now() - tickStart;
    theseed::foundation::MetricsRegistry::instance()
        .histogram("tick_duration_ms",
                   theseed::foundation::Histogram::Boundaries{
                       1.0, 2.0, 5.0, 10.0, 25.0, 50.0, 100.0, 250.0, 500.0, 1000.0},
                   "tick wall-clock duration in milliseconds")
        .observe(std::chrono::duration<double, std::milli>(elapsed).count());
}

void RealmApp::stop() {
    opsServer_.reset();
    opsInspector_.reset();
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
