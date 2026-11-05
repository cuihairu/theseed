#include "theseed/foundation/RateLimiter.h"
#include "theseed/foundation/RedisProvider.h"
#include "theseed/foundation/SessionStore.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <latch>
#include <iostream>
#include <map>
#include <mutex>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using theseed::foundation::InMemoryRedisProvider;
using theseed::foundation::RateLimiter;
using theseed::foundation::RedisDuration;
using theseed::foundation::SessionStore;
using theseed::foundation::SessionView;
using theseed::foundation::StoredSession;

static int testsPassed = 0;
static int testsFailed = 0;

#define TEST(name)                                              \
    do {                                                        \
        std::cout << "  " << (name) << "... " << std::flush;    \
    } while (0)

#define PASS()                                                  \
    do {                                                        \
        std::cout << "OK\n";                                    \
        ++testsPassed;                                          \
    } while (0)

#define FAIL(msg)                                               \
    do {                                                        \
        std::cout << "FAIL: " << (msg) << "\n";                 \
        ++testsFailed;                                          \
    } while (0)

// --- Redis provider tests ---

static void test_redis_set_get() {
    TEST("test_redis_set_get");
    InMemoryRedisProvider r;
    r.set("k", "v");
    auto v = r.get("k");
    if (!v || *v != "v") { FAIL("get returned wrong value"); return; }
    PASS();
}

static void test_redis_ttl_expires() {
    TEST("test_redis_ttl_expires");
    InMemoryRedisProvider r;
    r.set("k", "v", RedisDuration(1000));
    r.advanceClock(RedisDuration(999));
    if (!r.get("k").has_value()) { FAIL("expired too early"); return; }
    r.advanceClock(RedisDuration(2));
    if (r.get("k").has_value()) { FAIL("did not expire after ttl"); return; }
    PASS();
}

static void test_redis_del() {
    TEST("test_redis_del");
    InMemoryRedisProvider r;
    r.set("a", "1");
    if (!r.del("a")) { FAIL("del returned false"); return; }
    if (r.exists("a")) { FAIL("exists after del"); return; }
    if (r.del("a")) { FAIL("del of missing key returned true"); return; }
    PASS();
}

static void test_redis_zadd_zrange() {
    TEST("test_redis_zadd_zrange");
    InMemoryRedisProvider r;
    r.zadd("lb", "alice", 10.0);
    r.zadd("lb", "bob", 5.0);
    r.zadd("lb", "carol", 15.0);
    auto fwd = r.zrange("lb", 0, 2);
    if (fwd.size() != 3) { FAIL("expected 3 entries"); return; }
    if (fwd[0].first != "bob") { FAIL("ascending should start with bob"); return; }
    auto rev = r.zrevrange("lb", 0, 0);
    if (rev.size() != 1 || rev[0].first != "carol") { FAIL("rev top should be carol"); return; }
    if (r.zcard("lb") != 3) { FAIL("zcard should be 3"); return; }
    PASS();
}

static void test_redis_lock_acquire_release() {
    TEST("test_redis_lock_acquire_release");
    InMemoryRedisProvider r;
    auto tok1 = r.lock("resource", RedisDuration(5000));
    if (!tok1) { FAIL("first lock should succeed"); return; }
    if (r.lock("resource", RedisDuration(5000))) { FAIL("second lock should fail"); return; }
    if (!r.unlock("resource", *tok1)) { FAIL("owner unlock should succeed"); return; }
    auto tok2 = r.lock("resource", RedisDuration(5000));
    if (!tok2) { FAIL("lock after unlock should succeed"); return; }
    if (!r.unlock("resource", *tok2)) { FAIL("owner unlock 2 should succeed"); return; }
    PASS();
}

static void test_redis_lock_expires() {
    TEST("test_redis_lock_expires");
    InMemoryRedisProvider r;
    auto old = r.lock("resource", RedisDuration(1000));
    if (!old) { FAIL("first lock should succeed"); return; }
    r.advanceClock(RedisDuration(1001));
    // 过期后旧令牌不能再续期（锁已消失，原持有者失去所有权）。
    if (r.renewLock("resource", *old, RedisDuration(1000))) {
        FAIL("expired token must not renew a gone lock");
        return;
    }
    // 锁已过期消失：旧令牌解锁是幂等 no-op（无锁可清，返回 true）。
    if (!r.unlock("resource", *old)) {
        FAIL("unlock of a gone lock should be a no-op success");
        return;
    }
    auto fresh = r.lock("resource", RedisDuration(1000));
    if (!fresh) { FAIL("expired lock should be re-acquirable"); return; }
    if (!r.renewLock("resource", *fresh, RedisDuration(1000))) {
        FAIL("fresh token should renew");
        return;
    }
    if (!r.unlock("resource", *fresh)) { FAIL("fresh unlock should succeed"); return; }
    PASS();
}

static void test_redis_zrem() {
    TEST("test_redis_zrem");
    InMemoryRedisProvider r;
    r.zadd("idx", "a", 1.0);
    r.zadd("idx", "b", 2.0);
    if (!r.zrem("idx", "a")) { FAIL("zrem of present member should be true"); return; }
    if (r.zrem("idx", "a")) { FAIL("zrem of removed member should be false"); return; }
    if (r.zrem("no-such-set", "a")) { FAIL("zrem on missing set should be false"); return; }
    if (r.zcard("idx") != 1) { FAIL("zcard should drop to 1 after zrem"); return; }
    // stop 用 size_t 哨兵表示"直到末尾"（裸 -1 在 MSVC 触发 C4245；
    // 与 SessionStore/RedisProvider.cpp 同款惯例）。
    auto left = r.zrange("idx", 0, static_cast<std::size_t>(-1));
    if (left.size() != 1 || left[0].first != "b") { FAIL("wrong member left after zrem"); return; }
    PASS();
}

// --- SessionStore tests ---

