#pragma once

#include <memory>
#include <string>

namespace theseed::runtime {
class EntityDef;
}

namespace theseed::core {

using runtime::EntityDef;

class EntityDefLoader final {
public:
    static std::unique_ptr<EntityDef> loadFromString(const std::string& xml);
    static std::unique_ptr<EntityDef> loadFromFile(const std::string& path);
};

}  // namespace theseed::core
