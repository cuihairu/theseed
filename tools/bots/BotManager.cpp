#include "BotManager.h"

#include <cstdio>

namespace theseed::bots {

BotManager::BotManager(BotsConfig config) : config_(std::move(config)) {}

void BotManager::updateRamp(SteadyTime now) {
    if (bots_.size() >= config_.totalCount) return;
    if (bots_.empty()) {
        // 首个 bot 立即生成，斜坡计时从此起步。注意不能只重置 lastRampAt_
        // 再等 tickTime——bots_ 仍空的下一轮会再次重置，耗时永远归零，
        // 一个 bot 都生不出来。
        lastRampAt_ = now;
        bots_.push_back(std::make_unique<BotClient>(
            config_, config_.accountPrefix + std::to_string(bots_.size())));
        return;
    }
    if (now - lastRampAt_ < config_.tickTime) return;
    lastRampAt_ = now;
    const std::size_t batch = std::min(config_.tickCount,
                                       config_.totalCount - bots_.size());
    for (std::size_t i = 0; i < batch; ++i) {
        auto account = config_.accountPrefix + std::to_string(bots_.size());
        bots_.push_back(std::make_unique<BotClient>(config_, std::move(account)));
    }
}

void BotManager::updateStats(SteadyTime now) {
    static_cast<void>(now);  // 保持签名与打印周期语义一致（见 tick()）
    Aggregate agg;
    double rttMax = 0;
    for (const auto& bot : bots_) {
        ++agg.spawned;
        switch (bot->state()) {
            case BotClient::State::InGame: ++agg.inGame; break;
            case BotClient::State::Backoff:
            case BotClient::State::Idle:
            case BotClient::State::LoginConnecting:
            case BotClient::State::BaseConnecting:
                ++agg.backingOff;
                break;
            default:
                ++agg.pending;
                break;
        }
        const auto& s = bot->stats();
        agg.messagesIn += s.messagesIn;
        agg.messagesOut += s.messagesOut;
        agg.reconnects += s.reconnects;
        agg.protocolErrors += s.protocolErrors;
        agg.rttSamples += s.rttSamples;
        agg.rttLastMs = s.rttLastMs;  // 取末位 bot 的最近值（展示用）
        if (s.rttMaxMs > rttMax) rttMax = s.rttMaxMs;
    }
    agg.rttAvgMs = agg.rttSamples > 0
        ? [&] {
              double sum = 0;
              for (const auto& bot : bots_) sum += bot->stats().rttSumMs;
              return sum / static_cast<double>(agg.rttSamples);
          }()
        : 0.0;
    agg.rttMaxMs = rttMax;
    aggregate_ = agg;
}

void BotManager::tick(SteadyTime now) {
    updateRamp(now);
    for (auto& bot : bots_) bot->tick(now);
    // 聚合是 O(bots) 纯内存读，每 tick 刷新保证 aggregate() 总是新鲜值；
    // lastStatsAt_ 仅留给打印周期的语义（CLI 打印按 statsInterval 节流）。
    lastStatsAt_ = now;
    updateStats(now);
}

void BotManager::stop() {
    updateStats(std::chrono::steady_clock::now());
    for (auto& bot : bots_) {
        bot->forceDisconnect(std::chrono::steady_clock::now());
    }
    bots_.clear();
}

void BotManager::disconnectBot(std::size_t index, SteadyTime now) {
    if (index < bots_.size()) bots_[index]->forceDisconnect(now);
}

std::string renderStatsLine(const BotManager::Aggregate& agg) {
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "spawned=%zu in_game=%zu pending=%zu backoff=%zu "
                  "msg_in=%llu msg_out=%llu rtt_avg=%.1fms rtt_max=%.1fms "
                  "retries=%llu proto_err=%llu",
                  agg.spawned, agg.inGame, agg.pending, agg.backingOff,
                  static_cast<unsigned long long>(agg.messagesIn),
                  static_cast<unsigned long long>(agg.messagesOut),
                  agg.rttAvgMs, agg.rttMaxMs,
                  static_cast<unsigned long long>(agg.reconnects),
                  static_cast<unsigned long long>(agg.protocolErrors));
    return buf;
}

}  // namespace theseed::bots
