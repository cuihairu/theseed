#pragma once

#include "theseed/runtime/Entity.h"
#include "theseed/runtime/EntityDef.h"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace theseed::core {

class EntityDefRegistry final {
public:
    using EntityFactory = std::function<std::unique_ptr<runtime::Entity>(
        runtime::EntityId, runtime::EntitySide)>;

    bool loadFile(const std::string& path);
    std::size_t loadDirectory(const std::string& path);
    bool registerDef(std::shared_ptr<runtime::EntityDef> def);

    const std::shared_ptr<runtime::EntityDef>& getDef(const std::string& entityType) const;
    bool hasDef(const std::string& entityType) const;
    std::vector<std::string> entityTypes() const;
    std::size_t defCount() const;

    EntityFactory createFactory(const std::string& entityType) const;

private:
    void resolveInheritance();
    static inline const std::shared_ptr<runtime::EntityDef> nullDef{};

    std::unordered_map<std::string, std::shared_ptr<runtime::EntityDef>> defs_;
};

}  // namespace theseed::core
