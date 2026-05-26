#include "theseed/core/EntityDefRegistry.h"
#include "theseed/core/EntityDefLoader.h"

#include <filesystem>

namespace theseed::core {

bool EntityDefRegistry::loadFile(const std::string& path) {
    try {
        auto def = EntityDefLoader::loadFromFile(path);
        if (!def) {
            return false;
        }

        auto name = def->entityType();
        if (name.empty()) {
            return false;
        }

        defs_.insert_or_assign(std::move(name), std::shared_ptr<runtime::EntityDef>(std::move(def)));
        return true;
    } catch (...) {
        return false;
    }
}

std::size_t EntityDefRegistry::loadDirectory(const std::string& path) {
    std::size_t loaded = 0;

    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(path, ec)) {
        if (!entry.is_regular_file()) {
            continue;
        }

        auto ext = entry.path().extension().string();
        if (ext != ".xml" && ext != ".def") {
            continue;
        }

        if (loadFile(entry.path().string())) {
            ++loaded;
        }
    }

    return loaded;
}

bool EntityDefRegistry::registerDef(std::shared_ptr<runtime::EntityDef> def) {
    if (!def || def->entityType().empty()) {
        return false;
    }

    defs_.insert_or_assign(def->entityType(), std::move(def));
    return true;
}

const std::shared_ptr<runtime::EntityDef>& EntityDefRegistry::getDef(const std::string& entityType) const {
    auto it = defs_.find(entityType);
    if (it == defs_.end()) {
        return nullDef;
    }
    return it->second;
}

bool EntityDefRegistry::hasDef(const std::string& entityType) const {
    return defs_.contains(entityType);
}

std::vector<std::string> EntityDefRegistry::entityTypes() const {
    std::vector<std::string> types;
    types.reserve(defs_.size());
    for (const auto& [name, _] : defs_) {
        types.push_back(name);
    }
    return types;
}

std::size_t EntityDefRegistry::defCount() const {
    return defs_.size();
}

EntityDefRegistry::EntityFactory EntityDefRegistry::createFactory(const std::string& entityType) const {
    auto it = defs_.find(entityType);
    if (it == defs_.end()) {
        return nullptr;
    }

    auto def = it->second;
    return [def](runtime::EntityId id, runtime::EntitySide side) -> std::unique_ptr<runtime::Entity> {
        return std::make_unique<runtime::Entity>(id, side, *def);
    };
}

}  // namespace theseed::core
