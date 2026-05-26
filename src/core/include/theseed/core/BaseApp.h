#pragma once

#include "theseed/core/BaseRuntime.h"
#include "theseed/core/EntityDefRegistry.h"
#include "theseed/runtime/RuntimeLoop.h"

#include <memory>
#include <string>

namespace theseed::core {

class BaseApp final {
public:
    struct Config {
        std::string entityDefPath;
        runtime::ComponentId componentId = 1;
        runtime::Duration autoSaveInterval = std::chrono::seconds{30};
    };

    BaseApp(Config config,
            std::shared_ptr<runtime::IRuntimeTransport> transport,
            std::shared_ptr<IEntityStore> store);

    bool init();
    void attach(runtime::TickScheduler& scheduler);
    void detach(runtime::TickScheduler& scheduler);

    BaseRuntime& runtime();
    const BaseRuntime& runtime() const;
    EntityDefRegistry& registry();
    const EntityDefRegistry& registry() const;

    runtime::Entity* createEntity(const std::string& entityType);
    runtime::Entity* findEntity(runtime::EntityId id) const;
    bool destroyEntity(runtime::EntityId id);

private:
    Config config_;
    EntityDefRegistry registry_;
    std::unique_ptr<BaseRuntime> runtime_;
    std::shared_ptr<runtime::IRuntimeTransport> transport_;
    std::shared_ptr<IEntityStore> store_;
};

}  // namespace theseed::core
