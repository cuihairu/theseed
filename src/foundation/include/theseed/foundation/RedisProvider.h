#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace theseed::foundation {

// MVP Phase B-3: minimal Redis-shaped provider interface + in-memory
// implementation. Goal is to let login/session and rate-limit code be written
// against a clean abstraction today, so a real hiredis-backed implementation
// can be dropped in later without churning call sites.
//
// The interface is synchronous — fine for in-memory. A future async/hiredis
// implementation will likely return futures, but at that point we accept the
// signature churn in exchange for not prematurely pulling in <future> here.

using RedisDuration = std::chrono::milliseconds;

class IRedisProvider {
public:
    virtual ~IRedisProvider() = default;

    // String operations.
    virtual bool set(const std::string& key,
                     const std::string& value,
                     RedisDuration ttl = RedisDuration::zero()) = 0;
    virtual std::optional<std::string> get(const std::string& key) = 0;
    virtual bool del(const std::string& key) = 0;
    virtual bool exists(const std::string& key) = 0;
    virtual bool expire(const std::string& key, RedisDuration ttl) = 0;

    // Sorted-set (used for leaderboards / ranked sessions).
    virtual bool zadd(const std::string& key,
                      const std::string& member,
                      double score) = 0;
    virtual std::vector<std::pair<std::string, double>> zrange(
        const std::string& key,
        std::size_t start,
        std::size_t stop) = 0;
    virtual std::vector<std::pair<std::string, double>> zrevrange(
        const std::string& key,
        std::size_t start,
        std::size_t stop) = 0;
    virtual std::size_t zcard(const std::string& key) = 0;

    // Distributed lock: returns true if acquired. Caller must call unlock().
    virtual bool lock(const std::string& key, RedisDuration ttl) = 0;
    virtual void unlock(const std::string& key) = 0;
};

// In-memory implementation with TTL expiry. Suitable for tests and
// single-instance deployments; not for multi-process coordination.
class InMemoryRedisProvider final : public IRedisProvider {
public:
    InMemoryRedisProvider();
    ~InMemoryRedisProvider() override;

    // Advance the in-memory clock by `delta`. Production code does not call
    // this — it exists so tests can drive TTL expiry deterministically.
    void advanceClock(RedisDuration delta);

    // IRedisProvider
    bool set(const std::string& key,
             const std::string& value,
             RedisDuration ttl = RedisDuration::zero()) override;
    std::optional<std::string> get(const std::string& key) override;
    bool del(const std::string& key) override;
    bool exists(const std::string& key) override;
    bool expire(const std::string& key, RedisDuration ttl) override;

    bool zadd(const std::string& key,
              const std::string& member,
              double score) override;
    std::vector<std::pair<std::string, double>> zrange(
        const std::string& key,
        std::size_t start,
        std::size_t stop) override;
    std::vector<std::pair<std::string, double>> zrevrange(
        const std::string& key,
        std::size_t start,
        std::size_t stop) override;
    std::size_t zcard(const std::string& key) override;

    bool lock(const std::string& key, RedisDuration ttl) override;
    void unlock(const std::string& key) override;

private:
    struct StringEntry {
        std::string value;
        std::optional<std::chrono::steady_clock::time_point> expiresAt;
    };

    struct SortedSet {
        std::map<std::string, double> members;
    };

    struct LockEntry {
        std::chrono::steady_clock::time_point expiresAt;
    };

    void reapExpired();

    std::mutex mutex_;
    std::chrono::steady_clock::time_point now_;
    std::unordered_map<std::string, StringEntry> strings_;
    std::unordered_map<std::string, SortedSet> sets_;
    std::unordered_map<std::string, LockEntry> locks_;
};

}  // namespace theseed::foundation
