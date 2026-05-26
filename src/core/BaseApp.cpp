#include "theseed/core/BaseApp.h"

#include <stdexcept>

namespace theseed::core {

BaseApp::BaseApp(Config config,
                 std::shared_ptr<runtime::IRuntimeTransport> transport,
                 std::shared_ptr<IEntityStore> store)
    : config_(std::move(config)),
      transport_(std::move(transport)),
      store_(std::move(store)) {
    if (!transport_) {
        throw std::invalid_argument("base app requires transport");
    }
    if (!store_) {
        throw std::invalid_argument("base app requires entity store");
    }
}

bool BaseApp::init() {
    if (!config_.entityDefPath.empty()) {
        registry_.loadDirectory(config_.entityDefPath);
    }

    runtime_ = std::make_unique<BaseRuntime>(transport_, store_, config_.componentId);

    for (const auto& entityType : registry_.entityTypes()) {
        auto factory = registry_.createFactory(entityType);
        if (factory) {
            runtime_->registerEntityFactory(entityType, std::move(factory));
        }
    }

    runtime_->setAutoSaveInterval(config_.autoSaveInterval);
    return true;
}

void BaseApp::attach(runtime::TickScheduler& scheduler) {
    if (runtime_) {
        runtime_->attach(scheduler);
    }
}

void BaseApp::detach(runtime::TickScheduler& scheduler) {
    if (runtime_) {
        runtime_->detach(scheduler);
    }
}

BaseRuntime& BaseApp::runtime() {
    return *runtime_;
}

const BaseRuntime& BaseApp::runtime() const {
    return *runtime_;
}

EntityDefRegistry& BaseApp::registry() {
    return registry_;
}

const EntityDefRegistry& BaseApp::registry() const {
    return registry_;
}

runtime::Entity* BaseApp::createEntity(const std::string& entityType) {
    return runtime_ ? runtime_->createEntity(entityType) : nullptr;
}

runtime::Entity* BaseApp::findEntity(runtime::EntityId id) const {
    return runtime_ ? runtime_->findEntity(id) : nullptr;
}

bool BaseApp::destroyEntity(runtime::EntityId id) {
    return runtime_ ? runtime_->destroyEntity(id) : false;
}

}  // namespace theseed::core
