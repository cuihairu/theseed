#include "theseed/foundation/RateLimiter.h"
#include "theseed/foundation/RedisProvider.h"
#include "theseed/foundation/SessionStore.h"

#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

using theseed::foundation::InMemoryRedisProvider;
using theseed::foundation::RateLimiter;
using theseed::foundation::RedisDuration;
using theseed::foundation::SessionStore;
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
    if (!r.lock("resource", RedisDuration(5000))) { FAIL("first lock should succeed"); return; }
    if (r.lock("resource", RedisDuration(5000))) { FAIL("second lock should fail"); return; }
    r.unlock("resource");
    if (!r.lock("resource", RedisDuration(5000))) { FAIL("lock after unlock should succeed"); return; }
    PASS();
}

static void test_redis_lock_expires() {
    TEST("test_redis_lock_expires");
    InMemoryRedisProvider r;
    r.lock("resource", RedisDuration(1000));
    r.advanceClock(RedisDuration(1001));
    if (!r.lock("resource", RedisDuration(1000))) { FAIL("expired lock should be re-acquirable"); return; }
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
    // We cannot mock steady_clock directly; but tryConsume reads wall clock,
    // so we sleep ~120ms to allow at least one token to refill.
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    if (!limiter.tryConsume("k", cfg)) { FAIL("should refill after sleep"); return; }
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

int main() {
    test_redis_set_get();
    test_redis_ttl_expires();
    test_redis_del();
    test_redis_zadd_zrange();
    test_redis_lock_acquire_release();
    test_redis_lock_expires();

    test_session_save_load();
    test_session_ttl_expires();
    test_session_revoke();
    test_session_refresh_extends_ttl();
    test_session_decode_rejects_garbage();

    test_rate_limiter_allows_within_capacity();
    test_rate_limiter_refills_over_time();
    test_rate_limiter_reset();
    test_rate_limiter_independent_keys();

    std::cout << "  passed=" << testsPassed << " failed=" << testsFailed << "\n";
    return testsFailed == 0 ? 0 : 1;
}
