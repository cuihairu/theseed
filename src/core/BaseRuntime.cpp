#include "theseed/core/BaseRuntime.h"
#include "theseed/foundation/Metrics.h"
#include "theseed/runtime/PropertyReplication.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace theseed::core {

namespace {

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

}  // namespace

BaseRuntime::IngressPump::IngressPump(BaseRuntime& owner) : owner_(&owner) {}

void BaseRuntime::IngressPump::tick(runtime::TickContext& context) {
    static_cast<void>(context);
    owner_->pumpInbound();
}

BaseRuntime::TimerPump::TimerPump(BaseRuntime& owner) : owner_(&owner) {}

void BaseRuntime::TimerPump::tick(runtime::TickContext& context) {
    owner_->advanceTimers(context);
}

BaseRuntime::SyncBuildPump::SyncBuildPump(BaseRuntime& owner) : owner_(&owner) {}

void BaseRuntime::SyncBuildPump::tick(runtime::TickContext& context) {
    owner_->buildSync(context);
}

BaseRuntime::FlushPump::FlushPump(BaseRuntime& owner) : owner_(&owner) {}

void BaseRuntime::FlushPump::tick(runtime::TickContext& context) {
    static_cast<void>(context);
    owner_->flushRuntimeTransport();
}

BaseRuntime::BaseRuntime(std::shared_ptr<runtime::IRuntimeTransport> transport,
                         std::shared_ptr<IEntityStore> store,
                         runtime::ComponentId localComponentId)
    : transport_(std::move(transport)),
      store_(std::move(store)),
      localComponentId_(localComponentId),
      ingressPump_(*this),
      timerPump_(*this),
      syncBuildPump_(*this),
      flushPump_(*this) {
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
    scheduler.registerTickable(runtime::TickPhase::Timer, timerPump_);
    scheduler.registerTickable(runtime::TickPhase::SyncBuild, syncBuildPump_);
    scheduler.registerTickable(runtime::TickPhase::Flush, flushPump_);
}

void BaseRuntime::detach(runtime::TickScheduler& scheduler) {
    static_cast<void>(scheduler.unregisterTickable(runtime::TickPhase::Network, ingressPump_));
    static_cast<void>(scheduler.unregisterTickable(runtime::TickPhase::Timer, timerPump_));
    static_cast<void>(scheduler.unregisterTickable(runtime::TickPhase::SyncBuild, syncBuildPump_));
    static_cast<void>(scheduler.unregisterTickable(runtime::TickPhase::Flush, flushPump_));
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
    if (factoryHook_) factoryHook_(*ptr);
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
    if (factoryHook_) factoryHook_(*ptr);
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

    auto* entity = it->second.get();

    // If entity has a cell entity, request cell destruction first
    auto* cellCall = entity->cellEntityCall();
    if (cellCall && cellCall->isValid()) {
        entity->beginDestroy();
        pendingDestructions_.insert(id);
        requestDestroyCell(id, cellCall->targetComponent());
        return true;
    }

    // No cell entity — destroy immediately
    completeBaseDestruction(id);
    return true;
}

