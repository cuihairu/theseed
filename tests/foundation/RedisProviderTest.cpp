#include "theseed/foundation/RateLimiter.h"
#include "theseed/foundation/RedisProvider.h"
#include "theseed/foundation/SessionStore.h"

#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

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

static void test_redis_zrem() {
    TEST("test_redis_zrem");
    InMemoryRedisProvider r;
    r.zadd("idx", "a", 1.0);
    r.zadd("idx", "b", 2.0);
    if (!r.zrem("idx", "a")) { FAIL("zrem of present member should be true"); return; }
    if (r.zrem("idx", "a")) { FAIL("zrem of removed member should be false"); return; }
    if (r.zrem("no-such-set", "a")) { FAIL("zrem on missing set should be false"); return; }
    if (r.zcard("idx") != 1) { FAIL("zcard should drop to 1 after zrem"); return; }
    auto left = r.zrange("idx", 0, -1);
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

static void test_redis_zrange_score_tie_break_by_member() {
    TEST("test_redis_zrange_score_tie_break_by_member");
    InMemoryRedisProvider r;
    r.zadd("tie", "banana", 1.0);
    r.zadd("tie", "apple", 1.0);
    r.zadd("tie", "cherry", 0.5);
    auto range = r.zrange("tie", 0, -1);
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
    if (!r.lock("l", RedisDuration(5000))) { FAIL("lock should succeed"); return; }
    if (!r.exists("l")) { FAIL("locked key should exist"); return; }
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
    test_rate_limiter_reset();
    test_rate_limiter_independent_keys();
    test_rate_limiter_available_tokens();
    test_redis_zrange_score_tie_break_by_member();
    test_provider_scoped_construction();

    std::cout << "  passed=" << testsPassed << " failed=" << testsFailed << "\n";
    return testsFailed == 0 ? 0 : 1;
}
