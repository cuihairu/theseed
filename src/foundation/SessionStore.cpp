#include "theseed/foundation/SessionStore.h"

#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace theseed::foundation {

namespace {

constexpr std::string_view kFieldSeparator = "\x1f";  // unit separator
constexpr std::string_view kLineSeparator = "\n";

}  // namespace

SessionStore::SessionStore(std::shared_ptr<IRedisProvider> redis)
    : redis_(std::move(redis)) {
    if (!redis_) throw std::invalid_argument("SessionStore requires a redis provider");
}

bool SessionStore::save(const std::string& token,
                        const StoredSession& session,
                        RedisDuration ttl) {
    if (token.empty()) return false;
    return redis_->set("session:" + token, encode(session), ttl);
}

std::optional<StoredSession> SessionStore::load(const std::string& token) {
    if (token.empty()) return std::nullopt;
    auto blob = redis_->get("session:" + token);
    if (!blob) return std::nullopt;
    return decode(*blob);
}

bool SessionStore::refresh(const std::string& token, RedisDuration ttl) {
    if (token.empty()) return false;
    return redis_->expire("session:" + token, ttl);
}

bool SessionStore::revoke(const std::string& token) {
    if (token.empty()) return false;
    return redis_->del("session:" + token);
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
