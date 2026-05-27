#pragma once

#include "theseed/core/IEntityStore.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace theseed::core {

class FileEntityStore final : public IEntityStore {
public:
    explicit FileEntityStore(std::filesystem::path rootDir);

    bool load(EntityId id, const std::string& entityType, EntityData& out) override;
    bool save(EntityId id, const EntityData& data) override;
    bool remove(EntityId id) override;
    EntityId allocId() override;

    bool exists(EntityId id, const std::string& entityType) const;

private:
    std::filesystem::path entityPath(EntityId id, const std::string& entityType) const;
    std::filesystem::path metaPath() const;
    bool ensureDir(const std::string& entityType) const;

    std::filesystem::path rootDir_;
};

}  // namespace theseed::core
