#include "theseed/core/BaseRuntime.h"
#include "theseed/runtime/PropertyReplication.h"

#include <array>
#include <cstring>
#include <stdexcept>

namespace theseed::core {

namespace {

runtime::PropertyType dataTypeToPropertyType(DataType dt) {
    return static_cast<runtime::PropertyType>(static_cast<std::uint8_t>(dt));
}

DataType propertyTypeToDataType(runtime::PropertyType pt) {
    return static_cast<DataType>(static_cast<std::uint8_t>(pt));
}

EntityData entityToData(const runtime::Entity& entity) {
    EntityData data;
    data.id = entity.id();
    data.entityType = entity.entityType();

    const auto& block = entity.propertyBlock();
    const auto& def = block.def();

    for (const auto& desc : def.properties()) {
        if (desc.flags != runtime::PropertyFlag::None
            && !runtime::hasFlag(desc.flags, runtime::PropertyFlag::Persistent)) {
            continue;
        }

        PropertyData prop;
        prop.id = desc.id;
        prop.name = desc.name;
        prop.type = propertyTypeToDataType(desc.type);

        if (runtime::EntityDef::isVariableSized(desc.type)) {
            auto varData = block.getBlob(desc.id);
            prop.rawValue.assign(varData.begin(), varData.end());
        } else {
            prop.rawValue.resize(desc.size);
            std::memcpy(prop.rawValue.data(), block.data() + desc.offset, desc.size);
        }

        data.properties.push_back(std::move(prop));
    }

    return data;
}

void dataToEntity(const EntityData& data, runtime::Entity& entity) {
    auto& block = entity.propertyBlock();
    const auto& def = block.def();

    for (const auto& prop : data.properties) {
        const auto* desc = def.findProperty(prop.name);
        if (!desc) {
            continue;
        }

        if (runtime::EntityDef::isVariableSized(desc->type)) {
            block.setBlob(desc->id, prop.rawValue);
        } else {
            if (prop.rawValue.size() != desc->size) {
                continue;
            }
            std::memcpy(block.data() + desc->offset, prop.rawValue.data(), desc->size);
        }
    }

    entity.clearDirtyFlags();
}

std::shared_ptr<runtime::EntityDef> getOrCreateDef(
    const std::string& entityType,
    std::unordered_map<std::string, std::shared_ptr<runtime::EntityDef>>& defs) {
    auto it = defs.find(entityType);
    if (it != defs.end()) {
        return it->second;
    }

    auto def = std::make_shared<runtime::EntityDef>(entityType);
    defs.emplace(entityType, def);
    return def;
}

}  // namespace

BaseRuntime::IngressPump::IngressPump(BaseRuntime& owner) : owner_(&owner) {}

void BaseRuntime::IngressPump::tick(runtime::TickContext& context) {
    static_cast<void>(context);
    owner_->pumpInbound();
}

BaseRuntime::BaseRuntime(std::shared_ptr<runtime::IRuntimeTransport> transport,
                         std::shared_ptr<IEntityStore> store,
                         runtime::ComponentId localComponentId)
    : transport_(std::move(transport)),
      store_(std::move(store)),
      localComponentId_(localComponentId),
      ingressPump_(*this) {
    if (!transport_) {
        throw std::invalid_argument("base runtime requires transport");
    }
    if (!store_) {
        throw std::invalid_argument("base runtime requires entity store");
    }
    if (localComponentId_ == 0) {
        throw std::invalid_argument("base runtime requires non-zero local component id");
    }
}

void BaseRuntime::attach(runtime::TickScheduler& scheduler) {
    scheduler.registerTickable(runtime::TickPhase::Network, ingressPump_);
    scheduler.registerTickable(runtime::TickPhase::Flush, *this);
}

void BaseRuntime::detach(runtime::TickScheduler& scheduler) {
    static_cast<void>(scheduler.unregisterTickable(runtime::TickPhase::Network, ingressPump_));
    static_cast<void>(scheduler.unregisterTickable(runtime::TickPhase::Flush, *this));
}

bool BaseRuntime::registerEntityFactory(const std::string& entityType, EntityFactory factory) {
    if (entityType.empty() || !factory) {
        return false;
    }

    factories_.insert_or_assign(entityType, std::move(factory));
    return true;
}

runtime::Entity* BaseRuntime::createEntity(const std::string& entityType) {
    auto it = factories_.find(entityType);
    if (it == factories_.end()) {
        return nullptr;
    }

    auto id = store_->allocId();
    auto entity = it->second(id, runtime::EntitySide::Base);
    if (!entity) {
        return nullptr;
    }

    auto* ptr = entity.get();
    entities_.emplace(id, std::move(entity));
    ptr->setTransport(transport_.get());
    ptr->setTimerScheduleFns(
        [this, id](runtime::Duration delay, runtime::Entity::EntityTimerCallback cb) {
            return addEntityTimer(id, delay, [cb = std::move(cb), id, this]() {
                auto* e = findEntity(id);
                if (e) cb(*e);
            });
        },
        [this, id](runtime::Duration interval, runtime::Entity::EntityTimerCallback cb) {
            return addEntityPeriodicTimer(id, interval, [cb = std::move(cb), id, this]() {
                auto* e = findEntity(id);
                if (e) cb(*e);
            });
        });
    ptr->activate();
    ptr->notifyCreate();
    return ptr;
}

runtime::Entity* BaseRuntime::loadEntity(runtime::EntityId id, const std::string& entityType) {
    auto it = factories_.find(entityType);
    if (it == factories_.end()) {
        return nullptr;
    }

    EntityData data;
    if (!store_->load(id, entityType, data)) {
        return nullptr;
    }

    auto entity = it->second(id, runtime::EntitySide::Base);
    if (!entity) {
        return nullptr;
    }

    dataToEntity(data, *entity);
    auto* ptr = entity.get();
    entities_.emplace(id, std::move(entity));
    ptr->setTransport(transport_.get());
    ptr->setTimerScheduleFns(
        [this, id](runtime::Duration delay, runtime::Entity::EntityTimerCallback cb) {
            return addEntityTimer(id, delay, [cb = std::move(cb), id, this]() {
                auto* e = findEntity(id);
                if (e) cb(*e);
            });
        },
        [this, id](runtime::Duration interval, runtime::Entity::EntityTimerCallback cb) {
            return addEntityPeriodicTimer(id, interval, [cb = std::move(cb), id, this]() {
                auto* e = findEntity(id);
                if (e) cb(*e);
            });
        });
    ptr->activate();
    ptr->notifyRestore();
    return ptr;
}

std::size_t BaseRuntime::restoreEntities(const std::string& entityType) {
    auto ids = store_->listIdsByType(entityType);
    std::size_t restored = 0;
    for (auto id : ids) {
        if (entities_.contains(id)) continue;
        if (loadEntity(id, entityType)) {
            ++restored;
        }
    }
    return restored;
}

bool BaseRuntime::destroyEntity(runtime::EntityId id) {
    auto it = entities_.find(id);
    if (it == entities_.end()) {
        return false;
    }

    saveEntity(id);
    it->second->beginDestroy();
    it->second->notifyDestroy();
    it->second->destroy();
    cancelEntityTimers(id);
    entities_.erase(it);
    return true;
}

runtime::Entity* BaseRuntime::findEntity(runtime::EntityId id) const {
    auto it = entities_.find(id);
    if (it == entities_.end()) {
        return nullptr;
    }
    return it->second.get();
}

std::vector<runtime::Entity*> BaseRuntime::findEntitiesByType(const std::string& entityType) const {
    std::vector<runtime::Entity*> result;
    for (auto& [id, entity] : entities_) {
        if (entity->entityType() == entityType) {
            result.push_back(entity.get());
        }
    }
    return result;
}

void BaseRuntime::forEachEntity(std::function<void(runtime::Entity&)> callback) const {
    for (auto& [id, entity] : entities_) {
        callback(*entity);
    }
}

std::size_t BaseRuntime::entityCount() const {
    return entities_.size();
}

bool BaseRuntime::setCellEntityCall(runtime::EntityId id, runtime::ComponentId cellComponent) {
    auto* entity = findEntity(id);
    if (!entity) {
        return false;
    }

    entity->bindCellEntityCall(cellComponent);
    return true;
}

bool BaseRuntime::clearCellEntityCall(runtime::EntityId id) {
    auto* entity = findEntity(id);
    if (!entity) {
        return false;
    }

    entity->clearCellEntityCall();
    return true;
}

bool BaseRuntime::saveEntity(runtime::EntityId id) {
    auto* entity = findEntity(id);
    if (!entity) {
        return false;
    }

    auto data = entityToData(*entity);
    return store_->save(id, data);
}

void BaseRuntime::setAutoSaveInterval(runtime::Duration interval) {
    autoSaveInterval_ = interval;
    autoSaveAccumulator_ = {};
}

std::size_t BaseRuntime::pumpInbound() {
    std::array<runtime::RuntimeInvocation, 32> batch{};
    std::size_t total = 0;

    while (true) {
        auto count = transport_->receive(localComponentId_, batch.data(), batch.size());
        if (count == 0) {
            break;
        }

        for (std::size_t i = 0; i < count; ++i) {
            static_cast<void>(dispatchInvocation(batch[i]));
        }
        total += count;
    }

    return total;
}

bool BaseRuntime::dispatchInvocation(const runtime::RuntimeInvocation& invocation) {
    if (invocation.targetComponent != localComponentId_) {
        return false;
    }

    if (invocation.method == "entity.cellReady") {
        return handleCellReady(invocation);
    }
    if (invocation.method == "entity.cellDestroyed") {
        return handleCellDestroyed(invocation);
    }
    if (invocation.method == "property.syncToBase") {
        return handlePropertySyncFromCell(invocation);
    }

    auto* entity = findEntity(invocation.entityId);
    if (!entity) {
        return false;
    }

    return entity->dispatchInvocation(invocation);
}

void BaseRuntime::tick(runtime::TickContext& context) {
    timerWheel_.advance(context.deltaTime);
    syncToCells();

    if (autoSaveInterval_ > runtime::Duration{}) {
        autoSaveAccumulator_ += context.deltaTime;
        while (autoSaveAccumulator_ >= autoSaveInterval_) {
            autoSaveAccumulator_ -= autoSaveInterval_;
            autoSaveAll();
        }
    }
}

void BaseRuntime::autoSaveAll() {
    for (auto& [id, entity] : entities_) {
        if (entity->state() != runtime::EntityState::Active) continue;
        if (!entity->propertyBlock().dirtyMask().any()) continue;

        auto data = entityToData(*entity);
        store_->save(id, data);
        entity->clearDirtyFlags();
    }
}

TimerHandle BaseRuntime::addTimer(TimerWheel::Duration delay, TimerWheel::Callback callback) {
    return timerWheel_.addTimer(delay, std::move(callback));
}

TimerHandle BaseRuntime::addEntityTimer(runtime::EntityId entityId, TimerWheel::Duration delay, TimerWheel::Callback callback) {
    auto handle = timerWheel_.addTimer(delay, std::move(callback));
    entityTimers_[entityId].push_back(handle);
    return handle;
}

TimerHandle BaseRuntime::addEntityPeriodicTimer(runtime::EntityId entityId, TimerWheel::Duration interval, TimerWheel::Callback callback) {
    auto handle = timerWheel_.addPeriodic(interval, std::move(callback));
    entityTimers_[entityId].push_back(handle);
    return handle;
}

bool BaseRuntime::cancelTimer(TimerHandle handle) {
    return timerWheel_.cancel(handle);
}

void BaseRuntime::cancelEntityTimers(runtime::EntityId entityId) {
    auto it = entityTimers_.find(entityId);
    if (it == entityTimers_.end()) {
        return;
    }

    for (auto& handle : it->second) {
        timerWheel_.cancel(handle);
    }
    entityTimers_.erase(it);
}

bool BaseRuntime::handleCellReady(const runtime::RuntimeInvocation& invocation) {
    runtime::EntityId entityId = 0;
    runtime::ComponentId cellComponentId = 0;
    if (invocation.payload.size() < sizeof(entityId) + sizeof(cellComponentId)) return false;
    std::memcpy(&entityId, invocation.payload.data(), sizeof(entityId));
    std::memcpy(&cellComponentId, invocation.payload.data() + sizeof(entityId), sizeof(cellComponentId));

    auto* entity = findEntity(entityId);
    if (!entity) return false;

    entity->bindCellEntityCall(cellComponentId);
    entity->notifyCellReady();
    return true;
}

bool BaseRuntime::handleCellDestroyed(const runtime::RuntimeInvocation& invocation) {
    runtime::EntityId entityId = 0;
    if (invocation.payload.size() < sizeof(entityId)) return false;
    std::memcpy(&entityId, invocation.payload.data(), sizeof(entityId));

    auto* entity = findEntity(entityId);
    if (!entity) return false;

    entity->clearCellEntityCall();
    return true;
}

bool BaseRuntime::handlePropertySyncFromCell(const runtime::RuntimeInvocation& invocation) {
    if (invocation.payload.empty()) return false;

    auto* entity = findEntity(invocation.entityId);
    if (!entity) return false;

    try {
        auto deltas = runtime::PropertyReplication::decodeDelta(invocation.payload);
        entity->applyPropertyDelta(deltas);
        return true;
    } catch (...) {
        return false;
    }
}

bool BaseRuntime::requestCreateCell(runtime::EntityId entityId,
                                     const std::string& entityType,
                                     const runtime::Vector3& position,
                                     runtime::ComponentId targetCellApp) {
    auto* entity = findEntity(entityId);
    if (!entity) return false;

    // Payload: entityId(8) + baseComponentId(4) + posX(4) + posY(4) + posZ(4) + propertySnapshot(var)
    constexpr std::size_t headerSize = sizeof(runtime::EntityId) + sizeof(runtime::ComponentId)
                                     + sizeof(float) * 3;
    auto snapshot = entity->buildFullPropertySnapshot(runtime::PropertyFlag::Base);
    auto encoded = runtime::PropertyReplication::encodeDelta(snapshot);

    std::vector<std::byte> payload(headerSize + encoded.size());
    auto* p = payload.data();
    std::memcpy(p, &entityId, sizeof(entityId)); p += sizeof(entityId);
    std::memcpy(p, &localComponentId_, sizeof(localComponentId_)); p += sizeof(localComponentId_);
    std::memcpy(p, &position.x, sizeof(float)); p += sizeof(float);
    std::memcpy(p, &position.y, sizeof(float)); p += sizeof(float);
    std::memcpy(p, &position.z, sizeof(float)); p += sizeof(float);
    std::memcpy(p, encoded.data(), encoded.size());

    runtime::RuntimeInvocation invocation;
    invocation.entityId = entityId;
    invocation.targetComponent = targetCellApp;
    invocation.entityType = entityType;
    invocation.method = "entity.createCell";
    invocation.deliveryClass = runtime::DeliveryClass::ORDERED_RELIABLE;
    invocation.payload = std::move(payload);

    auto result = transport_->send(std::move(invocation));
    if (result == runtime::SendResult::Accepted) {
        entity->clearDirtyFlags();
    }
    return result == runtime::SendResult::Accepted;
}

bool BaseRuntime::requestDestroyCell(runtime::EntityId entityId,
                                      runtime::ComponentId targetCellApp) {
    // Payload: entityId(8) + baseComponentId(4)
    std::vector<std::byte> payload(sizeof(runtime::EntityId) + sizeof(runtime::ComponentId));
    std::memcpy(payload.data(), &entityId, sizeof(entityId));
    std::memcpy(payload.data() + sizeof(entityId), &localComponentId_, sizeof(localComponentId_));

    runtime::RuntimeInvocation invocation;
    invocation.entityId = entityId;
    invocation.targetComponent = targetCellApp;
    invocation.method = "entity.destroyCell";
    invocation.deliveryClass = runtime::DeliveryClass::ORDERED_RELIABLE;
    invocation.payload = std::move(payload);

    return transport_->send(std::move(invocation)) == runtime::SendResult::Accepted;
}

void BaseRuntime::syncToCells() {
    for (auto& [id, entity] : entities_) {
        if (entity->state() != runtime::EntityState::Active) continue;

        auto* cellCall = entity->cellEntityCall();
        if (!cellCall || !cellCall->isValid()) continue;

        auto deltas = entity->buildDirtyPropertyDelta(runtime::PropertyFlag::Base);
        if (deltas.empty()) continue;

        runtime::RuntimeInvocation invocation;
        invocation.entityId = entity->id();
        invocation.targetComponent = cellCall->targetComponent();
        invocation.entityType = entity->entityType();
        invocation.method = "property.syncToCell";
        invocation.deliveryClass = runtime::DeliveryClass::ORDERED_RELIABLE;
        invocation.payload = runtime::PropertyReplication::encodeDelta(deltas);

        if (transport_->send(std::move(invocation)) == runtime::SendResult::Accepted) {
            entity->clearDirtyFlags();
        }
    }
}

}  // namespace theseed::core