static void test_session_save_load() {
    TEST("test_session_save_load");
    auto redis = std::make_shared<InMemoryRedisProvider>();
    SessionStore store(redis);
    StoredSession s;
    s.accountId = "acc-1";
    s.realmId = "realm_a";
    s.userId = 42;
    s.metadata = R"({"client":"unity"})";
    if (!store.save("token-1", s, RedisDuration(60000))) { FAIL("save returned false"); return; }
    auto loaded = store.load("token-1");
    if (!loaded) { FAIL("load failed"); return; }
    if (loaded->accountId != "acc-1" || loaded->realmId != "realm_a" || loaded->userId != 42) {
        FAIL("loaded session mismatch"); return;
    }
    if (loaded->metadata != R"({"client":"unity"})") { FAIL("metadata mismatch"); return; }
    PASS();
}

static void test_session_ttl_expires() {
    TEST("test_session_ttl_expires");
    auto redis = std::make_shared<InMemoryRedisProvider>();
    SessionStore store(redis);
    StoredSession s;
    s.accountId = "acc-2";
    s.realmId = "realm_a";
    s.userId = 7;
    store.save("token-2", s, RedisDuration(1000));
    redis->advanceClock(RedisDuration(1001));
    if (store.load("token-2").has_value()) { FAIL("session should have expired"); return; }
    PASS();
}

static void test_session_revoke() {
    TEST("test_session_revoke");
    auto redis = std::make_shared<InMemoryRedisProvider>();
    SessionStore store(redis);
    StoredSession s;
    s.accountId = "acc-3";
    s.userId = 1;
    store.save("token-3", s, RedisDuration(60000));
    if (!store.revoke("token-3")) { FAIL("revoke returned false"); return; }
    if (store.load("token-3").has_value()) { FAIL("session still present after revoke"); return; }
    if (store.revoke("token-3")) { FAIL("revoke of missing token should be false"); return; }
    PASS();
}

static void test_session_refresh_extends_ttl() {
    TEST("test_session_refresh_extends_ttl");
    auto redis = std::make_shared<InMemoryRedisProvider>();
    SessionStore store(redis);
    StoredSession s;
    s.accountId = "acc-4";
    s.userId = 99;
    store.save("token-4", s, RedisDuration(2000));
    redis->advanceClock(RedisDuration(1500));
    if (!store.refresh("token-4", RedisDuration(2000))) { FAIL("refresh failed"); return; }
    redis->advanceClock(RedisDuration(1500));
    if (!store.load("token-4").has_value()) { FAIL("session expired despite refresh"); return; }
    PASS();
}

static void test_session_decode_rejects_garbage() {
    TEST("test_session_decode_rejects_garbage");
    auto bad = SessionStore::decode("garbage-no-separators");
    if (bad.has_value()) { FAIL("decode should reject malformed blob"); return; }
    PASS();
}

static void test_session_decode_rejects_bad_user_id() {
    TEST("test_session_decode_rejects_non_numeric_user_id");
    auto bad = SessionStore::decode("acc-1\x1f"
                                    "realm-1\x1f"
                                    "not-a-number\nmeta");
    if (bad.has_value()) { FAIL("decode should reject non-numeric userId"); return; }
    PASS();
}

static void test_session_list_enumerates_and_revoked_gone() {
    TEST("test_session_list_enumerates_and_revoked_gone");
    auto redis = std::make_shared<InMemoryRedisProvider>();
    SessionStore store(redis);
    StoredSession s;
    s.accountId = "acc-a";
    s.realmId = "realm-1";
    s.userId = 11;
    if (!store.save("token-l1", s, RedisDuration(60000))) { FAIL("save l1 failed"); return; }
    s.accountId = "acc-b";
    s.userId = 22;
    if (!store.save("token-l2", s, RedisDuration(60000))) { FAIL("save l2 failed"); return; }
    auto views = store.listSessions();
    if (views.size() != 2) { FAIL("expected 2 live sessions"); return; }
    // 内存提供者按成员字典序枚举；字段须逐项对上
    const SessionView* first = views[0].token == "token-l1" ? &views[0] : &views[1];
    const SessionView* second = first == &views[0] ? &views[1] : &views[0];
    if (first->accountId != "acc-a" || first->realmId != "realm-1" || first->userId != 11) {
        FAIL("first row fields mismatch"); return;
    }
    if (second->accountId != "acc-b" || second->userId != 22) {
        FAIL("second row fields mismatch"); return;
    }
    if (!store.revoke("token-l1")) { FAIL("revoke l1 failed"); return; }
    views = store.listSessions();
    if (views.size() != 1 || views[0].token != "token-l2") {
        FAIL("revoked session must leave the enumeration"); return;
    }
    PASS();
}

static void test_session_list_prunes_expired() {
    TEST("test_session_list_prunes_expired");
    auto redis = std::make_shared<InMemoryRedisProvider>();
    SessionStore store(redis);
    StoredSession s;
    s.accountId = "acc-c";
    store.save("token-e1", s, RedisDuration(1000));
    store.save("token-e2", s, RedisDuration(60000));
    redis->advanceClock(RedisDuration(1001));
    // load 已不命中（TTL 过期），索引成员由枚举惰性清账
    auto views = store.listSessions();
    if (views.size() != 1 || views[0].token != "token-e2") {
        FAIL("expired session must be pruned from enumeration"); return;
    }
    if (redis->zcard("sessions:index") != 1) { FAIL("index should keep exactly the live member"); return; }
    PASS();
}

static void test_session_list_prunes_corrupt_blob() {
    TEST("test_session_list_prunes_corrupt_blob");
    auto redis = std::make_shared<InMemoryRedisProvider>();
    SessionStore store(redis);
    StoredSession s;
    s.accountId = "acc-d";
    store.save("token-x1", s, RedisDuration(60000));
    store.save("token-x2", s, RedisDuration(60000));
    // 直接把一个会话键改写成损坏 blob（绕过 store 的编码器）
    redis->set("session:token-x1", "garbage-blob", RedisDuration(60000));
    auto views = store.listSessions();
    if (views.size() != 1 || views[0].token != "token-x2") {
        FAIL("corrupt entry must be pruned, live one kept"); return;
    }
    PASS();
}

