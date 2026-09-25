#include "theseed/foundation/RedisProvider.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>

namespace theseed::foundation {

InMemoryRedisProvider::InMemoryRedisProvider()
    : now_(std::chrono::steady_clock::now()) {}

InMemoryRedisProvider::~InMemoryRedisProvider() = default;  // LCOV_EXCL_LINE trivial 析构的 out-of-line 定义无机器码，gcc 不产生计数条目

void InMemoryRedisProvider::advanceClock(RedisDuration delta) {
    const std::lock_guard lock(mutex_);
    now_ += delta;
    reapExpired();
}

void InMemoryRedisProvider::reapExpired() {
    for (auto it = strings_.begin(); it != strings_.end();) {
        if (it->second.expiresAt.has_value() && now_ >= *it->second.expiresAt) {
            it = strings_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = locks_.begin(); it != locks_.end();) {
        if (now_ >= it->second.expiresAt) {
            it = locks_.erase(it);
        } else {
            ++it;
        }
    }
}

bool InMemoryRedisProvider::set(const std::string& key,
                                const std::string& value,
                                RedisDuration ttl) {
    const std::lock_guard lock(mutex_);
    StringEntry e;
    e.value = value;
    if (ttl > RedisDuration::zero()) {
        e.expiresAt = now_ + ttl;
    }
    strings_[key] = std::move(e);
    return true;
}

std::optional<std::string> InMemoryRedisProvider::get(const std::string& key) {
    const std::lock_guard lock(mutex_);
    reapExpired();
    auto it = strings_.find(key);
    if (it == strings_.end()) return std::nullopt;
    return it->second.value;
}

bool InMemoryRedisProvider::del(const std::string& key) {
    const std::lock_guard lock(mutex_);
    bool changed = false;
    changed |= strings_.erase(key) > 0;
    changed |= sets_.erase(key) > 0;
    changed |= locks_.erase(key) > 0;
    return changed;
}

bool InMemoryRedisProvider::exists(const std::string& key) {
    const std::lock_guard lock(mutex_);
    reapExpired();
    return strings_.count(key) > 0 || sets_.count(key) > 0 || locks_.count(key) > 0;
}

bool InMemoryRedisProvider::expire(const std::string& key, RedisDuration ttl) {
    const std::lock_guard lock(mutex_);
    auto it = strings_.find(key);
    if (it == strings_.end()) return false;
    it->second.expiresAt = now_ + ttl;
    return true;
}

bool InMemoryRedisProvider::zadd(const std::string& key,
                                  const std::string& member,
                                  double score) {
    const std::lock_guard lock(mutex_);
    auto& set = sets_[key];
    set.members[member] = score;
    return true;
}

std::vector<std::pair<std::string, double>> InMemoryRedisProvider::zrange(
    const std::string& key,
    std::size_t start,
    std::size_t stop) {
    std::vector<std::pair<std::string, double>> result;
    const std::lock_guard lock(mutex_);
    auto it = sets_.find(key);
    if (it == sets_.end()) return result;

    // Sort members ascending by score, tie-break by member name.
    std::vector<std::pair<std::string, double>> sorted(
        it->second.members.begin(), it->second.members.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) {
                  if (a.second != b.second) return a.second < b.second;
                  return a.first < b.first;
              });

    if (start >= sorted.size()) return result;
    std::size_t endIdx = std::min(stop, sorted.size() - 1);
    for (std::size_t i = start; i <= endIdx; ++i) {
        result.push_back(sorted[i]);
    }
    return result;
}

std::vector<std::pair<std::string, double>> InMemoryRedisProvider::zrevrange(
    const std::string& key,
    std::size_t start,
    std::size_t stop) {
    auto forward = zrange(key, 0, static_cast<std::size_t>(-1));
    std::reverse(forward.begin(), forward.end());
    if (start >= forward.size()) return {};
    std::size_t endIdx = std::min(stop, forward.size() - 1);
    std::vector<std::pair<std::string, double>> out;
    for (std::size_t i = start; i <= endIdx; ++i) {
        out.push_back(forward[i]);
    }
    return out;
}

std::size_t InMemoryRedisProvider::zcard(const std::string& key) {
    const std::lock_guard lock(mutex_);
    auto it = sets_.find(key);
    if (it == sets_.end()) return 0;
    return it->second.members.size();
}

bool InMemoryRedisProvider::lock(const std::string& key, RedisDuration ttl) {
    const std::lock_guard lock(mutex_);
    reapExpired();
    auto it = locks_.find(key);
    if (it != locks_.end() && now_ < it->second.expiresAt) {  // LCOV_EXCL_BR_LINE 上方 reapExpired 已清除全部过期锁，未过期假臂不可达
        return false;
    }
    LockEntry entry;
    entry.expiresAt = now_ + ttl;
    locks_[key] = entry;
    return true;
}

void InMemoryRedisProvider::unlock(const std::string& key) {
    const std::lock_guard lock(mutex_);
    locks_.erase(key);
}

}  // namespace theseed::foundation