#include "theseed/foundation/RateLimiter.h"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <stdexcept>

namespace theseed::foundation {

RateLimiter::RateLimiter(std::shared_ptr<IRedisProvider> redis)
    : redis_(std::move(redis)) {
    if (!redis_) throw std::invalid_argument("RateLimiter requires a redis provider");
}

bool RateLimiter::tryConsume(const std::string& key,
                              const Config& config,
                              std::int64_t cost) {
    if (key.empty() || cost <= 0) return false;
    if (config.capacity <= 0) return false;

    const auto now = std::chrono::steady_clock::now();
    const auto rk = redisKey(key);

    BucketState state;
    if (auto blob = redis_->get(rk)) {
        auto decoded = decode(*blob);
        if (decoded) state = *decoded;
    }
    if (state.tokens == 0.0 && state.lastTouch.time_since_epoch().count() == 0) {
        // First touch: start full.
        state.tokens = static_cast<double>(config.capacity);
        state.lastTouch = now;
    }

    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - state.lastTouch);
    const double refillRatePerMs = 1.0 / static_cast<double>(
        std::chrono::duration_cast<std::chrono::milliseconds>(config.refillInterval).count());
    const double refilled = std::max(0.0, elapsedMs.count() * refillRatePerMs);
    state.tokens = std::min(static_cast<double>(config.capacity), state.tokens + refilled);
    state.lastTouch = now;

    if (state.tokens < static_cast<double>(cost)) {
        redis_->set(rk, encode(state), RedisDuration::zero());
        return false;
    }
    state.tokens -= static_cast<double>(cost);
    redis_->set(rk, encode(state), RedisDuration::zero());
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