// --- RateLimiter tests ---

static void test_rate_limiter_allows_within_capacity() {
    TEST("test_rate_limiter_allows_within_capacity");
    auto redis = std::make_shared<InMemoryRedisProvider>();
    RateLimiter limiter(redis);
    RateLimiter::Config cfg;
    cfg.capacity = 3;
    cfg.refillInterval = std::chrono::seconds(60);
    for (int i = 0; i < 3; ++i) {
        if (!limiter.tryConsume("user", cfg)) {
            FAIL("capacity should allow 3"); return;
        }
    }
    if (limiter.tryConsume("user", cfg)) { FAIL("4th should be rejected"); return; }
    PASS();
}

static void test_rate_limiter_refills_over_time() {
    TEST("test_rate_limiter_refills_over_time");
    auto redis = std::make_shared<InMemoryRedisProvider>();
    RateLimiter limiter(redis);
    RateLimiter::Config cfg;
    cfg.capacity = 2;
    cfg.refillInterval = std::chrono::milliseconds(100);
    limiter.tryConsume("k", cfg);
    limiter.tryConsume("k", cfg);
    if (limiter.tryConsume("k", cfg)) { FAIL("should be empty before refill"); return; }
    // tryConsume uses the injected clock; advance it deterministically to
    // guarantee at least one refill without real sleeping.
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    limiter.setClock([&now] { return now; });
    now += std::chrono::milliseconds(120);
    if (!limiter.tryConsume("k", cfg)) { FAIL("should refill after sleep"); return; }
    PASS();
}

// P2 深审回归：refillInterval < 1ms（毫秒计数 0）时旧实现 1.0/0=inf，
// 任何 elapsed≥1ms 都把桶瞬间补满——容量被反复即时耗尽，限流器静默失效。
// 修复后钳到 1ms：短窗口内最多补 1 token，容量耗尽后不可能立即全量放行。
static void test_rate_limiter_sub_ms_refill_clamped() {
    TEST("test_rate_limiter_sub_ms_refill_clamped");
    auto redis = std::make_shared<InMemoryRedisProvider>();
    RateLimiter limiter(redis);
    RateLimiter::Config cfg;
    cfg.capacity = 5;
    cfg.refillInterval = std::chrono::milliseconds(0);  // < 1ms
    // 注入时钟必须先于消耗：若先真实消耗再注入，真实消耗与注入 now 之间
    // 被调度器抢占 ≥3ms 会让注入 elapsed≥5ms 补满容量，负断言误失败。
    // 注入固定时钟后 5 次消耗共享同一 lastTouch，推进量完全确定。
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    limiter.setClock([&now] { return now; });
    // 立即耗尽 5 个 token。
    for (int i = 0; i < 5; ++i) {
        if (!limiter.tryConsume("k", cfg)) { FAIL("capacity 5 should allow 5"); return; }
    }
    // 注入时钟推进 ≥1ms：旧实现 refillRate=inf → 5 token 立即回满并再次
    // 放行 5 个；修复后钳到 1ms → 只补 1 token，不足以再次放行 5 个。
    now += std::chrono::milliseconds(2);
    if (limiter.tryConsume("k", cfg, 5)) { FAIL("sub-ms refill must not refill full capacity instantly"); return; }
    PASS();
}

static void test_rate_limiter_reset() {
    TEST("test_rate_limiter_reset");
    auto redis = std::make_shared<InMemoryRedisProvider>();
    RateLimiter limiter(redis);
    RateLimiter::Config cfg;
    cfg.capacity = 1;
    cfg.refillInterval = std::chrono::seconds(60);
    limiter.tryConsume("k", cfg);
    if (limiter.tryConsume("k", cfg)) { FAIL("bucket should be empty"); return; }
    if (!limiter.reset("k")) { FAIL("reset returned false"); return; }
    if (!limiter.tryConsume("k", cfg)) { FAIL("should allow after reset"); return; }
    PASS();
}

static void test_rate_limiter_independent_keys() {
    TEST("test_rate_limiter_independent_keys");
    auto redis = std::make_shared<InMemoryRedisProvider>();
    RateLimiter limiter(redis);
    RateLimiter::Config cfg;
    cfg.capacity = 1;
    cfg.refillInterval = std::chrono::seconds(60);
    if (!limiter.tryConsume("a", cfg)) { FAIL("a should succeed"); return; }
    if (!limiter.tryConsume("b", cfg)) { FAIL("b should succeed (independent)"); return; }
    PASS();
}

static void test_rate_limiter_available_tokens() {
    TEST("test_rate_limiter_available_tokens");
    auto redis = std::make_shared<InMemoryRedisProvider>();
    RateLimiter limiter(redis);
    RateLimiter::Config cfg;
    cfg.capacity = 5;
    cfg.refillInterval = std::chrono::seconds(60);

    // 空键：直接拒绝
    if (limiter.availableTokens("", cfg) != 0.0) { FAIL("empty key should be 0"); return; }

    // bucket 不存在：返回满容量
    if (limiter.availableTokens("fresh", cfg) != 5.0) { FAIL("fresh key should be full"); return; }

    // 消费 2 个后可查剩余（decode 命中）
    if (!limiter.tryConsume("u", cfg, 2)) { FAIL("consume 2 should succeed"); return; }
    if (limiter.availableTokens("u", cfg) != 3.0) {
        FAIL("remaining=" + std::to_string(limiter.availableTokens("u", cfg)));
        return;
    }

    // redis 里的状态损坏：回退满容量
    redis->set("rate:bad", "garbage blob");
    if (limiter.availableTokens("bad", cfg) != 5.0) { FAIL("corrupt blob should be full"); return; }

    PASS();
}

