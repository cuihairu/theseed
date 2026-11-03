#pragma once

#include "BotsConfig.h"

#include "theseed/login/ClientSession.h"
#include "theseed/login/LoginProtocol.h"
#include "theseed/runtime/TcpConnection.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace theseed::bots {

using SteadyTime = std::chrono::steady_clock::time_point;

class BotClientTest;

// 单个 bot 的协议客户端状态机（真客户端协议栈：ClientSession +
// ClientProtocol/LoginProtocol，非旁路 mock）。
//
//   Idle → LoginConnecting → LoginSent(登录) → SelectSent(选区)
//        → BaseConnecting → EnterSent(EnterGame) → InGame
//   任一步失败/掉线 → Backoff(reconnectDelay) → LoginConnecting 重来。
//
// InGame 后按 moveInterval / heartbeatInterval 周期发 Action（移动 / 心跳）。
// RTT 采样：发送 EnterGame / Action 后记录 pendingSentAt，下一个入站帧到达
// 时取差值（含 EnterGame 延迟的近似应用层往返）。
class BotClient final {
public:
    enum class State {
        Idle,
        Backoff,
        LoginConnecting,
        LoginSent,
        SelectSent,
        BaseConnecting,
        EnterSent,
        InGame,
    };

    // 运行期统计（只增字段由 manager 聚合）。
    struct Stats final {
        std::uint64_t messagesIn = 0;
        std::uint64_t messagesOut = 0;
        std::uint64_t reconnects = 0;
        std::uint64_t protocolErrors = 0;
        std::uint64_t rttSamples = 0;
        double rttLastMs = 0;
        double rttSumMs = 0;
        double rttMaxMs = 0;
    };

    BotClient(const BotsConfig& config, std::string account);

    void tick(SteadyTime now);

    const std::string& account() const { return account_; }
    State state() const { return state_; }
    bool isInGame() const { return state_ == State::InGame; }
    std::uint64_t entityId() const { return entityId_; }
    const Stats& stats() const { return stats_; }

    // 测试/运维注入：主动断开（触发正常退避重连流程）。
    void forceDisconnect(SteadyTime now);

private:
    void scheduleRetry(SteadyTime now, const char* reason);
    void startLogin(SteadyTime now);
    void connectBase(SteadyTime now, const std::string& host, std::uint16_t port,
                     const std::string& token);
    void onMessage(theseed::login::ClientMessageType type,
                   std::span<const std::byte> payload);
    void sendFrame(const std::vector<std::byte>& frame, SteadyTime now);
    void markSent(SteadyTime now);
    void sampleRtt(SteadyTime now);
    void tickInGame(SteadyTime now);
    void sendAction(const std::string& name, const std::vector<float>& args,
                    SteadyTime now);
    void resetConnections();

    friend class BotClientTest;

    const BotsConfig& config_;
    std::string account_;

    State state_ = State::Idle;
    SteadyTime nextAttempt_{};   // Backoff 到期点
    SteadyTime stateSince_{};    // 当前状态进入点（应答超时守卫）
    SteadyTime lastMoveAt_{};
    SteadyTime lastHeartbeatAt_{};
    bool pendingSent_ = false;   // 已发待应答（RTT 采样窗）
    SteadyTime pendingSentAt_{};

    // 登录腿（LoginApp）
    std::shared_ptr<runtime::TcpConnection> loginConn_;
    std::unique_ptr<login::ClientSession> loginSession_;
    // 游戏腿（BaseApp）
    std::shared_ptr<runtime::TcpConnection> baseConn_;
    std::unique_ptr<login::ClientSession> baseSession_;

    std::string token_;          // SelectRealmResponse 返回的游戏 token
    std::string pendingBaseHost_;  // BaseConnecting 阶段待用的游戏服地址
    std::uint16_t pendingBasePort_ = 0;
    std::uint64_t entityId_ = 0;
    float posX_ = 0, posY_ = 0, posZ_ = 0;
    float vx_ = 1.0f;            // 移动脚本：线性游走方向
    std::uint32_t heartbeatSeq_ = 0;
    bool retryScheduled_ = false;

    Stats stats_;
};

}  // namespace theseed::bots
