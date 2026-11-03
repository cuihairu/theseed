#include "BotClient.h"

#include "theseed/foundation/MemoryStream.h"
#include "theseed/login/ClientProtocol.h"

#include <cstring>

namespace theseed::bots {

namespace {

using theseed::login::ClientMessageType;

// 登录/选区/进入游戏的应答等待上限。超窗按断线处理进入退避重连，
// 防止服务端半开连接下 bot 永久滞留等待态。
constexpr std::chrono::milliseconds kResponseTimeout{5000};

}  // namespace

BotClient::BotClient(const BotsConfig& config, std::string account)
    : config_(config), account_(std::move(account)) {}

void BotClient::forceDisconnect(SteadyTime now) {
    if (!isInGame() && state_ != State::EnterSent && state_ != State::LoginSent
        && state_ != State::SelectSent) {
        return;
    }
    // 本方法只在 tick 循环上下文调用（非协议回调内）：可安全关连接。
    if (loginSession_) loginSession_->close();
    if (baseSession_) baseSession_->close();
    scheduleRetry(now, "forced disconnect");
}

void BotClient::scheduleRetry(SteadyTime now, const char* reason) {
    // 注意：可能从协议回调内进入（onMessage 失败路径）。这里绝不
    // close/reset 连接对象——销毁正在派发回调的 ClientSession 会
    // use-after-free。只排期退避；旧连接对象由下一次 startLogin 的
    // resetConnections 统一回收（对端已死连接由 isConnected 判定）。
    ++stats_.reconnects;
    state_ = State::Backoff;
    nextAttempt_ = now + config_.reconnectDelay;
    // reason 目前仅用于将来接日志；行为上静默退避，避免大群 bot 刷屏
    static_cast<void>(reason);
}

void BotClient::resetConnections() {
    if (loginSession_) loginSession_->close();
    loginSession_.reset();
    loginConn_.reset();
    if (baseSession_) baseSession_->close();
    baseSession_.reset();
    baseConn_.reset();
    pendingSent_ = false;
    token_.clear();
}

void BotClient::startLogin(SteadyTime now) {
    resetConnections();
    loginConn_ = runtime::TcpConnection::create();
    loginSession_ = std::make_unique<login::ClientSession>(loginConn_);
    loginSession_->setMessageCallback(
        [this](ClientMessageType type, std::span<const std::byte> payload) {
            onMessage(type, payload);
        });
    if (!loginConn_->connect(config_.loginHost, config_.loginPort)) {
        scheduleRetry(now, "login connect failed");
        return;
    }
    loginSession_->send(login::LoginProtocol::encodeLogin(account_, config_.accountPassword));
    ++stats_.messagesOut;
    markSent(now);
    stateSince_ = now;
    state_ = State::LoginSent;
}

void BotClient::connectBase(SteadyTime now, const std::string& host,
                            std::uint16_t port, const std::string& token) {
    // 换腿：关登录连接，开 BaseApp 连接。
    if (loginSession_) loginSession_->close();
    loginSession_.reset();
    loginConn_.reset();

    token_ = token;
    baseConn_ = runtime::TcpConnection::create();
    baseSession_ = std::make_unique<login::ClientSession>(baseConn_);
    baseSession_->setMessageCallback(
        [this](ClientMessageType type, std::span<const std::byte> payload) {
            onMessage(type, payload);
        });
    if (!baseConn_->connect(host, port)) {
        scheduleRetry(now, "base connect failed");
        return;
    }
    baseSession_->send(login::ClientProtocol::encodeEnterGame(token_));
    ++stats_.messagesOut;
    markSent(now);
    stateSince_ = now;
    state_ = State::EnterSent;
}

void BotClient::markSent(SteadyTime now) {
    pendingSent_ = true;
    pendingSentAt_ = now;
}

void BotClient::sampleRtt(SteadyTime now) {
    if (!pendingSent_) return;
    pendingSent_ = false;
    const double ms = std::chrono::duration<double, std::milli>(now - pendingSentAt_).count();
    ++stats_.rttSamples;
    stats_.rttLastMs = ms;
    stats_.rttSumMs += ms;
    if (ms > stats_.rttMaxMs) stats_.rttMaxMs = ms;
}

void BotClient::sendFrame(const std::vector<std::byte>& frame, SteadyTime now) {
    login::ClientSession* session = baseSession_ ? baseSession_.get() : loginSession_.get();
    if (!session || !session->isConnected()) {
        scheduleRetry(now, "send on closed session");
        return;
    }
    if (!session->send(std::span<const std::byte>(frame.data(), frame.size()))) {
        scheduleRetry(now, "send backpressure");
        return;
    }
    ++stats_.messagesOut;
}

void BotClient::sendAction(const std::string& name, const std::vector<float>& args,
                           SteadyTime now) {
    theseed::foundation::MemoryStream payload;
    payload.writeUint64(entityId_);
    payload.writeUint32(static_cast<std::uint32_t>(name.size()));
    if (!name.empty()) {
        payload.writeBytes(reinterpret_cast<const std::byte*>(name.data()), name.size());
    }
    payload.writeUint32(static_cast<std::uint32_t>(args.size() * sizeof(float)));
    for (float f : args) {
        std::byte buf[sizeof(float)];
        std::memcpy(buf, &f, sizeof(float));
        payload.writeBytes(buf, sizeof(float));
    }
    const auto raw = std::span<const std::byte>(payload.data(), payload.size());
    sendFrame(login::LoginProtocol::frameMessage(ClientMessageType::Action, raw), now);
    markSent(now);
}

void BotClient::tickInGame(SteadyTime now) {
    if (!baseSession_ || !baseSession_->isConnected()) {
        scheduleRetry(now, "base connection lost");
        return;
    }
    if (now - lastMoveAt_ >= config_.moveInterval) {
        lastMoveAt_ = now;
        posX_ += vx_;
        if (posX_ > 100.0f) vx_ = -1.0f;
        if (posX_ < -100.0f) vx_ = 1.0f;
        sendAction(config_.moveAction, {posX_, posY_, posZ_}, now);
    }
    if (now - lastHeartbeatAt_ >= config_.heartbeatInterval) {
        lastHeartbeatAt_ = now;
        ++heartbeatSeq_;
        sendAction(config_.heartbeatAction,
                   {static_cast<float>(heartbeatSeq_)}, now);
    }
}

void BotClient::tick(SteadyTime now) {
    switch (state_) {
        case State::Idle:
        case State::Backoff:
            if (now >= nextAttempt_) startLogin(now);
            break;
        case State::LoginConnecting:
            startLogin(now);
            break;
        case State::LoginSent:
        case State::SelectSent:
            if (loginSession_) loginSession_->pump();
            if (!loginSession_ || !loginSession_->isConnected()) {
                scheduleRetry(now, "login connection lost");
            } else if (now - stateSince_ > kResponseTimeout) {
                scheduleRetry(now, "login response timeout");
            }
            break;
        case State::BaseConnecting:
            // SelectRealmResponse 在回调内只登记参数（见 onMessage 注释），
            // 换腿（关登录连接 / 开 BaseApp 连接）延后到 tick 上下文执行。
            connectBase(now, pendingBaseHost_, pendingBasePort_, token_);
            break;
        case State::EnterSent:
            if (baseSession_) baseSession_->pump();
            if (!baseSession_ || !baseSession_->isConnected()) {
                scheduleRetry(now, "base connection lost");
            } else if (now - stateSince_ > kResponseTimeout) {
                scheduleRetry(now, "enter-game response timeout");
            }
            break;
        case State::InGame:
            if (baseSession_) baseSession_->pump();
            if (!baseSession_ || !baseSession_->isConnected()) {
                scheduleRetry(now, "base connection lost");
            } else {
                tickInGame(now);
            }
            break;
    }
}

void BotClient::onMessage(theseed::login::ClientMessageType type,
                          std::span<const std::byte> payload) {
    // 退避期到达的陈旧帧（旧连接缓冲里残余的应答）直接忽略——本回调
    // 由 ClientSession::tryParseMessages 在持其 recvBuffer_ 指针时派发，
    // 任何分支都不允许销毁会话对象。
    if (state_ == State::Backoff) return;
    const auto now = std::chrono::steady_clock::now();
    ++stats_.messagesIn;
    sampleRtt(now);

    using theseed::login::ClientProtocol;
    using theseed::login::LoginProtocol;

    switch (type) {
        case ClientMessageType::LoginResponse: {
            login::LoginResponse resp;
            if (state_ != State::LoginSent || !LoginProtocol::decodeLoginResponse(payload, resp)) {
                ++stats_.protocolErrors;
                scheduleRetry(now, "bad login response");
                return;
            }
            if (!resp.success) {
                scheduleRetry(now, "login rejected");
                return;
            }
            stateSince_ = now;
            state_ = State::SelectSent;
            sendFrame(LoginProtocol::encodeSelectRealm(config_.realmId), now);
            break;
        }
        case ClientMessageType::SelectRealmResponse: {
            login::SelectRealmResponse resp;
            if (state_ != State::SelectSent || !LoginProtocol::decodeSelectRealmResponse(payload, resp)) {
                ++stats_.protocolErrors;
                scheduleRetry(now, "bad select-realm response");
                return;
            }
            if (!resp.success) {
                scheduleRetry(now, "select realm rejected");
                return;
            }
            // 只登记参数，不在这里换腿（回调内销毁 loginSession_ 会
            // use-after-free）。真正的换腿由下一轮 tick 的 BaseConnecting
            // 分支执行。
            pendingBaseHost_ = config_.baseHost.empty() ? resp.host : config_.baseHost;
            pendingBasePort_ = config_.basePort != 0 ? config_.basePort : resp.port;
            token_ = resp.token;
            stateSince_ = now;
            state_ = State::BaseConnecting;
            break;
        }
        case ClientMessageType::EnterGameResponse: {
            // EnterGameResponse: u8 success + u64 entityId + string type + string error
            if (state_ != State::EnterSent || payload.size() < 9) {
                ++stats_.protocolErrors;
                scheduleRetry(now, "bad enter-game response");
                return;
            }
            const bool success = static_cast<std::uint8_t>(payload[0]) != 0;
            std::uint64_t eid = 0;
            std::memcpy(&eid, payload.data() + 1, 8);
            if (!success) {
                scheduleRetry(now, "enter game rejected");
                return;
            }
            entityId_ = eid;
            lastMoveAt_ = now - config_.moveInterval;    // 首个 tick 立即动身
            lastHeartbeatAt_ = now - config_.heartbeatInterval;
            state_ = State::InGame;
            break;
        }
        case ClientMessageType::ErrorResponse: {
            std::string message;
            if (LoginProtocol::decodeError(payload, message)) {
                ++stats_.protocolErrors;
            }
            scheduleRetry(now, "server error frame");
            break;
        }
        case ClientMessageType::EntityEnter:
        case ClientMessageType::EntityLeave:
        case ClientMessageType::PropertySync:
        case ClientMessageType::EntityEvent:
        case ClientMessageType::SpaceChange:
            // 下行镜像/事件：仅计数吞吐（已在入口 ++messagesIn）。
            break;
        default:
            ++stats_.protocolErrors;
            break;
    }
}

}  // namespace theseed::bots