// P2 深审回归：tryConsume 的跨 Redis get→set 读改写原先无互斥，两个并发
// 调用者都能读到同一份满桶快照并各自放行，造成并发超发。RendezvousProvider
// 的 get 等待另一读者也进入 get 后再返回快照，把竞态窗口钉死成两读者交会：
// 未修复实现两个线程都读到满桶 → 双双放行（超发）；修复后分布式锁串行化
// tryConsume，后到者只能在锁释放后读到已扣减的桶，放行数恰为 1。
class RendezvousRateLimitProvider final
    : public theseed::foundation::IRedisProvider {
public:
    std::optional<std::string> get(const std::string& key) override {
        std::string snapshot;
        {
            const std::lock_guard lock(mutex_);
            auto it = kv_.find(key);
            if (it != kv_.end()) snapshot = it->second;
            ++readers_;
            if (readers_ == 2) ready_.notify_all();
        }
        std::unique_lock lock(mutex_);
        // 修复后持锁的读者等不到第二读者（它阻塞在分布式锁上），有限期超时
        // 后继续推进；未修复实现两读者及时交会，双双返回同一份满桶快照。
        ready_.wait_for(lock, std::chrono::milliseconds(200),
                        [this] { return readers_ >= 2; });
        if (snapshot.empty()) return std::nullopt;
        return snapshot;
    }

    bool set(const std::string& key, const std::string& value,
             RedisDuration) override {
        const std::lock_guard lock(mutex_);
        kv_[key] = value;
        return true;
    }
    bool del(const std::string& key) override {
        const std::lock_guard lock(mutex_);
        return kv_.erase(key) > 0;
    }
    bool exists(const std::string& key) override {
        const std::lock_guard lock(mutex_);
        return kv_.count(key) > 0;
    }
    bool expire(const std::string&, RedisDuration) override { return true; }
    bool zadd(const std::string&, const std::string&, double) override { return true; }
    std::vector<std::pair<std::string, double>> zrange(const std::string&,
                                                       std::size_t,
                                                       std::size_t) override { return {}; }
    std::vector<std::pair<std::string, double>> zrevrange(const std::string&,
                                                          std::size_t,
                                                          std::size_t) override { return {}; }
    std::size_t zcard(const std::string&) override { return 0; }
    bool zrem(const std::string&, const std::string&) override { return false; }
    std::optional<theseed::foundation::IRedisProvider::LockToken>
    lock(const std::string& key, RedisDuration) override {
        const std::lock_guard lock(mutex_);
        if (locks_.count(key) > 0) return std::nullopt;
        const auto token = std::to_string(++lockSeq_);
        locks_[key] = token;
        return token;
    }
    bool unlock(const std::string& key,
                const theseed::foundation::IRedisProvider::LockToken& token)
        override {
        const std::lock_guard lock(mutex_);
        const auto it = locks_.find(key);
        if (it == locks_.end() || it->second != token) return false;
        locks_.erase(it);
        return true;
    }
    bool renewLock(const std::string&,
                   const theseed::foundation::IRedisProvider::LockToken&,
                   RedisDuration) override {
        return true;
    }
    bool setIfLockOwner(const std::string& lockKey,
                        const theseed::foundation::IRedisProvider::LockToken& token,
                        const std::string& key,
                        const std::string& value,
                        RedisDuration) override {
        const std::lock_guard lock(mutex_);
        const auto it = locks_.find(lockKey);
        if (it == locks_.end() || it->second != token) return false;
        kv_[key] = value;
        return true;
    }

private:
    std::mutex mutex_;
    std::condition_variable ready_;
    int readers_ = 0;
    std::map<std::string, std::string> kv_;
    std::uint64_t lockSeq_ = 0;
    std::map<std::string, std::string> locks_;
};

