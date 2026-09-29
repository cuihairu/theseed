// SessionStore 直测：token 会话存储在 IRedisProvider 之上的双写契约——
// 主键 set 成功后索引 zadd 才有意义；任一写失败都必须如实上抛（save 返回
// false），set 失败时短路不碰索引（不留"会话在而索引缺失"之外的第三态：
// 索引在而会话缺失）。既有 LoginAppTest 只经 InMemoryRedisProvider 覆盖
// 全成功路径，这里用可注入失败的 fake 逐臂锚定。
#include "theseed/foundation/SessionStore.h"

#include <chrono>
#include <cstddef>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using theseed::foundation::IRedisProvider;
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

namespace {

// 最小 fake：内存 map 语义 + set/zadd 失败注入开关。其余原语桩实现
//（SessionStore 不触达）。
class FakeRedisProvider final : public IRedisProvider {
public:
    bool set(const std::string& key, const std::string& value,
             RedisDuration) override {
        if (failSet) return false;
        kv_[key] = value;
        return true;
    }
    std::optional<std::string> get(const std::string& key) override {
        const auto it = kv_.find(key);
        return it == kv_.end() ? std::nullopt : std::optional{it->second};
    }
    bool del(const std::string& key) override { return kv_.erase(key) > 0; }
    bool exists(const std::string& key) override {
        return kv_.find(key) != kv_.end();
    }
    bool expire(const std::string&, RedisDuration) override { return true; }

    bool zadd(const std::string& key, const std::string& member,
              double score) override {
        ++zaddCalls;
        if (failZadd) return false;
        zsets_[key].emplace(member, score);
        return true;
    }
    std::vector<std::pair<std::string, double>> zrange(const std::string& key,
                                                       std::size_t,
                                                       std::size_t) override {
        std::vector<std::pair<std::string, double>> out;
        for (const auto& [member, score] : zsets_[key]) {
            out.emplace_back(member, score);
        }
        return out;
    }
    std::vector<std::pair<std::string, double>> zrevrange(const std::string&,
                                                          std::size_t,
                                                          std::size_t) override {
        return {};
    }
    std::size_t zcard(const std::string& key) override {
        return zsets_[key].size();
    }
    bool zrem(const std::string& key, const std::string& member) override {
        return zsets_[key].erase(member) > 0;
    }
    bool lock(const std::string&, RedisDuration) override { return true; }
    void unlock(const std::string&) override {}

    bool failSet = false;
    bool failZadd = false;
    int zaddCalls = 0;

private:
    std::map<std::string, std::string> kv_;
    std::map<std::string, std::map<std::string, double>> zsets_;
};

StoredSession sampleSession() {
    StoredSession session;
    session.accountId = "carl";
    session.realmId = "default";
    session.userId = 42;
    session.metadata = "{\"client\":\"unit\"}";
    return session;
}

}  // namespace

static void test_save_round_trip_and_index_visibility() {
    TEST("test_save_round_trip_and_index_visibility");
    auto redis = std::make_shared<FakeRedisProvider>();
    SessionStore store(redis);
    if (!store.save("tok-1", sampleSession(),
                    std::chrono::milliseconds{5000})) {
        FAIL("save must succeed when both writes land");
        return;
    }
    const auto loaded = store.load("tok-1");
    if (!loaded) { FAIL("load must read back the saved session"); return; }
    if (loaded->accountId != "carl" || loaded->realmId != "default" ||
        loaded->userId != 42 || loaded->metadata != "{\"client\":\"unit\"}") {
        FAIL("round-trip must preserve all fields");
        return;
    }
    const auto sessions = store.listSessions();
    if (sessions.size() != 1 || sessions[0].token != "tok-1" ||
        sessions[0].accountId != "carl") {
        FAIL("enumeration index must expose the saved session");
        return;
    }
    PASS();
}

static void test_save_short_circuits_when_set_fails() {
    TEST("test_save_short_circuits_when_set_fails");
    auto redis = std::make_shared<FakeRedisProvider>();
    redis->failSet = true;  // 主键写失败：会话落不了盘
    SessionStore store(redis);
    if (store.save("tok-2", sampleSession(),
                   std::chrono::milliseconds{5000})) {
        FAIL("save must report failure when the session key write fails");
        return;
    }
    if (redis->zaddCalls != 0) {
        FAIL("set failure must short-circuit before the index write");
        return;
    }
    // 会话键确实不存在（fake 里就没有）
    if (store.load("tok-2")) { FAIL("failed save must not be loadable"); return; }
    PASS();
}

static void test_save_reports_index_write_failure() {
    TEST("test_save_reports_index_write_failure");
    auto redis = std::make_shared<FakeRedisProvider>();
    redis->failZadd = true;  // 主键写成功、索引写失败：双写分歧如实上抛
    SessionStore store(redis);
    if (store.save("tok-3", sampleSession(),
                   std::chrono::milliseconds{5000})) {
        FAIL("save must report failure when the index write fails");
        return;
    }
    if (redis->zaddCalls != 1) {
        FAIL("index write must have been attempted after a successful set");
        return;
    }
    // 调用方拿到 false 后重试是预期路径：会话键本身在（上抛不回滚），
    // 枚举面看不到它（索引缺失）——这是注释里声明的不一致形态。
    if (!store.load("tok-3")) {
        FAIL("session key must survive a failed index write for retry");
        return;
    }
    if (!store.listSessions().empty()) {
        FAIL("a session without an index entry must stay invisible");
        return;
    }
    PASS();
}

static void test_empty_token_rejected() {
    TEST("test_empty_token_rejected");
    auto redis = std::make_shared<FakeRedisProvider>();
    SessionStore store(redis);
    if (store.save("", sampleSession(), std::chrono::milliseconds{5000})) {
        FAIL("empty token must be rejected");
        return;
    }
    if (store.load("")) { FAIL("empty token must not load"); return; }
    PASS();
}

int main() {
    std::cout << "SessionStoreTest:\n";
    test_save_round_trip_and_index_visibility();
    test_save_short_circuits_when_set_fails();
    test_save_reports_index_write_failure();
    test_empty_token_rejected();
    std::cout << "\n" << testsPassed << " passed, " << testsFailed
              << " failed\n";
    return testsFailed == 0 ? 0 : 1;
}
