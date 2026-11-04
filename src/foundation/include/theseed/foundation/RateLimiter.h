#pragma once

#include "theseed/foundation/RedisProvider.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace theseed::foundation {

// MVP Phase B-3: token-bucket rate limiter on top of IRedisProvider.
//
// Each `tryConsume` reads the bucket for `key`, refills based on elapsed
// wall-clock since last touch, then subtracts one token if available.
// Backed by a small JSON-ish blob in redis so a future hiredis provider
// makes the limit visible across processes.

class RateLimiter final {
public:
    struct Config final {
        std::int64_t capacity = 10;             // max tokens
        RedisDuration refillInterval = std::chrono::seconds(1);  // per token
        // 分布式锁获取上限：超过则放弃本次消耗（返回 false），避免无界
        // 忙等阻塞调用线程（活跃性回归）。零或负值表示不启用超时（仍等锁，
        // 仅供测试显式控制）。
        RedisDuration lockTimeout = std::chrono::seconds(2);
        // 锁 TTL：临界区 get→refill→set 跨两次 Redis 往返，若持锁执行超过
        // TTL 锁会提前过期，后到者并发进入临界区使读改写竞态复发。设置足够
        // 大的初始 TTL，并在临界区内续期（若后端支持 renewLock）。
        RedisDuration lockTtl = std::chrono::seconds(30);
    };

    explicit RateLimiter(std::shared_ptr<IRedisProvider> redis);

    // Test seam: inject a deterministic clock so refill timing can be
    // asserted without sleeping. `nowFn` must return a monotonic instant.
    void setClock(std::function<std::chrono::steady_clock::time_point()> nowFn);

    // Attempt to consume `cost` tokens for `key`. Returns true if allowed,
    // false if the bucket does not have enough capacity. The redis blob is
    // updated atomically with respect to other tryConsume calls on this
    // limiter instance (in-memory provider is single-threaded by design).
    bool tryConsume(const std::string& key,
                    const Config& config,
                    std::int64_t cost = 1);

    // Inspect current token count for `key` without consuming. Useful for
    // diagnostics / metrics.
    double availableTokens(const std::string& key, const Config& config);

    // Reset the bucket for `key`. Useful for tests and admin overrides.
    bool reset(const std::string& key);

private:
    struct BucketState {
        double tokens = 0.0;
        std::chrono::steady_clock::time_point lastTouch;
    };

    static std::string redisKey(const std::string& key);
    static std::string encode(const BucketState& state);
    static std::optional<BucketState> decode(const std::string& blob);

    std::shared_ptr<IRedisProvider> redis_;
    std::function<std::chrono::steady_clock::time_point()> nowFn_ = [] {
        return std::chrono::steady_clock::now();
    };
};

}  // namespace theseed::foundation
