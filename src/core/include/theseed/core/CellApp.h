#pragma once

#include "theseed/core/EntityDefRegistry.h"
#include "theseed/runtime/CellRuntime.h"
#include "theseed/runtime/RuntimeLoop.h"
#include "theseed/runtime/Space.h"
#include "theseed/runtime/SpaceRuntime.h"

#include <memory>
#include <string>
#include <unordered_map>

namespace theseed::core {

class CellApp final {
public:
    struct Config {
        std::string entityDefPath;
        runtime::ComponentId componentId = 2;
        runtime::SpaceId defaultSpaceId = 1;
    };

    CellApp(Config config, std::shared_ptr<runtime::IRuntimeTransport> transport);

    bool init();
    void attach(runtime::TickScheduler& scheduler);
    void detach(runtime::TickScheduler& scheduler);

    runtime::CellRuntime& runtime();
    const runtime::CellRuntime& runtime() const;
    EntityDefRegistry& registry();
    const EntityDefRegistry& registry() const;

    runtime::Entity* createEntity(const std::string& entityType,
                                  const runtime::Vector3& position,
                                  runtime::EntityId id = 0);
    runtime::Entity* findEntity(runtime::EntityId id) const;
    bool destroyEntity(runtime::EntityId id);

private:
    Config config_;
    EntityDefRegistry registry_;
    std::unique_ptr<runtime::SpaceRuntime> spaceRuntime_;
    std::unique_ptr<runtime::CellRuntime> runtime_;
    std::shared_ptr<runtime::IRuntimeTransport> transport_;
    std::unordered_map<runtime::EntityId, std::unique_ptr<runtime::Entity>> ownedEntities_;
};

}  // namespace theseed::core
