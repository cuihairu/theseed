#include "theseed/core/FileEntityStore.h"
#include "theseed/core/EntityData.h"

#include <cstring>
#include <fstream>

namespace theseed::core {

FileEntityStore::FileEntityStore(std::filesystem::path rootDir)
    : rootDir_(std::move(rootDir)) {
    std::filesystem::create_directories(rootDir_);
}

std::filesystem::path FileEntityStore::entityPath(EntityId id,
                                                   const std::string& entityType) const {
    return rootDir_ / entityType / (std::to_string(id) + ".dat");
}

std::filesystem::path FileEntityStore::metaPath() const {
    return rootDir_ / "_next_id.dat";
}

bool FileEntityStore::ensureDir(const std::string& entityType) const {
    auto dir = rootDir_ / entityType;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return !ec;
}

bool FileEntityStore::load(EntityId id, const std::string& entityType, EntityData& out) {
    auto path = entityPath(id, entityType);
    if (!std::filesystem::exists(path)) {
        return false;
    }

    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return false;
    }

    auto size = file.tellg();
    if (size <= 0) {
        return false;
    }
    file.seekg(0);

    std::vector<std::byte> buffer(static_cast<std::size_t>(size));
    file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(size));
    if (!file) {
        return false;
    }

    MemoryStream stream;
    stream.writeBytes(buffer.data(), buffer.size());
    stream.resetRead();
    return decodeEntityData(stream, out);
}

bool FileEntityStore::save(EntityId id, const EntityData& data) {
    if (!ensureDir(data.entityType)) {
        return false;
    }

    MemoryStream stream;
    encodeEntityData(stream, data);

    auto path = entityPath(id, data.entityType);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        return false;
    }

    file.write(reinterpret_cast<const char*>(stream.data()),
               static_cast<std::streamsize>(stream.size()));
    return file.good();
}

bool FileEntityStore::remove(EntityId id) {
    // Scan all subdirectories for the entity file
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(rootDir_, ec)) {
        if (!entry.is_directory()) continue;
        auto path = entry.path() / (std::to_string(id) + ".dat");
        if (std::filesystem::exists(path)) {
            return std::filesystem::remove(path, ec);
        }
    }
    return false;
}

EntityId FileEntityStore::allocId() {
    EntityId nextId = 1;

    auto path = metaPath();
    if (std::filesystem::exists(path)) {
        std::ifstream file(path, std::ios::binary);
        if (file.is_open()) {
            file.read(reinterpret_cast<char*>(&nextId), sizeof(nextId));
            if (!file) {
                nextId = 1;
            }
        }
    }

    EntityId allocated = nextId;
    ++nextId;

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (file.is_open()) {
        file.write(reinterpret_cast<const char*>(&nextId), sizeof(nextId));
    }

    return allocated;
}

bool FileEntityStore::exists(EntityId id, const std::string& entityType) const {
    return std::filesystem::exists(entityPath(id, entityType));
}

std::vector<EntityId> FileEntityStore::listIdsByType(const std::string& entityType) {
    std::vector<EntityId> ids;
    auto dir = rootDir_ / entityType;
    if (!std::filesystem::exists(dir)) return ids;

    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".dat") continue;

        auto stem = entry.path().stem().string();
        try {
            auto id = static_cast<EntityId>(std::stoull(stem));
            ids.push_back(id);
        } catch (...) {
            continue;
        }
    }
    return ids;
}

std::vector<std::string> FileEntityStore::listEntityTypes() {
    std::vector<std::string> types;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(rootDir_, ec)) {
        if (!entry.is_directory()) continue;
        auto name = entry.path().filename().string();
        if (name.starts_with('_')) continue;
        types.push_back(std::move(name));
    }
    return types;
}

}  // namespace theseed::core
