#include "theseed/foundation/RateLimiter.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <cstdint>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace theseed::foundation {

RateLimiter::RateLimiter(std::shared_ptr<IRedisProvider> redis)
    : redis_(std::move(redis)) {
    if (!redis_) throw std::invalid_argument("RateLimiter requires a redis provider");
}

void RateLimiter::setClock(std::function<std::chrono::steady_clock::time_point()> nowFn) {
    nowFn_ = std::move(nowFn);
}

bool RateLimiter::tryConsume(const std::string& key,
                              const Config& config,
                              std::int64_t cost) {
    if (key.empty() || cost <= 0) return false;
    if (config.capacity <= 0) return false;

    const auto rk = redisKey(key);
    const auto lockKey = rk + ":lock";
    // 锁获取带超时上限：锁不可用（Redis 不可达 / 持锁者未释放且 TTL 未到）
    // 时不再永久忙等，避免调用线程被无界阻塞（活跃性回归）。超时后放弃本次
    // 消耗并返回 false，调用方按"被限流"处理。
    const auto lockTtl = config.lockTtl;
    const auto lockTimeout = config.lockTimeout;
    const auto lockStart = std::chrono::steady_clock::now();
    // 获取锁并持有其所有权令牌：后续 renewLock/unlock 都必须携带同一令牌，
    // 防止锁过期被后到者重新持有后，原持有者误认领新锁并清掉它。
    auto lockToken = redis_->lock(lockKey, lockTtl);
    while (!lockToken.has_value()) {
        if (lockTimeout > RedisDuration::zero() &&
            std::chrono::steady_clock::now() - lockStart >= lockTimeout) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        lockToken = redis_->lock(lockKey, lockTtl);
    }
    struct UnlockGuard {
        IRedisProvider& redis;
        const std::string& key;
        const IRedisProvider::LockToken& token;
        ~UnlockGuard() { redis.unlock(key, token); }
    } unlockGuard{*redis_, lockKey, *lockToken};
    const auto now = nowFn_();

    BucketState state;
    if (auto blob = redis_->get(rk)) {
        auto decoded = decode(*blob);
        if (decoded) state = *decoded;
    }
    // 续期（同步版）：临界区跨 get→refill→写回多次 Redis 往返，若执行
    // 超过 lockTtl 锁会提前过期，后到者再次获锁并发进入临界区使读改写
    // 竞态复发。get 后续期一次维持锁活跃；续期失败（含令牌不匹配，即锁
    // 已被并发者重新持有）说明不再独占，放弃本次消耗避免无锁保护下继续
    // 读改写。写回本身由 setIfLockOwner 原子完成（见下）。
    if (!redis_->renewLock(lockKey, *lockToken, lockTtl)) {
        return false;
    }
    if (state.tokens == 0.0 && state.lastTouch.time_since_epoch().count() == 0) {
        // First touch: start full.
        state.tokens = static_cast<double>(config.capacity);
        state.lastTouch = now;
    }

    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - state.lastTouch);
    // P2 深审：refillInterval < 1ms 时毫秒计数为 0，1.0/0 得 inf，refilled
    // 恒为无穷→桶永远满，限流器静默失效。钳到 1ms 下限，保证 refillRate
    // 有限且按最慢可表达粒度补充。
    const auto refillMs = std::max<std::int64_t>(
        1, std::chrono::duration_cast<std::chrono::milliseconds>(config.refillInterval).count());
    const double refillRatePerMs = 1.0 / static_cast<double>(refillMs);
    const double refilled = std::max(0.0, elapsedMs.count() * refillRatePerMs);
    state.tokens = std::min(static_cast<double>(config.capacity), state.tokens + refilled);
    state.lastTouch = now;

    // 写回不再走「续期 + 裸 set」两步：续期成功后锁仍可能在 set 执行前
    // 过期被并发者夺走，旧持有者的裸 set 会覆盖新持有者的桶状态造成超额
    // 放行。setIfLockOwner 把「所有权校验 → 写回 → 续期」合成一步原子操作
    // （真实 Redis 后端用单条 Lua 脚本实现），写回本身即最后一次所有权
    // 校验点：令牌失配即拒绝写入并放弃本次消耗。
    if (state.tokens < static_cast<double>(cost)) {
        // 桶不足：refill 结果仍需写回，但本次消耗不成立。
        redis_->setIfLockOwner(lockKey, *lockToken, rk, encode(state), lockTtl);
        return false;
    }
    state.tokens -= static_cast<double>(cost);
    if (!redis_->setIfLockOwner(lockKey, *lockToken, rk, encode(state), lockTtl)) {
        return false;
    }
    return true;
}

double RateLimiter::availableTokens(const std::string& key, const Config& config) {
    if (key.empty()) return 0.0;
    const auto rk = redisKey(key);
    auto blob = redis_->get(rk);
    if (!blob) return static_cast<double>(config.capacity);
    auto state = decode(*blob);
    if (!state) return static_cast<double>(config.capacity);
    return state->tokens;
}

bool RateLimiter::reset(const std::string& key) {
    if (key.empty()) return false;
    return redis_->del(redisKey(key));
}

std::string RateLimiter::redisKey(const std::string& key) {
    return "rate:" + key;
}

std::string RateLimiter::encode(const BucketState& state) {
    std::ostringstream out;
    out << state.tokens << ' '
        << state.lastTouch.time_since_epoch().count();
    return out.str();
}

std::optional<RateLimiter::BucketState> RateLimiter::decode(const std::string& blob) {
    std::istringstream in(blob);
    BucketState s;
    long long sinceEpoch = 0;
    if (!(in >> s.tokens >> sinceEpoch)) return std::nullopt;
    s.lastTouch = std::chrono::steady_clock::time_point(
        std::chrono::steady_clock::duration(sinceEpoch));
    return s;
}

}  // namespace theseed::foundation
