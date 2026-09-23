#include "theseed/core/EntityDefRegistry.h"
#include "theseed/core/EntityDefLoader.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace theseed::core {

bool EntityDefRegistry::loadFile(const std::string& path) {
    try {
        auto def = EntityDefLoader::loadFromFile(path);

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

    resolveInheritance();
    return loaded;
}

void EntityDefRegistry::resolveInheritance() {
    // mergeFrom 是一次性的（合并后拒绝再合并），因此必须在合并前保证
    // 父链已完整解析。深度优先递归解析，与 defs_ 的遍历顺序无关，
    // resolved 先标记防止 extends 环。
    std::unordered_set<std::string> resolved;
    std::function<void(const std::string&)> resolve;
    resolve = [&](const std::string& name) {  // LCOV_EXCL_LINE std::function 赋值的闭包包装符号使本行条目恒 0（lambda 体有计数），gcc 归因伪影
        auto it = defs_.find(name);
        if (it == defs_.end() || resolved.contains(name)) {
            return;
        }
        resolved.insert(name);

        const auto& parentName = it->second->parentType();
        if (parentName.empty() || parentName == name) {
            return;
        }
        auto parentIt = defs_.find(parentName);
        if (parentIt == defs_.end()) {
            return;
        }

        resolve(parentName);
        static_cast<void>(it->second->mergeFrom(*parentIt->second));
    };

    for (const auto& [name, def] : defs_) {
        static_cast<void>(def);
        resolve(name);
    }
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