// 回归锚：在 RateLimiter 临界区内故意让原锁过期并由“后到者”重新持有。
// StealBeforeFirstRenewal 对应第一次续期前丢锁；StealAfterFirstRenewal
// 对应第一次续期成功后、写回前丢锁。旧实现按 key 而非持有者令牌操作锁，
// 会误续期/误释放后到者；且写回前没有第二次续期，会在丢锁后继续 set。
class LockStealingRateLimitProvider final
    : public theseed::foundation::IRedisProvider {
public:
    enum class StealStage { BeforeFirstRenewal, AfterFirstRenewal, AtWrite };

    explicit LockStealingRateLimitProvider(StealStage stage)
        : stage_(stage) {}

    bool set(const std::string& key,
             const std::string& value,
             RedisDuration ttl) override {
        ++setCalls_;
        if (stage_ == StealStage::AtWrite && !stolen_) {
            stealLock(key + ":lock", lockTtl_);
            stolen_ = true;
        }
        return inner_.set(key, value, ttl);
    }
    std::optional<std::string> get(const std::string& key) override {
        return inner_.get(key);
    }
    bool del(const std::string& key) override { return inner_.del(key); }
    bool exists(const std::string& key) override { return inner_.exists(key); }
    bool expire(const std::string& key, RedisDuration ttl) override {
        return inner_.expire(key, ttl);
    }
    bool zadd(const std::string& key, const std::string& member,
              double score) override {
        return inner_.zadd(key, member, score);
    }
    std::vector<std::pair<std::string, double>> zrange(
        const std::string& key, std::size_t start, std::size_t stop) override {
        return inner_.zrange(key, start, stop);
    }
    std::vector<std::pair<std::string, double>> zrevrange(
        const std::string& key, std::size_t start, std::size_t stop) override {
        return inner_.zrevrange(key, start, stop);
    }
    std::size_t zcard(const std::string& key) override {
        return inner_.zcard(key);
    }
    bool zrem(const std::string& key, const std::string& member) override {
        return inner_.zrem(key, member);
    }

    std::optional<LockToken> lock(const std::string& key,
                                  RedisDuration ttl) override {
        auto token = inner_.lock(key, ttl);
        if (token) lockTtl_ = ttl;
        return token;
    }

    bool renewLock(const std::string& key,
                   const LockToken& token,
                   RedisDuration ttl) override {
        ++renewCalls_;
        if (renewCalls_ == 1 && stage_ == StealStage::BeforeFirstRenewal) {
            stealLock(key, ttl);
            return inner_.renewLock(key, token, ttl);
        }
        if (renewCalls_ == 1 && stage_ == StealStage::AfterFirstRenewal) {
            if (!inner_.renewLock(key, token, ttl)) {
                return false;
            }
            stealLock(key, ttl);
            return true;
        }
        return inner_.renewLock(key, token, ttl);
    }

    bool unlock(const std::string& key, const LockToken& token) override {
        return inner_.unlock(key, token);
    }

    // AtWrite：在「最后一次续期成功之后、写回落地之前」的窗口里把锁偷走，
    // 再转发本次原子写回。旧令牌必须被 setIfLockOwner 的所有权校验拒绝；
    // 未修复实现（裸 set）会照写不误、覆盖新持有者的桶状态。
    bool setIfLockOwner(const std::string& lockKey,
                        const LockToken& token,
                        const std::string& key,
                        const std::string& value,
                        RedisDuration lockTtl) override {
        if (stage_ == StealStage::AtWrite && !stolen_) {
            stealLock(lockKey, lockTtl);
            stolen_ = true;
        }
        return inner_.setIfLockOwner(lockKey, token, key, value, lockTtl);
    }

    int setCalls() const { return setCalls_; }
    // 新持有者的锁在本次消耗结束后是否仍完好（未被旧持有者的写回破坏）。
    bool stolenLockStillHeld(RedisDuration ttl) {
        return stolenToken_.has_value() &&
               inner_.renewLock(stolenLockKey_, *stolenToken_, ttl);
    }

private:
    void stealLock(const std::string& key, RedisDuration ttl) {
        inner_.advanceClock(lockTtl_);
        inner_.del(key);
        stolenToken_ = inner_.lock(key, ttl);
        stolenLockKey_ = key;
    }

    StealStage stage_;
    InMemoryRedisProvider inner_;
    RedisDuration lockTtl_ = RedisDuration(1);
    std::optional<LockToken> stolenToken_;
    std::string stolenLockKey_;
    bool stolen_ = false;
    int renewCalls_ = 0;
    int setCalls_ = 0;
};

static void test_rate_limiter_no_concurrent_over_admit() {
    TEST("test_rate_limiter_no_concurrent_over_admit");
    auto redis = std::make_shared<RendezvousRateLimitProvider>();
    RateLimiter limiter(redis);
    RateLimiter::Config cfg;
    cfg.capacity = 1;
    cfg.refillInterval = std::chrono::seconds(60);
    // 两读者用 latch 同时释放、同时进入 tryConsume，把"一者先完成再进入
    // get"的概率性排除：未修复实现两读者必然交会读到同一份满桶快照而超发
    // （确定性失败），修复后串行化保证后到者读到已扣减桶（确定性通过）。
    std::latch start(2);

    std::atomic<int> allowed = 0;
    std::vector<std::thread> threads;
    threads.reserve(2);
    for (int i = 0; i < 2; ++i) {
        threads.emplace_back([&] {
            start.wait();
            if (limiter.tryConsume("k", cfg)) allowed.fetch_add(1);
        });
    }
    start.count_down(2);
    for (auto& thread : threads) thread.join();
    if (allowed.load() != 1) {
        FAIL("allowed=" + std::to_string(allowed.load()));
        return;
    }
    PASS();
}

// 深审回归：tryConsume 的分布式锁获取循环原先无重试上限/超时，锁持续
// 不可得（Redis 不可达 / 持锁者未释放且 TTL 未到）时调用线程永久忙等。
// 修复后带 lockTimeout 上限：超时即放弃本次消耗（返回 false），不阻塞。
static void test_rate_limiter_lock_timeout() {
    TEST("test_rate_limiter_lock_timeout");
    auto redis = std::make_shared<InMemoryRedisProvider>();
    RateLimiter limiter(redis);
    RateLimiter::Config cfg;
    cfg.capacity = 1;
    cfg.refillInterval = std::chrono::seconds(60);
    cfg.lockTimeout = std::chrono::milliseconds(1);

    // 预先占住锁：lock 成功后另一 tryConsume 只能等锁，超时后放弃。
    auto held = redis->lock("rate:k:lock", RedisDuration(60000));
    if (!held) {
        FAIL("pre-lock should succeed");
        return;
    }
    if (limiter.tryConsume("k", cfg)) {
        FAIL("lock timeout should return false, not block");
        return;
    }
    redis->unlock("rate:k:lock", *held);
    if (!limiter.tryConsume("k", cfg)) {
        FAIL("after unlock consume should succeed");
        return;
    }
    PASS();
}

// 深审回归：分布式锁 TTL 固定且无续期时，临界区执行超 TTL 会提前过期，
// 后到者再次获锁并发进入临界区使读改写竞态复发。修复后在临界区内续期，
// 持锁者执行时长超过原 TTL 仍独占（续期成功），并发者不能进入。
static void test_rate_limiter_lock_renewal() {
    TEST("test_rate_limiter_lock_renewal");
    auto redis = std::make_shared<InMemoryRedisProvider>();
    RateLimiter limiter(redis);
    RateLimiter::Config cfg;
    cfg.capacity = 1;
    cfg.refillInterval = std::chrono::seconds(60);
    cfg.lockTtl = std::chrono::milliseconds(5);

    // 直接验证 renewLock 续期语义：占锁后推进时钟越过原 TTL，续期前锁
    // 已过期（不可再续），续期后锁 TTL 被延长。
    auto held = redis->lock("rate:k:lock", cfg.lockTtl);
    if (!held) {
        FAIL("lock should succeed");
        return;
    }
    redis->advanceClock(RedisDuration(3));  // 仍在 TTL 内
    if (!redis->renewLock("rate:k:lock", *held, cfg.lockTtl)) {
        FAIL("renewLock within TTL should succeed");
        return;
    }
    redis->advanceClock(RedisDuration(3));  // 续期后 TTL 重新起算，仍有效
    if (redis->lock("rate:k:lock", cfg.lockTtl)) {
        FAIL("renewed lock should still be held");
        return;
    }
    redis->unlock("rate:k:lock", *held);
    PASS();
}

