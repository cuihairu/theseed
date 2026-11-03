#pragma once

#include "BotsConfig.h"
#include "BotClient.h"

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace theseed::bots {

// bot 群管理器：斜坡加压（每 tickTime 加 tickCount 个，直到 totalCount）、
// 每 tick 驱动全部 bot、周期聚合统计。
class BotManager final {
public:
    // 聚合统计快照。
    struct Aggregate final {
        std::size_t spawned = 0;      // 已创建的 bot 总数（斜坡目标）
        std::size_t inGame = 0;       // 已进入游戏（在线）
        std::size_t pending = 0;      // 登录/选区/进游戏中
        std::size_t backingOff = 0;   // 退避等待重连
        std::uint64_t messagesIn = 0;
        std::uint64_t messagesOut = 0;
        std::uint64_t reconnects = 0;
        std::uint64_t protocolErrors = 0;
        std::uint64_t rttSamples = 0;
        double rttLastMs = 0;
        double rttAvgMs = 0;
        double rttMaxMs = 0;
    };

    explicit BotManager(BotsConfig config);

    // 主循环驱动。斜坡生成 + 全员 tick + 周期统计。
    void tick(SteadyTime now);

    // 最近一次统计打印（statsInterval 周期刷新）。
    const Aggregate& aggregate() const { return aggregate_; }
    const std::vector<std::unique_ptr<BotClient>>& bots() const { return bots_; }
    const BotsConfig& config() const { return config_; }

    // 停机：关闭全部连接（打印最终统计前调用）。
    void stop();

    // 测试入口：断开第 index 个 bot（触发重连流程）。
    void disconnectBot(std::size_t index, SteadyTime now);

private:
    void updateRamp(SteadyTime now);
    void updateStats(SteadyTime now);

    BotsConfig config_;
    std::vector<std::unique_ptr<BotClient>> bots_;
    SteadyTime lastRampAt_{};
    SteadyTime lastStatsAt_{};
    Aggregate aggregate_;
};

// 单行可读统计（供 CLI 周期打印与测试断言复用）。
std::string renderStatsLine(const BotManager::Aggregate& agg);

}  // namespace theseed::bots
