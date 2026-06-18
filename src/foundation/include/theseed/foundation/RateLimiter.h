#pragma once

#include "theseed/foundation/RedisProvider.h"

#include <chrono>
#include <cstdint>
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
    };

    explicit RateLimiter(std::shared_ptr<IRedisProvider> redis);

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
};

}  // namespace theseed::foundation
