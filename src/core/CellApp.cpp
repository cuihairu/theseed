#include "theseed/core/CellApp.h"

#include <stdexcept>

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

    return true;
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

    runtime_->removeEntity(id);
    ownedEntities_.erase(id);
    return true;
}

}  // namespace theseed::core
