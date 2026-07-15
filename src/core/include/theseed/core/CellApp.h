#pragma once

#include "theseed/core/EntityDefRegistry.h"
#include "theseed/ops/OpsInspector.h"
#include "theseed/ops/OpsServer.h"
#include "theseed/runtime/CellRuntime.h"
#include "theseed/runtime/RuntimeLoop.h"
#include "theseed/runtime/Space.h"
#include "theseed/runtime/SpaceRuntime.h"
#include "theseed/runtime/TransportStatsCollector.h"

#include <memory>
#include <string>
#include <unordered_map>

namespace theseed::core {

class CellApp final {
public:
    struct OpsConfig final {
        bool enabled = false;
        std::string host = "127.0.0.1";
        std::uint16_t port = 20040;
        std::size_t maxConnections = 8;
    };

    struct Config {
        std::string entityDefPath;
        runtime::ComponentId componentId = 2;
        runtime::SpaceId defaultSpaceId = 1;
        OpsConfig ops;
    };

    CellApp(Config config, std::shared_ptr<runtime::IRuntimeTransport> transport);

    bool init();
    void attach(runtime::TickScheduler& scheduler);
    void detach(runtime::TickScheduler& scheduler);

    // 每帧驱动可观测性采集（entity_count gauge + transport stats）。
    // CellApp main 在每次 tick 后调用。
    void tick();

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
    runtime::TransportStatsCollector transportStatsCollector_;
    std::unordered_map<runtime::EntityId, std::unique_ptr<runtime::Entity>> ownedEntities_;

    std::unique_ptr<ops::OpsInspector> opsInspector_;
    std::unique_ptr<ops::OpsServer> opsServer_;
};

}  // namespace theseed::core
