#pragma once

#include <chrono>
#include <cstddef>
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
    virtual ~IRedisProvider() = default;  // LCOV_EXCL_LINE C++ ABI：trivial 虚析构是空体，gcc 不为其生成计数指令，D0/D1/D2 三符号变体恒 0（结构不可测）

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
    // Remove `member` from the sorted set. Returns true if it was present.
    // (SessionStore's secondary index uses this to retire entries.)
    virtual bool zrem(const std::string& key, const std::string& member) = 0;

    // Distributed lock. Each acquisition mints a unique owner token; every
    // subsequent renewLock/unlock on this lock must present the same token so
    // a holder whose lock expired and was re-acquired by another party cannot
    // accidentally renew or release the new holder's lock.
    using LockToken = std::string;
    // Returns an owner token when acquired, or nullopt if the lock is busy.
    // The caller must pass the token back to renewLock/unlock.
    virtual std::optional<LockToken> lock(const std::string& key,
                                          RedisDuration ttl) = 0;
    // Release the lock only if `token` still owns it. Returns true when the
    // lock was released (or was already gone), false when the lock now belongs
    // to a different owner and must be left untouched.
    virtual bool unlock(const std::string& key, const LockToken& token) = 0;
    // Extend the TTL only if `token` still owns the lock. Returns false when
    // the caller definitively no longer holds the lock (expired, missing, or
    // re-acquired by a different owner).
    virtual bool renewLock(const std::string& key,
                           const LockToken& token,
                           RedisDuration ttl) = 0;
    // Atomic write-under-ownership: store `value` at `key` only if `token`
    // still owns the lock at `lockKey`, extending that lock's TTL in the same
    // atomic step. Returns false when ownership was lost — nothing is written
    // and the new owner's state is left untouched. This closes the race where
    // a separate renewLock succeeded but the lock expired before a plain set
    // landed, letting the stale holder overwrite the new holder's state. A
    // real Redis backend implements this as one Lua script (token check + SET
    // + PEXPIRE); the value is stored without expiry.
    virtual bool setIfLockOwner(const std::string& lockKey,
                                const LockToken& token,
                                const std::string& key,
                                const std::string& value,
                                RedisDuration lockTtl) = 0;
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
    bool zrem(const std::string& key, const std::string& member) override;

    std::optional<LockToken> lock(const std::string& key,
                                  RedisDuration ttl) override;
    bool unlock(const std::string& key, const LockToken& token) override;
    bool renewLock(const std::string& key,
                   const LockToken& token,
                   RedisDuration ttl) override;
    bool setIfLockOwner(const std::string& lockKey,
                        const LockToken& token,
                        const std::string& key,
                        const std::string& value,
                        RedisDuration lockTtl) override;

private:
    struct StringEntry {
        std::string value;
        std::optional<std::chrono::steady_clock::time_point> expiresAt;
    };

    struct SortedSet {
        std::map<std::string, double> members;
    };

    struct LockEntry {
        std::string token;
        std::chrono::steady_clock::time_point expiresAt;
    };

    void reapExpired();

    std::mutex mutex_;
    std::chrono::steady_clock::time_point now_;
    std::uint64_t lockTokenSeq_ = 0;
    std::unordered_map<std::string, StringEntry> strings_;
    std::unordered_map<std::string, SortedSet> sets_;
    std::unordered_map<std::string, LockEntry> locks_;
};

}  // namespace theseed::foundation
