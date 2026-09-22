#include "theseed/core/CellApp.h"
#include "theseed/foundation/Metrics.h"

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace theseed::core {

CellApp::CellApp(Config config, std::shared_ptr<runtime::IRuntimeTransport> transport)
    : config_(std::move(config)),
      transport_(std::move(transport)) {
    if (!transport_) {
        throw std::invalid_argument("cell app requires transport");
    }
}

bool CellApp::init() {
    if (!config_.entityDefPath.empty()) {
        registry_.loadDirectory(config_.entityDefPath);
    }

    auto space = std::make_unique<runtime::Space>(
        config_.defaultSpaceId,
        "default",
        std::make_unique<runtime::SingleCellTopology>(1));
    space->initialize();

    auto spaceRuntime = std::make_unique<runtime::SpaceRuntime>(std::move(space));
    runtime_ = std::make_unique<runtime::CellRuntime>(
        std::move(spaceRuntime), transport_, config_.componentId);

    for (const auto& entityType : registry_.entityTypes()) {
        auto factory = registry_.createFactory(entityType);
        if (factory) {
            runtime_->registerEntityFactory(entityType, std::move(factory));
        }
    }

    if (config_.ops.enabled) {
        ops::ProcessInfo info{};
        info.role = "CellApp";
        info.version = "0.1.0";
        info.startTime = std::chrono::system_clock::now();
        info.componentId = config_.componentId;

        opsInspector_ = std::make_unique<ops::OpsInspector>(std::move(info), [this] {
            ops::RuntimeInfo rt{};
            if (runtime_) {
                rt.entityCount = runtime_->spaceRuntime().space().entityCount();
            }
            rt.entityTypes = registry_.entityTypes();
            if (transport_) {
                rt.transportStats = transport_->stats();
            }
            return rt;
        });

        ops::OpsServer::Config opsCfg{};
        opsCfg.host = config_.ops.host;
        opsCfg.port = config_.ops.port;
        opsCfg.maxConnections = config_.ops.maxConnections;
        opsServer_ = std::make_unique<ops::OpsServer>(opsCfg, *opsInspector_);
        opsServer_->start();
    }

    return true;
}

void CellApp::tick() {
    // Phase B MVP metric：cell 侧实体数量。
    if (runtime_) {
        const auto count = runtime_->spaceRuntime().space().entityCount();
        theseed::foundation::MetricsRegistry::instance()
            .gauge("entity_count", "live entities held by this runtime")
            .set(static_cast<std::int64_t>(count));
    }
    if (transport_) {
        transportStatsCollector_.collect(transport_->stats());
    }
    if (opsServer_) {
        opsServer_->tick();
    }
}

void CellApp::attach(runtime::TickScheduler& scheduler) {
    if (runtime_) {
        runtime_->attach(scheduler);
    }
}

void CellApp::detach(runtime::TickScheduler& scheduler) {
    if (runtime_) {
        runtime_->detach(scheduler);
    }
}

runtime::CellRuntime& CellApp::runtime() {
    return *runtime_;
}

const runtime::CellRuntime& CellApp::runtime() const {
    return *runtime_;
}

EntityDefRegistry& CellApp::registry() {
    return registry_;
}

const EntityDefRegistry& CellApp::registry() const {
    return registry_;
}

runtime::Entity* CellApp::createEntity(const std::string& entityType,
                                        const runtime::Vector3& position,
                                        runtime::EntityId id) {
    auto def = registry_.getDef(entityType);
    if (!def) {
        return nullptr;
    }

    static runtime::EntityId nextId = 1000;
    if (id == 0) {
        id = nextId++;
    }

    auto entity = std::make_unique<runtime::Entity>(id, runtime::EntitySide::Cell, *def);
    auto* ptr = entity.get();
    ptr->activate();
    ptr->notifyCreate();

    ownedEntities_.emplace(id, std::move(entity));
    runtime_->addEntity(*ptr, position);
    return ptr;
}

runtime::Entity* CellApp::findEntity(runtime::EntityId id) const {
    return runtime_ ? runtime_->findEntity(id) : nullptr;
}

bool CellApp::destroyEntity(runtime::EntityId id) {
    if (!runtime_) {
        return false;
    }

    auto it = ownedEntities_.find(id);
    if (it == ownedEntities_.end()) {
        return false;
    }

    it->second->beginDestroy();
    it->second->notifyDestroy();
    runtime_->removeEntity(id);
    it->second->destroy();
    ownedEntities_.erase(it);
    return true;
}

}  // namespace theseed::core