// 深审回归：锁过期被后到者重新持有后，原持有者的续期/解锁不得误认领新
// 持有者的锁。未修复实现仅按锁存在判断，会误续期并最终在 UnlockGuard 里
// 清掉新持有者的锁。本测试直接验证持有者令牌语义。
static void test_rate_limiter_lock_owner_token() {
    TEST("test_rate_limiter_lock_owner_token");
    auto redis = std::make_shared<InMemoryRedisProvider>();
    RateLimiter::Config cfg;
    cfg.lockTtl = std::chrono::milliseconds(5);

    auto owner = redis->lock("rate:k:lock", cfg.lockTtl);
    if (!owner) { FAIL("owner lock should succeed"); return; }
    redis->advanceClock(RedisDuration(6));  // 越过 TTL，原锁过期
    auto newcomer = redis->lock("rate:k:lock", cfg.lockTtl);
    if (!newcomer) { FAIL("newcomer should re-acquire after expiry"); return; }
    if (newcomer == owner) { FAIL("tokens must be distinct"); return; }

    // 原持有者的旧令牌：续期与解锁都必须失败，且不得清掉新持有者的锁。
    if (redis->renewLock("rate:k:lock", *owner, cfg.lockTtl)) {
        FAIL("stale token must not renew newcomer's lock");
        return;
    }
    if (redis->unlock("rate:k:lock", *owner)) {
        FAIL("stale token must not release newcomer's lock");
        return;
    }
    if (redis->lock("rate:k:lock", cfg.lockTtl)) {
        FAIL("newcomer's lock must survive stale-token operations");
        return;
    }
    if (!redis->renewLock("rate:k:lock", *newcomer, cfg.lockTtl)) {
        FAIL("newcomer should renew its own lock");
        return;
    }
    if (!redis->unlock("rate:k:lock", *newcomer)) {
        FAIL("newcomer should release its own lock");
        return;
    }
    PASS();
}

// 深审回归：临界区第一次续期前锁被后到者夺走，续期必须失败并放弃消耗，
// 不得继续写回（无锁保护下 set 会把并发者的读改写覆盖掉）。
static void test_rate_limiter_lock_stolen_before_first_renewal() {
    TEST("test_rate_limiter_lock_stolen_before_first_renewal");
    auto redis = std::make_shared<LockStealingRateLimitProvider>(
        LockStealingRateLimitProvider::StealStage::BeforeFirstRenewal);
    RateLimiter limiter(redis);
    RateLimiter::Config cfg;
    cfg.capacity = 1;
    cfg.refillInterval = std::chrono::seconds(60);

    if (limiter.tryConsume("k", cfg)) {
        FAIL("consume must be refused when lock is stolen before renewal");
        return;
    }
    if (redis->setCalls() != 0) {
        FAIL("no set may occur once ownership is lost");
        return;
    }
    PASS();
}

// 深审回归：第一次续期成功后、写回前锁被后到者夺走，写回前必须再次续期，
// 续期失败则放弃写回。未修复实现只在 get 后续期一次，会在丢锁后仍 set，
// 覆盖新持有者的写回（读改写互斥失效）。
static void test_rate_limiter_lock_stolen_after_first_renewal() {
    TEST("test_rate_limiter_lock_stolen_after_first_renewal");
    auto redis = std::make_shared<LockStealingRateLimitProvider>(
        LockStealingRateLimitProvider::StealStage::AfterFirstRenewal);
    RateLimiter limiter(redis);
    RateLimiter::Config cfg;
    cfg.capacity = 1;
    cfg.refillInterval = std::chrono::seconds(60);

    if (limiter.tryConsume("k", cfg)) {
        FAIL("consume must be refused when lock is stolen before write-back");
        return;
    }
    if (redis->setCalls() != 0) {
        FAIL("no set may occur once ownership is lost before write-back");
        return;
    }
    PASS();
}

// 深审回归：桶不足且锁在写回前被夺走时，写回前的续期必须失败并放弃消耗
// （不得把无锁保护的 refill 结果写回覆盖并发者的桶）。
static void test_rate_limiter_lock_stolen_insufficient_bucket() {
    TEST("test_rate_limiter_lock_stolen_insufficient_bucket");
    auto redis = std::make_shared<LockStealingRateLimitProvider>(
        LockStealingRateLimitProvider::StealStage::AfterFirstRenewal);
    RateLimiter limiter(redis);
    RateLimiter::Config cfg;
    cfg.capacity = 1;
    cfg.refillInterval = std::chrono::seconds(60);

    // 首次触碰桶满 1 但 cost=2 > 容量：桶不足臂。锁在第一次续期后被夺走，
    // 写回前的第二次续期必须失败并放弃，不得 set。
    if (limiter.tryConsume("k", cfg, 2)) {
        FAIL("consume must be refused on insufficient bucket when lock is stolen");
        return;
    }
    if (redis->setCalls() != 0) {
        FAIL("no set may occur once ownership is lost on insufficient bucket");
        return;
    }
    PASS();
}

