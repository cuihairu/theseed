#pragma once

#include "theseed/core/EntityData.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace theseed::core {

class IEntityStore {
public:
    virtual ~IEntityStore() = default;

    virtual bool load(EntityId id, const std::string& entityType, EntityData& out) = 0;
    virtual bool save(EntityId id, const EntityData& data) = 0;
    virtual bool remove(EntityId id) = 0;
    virtual EntityId allocId() = 0;
};

class InMemoryEntityStore final : public IEntityStore {
public:
    InMemoryEntityStore() = default;

    bool load(EntityId id, const std::string& entityType, EntityData& out) override;
    bool save(EntityId id, const EntityData& data) override;
    bool remove(EntityId id) override;
    EntityId allocId() override;

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

}  // namespace theseed::core
