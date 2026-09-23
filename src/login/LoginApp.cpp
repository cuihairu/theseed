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
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <utility>

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

        std::shared_ptr<runtime::IRuntimeTransport> transport;
        if (config_.dbTransportFactory) {
            transport = config_.dbTransportFactory(config_.dbHost, config_.dbPort);
        } else {
            auto conn = runtime::TcpConnection::create();
            if (!conn->connect(config_.dbHost, config_.dbPort)) {
                // LCOV_EXCL_START Linux 非阻塞 connect 对无服务端口恒返回 EINPROGRESS；inet_pton 无 DNS，坏主机名解析为 0.0.0.0 同样如此
                return;
                // LCOV_EXCL_STOP
            }
            transport = std::make_shared<runtime::NetworkTransport>(conn);
        }
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
    const auto tickStart = std::chrono::steady_clock::now();
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

    // tick_duration_ms：LoginApp 不走 TickScheduler，自行观测，桶边界与之一致。
    const auto elapsed = std::chrono::steady_clock::now() - tickStart;
    theseed::foundation::MetricsRegistry::instance()
        .histogram("tick_duration_ms",
                   theseed::foundation::Histogram::Boundaries{1.0, 2.0, 5.0, 10.0, 25.0, 50.0, 100.0, 250.0, 500.0, 1000.0},
                   "tick wall-clock duration in milliseconds")
        .observe(std::chrono::duration<double, std::milli>(elapsed).count());
}

void LoginApp::stop() {
    sessions_.clear();
    hub_.reset();
    listener_.close();
}

const std::vector<RealmInfo>& LoginApp::realms() const {
    return config_.realms;
}

void LoginApp::handleClientMessage(ClientSession* session,
                                   ClientMessageType type,
                                   std::span<const std::byte> payload) {
    onClientMessage(session, type, payload);
}

runtime::RuntimeInvocation LoginApp::dbRequest(const std::string& method,
                                                std::span<const std::byte> payload) {
    runtime::RuntimeInvocation inv;
    inv.sourceComponent = config_.localComponentId;
    inv.targetComponent = config_.dbComponentId;
    inv.entityId = 0;
    inv.method = method;
    inv.payload = std::vector<std::byte>(payload.begin(), payload.end());

    if (hub_->send(std::move(inv)) != runtime::SendResult::Accepted) {
        return {};
    }
    hub_->flush();

    // 等待应答，上限 dbRequestTimeout：DBApp 失联时退化为失败返回而不是忙等挂死。
    // 单 DBApp 拓扑下杂散消息只会是过期应答，丢弃后继续等本次的。
    const auto deadline = runtime::Clock::now() + config_.dbRequestTimeout;
    const auto expect = std::string(method) + ".ok";
    runtime::RuntimeInvocation resp;
    while (runtime::Clock::now() < deadline) {
        hub_->tick();
        if (hub_->receive(config_.localComponentId, &resp, 1) > 0) {
            if (resp.method == expect) return resp;
            continue;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return {};
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

    // 限流：按 account 维度节流登录尝试。放在鉴权之前，避免无谓的 DB 往返。
    if (config_.rateLimiter && !account.empty() &&
        !config_.rateLimiter->tryConsume("login:" + account, config_.rateLimitConfig)) {
        resp.success = false;
        resp.error = "rate limited";
        theseed::foundation::MetricsRegistry::instance()
            .counter("login_rate_limited_count",
                     "login attempts rejected by rate limiter")
            .increment();
        auto data = LoginProtocol::encodeLoginResponse(resp);
        session->send(std::span<const std::byte>(data.data(), data.size()));
        return;
    }

    // 记录成功登录后写入会话存储的辅助 lambda。
    // realmId 在 login 阶段为空（SelectRealm 时才确定），这里先存基础会话，
    // 由 handleSelectRealm 在确定 realm 后补写。
    auto persistSession = [&](const std::string& token, std::int64_t userId) {
        if (config_.sessionStore && !token.empty()) {
            foundation::StoredSession s;
            s.accountId = account;
            s.userId = userId;
            config_.sessionStore->save(token, s, config_.sessionTtl);
        }
    };

    if (config_.authType == "null") {
        resp.success = true;
        resp.token = SessionToken::issue(account, "");
        resp.realms = config_.realms;
        persistSession(resp.token, 0);
    } else if (config_.authType == "db" && hub_) {
        // Query DBApp for account
        auto req = db::DBProtocol::encodeQueryAccountRequest(account);
        auto respPayload = dbRequest(db::DBMethod::kQueryAccount,
                                      std::span<const std::byte>(req.data(), req.size()));

        if (respPayload.method != db::DBMethod::kQueryAccountOk) {
            // DBApp 失联/超时：不能确定账号是否存在，直接失败（不进 auto-register）
            resp.success = false;
            resp.error = "database unavailable";
            theseed::foundation::MetricsRegistry::instance()
                .counter("login_db_unavailable_count",
                         "login attempts aborted because DBApp did not answer in time")
                .increment();
        } else {
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
                persistSession(resp.token, static_cast<std::int64_t>(accountId));
            } else if (!found) {
                // Auto-register: create new account
                auto createReq = db::DBProtocol::encodeCreateAccountRequest(account, password);
                auto createRespPayload = dbRequest(db::DBMethod::kCreateAccount,
                                                    std::span<const std::byte>(createReq.data(), createReq.size()));
                bool created = false;
                if (createRespPayload.method == db::DBMethod::kCreateAccountOk) {
                    db::DBProtocol::decodeCreateAccountResponse(
                        std::span<const std::byte>(createRespPayload.payload.data(), createRespPayload.payload.size()),
                        created, accountId);
                }

                if (created) {
                    resp.success = true;
                    resp.token = SessionToken::issue(account, "");
                    resp.realms = config_.realms;
                    persistSession(resp.token, static_cast<std::int64_t>(accountId));
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
        }
    } else {
        // Fallback "password" mode: check non-empty
        if (!account.empty() && !password.empty()) {
            resp.success = true;
            resp.token = SessionToken::issue(account, "");
            resp.realms = config_.realms;
            persistSession(resp.token, 0);
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