// 深审回归：setIfLockOwner 原子写回语义——令牌仍属当前持有者时写入并
// 顺带续期锁；令牌失配或锁已消失时拒绝写入且不碰任何数据。真实 Redis
// 后端应以单条 Lua 脚本（token 校验 + SET + PEXPIRE）实现同一原子性。
static void test_redis_set_if_lock_owner_atomic() {
    TEST("test_redis_set_if_lock_owner_atomic");
    InMemoryRedisProvider r;
    const auto ttl = std::chrono::milliseconds(5);

    auto owner = r.lock("rate:k:lock", ttl);
    if (!owner) { FAIL("owner lock should succeed"); return; }
    r.advanceClock(RedisDuration(2));  // 距原过期还剩 3ms
    if (!r.setIfLockOwner("rate:k:lock", *owner, "rate:k", "blob", ttl)) {
        FAIL("owner write should succeed");
        return;
    }
    if (auto v = r.get("rate:k"); !v || *v != "blob") {
        FAIL("atomic write should store the value");
        return;
    }
    // 写回把锁续到 now+5（原口径 now+3 已过）：推进 4ms 后锁仍活着证明
    // 续期生效；未续期的锁在此刻早已过期。
    r.advanceClock(RedisDuration(4));
    if (!r.renewLock("rate:k:lock", *owner, ttl)) {
        FAIL("atomic write should extend the lock TTL");
        return;
    }

    r.advanceClock(RedisDuration(6));  // 全部锁过期，后到者重新持有
    auto newcomer = r.lock("rate:k:lock", ttl);
    if (!newcomer) { FAIL("newcomer should re-acquire after expiry"); return; }
    if (!r.setIfLockOwner("rate:k:lock", *newcomer, "rate:k", "fresh", ttl)) {
        FAIL("newcomer write should succeed");
        return;
    }
    // 旧令牌的原子写回必须被拒绝，且不得覆盖新持有者的数据。
    if (r.setIfLockOwner("rate:k:lock", *owner, "rate:k", "stale", ttl)) {
        FAIL("stale token must not write");
        return;
    }
    if (auto v = r.get("rate:k"); !v || *v != "fresh") {
        FAIL("rejected write must not touch stored state");
        return;
    }
    // 锁完全消失（已过期回收/从未持有）时同样拒绝。
    if (r.setIfLockOwner("rate:none:lock", "ghost", "rate:none", "x", ttl)) {
        FAIL("missing lock must refuse the write");
        return;
    }
    PASS();
}

// 深审回归：最后一次续期成功后、写回执行前锁过期被后到者夺走——
// 「续期 + 裸 set」两步写回在该窗口会把新持有者的桶状态覆盖掉（超额放行
// 复发）。修复后写回用 setIfLockOwner 原子完成，写回本身即所有权校验点：
// 旧令牌的写回被拒绝，本次消耗放弃，新持有者的锁与数据完好。
static void test_rate_limiter_lock_stolen_between_renewal_and_write() {
    TEST("test_rate_limiter_lock_stolen_between_renewal_and_write");
    auto redis = std::make_shared<LockStealingRateLimitProvider>(
        LockStealingRateLimitProvider::StealStage::AtWrite);
    RateLimiter limiter(redis);
    RateLimiter::Config cfg;
    cfg.capacity = 1;
    cfg.refillInterval = std::chrono::seconds(60);

    if (limiter.tryConsume("k", cfg)) {
        FAIL("consume must be refused when lock is stolen before write-back");
        return;
    }
    if (redis->setCalls() != 0) {
        FAIL("no plain set may occur once ownership is lost");
        return;
    }
    if (!redis->stolenLockStillHeld(std::chrono::milliseconds(5))) {
        FAIL("new owner's lock must survive the rejected write");
        return;
    }
    PASS();
}

static void test_redis_zrange_score_tie_break_by_member() {
    TEST("test_redis_zrange_score_tie_break_by_member");
    InMemoryRedisProvider r;
    r.zadd("tie", "banana", 1.0);
    r.zadd("tie", "apple", 1.0);
    r.zadd("tie", "cherry", 0.5);
    auto range = r.zrange("tie", 0, static_cast<std::size_t>(-1));
    bool ok = range.size() == 3;
    ok = ok && range[0].first == "cherry";   // 低分在前
    ok = ok && range[1].first == "apple";    // 同分按成员名字典序
    ok = ok && range[2].first == "banana";
    if (ok) PASS(); else FAIL("tie-break order wrong");
}

static void test_provider_scoped_construction() {
    TEST("test_provider_scoped_construction");
    {
        InMemoryRedisProvider scoped;
        scoped.set("k", "v");
    }   // 出作用域：构造/析构完整走一遍
    PASS();
}

// --- Redis provider 防御臂 / 缺失键 ---

static void test_redis_exists_across_kinds() {
    TEST("test_redis_exists_string_set_lock");
    InMemoryRedisProvider r;
    // string 键：第一短路臂
    r.set("s", "1");
    if (!r.exists("s")) { FAIL("string key should exist"); return; }
    // zset 键：第一臂 false、第二臂 true
    r.zadd("z", "m", 1.0);
    if (!r.exists("z")) { FAIL("zset key should exist"); return; }
    // lock 键：前两臂 false、第三臂 true
    auto lockTok = r.lock("l", RedisDuration(5000));
    if (!lockTok) { FAIL("lock should succeed"); return; }
    if (!r.exists("l")) { FAIL("locked key should exist"); return; }
    r.unlock("l", *lockTok);
    // 全无：三臂全 false
    if (r.exists("none")) { FAIL("missing key should not exist"); return; }
    PASS();
}

static void test_redis_missing_key_ops() {
    TEST("test_redis_missing_key_expire_zrange_zcard");
    InMemoryRedisProvider r;
    if (r.expire("nope", RedisDuration(1000))) { FAIL("expire missing should be false"); return; }
    if (!r.zrange("nope", 0, 10).empty()) { FAIL("zrange missing should be empty"); return; }
    if (r.zcard("nope") != 0) { FAIL("zcard missing should be 0"); return; }
    r.zadd("tie", "a", 1.0);
    r.zadd("tie", "b", 2.0);
    if (!r.zrange("tie", 5, 9).empty()) { FAIL("zrange start out of range should be empty"); return; }
    if (!r.zrevrange("tie", 5, 9).empty()) { FAIL("zrevrange start out of range should be empty"); return; }
    PASS();
}