void BaseRuntime::completeBaseDestruction(runtime::EntityId entityId) {
    auto it = entities_.find(entityId);
    if (it == entities_.end()) return;

    auto* entity = it->second.get();
    auto entityType = entity->entityType();

    saveEntity(entityId);

    entity->beginDestroy();
    entity->notifyDestroy();
    entity->destroy();
    cancelEntityTimers(entityId);
    pendingDestructions_.erase(entityId);
    entities_.erase(it);

    if (onEntityDestroyed_) {
        onEntityDestroyed_(entityId, entityType);
    }
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

std::vector<runtime::Entity*> BaseRuntime::findEntitiesByTag(const std::string& tag) const {
    std::vector<runtime::Entity*> result;
    for (auto& [id, entity] : entities_) {
        if (entity->hasTag(tag)) {
            result.push_back(entity.get());
        }
    }
    return result;
}

std::vector<runtime::Entity*> BaseRuntime::queryEntities(std::function<bool(const runtime::Entity&)> predicate) const {
    std::vector<runtime::Entity*> result;
    for (auto& [id, entity] : entities_) {
        if (predicate(*entity)) {
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
    const auto n = entities_.size();
    theseed::foundation::MetricsRegistry::instance()
        .gauge("entity_count", "live entities held by this runtime")
        .set(static_cast<std::int64_t>(n));
    return n;
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
    if (!store_->save(id, data)) {
        return false;
    }

    entity->clearPersistenceDirtyFlags();
    return true;
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
    if (invocation.method == "aoi.enter") {
        return handleAoIEnter(invocation);
    }
    if (invocation.method == "aoi.leave") {
        return handleAoILeave(invocation);
    }
    if (invocation.method == "entity.spawnRequest") {
        return handleSpawnRequest(invocation);
    }
    if (invocation.method == "entity.clientEvent") {
        return handleClientEvent(invocation);
    }
    if (invocation.method == "witness.propertySync") {
        return handleWitnessSync(invocation);
    }
    if (invocation.method == "entity.spaceChanged") {
        return handleSpaceChanged(invocation);
    }

    auto* entity = findEntity(invocation.entityId);
    if (!entity) {
        return false;
    }

    return entity->dispatchInvocation(invocation);
}

void BaseRuntime::tick(runtime::TickContext& context) {
    advanceTimers(context);
    buildSync(context);
    flushRuntimeTransport();
}

void BaseRuntime::advanceTimers(runtime::TickContext& context) {
    timerWheel_.advance(context.deltaTime);
}

void BaseRuntime::buildSync(runtime::TickContext& context) {
    syncToCells();
    if (autoSaveInterval_ > runtime::Duration{}) {
        autoSaveAccumulator_ += context.deltaTime;
        while (autoSaveAccumulator_ >= autoSaveInterval_) {
            autoSaveAccumulator_ -= autoSaveInterval_;
            autoSaveAll();
        }
    }
}

void BaseRuntime::flushRuntimeTransport() {
    for (auto& pending : pendingRuntimeSync_) {
        if (transport_->send(std::move(pending.invocation)) == runtime::SendResult::Accepted) {
            if (auto* entity = findEntity(pending.clearDirtyEntityId)) {
                entity->clearRuntimeDirtyFlags();
            }
        }
    }
    pendingRuntimeSync_.clear();
    transport_->flush();
}

void BaseRuntime::autoSaveAll() {
    for (auto& [id, entity] : entities_) {
        if (entity->state() != runtime::EntityState::Active) continue;
        if (!entity->propertyBlock().persistenceDirtyMask().any()) continue;

        auto data = entityToData(*entity);
        if (store_->save(id, data)) {
            entity->clearPersistenceDirtyFlags();
        }
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

    // If entity is pending destruction, complete the base cleanup
    if (pendingDestructions_.contains(entityId)) {
        completeBaseDestruction(entityId);
        return true;
    }

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
        entity->applyPropertyDelta(deltas, runtime::PropertyDirtyTarget::Client
                                           | runtime::PropertyDirtyTarget::Persistence);
        return true;
    } catch (...) {
        return false;
    }
}

bool BaseRuntime::requestCreateCell(runtime::EntityId entityId,
                                     const std::string& entityType,
                                     const runtime::Vector3& position,
                                     runtime::ComponentId targetCellApp,
                                     runtime::SpaceId spaceId) {
    auto* entity = findEntity(entityId);
    if (!entity) return false;

    // Payload: spaceId(4) + entityId(8) + baseComponentId(4) + posX(4) + posY(4) + posZ(4) + propertySnapshot(var)
    constexpr std::size_t headerSize = sizeof(runtime::SpaceId) + sizeof(runtime::EntityId)
                                     + sizeof(runtime::ComponentId) + sizeof(float) * 3;
    auto snapshot = entity->buildFullPropertySnapshot(runtime::PropertyFlag::Base);
    auto encoded = runtime::PropertyReplication::encodeDelta(snapshot);

    std::vector<std::byte> payload(headerSize + encoded.size());
    auto* p = payload.data();
    std::memcpy(p, &spaceId, sizeof(spaceId)); p += sizeof(spaceId);
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
        entity->clearRuntimeDirtyFlags();
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

bool BaseRuntime::requestTeleport(runtime::EntityId entityId,
                                   runtime::SpaceId targetSpaceId,
                                   const runtime::Vector3& position) {
    auto* entity = findEntity(entityId);
    if (!entity) return false;

    auto* cellCall = entity->cellEntityCall();
    if (!cellCall || !cellCall->isValid()) return false;

    // Payload: entityId(8) + spaceId(4) + posX(4) + posY(4) + posZ(4)
    constexpr std::size_t payloadSize = sizeof(runtime::EntityId) + sizeof(runtime::SpaceId)
                                       + sizeof(float) * 3;
    std::vector<std::byte> payload(payloadSize);
    auto* p = payload.data();
    std::memcpy(p, &entityId, sizeof(entityId)); p += sizeof(entityId);
    std::memcpy(p, &targetSpaceId, sizeof(targetSpaceId)); p += sizeof(targetSpaceId);
    std::memcpy(p, &position.x, sizeof(float)); p += sizeof(float);
    std::memcpy(p, &position.y, sizeof(float)); p += sizeof(float);
    std::memcpy(p, &position.z, sizeof(float));

    runtime::RuntimeInvocation invocation;
    invocation.entityId = entityId;
    invocation.targetComponent = cellCall->targetComponent();
    invocation.entityType = entity->entityType();
    invocation.method = "entity.teleport";
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

        pendingRuntimeSync_.push_back(PendingRuntimeSync{
            .invocation = std::move(invocation),
            .clearDirtyEntityId = entity->id(),
        });
    }
}

void BaseRuntime::setOnAoIEnter(AoIEnterCallback cb) {
    onAoIEnter_ = std::move(cb);
}

void BaseRuntime::setOnAoILeave(AoILeaveCallback cb) {
    onAoILeave_ = std::move(cb);
}

void BaseRuntime::setOnClientEvent(ClientEventCallback cb) {
    onClientEvent_ = std::move(cb);
}

void BaseRuntime::setOnWitnessSync(WitnessSyncCallback cb) {
    onWitnessSync_ = std::move(cb);
}

void BaseRuntime::setOnSpaceChange(SpaceChangeCallback cb) {
    onSpaceChange_ = std::move(cb);
}

void BaseRuntime::setOnEntityDestroyed(DestructionCallback cb) {
    onEntityDestroyed_ = std::move(cb);
}

void BaseRuntime::setEntityFactoryHook(EntityFactoryHook hook) {
    factoryHook_ = std::move(hook);
}

bool BaseRuntime::handleAoIEnter(const runtime::RuntimeInvocation& invocation) {
    // payload: observerId(8) + targetId(8) + typeLen(4) + type(N) + hasPos(1) + [posX(4)+posY(4)+posZ(4)]
    auto& p = invocation.payload;
    std::size_t offset = 0;
    if (offset + sizeof(runtime::EntityId) * 2 > p.size()) return false;

    runtime::EntityId observerId = 0;
    std::memcpy(&observerId, p.data() + offset, sizeof(observerId));
    offset += sizeof(observerId);

    runtime::EntityId targetId = 0;
    std::memcpy(&targetId, p.data() + offset, sizeof(targetId));
    offset += sizeof(targetId);

    if (offset + sizeof(std::uint32_t) > p.size()) return false;
    std::uint32_t typeLen = 0;
    std::memcpy(&typeLen, p.data() + offset, sizeof(typeLen));
    offset += sizeof(typeLen);

    if (offset + typeLen > p.size()) return false;
    std::string targetType(reinterpret_cast<const char*>(p.data() + offset), typeLen);
    offset += typeLen;

    bool hasPosition = false;
    runtime::Vector3 position{};
    if (offset + 1 <= p.size()) {
        hasPosition = static_cast<bool>(p[offset]);
        offset += 1;
        if (hasPosition && offset + sizeof(float) * 3 <= p.size()) {
            std::memcpy(&position.x, p.data() + offset, sizeof(float));
            offset += sizeof(float);
            std::memcpy(&position.y, p.data() + offset, sizeof(float));
            offset += sizeof(float);
            std::memcpy(&position.z, p.data() + offset, sizeof(float));
        }
    }

    if (onAoIEnter_) {
        onAoIEnter_(observerId, targetId, targetType, hasPosition, position);
    }
    return true;
}

bool BaseRuntime::handleAoILeave(const runtime::RuntimeInvocation& invocation) {
    auto& p = invocation.payload;
    if (p.size() < sizeof(runtime::EntityId) * 2) return false;

    runtime::EntityId observerId = 0;
    std::memcpy(&observerId, p.data(), sizeof(observerId));

    runtime::EntityId targetId = 0;
    std::memcpy(&targetId, p.data() + sizeof(observerId), sizeof(targetId));

    if (onAoILeave_) {
        onAoILeave_(observerId, targetId);
    }
    return true;
}

bool BaseRuntime::handleSpawnRequest(const runtime::RuntimeInvocation& invocation) {
    // payload: typeLen(4) + type(N) + posX(4) + posY(4) + posZ(4)
    auto& p = invocation.payload;
    std::size_t offset = 0;
    if (offset + 4 > p.size()) return false;

    std::uint32_t typeLen = 0;
    std::memcpy(&typeLen, p.data() + offset, 4);
    offset += 4;
    if (offset + typeLen > p.size()) return false;

    std::string entityType(reinterpret_cast<const char*>(p.data() + offset), typeLen);
    offset += typeLen;

    runtime::Vector3 position{};
    if (offset + sizeof(float) * 3 <= p.size()) {
        std::memcpy(&position.x, p.data() + offset, sizeof(float));
        offset += sizeof(float);
        std::memcpy(&position.y, p.data() + offset, sizeof(float));
        offset += sizeof(float);
        std::memcpy(&position.z, p.data() + offset, sizeof(float));
    }

    // Create the new base entity
    auto* newEntity = createEntity(entityType);
    if (!newEntity) return false;

    // Find the requesting entity's CellApp to create the cell on the same CellApp
    auto* requester = findEntity(invocation.entityId);
    if (!requester) {
        destroyEntity(newEntity->id());
        return false;
    }

    auto* cellCall = requester->cellEntityCall();
    if (!cellCall || !cellCall->isValid()) {
        destroyEntity(newEntity->id());
        return false;
    }

    return requestCreateCell(newEntity->id(), entityType, position, cellCall->targetComponent());
}

bool BaseRuntime::handleClientEvent(const runtime::RuntimeInvocation& invocation) {
    if (!onClientEvent_) return false;

    // Payload: entityId(8) + count(4) + [nameLen(4) + name + dataLen(4) + data]...
    auto& p = invocation.payload;
    std::size_t offset = 0;
    if (offset + sizeof(runtime::EntityId) + sizeof(std::uint32_t) > p.size()) return false;

    runtime::EntityId entityId = 0;
    std::memcpy(&entityId, p.data() + offset, sizeof(entityId));
    offset += sizeof(entityId);

    std::uint32_t count = 0;
    std::memcpy(&count, p.data() + offset, sizeof(count));
    offset += sizeof(count);

    for (std::uint32_t i = 0; i < count; ++i) {
        if (offset + sizeof(std::uint32_t) > p.size()) return false;
        std::uint32_t nameLen = 0;
        std::memcpy(&nameLen, p.data() + offset, sizeof(nameLen));
        offset += sizeof(nameLen);

        if (offset + nameLen > p.size()) return false;
        std::string eventName(reinterpret_cast<const char*>(p.data() + offset), nameLen);
        offset += nameLen;

        if (offset + sizeof(std::uint32_t) > p.size()) return false;
        std::uint32_t dataLen = 0;
        std::memcpy(&dataLen, p.data() + offset, sizeof(dataLen));
        offset += sizeof(dataLen);

        if (offset + dataLen > p.size()) return false;
        std::span<const std::byte> eventData(p.data() + offset, dataLen);
        offset += dataLen;

        onClientEvent_(entityId, eventName, eventData);
    }

    return true;
}

bool BaseRuntime::handleWitnessSync(const runtime::RuntimeInvocation& invocation) {
    if (!onWitnessSync_) return false;

    auto& p = invocation.payload;
    std::size_t offset = 0;
    if (offset + sizeof(runtime::EntityId) + sizeof(std::uint32_t) > p.size()) return false;

    runtime::EntityId observerId = 0;
    std::memcpy(&observerId, p.data() + offset, sizeof(observerId));
    offset += sizeof(observerId);

    std::uint32_t count = 0;
    std::memcpy(&count, p.data() + offset, sizeof(count));
    offset += sizeof(count);

    for (std::uint32_t i = 0; i < count; ++i) {
        if (offset + sizeof(runtime::EntityId) + sizeof(std::uint8_t) > p.size()) return false;
        runtime::EntityId targetEntityId = 0;
        std::memcpy(&targetEntityId, p.data() + offset, sizeof(targetEntityId));
        offset += sizeof(targetEntityId);

        std::uint8_t hasPos = 0;
        std::memcpy(&hasPos, p.data() + offset, sizeof(hasPos));
        offset += sizeof(hasPos);

        runtime::Vector3 position{};
        if (hasPos) {
            if (offset + sizeof(float) * 3 > p.size()) return false;
            std::memcpy(&position.x, p.data() + offset, sizeof(float)); offset += sizeof(float);
            std::memcpy(&position.y, p.data() + offset, sizeof(float)); offset += sizeof(float);
            std::memcpy(&position.z, p.data() + offset, sizeof(float)); offset += sizeof(float);
        }

        if (offset + sizeof(std::uint32_t) > p.size()) return false;
        std::uint32_t deltaLen = 0;
        std::memcpy(&deltaLen, p.data() + offset, sizeof(deltaLen));
        offset += sizeof(deltaLen);

        if (offset + deltaLen > p.size()) return false;
        std::span<const std::byte> propertyData(p.data() + offset, deltaLen);
        offset += deltaLen;

        onWitnessSync_(observerId, targetEntityId, propertyData, hasPos != 0, position);
    }

    return true;
}

bool BaseRuntime::handleSpaceChanged(const runtime::RuntimeInvocation& invocation) {
    // Payload: entityId(8) + spaceId(4) + posX(4) + posY(4) + posZ(4)
    auto& p = invocation.payload;
    constexpr std::size_t expectedSize = sizeof(runtime::EntityId) + sizeof(runtime::SpaceId)
                                       + sizeof(float) * 3;
    if (p.size() < expectedSize) return false;

    std::size_t offset = 0;
    runtime::EntityId entityId = 0;
    std::memcpy(&entityId, p.data() + offset, sizeof(entityId));
    offset += sizeof(entityId);

    runtime::SpaceId spaceId = 0;
    std::memcpy(&spaceId, p.data() + offset, sizeof(spaceId));
    offset += sizeof(spaceId);

    runtime::Vector3 position{};
    std::memcpy(&position.x, p.data() + offset, sizeof(float)); offset += sizeof(float);
    std::memcpy(&position.y, p.data() + offset, sizeof(float)); offset += sizeof(float);
    std::memcpy(&position.z, p.data() + offset, sizeof(float));

    if (onSpaceChange_) {
        onSpaceChange_(entityId, spaceId, position);
    }

    return true;
}

runtime::GroupManager& BaseRuntime::groupManager() {
    return groupManager_;
}

const runtime::GroupManager& BaseRuntime::groupManager() const {
    return groupManager_;
}

}  // namespace theseed::core
