#include "theseed/foundation/SessionStore.h"

#include <chrono>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace theseed::foundation {

namespace {

constexpr std::string_view kFieldSeparator = "\x1f";  // unit separator

// 会话键前缀与枚举索引键：索引是独立键空间的 zset（member=token），与
// "session:<token>" 字符串键不撞名——token 取 "index" 也落在单数前缀下。
constexpr std::string_view kSessionKeyPrefix = "session:";
constexpr std::string_view kIndexKey = "sessions:index";

// 索引 score = 保存时刻（epoch 毫秒）：跨进程语义（保存先后可辨），
// 枚举顺序仍由提供者定（内存实现按成员字典序），不承诺时序。
double indexScore() {
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch());
    return static_cast<double>(millis.count());
}

}  // namespace

SessionStore::SessionStore(std::shared_ptr<IRedisProvider> redis)
    : redis_(std::move(redis)) {
    if (!redis_) throw std::invalid_argument("SessionStore requires a redis provider");
}

bool SessionStore::save(const std::string& token,
                        const StoredSession& session,
                        RedisDuration ttl) {
    if (token.empty()) return false;
    const bool stored = redis_->set(std::string(kSessionKeyPrefix) + token,
                                    encode(session), ttl);
    // 会话在而索引缺失 = 该会话对枚举面不可见的不一致——索引写失败如实
    // 上抛（短路 false），宁可让调用方重试，不静默吞掉双写分歧。
    return stored && redis_->zadd(std::string(kIndexKey), token, indexScore());
}

std::optional<StoredSession> SessionStore::load(const std::string& token) {
    if (token.empty()) return std::nullopt;
    auto blob = redis_->get(std::string(kSessionKeyPrefix) + token);
    if (!blob) return std::nullopt;
    return decode(*blob);
}

bool SessionStore::refresh(const std::string& token, RedisDuration ttl) {
    if (token.empty()) return false;
    return redis_->expire(std::string(kSessionKeyPrefix) + token, ttl);
}

bool SessionStore::revoke(const std::string& token) {
    if (token.empty()) return false;
    const bool removed = redis_->del(std::string(kSessionKeyPrefix) + token);
    // 只在命中时摘索引：未命中（会话已过期/不存在）时索引成员要么已由
    // 枚举惰性清账，要么本就不该存在——跨进程竞争下不误删他人视图。
    if (removed) {
        redis_->zrem(std::string(kIndexKey), token);
    }
    return removed;
}

std::vector<SessionView> SessionStore::listSessions() {
    std::vector<SessionView> views;
    // 索引与会话键是两次写、无法原子：枚举顺路清账（过期/损坏条目在这里
    // 摘除），保证吐出的每一行都对应一条活着且可解码的会话。
    for (const auto& member : redis_->zrange(std::string(kIndexKey), 0, -1)) {
        const auto& token = member.first;
        auto blob = redis_->get(std::string(kSessionKeyPrefix) + token);
        if (!blob) {
            redis_->zrem(std::string(kIndexKey), token);  // TTL 过期残留
            continue;
        }
        auto session = decode(*blob);
        if (!session) {
            redis_->zrem(std::string(kIndexKey), token);  // blob 损坏，索引自愈
            continue;
        }
        SessionView view;
        view.token = token;
        view.accountId = session->accountId;
        view.realmId = session->realmId;
        view.userId = session->userId;
        views.push_back(std::move(view));
    }
    return views;
}

std::string SessionStore::encode(const StoredSession& session) {
    std::ostringstream out;
    out << session.accountId << kFieldSeparator
        << session.realmId << kFieldSeparator
        << session.userId << kFieldSeparator
        << session.metadata;
    return out.str();
}

std::optional<StoredSession> SessionStore::decode(const std::string& blob) {
    std::istringstream in(blob);
    std::string accountId;
    std::string realmId;
    std::string userIdStr;
    std::string metadata;

    if (!std::getline(in, accountId, '\x1f')) return std::nullopt;
    if (!std::getline(in, realmId, '\x1f')) return std::nullopt;
    if (!std::getline(in, userIdStr, '\x1f')) return std::nullopt;
    if (!std::getline(in, metadata, '\n')) {
        metadata.clear();
    }

    StoredSession s;
    s.accountId = std::move(accountId);
    s.realmId = std::move(realmId);
    try {
        s.userId = std::stoll(userIdStr);
    } catch (...) {
        return std::nullopt;
    }
    s.metadata = std::move(metadata);
    return s;
}

}  // namespace theseed::foundation