// --- SessionStore 防御臂 ---

static void test_session_null_redis_throws() {
    TEST("test_session_null_redis_throws");
    bool threw = false;
    try {
        SessionStore store(nullptr);
        static_cast<void>(store);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    if (!threw) { FAIL("null redis should throw"); return; }
    PASS();
}

static void test_session_empty_token_rejected() {
    TEST("test_session_empty_token_rejected");
    auto redis = std::make_shared<InMemoryRedisProvider>();
    SessionStore store(redis);
    StoredSession s;
    s.accountId = "a";
    if (store.save("", s, RedisDuration(1000))) { FAIL("save empty token"); return; }
    if (store.load("").has_value()) { FAIL("load empty token"); return; }
    if (store.refresh("", RedisDuration(1000))) { FAIL("refresh empty token"); return; }
    if (store.revoke("")) { FAIL("revoke empty token"); return; }
    PASS();
}

static void test_session_decode_truncated_fields() {
    TEST("test_session_decode_truncated_fields");
    // 空 blob：第一段 getline 失败
    if (SessionStore::decode("").has_value()) { FAIL("empty blob should be rejected"); return; }
    // 前两段完整、第三段缺失（userId 段为空 → stoll 抛异常被吞）
    if (SessionStore::decode("acc\x1f" "realm\x1f").has_value()) {
        FAIL("missing userId should be rejected");
        return;
    }
    PASS();
}

// --- RateLimiter 防御臂 ---

static void test_rate_limiter_null_redis_throws() {
    TEST("test_rate_limiter_null_redis_throws");
    bool threw = false;
    try {
        RateLimiter limiter(nullptr);
        static_cast<void>(limiter);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    if (!threw) { FAIL("null redis should throw"); return; }
    PASS();
}

static void test_rate_limiter_invalid_inputs_rejected() {
    TEST("test_rate_limiter_invalid_inputs_rejected");
    auto redis = std::make_shared<InMemoryRedisProvider>();
    RateLimiter limiter(redis);
    RateLimiter::Config cfg;
    cfg.capacity = 3;
    cfg.refillInterval = std::chrono::seconds(60);
    // 空 key
    if (limiter.tryConsume("", cfg)) { FAIL("empty key should be rejected"); return; }
    // cost 非正
    if (limiter.tryConsume("k", cfg, 0)) { FAIL("zero cost should be rejected"); return; }
    if (limiter.tryConsume("k", cfg, -1)) { FAIL("negative cost should be rejected"); return; }
    // capacity 非正
    RateLimiter::Config bad = cfg;
    bad.capacity = 0;
    if (limiter.tryConsume("k", bad)) { FAIL("zero capacity should be rejected"); return; }
    // reset 空 key
    if (limiter.reset("")) { FAIL("reset empty key should be false"); return; }
    PASS();
}

static void test_rate_limiter_corrupt_state_resets_bucket() {
    TEST("test_rate_limiter_corrupt_state_resets_bucket");
    auto redis = std::make_shared<InMemoryRedisProvider>();
    RateLimiter limiter(redis);
    RateLimiter::Config cfg;
    cfg.capacity = 2;
    cfg.refillInterval = std::chrono::seconds(60);
    // 预埋损坏 blob：decode 失败 → 视作首次接触，桶满
    redis->set("rate:corrupt", "not-a-bucket");
    if (!limiter.tryConsume("corrupt", cfg)) { FAIL("corrupt blob should start full"); return; }
    if (!limiter.tryConsume("corrupt", cfg)) { FAIL("second consume should succeed"); return; }
    if (limiter.tryConsume("corrupt", cfg)) { FAIL("bucket should now be empty"); return; }
    PASS();
}

int main() {
    test_redis_set_get();
    test_redis_ttl_expires();
    test_redis_del();
    test_redis_zadd_zrange();
    test_redis_zrem();
    test_redis_exists_across_kinds();
    test_redis_missing_key_ops();
    test_redis_lock_acquire_release();
    test_redis_lock_expires();

    test_session_save_load();
    test_session_ttl_expires();
    test_session_revoke();
    test_session_refresh_extends_ttl();
    test_session_decode_rejects_garbage();
    test_session_decode_rejects_bad_user_id();
    test_session_list_enumerates_and_revoked_gone();
    test_session_list_prunes_expired();
    test_session_list_prunes_corrupt_blob();
    test_session_null_redis_throws();
    test_session_empty_token_rejected();
    test_session_decode_truncated_fields();

    test_rate_limiter_allows_within_capacity();
    test_rate_limiter_null_redis_throws();
    test_rate_limiter_invalid_inputs_rejected();
    test_rate_limiter_corrupt_state_resets_bucket();
    test_rate_limiter_refills_over_time();
    test_rate_limiter_sub_ms_refill_clamped();
    test_rate_limiter_no_concurrent_over_admit();
    test_rate_limiter_lock_timeout();
    test_rate_limiter_lock_renewal();
    test_rate_limiter_lock_owner_token();
    test_rate_limiter_lock_stolen_before_first_renewal();
    test_rate_limiter_lock_stolen_after_first_renewal();
    test_rate_limiter_lock_stolen_insufficient_bucket();
    test_rate_limiter_lock_stolen_between_renewal_and_write();
    test_redis_set_if_lock_owner_atomic();
    test_rate_limiter_reset();
    test_rate_limiter_independent_keys();
    test_rate_limiter_available_tokens();
    test_redis_zrange_score_tie_break_by_member();
    test_provider_scoped_construction();

    std::cout << "  passed=" << testsPassed << " failed=" << testsFailed << "\n";
    return testsFailed == 0 ? 0 : 1;
}
