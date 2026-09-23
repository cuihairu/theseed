#pragma once

#include "theseed/core/EntityData.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace theseed::core {

class IEntityStore {
public:
    virtual ~IEntityStore() = default;  // LCOV_EXCL_LINE C++ ABI：trivial 虚析构是空体，gcc 不为其生成计数指令，D0/D1/D2 三符号变体恒 0（结构不可测）

    virtual bool load(EntityId id, const std::string& entityType, EntityData& out) = 0;
    virtual bool save(EntityId id, const EntityData& data) = 0;
    virtual bool remove(EntityId id) = 0;
    virtual EntityId allocId() = 0;
    virtual std::vector<EntityId> listIdsByType(const std::string& entityType) = 0;
    virtual std::vector<std::string> listEntityTypes() = 0;
};

class InMemoryEntityStore final : public IEntityStore {
public:
    InMemoryEntityStore() = default;

    bool load(EntityId id, const std::string& entityType, EntityData& out) override;
    bool save(EntityId id, const EntityData& data) override;
    bool remove(EntityId id) override;
    EntityId allocId() override;
    std::vector<EntityId> listIdsByType(const std::string& entityType) override;
    std::vector<std::string> listEntityTypes() override;

    std::size_t count() const;
    bool exists(EntityId id) const;

private:
    struct StoreEntry {
        std::string entityType;
        MemoryStream stream;
    };

    std::unordered_map<EntityId, StoreEntry> entries_;
    EntityId nextId_ = 1;
};

inline bool InMemoryEntityStore::load(EntityId id, const std::string& entityType, EntityData& out) {
    auto it = entries_.find(id);
    if (it == entries_.end()) return false;
    if (it->second.entityType != entityType) return false;

    it->second.stream.resetRead();
    return decodeEntityData(it->second.stream, out);
}

inline bool InMemoryEntityStore::save(EntityId id, const EntityData& data) {
    StoreEntry entry;
    entry.entityType = data.entityType;
    encodeEntityData(entry.stream, data);
    entries_[id] = std::move(entry);
    return true;
}

inline bool InMemoryEntityStore::remove(EntityId id) {
    return entries_.erase(id) > 0;
}

inline EntityId InMemoryEntityStore::allocId() {
    return nextId_++;
}

inline std::size_t InMemoryEntityStore::count() const {
    return entries_.size();
}

inline bool InMemoryEntityStore::exists(EntityId id) const {
    return entries_.find(id) != entries_.end();
}

inline std::vector<EntityId> InMemoryEntityStore::listIdsByType(const std::string& entityType) {
    // entries_ 是 unordered_map，遍历顺序不定；下游（DBApp 列表、ops 面板、
    // 分页对账）依赖稳定输出，统一按 id 升序。
    std::vector<EntityId> ids;
    for (const auto& [id, entry] : entries_) {
        if (entry.entityType == entityType) {
            ids.push_back(id);
        }
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

inline std::vector<std::string> InMemoryEntityStore::listEntityTypes() {
    std::unordered_set<std::string> types;
    for (const auto& [id, entry] : entries_) {
        types.insert(entry.entityType);
    }
    std::vector<std::string> result(types.begin(), types.end());
    std::sort(result.begin(), result.end());
    return result;
}

}  // namespace theseed::core
