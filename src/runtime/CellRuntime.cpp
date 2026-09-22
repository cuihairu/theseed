#include "theseed/runtime/CellRuntime.h"
#include "theseed/foundation/Logger.h"
#include "theseed/foundation/MemoryStream.h"
#include "theseed/foundation/Metrics.h"
#include "theseed/foundation/TimerWheel.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace theseed::runtime {

namespace {

theseed::foundation::Counter& migrationRouteForwardsCounter() {
    return theseed::foundation::MetricsRegistry::instance().counter(
        "migration_route_forwards_total",
        "messages forwarded via migration routing window");
}

theseed::foundation::Counter& migrationRouteExpiredDropsCounter() {
    return theseed::foundation::MetricsRegistry::instance().counter(
        "migration_route_expired_drops_total",
        "messages dropped after migration route window expired");
}

std::vector<std::byte> encodeMigrationEpoch(MigrationEpoch epoch) {
    std::vector<std::byte> payload(sizeof(epoch));
    std::memcpy(payload.data(), &epoch, sizeof(epoch));
    return payload;
}

MigrationEpoch decodeMigrationEpoch(std::span<const std::byte> payload) {
    if (payload.size() != sizeof(MigrationEpoch)) {
        throw std::invalid_argument("migration epoch payload size mismatch");
    }

    MigrationEpoch epoch = 0;
    std::memcpy(&epoch, payload.data(), sizeof(epoch));
    return epoch;
}

// entity.createCell payload: spaceId(4) + entityId(8) + baseComponentId(4) + posX(4) + posY(4) + posZ(4)
struct CellCreationPayload {
    SpaceId spaceId = 0;
    EntityId entityId = 0;
    ComponentId baseComponentId = 0;
    float posX = 0;
    float posY = 0;
    float posZ = 0;
};

std::vector<std::byte> encodeCellCreation(EntityId entityId, ComponentId baseComponentId,
                                            const Vector3& position, SpaceId spaceId) {
    CellCreationPayload p;
    p.spaceId = spaceId;
    p.entityId = entityId;
    p.baseComponentId = baseComponentId;
    p.posX = position.x;
    p.posY = position.y;
    p.posZ = position.z;
    std::vector<std::byte> payload(sizeof(p));
    std::memcpy(payload.data(), &p, sizeof(p));
    return payload;
}

bool decodeCellCreation(std::span<const std::byte> payload, CellCreationPayload& out) {
    if (payload.size() < sizeof(CellCreationPayload)) return false;
    std::memcpy(&out, payload.data(), sizeof(out));
    return true;
}

// entity.cellReady payload: entityId(8) + cellComponentId(4) = 12
std::vector<std::byte> encodeCellReady(EntityId entityId, ComponentId cellComponentId) {
    std::vector<std::byte> payload(sizeof(EntityId) + sizeof(ComponentId));
    std::memcpy(payload.data(), &entityId, sizeof(entityId));
    std::memcpy(payload.data() + sizeof(entityId), &cellComponentId, sizeof(cellComponentId));
    return payload;
}

bool decodeCellReady(std::span<const std::byte> payload, EntityId& entityId, ComponentId& cellComponentId) {
    if (payload.size() != sizeof(EntityId) + sizeof(ComponentId)) return false;
    std::memcpy(&entityId, payload.data(), sizeof(entityId));
    std::memcpy(&cellComponentId, payload.data() + sizeof(entityId), sizeof(cellComponentId));
    return true;
}

// entity.destroyCell payload: entityId(8) + baseComponentId(4) = 12
// entity.cellDestroyed payload: entityId(8) = 8

}  // namespace

CellRuntime::IngressPump::IngressPump(CellRuntime& owner) : owner_(&owner) {}

void CellRuntime::IngressPump::tick(TickContext& context) {
    static_cast<void>(context);
    owner_->pumpInbound();
}

CellRuntime::TimerPump::TimerPump(CellRuntime& owner) : owner_(&owner) {}

void CellRuntime::TimerPump::tick(TickContext& context) {
    owner_->advanceTimers(context);
}

CellRuntime::SyncBuildPump::SyncBuildPump(CellRuntime& owner) : owner_(&owner) {}

void CellRuntime::SyncBuildPump::tick(TickContext& context) {
    owner_->buildSync(context);
}

CellRuntime::FlushPump::FlushPump(CellRuntime& owner) : owner_(&owner) {}

void CellRuntime::FlushPump::tick(TickContext& context) {
    static_cast<void>(context);
    owner_->flushRuntimeTransport();
}

CellRuntime::CellRuntime(std::unique_ptr<SpaceRuntime> spaceRuntime,
                         std::shared_ptr<IRuntimeTransport> transport,
                         ComponentId localComponentId)
    : defaultSpaceId_(spaceRuntime ? spaceRuntime->space().id() : 0),
      transport_(std::move(transport)),
      localComponentId_(localComponentId),
      ingressPump_(*this),
      timerPump_(*this),
      syncBuildPump_(*this),
      flushPump_(*this),
      timerWheel_(std::make_unique<foundation::TimerWheel>()) {
    if (!spaceRuntime) {
        throw std::invalid_argument("cell runtime requires space runtime");
    }
    if (!transport_) {
        throw std::invalid_argument("cell runtime requires transport");
    }
    if (localComponentId_ == 0) {
        throw std::invalid_argument("cell runtime requires non-zero local component id");
    }
    defaultSpaceId_ = spaceRuntime->space().id();
    spaceRuntimes_.emplace(defaultSpaceId_, std::move(spaceRuntime));
}

CellRuntime::~CellRuntime() = default;

SpaceRuntime& CellRuntime::spaceRuntime() {
    return *spaceRuntimes_.at(defaultSpaceId_);
}

const SpaceRuntime& CellRuntime::spaceRuntime() const {
    return *spaceRuntimes_.at(defaultSpaceId_);
}

IRuntimeTransport& CellRuntime::transport() {
    return *transport_;
}

ComponentId CellRuntime::localComponentId() const {
    return localComponentId_;
}

void CellRuntime::attach(TickScheduler& scheduler) {
    scheduler_ = &scheduler;
    scheduler.registerTickable(TickPhase::Network, ingressPump_);
    scheduler.registerTickable(TickPhase::Timer, timerPump_);
    for (auto& [id, sr] : spaceRuntimes_) {
        sr->attach(scheduler);
    }
    scheduler.registerTickable(TickPhase::SyncBuild, syncBuildPump_);
    scheduler.registerTickable(TickPhase::Flush, flushPump_);
}

void CellRuntime::detach(TickScheduler& scheduler) {
    scheduler_ = nullptr;
    static_cast<void>(scheduler.unregisterTickable(TickPhase::Network, ingressPump_));
    static_cast<void>(scheduler.unregisterTickable(TickPhase::Timer, timerPump_));
    static_cast<void>(scheduler.unregisterTickable(TickPhase::SyncBuild, syncBuildPump_));
    static_cast<void>(scheduler.unregisterTickable(TickPhase::Flush, flushPump_));
    for (auto& [id, sr] : spaceRuntimes_) {
        sr->detach(scheduler);
    }
}

void CellRuntime::addEntity(Entity& entity, const Vector3& position) {
    addEntity(entity, position, defaultSpaceId_);
}

void CellRuntime::addEntity(Entity& entity, const Vector3& position, SpaceId spaceId) {
    auto* sr = findSpaceRuntime(spaceId);
    if (!sr) return;
    sr->addEntity(entity, position);
    entitySpaceMap_[entity.id()] = spaceId;
    entity.notifyEnterSpace(sr->space().id());
}

void CellRuntime::removeEntity(EntityId entityId) {
    auto spaceIt = entitySpaceMap_.find(entityId);
    if (spaceIt != entitySpaceMap_.end()) {
        auto* entity = findEntity(entityId);
        if (entity != nullptr) {
            entity->notifyLeaveSpace(spaceIt->second);
        }
        auto* sr = findSpaceRuntime(spaceIt->second);
        if (sr) {
            sr->removeEntity(entityId);
        }
        entitySpaceMap_.erase(spaceIt);
    }
    ghostBindings_.erase(entityId);
    ownedEntities_.erase(entityId);
}

bool CellRuntime::teleportEntity(EntityId entityId, SpaceId targetSpaceId, const Vector3& position) {
    auto spaceIt = entitySpaceMap_.find(entityId);
    if (spaceIt == entitySpaceMap_.end()) return false;
    if (spaceIt->second == targetSpaceId) return false;

    auto* oldSr = findSpaceRuntime(spaceIt->second);
    auto* newSr = findSpaceRuntime(targetSpaceId);
    if (!oldSr || !newSr) return false;

    auto* entity = oldSr->space().findEntity(entityId);
    if (!entity) return false;

    oldSr->removeEntity(entityId);
    entity->notifyLeaveSpace(oldSr->space().id());

    newSr->addEntity(*entity, position);
    entitySpaceMap_[entityId] = targetSpaceId;
    entity->notifyEnterSpace(newSr->space().id());

    // Notify base of space change
    if (entity->baseEntityCall() && entity->baseEntityCall()->isValid()) {
        RuntimeInvocation spaceChanged;
        spaceChanged.entityId = entityId;
        spaceChanged.targetComponent = entity->baseEntityCall()->targetComponent();
        spaceChanged.entityType = entity->entityType();
        spaceChanged.method = "entity.spaceChanged";
        spaceChanged.deliveryClass = DeliveryClass::ORDERED_RELIABLE;
        // Payload: entityId(8) + spaceId(4) + posX(4) + posY(4) + posZ(4)
        std::vector<std::byte> payload(sizeof(EntityId) + sizeof(SpaceId) + sizeof(float) * 3);
        auto* p = payload.data();
        std::memcpy(p, &entityId, sizeof(EntityId)); p += sizeof(EntityId);
        std::memcpy(p, &targetSpaceId, sizeof(SpaceId)); p += sizeof(SpaceId);
        std::memcpy(p, &position.x, sizeof(float)); p += sizeof(float);
        std::memcpy(p, &position.y, sizeof(float)); p += sizeof(float);
        std::memcpy(p, &position.z, sizeof(float));
        spaceChanged.payload = std::move(payload);
        transport_->send(std::move(spaceChanged));
    }

    return true;
}

Entity* CellRuntime::findEntity(EntityId entityId) const {
    auto spaceIt = entitySpaceMap_.find(entityId);
    if (spaceIt == entitySpaceMap_.end()) return nullptr;
    auto* sr = findSpaceRuntime(spaceIt->second);
    if (!sr) return nullptr;
    return sr->space().findEntity(entityId);
}

void CellRuntime::forEachEntity(std::function<void(Entity&)> callback) const {
    for (auto& [id, entity] : ownedEntities_) {
        callback(*entity);
    }
}

bool CellRuntime::registerEntityFactory(std::string entityType, EntityFactory factory) {
    if (entityType.empty() || !factory) {
        return false;
    }

    entityFactories_.insert_or_assign(std::move(entityType), std::move(factory));
    return true;
}

bool CellRuntime::beginMigration(EntityId entityId,
                                 ComponentId targetComponent,
                                 MigrationEpoch epoch) {
    if (targetComponent == 0 || targetComponent == localComponentId_) {
        return false;
    }

    auto* entity = findEntity(entityId);
    if (entity == nullptr || entity->state() != EntityState::Active) {
        return false;
    }

    auto spaceIt = entitySpaceMap_.find(entityId);
    if (spaceIt == entitySpaceMap_.end()) return false;
    auto* sr = findSpaceRuntime(spaceIt->second);
    if (!sr) return false;

    const auto position = sr->space().entityPosition(entityId);
    if (!position.has_value()) {
        return false;
    }

    const auto snapshot = EntityMigration::capture(*entity,
                                                   epoch,
                                                   localComponentId_,
                                                   targetComponent,
                                                   position,
                                                   sr->space().id());

    RuntimeInvocation invocation;
    invocation.entityId = entityId;
    invocation.targetComponent = targetComponent;
    invocation.entityType = entity->entityType();
    invocation.method = "migration.transfer";
    invocation.deliveryClass = DeliveryClass::ORDERED_RELIABLE;
    invocation.payload = EntityMigration::encode(snapshot);
    if (transport_->send(std::move(invocation)) != SendResult::Accepted) {
        return false;
    }

    entity->beginMigration();
    migrationRoutes_[entityId] = MigrationRoute{
        .targetComponent = targetComponent,
        .epoch = epoch,
        .expiry = Clock::now() + std::chrono::seconds{10},
    };
    return true;
}

bool CellRuntime::clearMigrationRoute(EntityId entityId) {
    return migrationRoutes_.erase(entityId) > 0;
}

GhostManager& CellRuntime::ensureRealGhost(Entity& entity, ComponentId ghostTarget) {
    auto& binding = ghostBindings_[entity.id()];
    if (!binding.manager) {
        binding.manager = std::make_unique<GhostManager>();
        binding.manager->attach(entity);
    }

    binding.manager->setReal(localComponentId_);
    binding.manager->createGhost(ghostTarget);
    return *binding.manager;
}

GhostManager& CellRuntime::ensureGhostProxy(Entity& entity, ComponentId realTarget) {
    auto& binding = ghostBindings_[entity.id()];
    if (!binding.manager) {
        binding.manager = std::make_unique<GhostManager>();
        binding.manager->attach(entity);
    }

    binding.manager->setGhost(realTarget);
    return *binding.manager;
}

GhostManager* CellRuntime::findGhostManager(EntityId entityId) const {
    const auto iter = ghostBindings_.find(entityId);
    if (iter == ghostBindings_.end()) {
        return nullptr;
    }

    return iter->second.manager.get();
}

std::size_t CellRuntime::pumpInbound() {
    std::array<RuntimeInvocation, 32> batch{};
    std::size_t total = 0;
    while (true) {
        const auto count =
            transport_->receive(localComponentId_, batch.data(), batch.size());
        if (count == 0) {
            break;
        }

        for (std::size_t index = 0; index < count; ++index) {
            static_cast<void>(dispatchInvocation(batch[index]));
        }
        total += count;
    }

    return total;
}

bool CellRuntime::forwardGhostMethod(EntityId entityId,
                                     std::string method,
                                     std::span<const std::byte> payload) {
    auto* manager = findGhostManager(entityId);
    if (manager == nullptr) {
        return false;
    }

    const auto invocation = manager->forwardToReal(std::move(method), payload);
    if (!invocation.has_value()) {
        return false;
    }

    return transport_->send(*invocation) == SendResult::Accepted;
}

bool CellRuntime::dispatchInvocation(const RuntimeInvocation& invocation) {
    if (invocation.method == "ghost.sync") {
        return applyGhostSync(invocation);
    }
    if (invocation.method == "migration.transfer") {
        return applyMigrationTransfer(invocation);
    }
    if (invocation.method == "migration.commit") {
        return applyMigrationCommit(invocation);
    }
    if (invocation.method == "entity.createCell") {
        return handleCreateCell(invocation);
    }
    if (invocation.method == "entity.destroyCell") {
        return handleDestroyCell(invocation);
    }
    if (invocation.method == "property.syncToCell") {
        return handlePropertySyncFromBase(invocation);
    }
    if (invocation.method == "entity.action") {
        return handleEntityAction(invocation);
    }
    if (invocation.method == "entity.teleport") {
        return handleTeleport(invocation);
    }

    if (invocation.targetComponent != localComponentId_) {
        return false;
    }

    if (routeMigratingInvocation(invocation)) {
        return true;
    }

    auto* entity = findEntity(invocation.entityId);
    if (entity == nullptr) {
        return false;
    }

    auto* manager = findGhostManager(invocation.entityId);
    if (manager != nullptr && manager->isGhost()) {
        const auto forwarded = manager->forwardToReal(invocation.method, invocation.payload);
        if (!forwarded.has_value()) {
            return false;
        }

        return transport_->send(*forwarded) == SendResult::Accepted;
    }

    return entity->dispatchInvocation(invocation);
}

bool CellRuntime::applyMigrationTransfer(const RuntimeInvocation& invocation) {
    if (invocation.targetComponent != localComponentId_) {
        return false;
    }

    const auto snapshot = EntityMigration::decode(invocation.payload);
    if (snapshot.targetComponent != localComponentId_ || !snapshot.position.has_value()) {
        return false;
    }
    if (findEntity(snapshot.entityId) != nullptr) {
        return false;
    }

    const auto factoryIter = entityFactories_.find(snapshot.entityType);
    if (factoryIter == entityFactories_.end()) {
        return false;
    }

    auto entity = factoryIter->second(snapshot.entityId, snapshot.side);
    if (!entity) {
        return false;
    }

    auto* entityPtr = entity.get();
    EntityMigration::restore(*entityPtr, snapshot);
    entityPtr->setTransport(transport_.get());
    if (factoryHook_) factoryHook_(*entityPtr);
    auto targetSpaceId = snapshot.spaceId;
    if (!findSpaceRuntime(targetSpaceId)) {
        targetSpaceId = defaultSpaceId_;
    }
    addEntity(*entityPtr, *snapshot.position, targetSpaceId);
    ownedEntities_.emplace(snapshot.entityId, std::move(entity));

    // Notify base that entity is now on this CellApp
    if (snapshot.baseCall.has_value()) {
        RuntimeInvocation cellReady;
        cellReady.entityId = snapshot.entityId;
        cellReady.targetComponent = snapshot.baseCall->targetComponent;
        cellReady.entityType = snapshot.entityType;
        cellReady.method = "entity.cellReady";
        cellReady.deliveryClass = DeliveryClass::ORDERED_RELIABLE;
        cellReady.payload = encodeCellReady(snapshot.entityId, localComponentId_);
        transport_->send(std::move(cellReady));
    }

    RuntimeInvocation commit;
    commit.entityId = snapshot.entityId;
    commit.targetComponent = snapshot.sourceComponent;
    commit.entityType = snapshot.entityType;
    commit.method = "migration.commit";
    commit.deliveryClass = DeliveryClass::ORDERED_RELIABLE;
    commit.payload = encodeMigrationEpoch(snapshot.epoch);
    return transport_->send(std::move(commit)) == SendResult::Accepted;
}

bool CellRuntime::applyMigrationCommit(const RuntimeInvocation& invocation) {
    if (invocation.targetComponent != localComponentId_) {
        return false;
    }

    const auto iter = migrationRoutes_.find(invocation.entityId);
    if (iter == migrationRoutes_.end()) {
        return false;
    }

    const auto epoch = decodeMigrationEpoch(invocation.payload);
    if (iter->second.epoch != epoch) {
        return false;
    }

    auto* entity = findEntity(invocation.entityId);
    if (entity == nullptr || entity->state() != EntityState::Migrating) {
        return false;
    }

    entity->destroy();
    removeEntity(invocation.entityId);
    // Note: migrationRoutes_ is intentionally left in place. The route window
    // (default TTL 10s) keeps catching straggler messages addressed to the old
    // CellApp until either clearMigrationRoute() is called or the TTL expires.
    return true;
}

bool CellRuntime::routeMigratingInvocation(const RuntimeInvocation& invocation) {
    const auto iter = migrationRoutes_.find(invocation.entityId);
    if (iter == migrationRoutes_.end()) {
        return false;
    }

    if (Clock::now() >= iter->second.expiry) {
        // Per docs/design/2-replication-and-space/05-entity-migration.md section 3:
        // a message arriving after the route window closed means the routing table
        // upstream has not finished updating. Surface as warn so it is observable.
        migrationRouteExpiredDropsCounter().increment();
        theseed::foundation::logWarn("migration route window expired but message arrived",
            {{"entity_id", static_cast<std::int64_t>(invocation.entityId)},
             {"epoch", static_cast<std::int64_t>(iter->second.epoch)},
             {"method", invocation.method}});
        migrationRoutes_.erase(iter);
        return false;
    }

    RuntimeInvocation forwarded = invocation;
    forwarded.targetComponent = iter->second.targetComponent;
    if (transport_->send(std::move(forwarded)) == SendResult::Accepted) {
        migrationRouteForwardsCounter().increment();
        return true;
    }
    return false;
}

bool CellRuntime::applyGhostSync(const RuntimeInvocation& invocation) {
    if (invocation.method != "ghost.sync") {
        return false;
    }

    if (invocation.targetComponent != localComponentId_) {
        return false;
    }

    auto* entity = findEntity(invocation.entityId);
    if (entity == nullptr) {
        return false;
    }

    auto* manager = findGhostManager(invocation.entityId);
    if (manager == nullptr || !manager->isGhost()) {
        return false;
    }

    const auto deltas = PropertyReplication::decodeDelta(invocation.payload);
    entity->applyPropertyDelta(deltas);
    entity->clearDirtyFlags();
    return true;
}

bool CellRuntime::handleCreateCell(const RuntimeInvocation& invocation) {
    CellCreationPayload params;
    if (!decodeCellCreation(invocation.payload, params)) return false;

    const auto factoryIter = entityFactories_.find(invocation.entityType);
    if (factoryIter == entityFactories_.end()) return false;

    if (findEntity(params.entityId) != nullptr) return false;

    auto entity = factoryIter->second(params.entityId, EntitySide::Cell);
    if (!entity) return false;

    auto* entityPtr = entity.get();
    entityPtr->setTransport(transport_.get());
    entityPtr->setTimerScheduleFns(
        [this, params](Duration delay, Entity::EntityTimerCallback cb) {
            return addEntityTimer(params.entityId, delay, [cb = std::move(cb), this, params]() {
                auto* e = findEntity(params.entityId);
                if (e) cb(*e);
            });
        },
        [this, params](Duration interval, Entity::EntityTimerCallback cb) {
            return addEntityPeriodicTimer(params.entityId, interval, [cb = std::move(cb), this, params]() {
                auto* e = findEntity(params.entityId);
                if (e) cb(*e);
            });
        });
    entityPtr->activate();
    entityPtr->notifyCreate();
    if (factoryHook_) factoryHook_(*entityPtr);
    entityPtr->bindBaseEntityCall(params.baseComponentId);

    // Apply initial property snapshot from base (if present)
    if (invocation.payload.size() > sizeof(CellCreationPayload)) {
        try {
            auto snapshotSpan = std::span<const std::byte>(
                invocation.payload.data() + sizeof(CellCreationPayload),
                invocation.payload.size() - sizeof(CellCreationPayload));
            auto deltas = PropertyReplication::decodeDelta(snapshotSpan);
            entityPtr->applyPropertyDelta(deltas);
            entityPtr->clearDirtyFlags();
        } catch (...) {
            // Ignore malformed snapshot
        }
    }

    Vector3 position{params.posX, params.posY, params.posZ};

    // Ensure target space exists
    if (params.spaceId != 0 && !findSpaceRuntime(params.spaceId)) {
        createSpace(params.spaceId, "space_" + std::to_string(params.spaceId));
    }
    auto targetSpace = params.spaceId != 0 ? params.spaceId : defaultSpaceId_;
    addEntity(*entityPtr, position, targetSpace);
    ownedEntities_.emplace(params.entityId, std::move(entity));

    // Notify base that cell is ready
    RuntimeInvocation ready;
    ready.entityId = params.entityId;
    ready.targetComponent = params.baseComponentId;
    ready.entityType = invocation.entityType;
    ready.method = "entity.cellReady";
    ready.deliveryClass = DeliveryClass::ORDERED_RELIABLE;
    ready.payload = encodeCellReady(params.entityId, localComponentId_);
    return transport_->send(std::move(ready)) == SendResult::Accepted;
}

bool CellRuntime::handleDestroyCell(const RuntimeInvocation& invocation) {
    if (invocation.payload.size() < sizeof(EntityId)) return false;

    auto* entity = findEntity(invocation.entityId);
    if (entity == nullptr) return false;

    ComponentId baseComponentId = 0;
    if (invocation.payload.size() >= sizeof(EntityId) + sizeof(ComponentId)) {
        std::memcpy(&baseComponentId, invocation.payload.data() + sizeof(EntityId), sizeof(ComponentId));
    }

    entity->beginDestroy();
    entity->notifyDestroy();
    cancelEntityTimers(invocation.entityId);
    entity->destroy();
    removeEntity(invocation.entityId);

    if (baseComponentId != 0) {
        RuntimeInvocation destroyed;
        destroyed.entityId = invocation.entityId;
        destroyed.targetComponent = baseComponentId;
        destroyed.entityType = invocation.entityType;
        destroyed.method = "entity.cellDestroyed";
        destroyed.deliveryClass = DeliveryClass::ORDERED_RELIABLE;

        std::vector<std::byte> payload(sizeof(EntityId));
        std::memcpy(payload.data(), &invocation.entityId, sizeof(EntityId));
        destroyed.payload = std::move(payload);
        static_cast<void>(transport_->send(std::move(destroyed)));
    }

    return true;
}

bool CellRuntime::handlePropertySyncFromBase(const RuntimeInvocation& invocation) {
    if (invocation.payload.empty()) return false;

    auto* entity = findEntity(invocation.entityId);
    if (!entity) return false;

    try {
        auto deltas = PropertyReplication::decodeDelta(invocation.payload);
        entity->applyPropertyDelta(deltas, PropertyDirtyTarget::View
                                           | PropertyDirtyTarget::Client);
        return true;
    } catch (...) {
        return false;
    }
}

bool CellRuntime::handleEntityAction(const RuntimeInvocation& invocation) {
    auto& p = invocation.payload;
    std::size_t offset = 0;
    if (offset + 4 > p.size()) return false;

    std::uint32_t nameLen = 0;
    std::memcpy(&nameLen, p.data() + offset, 4);
    offset += 4;
    if (offset + nameLen > p.size()) return false;
    std::string actionName(reinterpret_cast<const char*>(p.data() + offset), nameLen);
    offset += nameLen;

    if (offset + 4 > p.size()) return false;
    std::uint32_t dataLen = 0;
    std::memcpy(&dataLen, p.data() + offset, 4);
    offset += 4;
    if (offset + dataLen > p.size()) return false;
    std::vector<std::byte> actionData(dataLen);
    if (dataLen > 0) {
        std::memcpy(actionData.data(), p.data() + offset, dataLen);
    }

    auto* entity = findEntity(invocation.entityId);
    if (!entity || !entity->isActive()) return false;

    entity->pushInput(Entity::InputAction{
        .name = std::move(actionName),
        .payload = std::move(actionData),
    });
    return true;
}

bool CellRuntime::handleTeleport(const RuntimeInvocation& invocation) {
    // Payload: entityId(8) + spaceId(4) + posX(4) + posY(4) + posZ(4)
    auto& p = invocation.payload;
    if (p.size() < sizeof(EntityId) + sizeof(SpaceId) + sizeof(float) * 3) return false;

    std::size_t offset = 0;
    EntityId entityId = 0;
    std::memcpy(&entityId, p.data() + offset, sizeof(EntityId)); offset += sizeof(EntityId);
    SpaceId spaceId = 0;
    std::memcpy(&spaceId, p.data() + offset, sizeof(SpaceId)); offset += sizeof(SpaceId);
    Vector3 position{};
    std::memcpy(&position.x, p.data() + offset, sizeof(float)); offset += sizeof(float);
    std::memcpy(&position.y, p.data() + offset, sizeof(float)); offset += sizeof(float);
    std::memcpy(&position.z, p.data() + offset, sizeof(float));

    // Ensure target space exists
    if (!findSpaceRuntime(spaceId)) {
        createSpace(spaceId, "space_" + std::to_string(spaceId));
    }

    return teleportEntity(entityId, spaceId, position);
}

bool CellRuntime::requestSpawnEntity(const std::string& entityType,
                                      const Vector3& position,
                                      EntityId creatorEntityId) {
    auto* entity = findEntity(creatorEntityId);
    if (!entity) return false;

    auto* baseCall = entity->baseEntityCall();
    if (!baseCall || !baseCall->isValid()) return false;

    // payload: typeLen(4) + type(N) + posX(4) + posY(4) + posZ(4)
    auto typeLen = static_cast<std::uint32_t>(entityType.size());
    std::vector<std::byte> payload(4 + typeLen + sizeof(float) * 3);
    auto* p = payload.data();
    std::memcpy(p, &typeLen, 4); p += 4;
    if (typeLen > 0) {
        std::memcpy(p, entityType.data(), typeLen);
    }
    p += typeLen;
    std::memcpy(p, &position.x, sizeof(float)); p += sizeof(float);
    std::memcpy(p, &position.y, sizeof(float)); p += sizeof(float);
    std::memcpy(p, &position.z, sizeof(float));

    return baseCall->call(*transport_, "entity.spawnRequest",
                          std::span<const std::byte>(payload.data(), payload.size()))
           == SendResult::Accepted;
}

SpaceRuntime* CellRuntime::findSpaceRuntime(SpaceId id) const {
    auto it = spaceRuntimes_.find(id);
    if (it == spaceRuntimes_.end()) return nullptr;
    return it->second.get();
}

SpaceId CellRuntime::findEntitySpace(EntityId id) const {
    auto it = entitySpaceMap_.find(id);
    if (it == entitySpaceMap_.end()) return 0;
    return it->second;
}

bool CellRuntime::createSpace(SpaceId id, std::string name) {
    if (spaceRuntimes_.contains(id)) return false;

    auto space = std::make_unique<Space>(id, std::move(name),
                                          std::make_unique<SingleCellTopology>(1));
    space->initialize();
    auto sr = std::make_unique<SpaceRuntime>(std::move(space));

    if (scheduler_) {
        sr->attach(*scheduler_);
        static_cast<void>(scheduler_->unregisterTickable(TickPhase::SyncBuild, syncBuildPump_));
        scheduler_->registerTickable(TickPhase::SyncBuild, syncBuildPump_);
    }

    spaceRuntimes_.emplace(id, std::move(sr));
    return true;
}

bool CellRuntime::destroySpace(SpaceId id) {
    if (id == defaultSpaceId_) return false;

    auto it = spaceRuntimes_.find(id);
    if (it == spaceRuntimes_.end()) return false;

    auto& sr = it->second;

    // Destroy all entities in this space
    auto entitiesInSpace = sr->space().entities();
    for (auto* entity : entitiesInSpace) {
        if (!entity) continue;
        entity->beginDestroy();
        entity->notifyDestroy();
        cancelEntityTimers(entity->id());
        entity->destroy();
        entitySpaceMap_.erase(entity->id());
        ghostBindings_.erase(entity->id());
        ownedEntities_.erase(entity->id());
    }

    if (scheduler_) {
        sr->detach(*scheduler_);
    }

    spaceRuntimes_.erase(it);
    return true;
}

void CellRuntime::advanceTimers(TickContext& context) {
    timerWheel_->advance(context.deltaTime);
}

void CellRuntime::buildSync(TickContext& context) {
    static_cast<void>(context);
    flushAoIEvents();
    syncToBases();
    flushClientEvents();
    flushWitnessSync();
    syncRealGhosts();
}

void CellRuntime::tick(TickContext& context) {
    advanceTimers(context);
    buildSync(context);
    flushRuntimeTransport();
}

void CellRuntime::flushRuntimeTransport() {
    for (auto& invocation : pendingRuntimeSync_) {
        static_cast<void>(transport_->send(std::move(invocation)));
    }
    pendingRuntimeSync_.clear();
    transport_->flush();
}

foundation::TimerHandle CellRuntime::addTimer(Duration delay, TimerCallback callback) {
    return timerWheel_->addTimer(delay, std::move(callback));
}

foundation::TimerHandle CellRuntime::addEntityTimer(EntityId entityId, Duration delay, TimerCallback callback) {
    auto handle = timerWheel_->addTimer(delay, std::move(callback));
    entityTimers_[entityId].push_back(handle);
    return handle;
}

foundation::TimerHandle CellRuntime::addEntityPeriodicTimer(EntityId entityId, Duration interval, TimerCallback callback) {
    auto handle = timerWheel_->addPeriodic(interval, std::move(callback));
    entityTimers_[entityId].push_back(handle);
    return handle;
}

bool CellRuntime::cancelTimer(foundation::TimerHandle handle) {
    return timerWheel_->cancel(handle);
}

void CellRuntime::cancelEntityTimers(EntityId entityId) {
    auto it = entityTimers_.find(entityId);
    if (it == entityTimers_.end()) return;
    for (auto& handle : it->second) {
        timerWheel_->cancel(handle);
    }
    entityTimers_.erase(it);
}

void CellRuntime::syncRealGhosts() {
    for (auto& [entityId, binding] : ghostBindings_) {
        static_cast<void>(entityId);
        if (binding.manager == nullptr || !binding.manager->isReal() || !binding.manager->hasGhost()) {
            continue;
        }

        auto* entity = binding.manager->owner();
        if (entity == nullptr || entity->state() != EntityState::Active) {
            continue;
        }

        const auto* delta = [&]() -> const std::vector<PropertyDelta>* {
            auto spaceIt = entitySpaceMap_.find(entity->id());
            if (spaceIt == entitySpaceMap_.end()) return nullptr;
            auto* sr = findSpaceRuntime(spaceIt->second);
            if (!sr) return nullptr;
            return sr->findStagedDelta(entity->id());
        }();
        if (delta == nullptr || delta->empty()) {
            continue;
        }

        RuntimeInvocation invocation;
        invocation.entityId = entity->id();
        invocation.targetComponent = binding.manager->ghostTarget();
        invocation.entityType = entity->entityType();
        invocation.method = "ghost.sync";
        invocation.deliveryClass = DeliveryClass::ORDERED_RELIABLE;
        invocation.payload = PropertyReplication::encodeDelta(*delta);
        pendingRuntimeSync_.push_back(std::move(invocation));
    }
}

void CellRuntime::syncToBases() {
    for (auto& [entityId, entityPtr] : ownedEntities_) {
        auto* entity = entityPtr.get();
        if (entity == nullptr || entity->state() != EntityState::Active) continue;

        auto* baseCall = entity->baseEntityCall();
        if (baseCall == nullptr || !baseCall->isValid()) continue;

        auto* sr = findSpaceRuntime(findEntitySpace(entityId));
        if (sr == nullptr) continue;

        auto deltas = sr->findStagedDelta(entityId, PropertyFlag::Cell);
        if (deltas.empty()) continue;

        RuntimeInvocation invocation;
        invocation.entityId = entity->id();
        invocation.targetComponent = baseCall->targetComponent();
        invocation.entityType = entity->entityType();
        invocation.method = "property.syncToBase";
        invocation.deliveryClass = DeliveryClass::ORDERED_RELIABLE;
        invocation.payload = PropertyReplication::encodeDelta(deltas);

        pendingRuntimeSync_.push_back(std::move(invocation));
    }
}

void CellRuntime::flushAoIEvents() {
    std::vector<AoIEvent> events;
    for (auto& [id, sr] : spaceRuntimes_) {
        auto spaceEvents = sr->collectAoIEvents();
        events.insert(events.end(), std::make_move_iterator(spaceEvents.begin()),
                      std::make_move_iterator(spaceEvents.end()));
    }
    for (auto& event : events) {
        auto* observer = findEntity(event.observerId);
        if (!observer) continue;
        auto* baseCall = observer->baseEntityCall();
        if (!baseCall || !baseCall->isValid()) continue;

        if (event.type == AoIEventType::Enter) {
            auto* target = findEntity(event.targetId);
            if (!target) continue;

            foundation::MemoryStream ms;
            ms.writeUint64(event.observerId);
            ms.writeUint64(event.targetId);
            auto& typeName = target->entityType();
            ms.writeUint32(static_cast<std::uint32_t>(typeName.size()));
            if (!typeName.empty()) {
                ms.writeBytes(reinterpret_cast<const std::byte*>(typeName.data()),
                              typeName.size());
            }
            // Position
            if (target->hasPosition()) {
                auto pos = target->position();
                ms.writeUint8(1);
                std::byte fbuf[sizeof(float)];
                std::memcpy(fbuf, &pos.x, sizeof(float));
                ms.writeBytes(fbuf, sizeof(float));
                std::memcpy(fbuf, &pos.y, sizeof(float));
                ms.writeBytes(fbuf, sizeof(float));
                std::memcpy(fbuf, &pos.z, sizeof(float));
                ms.writeBytes(fbuf, sizeof(float));
            } else {
                ms.writeUint8(0);
            }

            RuntimeInvocation inv;
            inv.entityId = event.observerId;
            inv.targetComponent = baseCall->targetComponent();
            inv.entityType = observer->entityType();
            inv.method = "aoi.enter";
            inv.deliveryClass = DeliveryClass::ORDERED_RELIABLE;
            inv.payload = std::vector<std::byte>(ms.data(), ms.data() + ms.size());
            pendingRuntimeSync_.push_back(std::move(inv));
        } else {
            std::vector<std::byte> payload(sizeof(EntityId) * 2);
            std::memcpy(payload.data(), &event.observerId, sizeof(EntityId));
            std::memcpy(payload.data() + sizeof(EntityId), &event.targetId, sizeof(EntityId));

            RuntimeInvocation inv;
            inv.entityId = event.observerId;
            inv.targetComponent = baseCall->targetComponent();
            inv.entityType = observer->entityType();
            inv.method = "aoi.leave";
            inv.deliveryClass = DeliveryClass::ORDERED_RELIABLE;
            inv.payload = std::move(payload);
            pendingRuntimeSync_.push_back(std::move(inv));
        }
    }
}

void CellRuntime::flushClientEvents() {
    for (auto& [entityId, entityPtr] : ownedEntities_) {
        auto* entity = entityPtr.get();
        if (!entity || entity->state() != EntityState::Active) continue;

        auto* baseCall = entity->baseEntityCall();
        if (!baseCall || !baseCall->isValid()) continue;

        auto events = entity->flushClientEvents();
        if (events.empty()) continue;

        // Payload: entityId(8) + count(4) + [nameLen(4) + name + dataLen(4) + data]...
        foundation::MemoryStream ms;
        ms.writeUint64(entity->id());
        ms.writeUint32(static_cast<std::uint32_t>(events.size()));
        for (auto& ev : events) {
            auto nameLen = static_cast<std::uint32_t>(ev.name.size());
            ms.writeUint32(nameLen);
            if (nameLen > 0) {
                ms.writeBytes(reinterpret_cast<const std::byte*>(ev.name.data()), nameLen);
            }
            auto dataLen = static_cast<std::uint32_t>(ev.data.size());
            ms.writeUint32(dataLen);
            if (dataLen > 0) {
                ms.writeBytes(ev.data.data(), dataLen);
            }
        }

        RuntimeInvocation inv;
        inv.entityId = entity->id();
        inv.targetComponent = baseCall->targetComponent();
        inv.entityType = entity->entityType();
        inv.method = "entity.clientEvent";
        inv.deliveryClass = DeliveryClass::UNORDERED_LOSSY;
        inv.payload = std::vector<std::byte>(ms.data(), ms.data() + ms.size());
        pendingRuntimeSync_.push_back(std::move(inv));
    }
}

void CellRuntime::flushWitnessSync() {
    for (auto& [spaceId, sr] : spaceRuntimes_) {
        static_cast<void>(spaceId);
        auto observerDeltas = sr->collectWitnessDeltas();
        for (auto& od : observerDeltas) {
            auto* observer = findEntity(od.observerId);
            if (!observer) continue;
            auto* baseCall = observer->baseEntityCall();
            if (!baseCall || !baseCall->isValid()) continue;

            // Payload: observerId(8) + count(4) + [targetEntityId(8) + hasPos(1) + [posX(4)+posY(4)+posZ(4)] + deltaLen(4) + delta(N)]...
            foundation::MemoryStream ms;
            ms.writeUint64(od.observerId);
            ms.writeUint32(static_cast<std::uint32_t>(od.deltas.size()));
            for (auto& delta : od.deltas) {
                ms.writeUint64(delta.entityId);
                bool hasPosition = delta.position.has_value();
                ms.writeUint8(hasPosition ? 1 : 0);
                if (hasPosition) {
                    std::byte fbuf[sizeof(float)];
                    std::memcpy(fbuf, &delta.position->x, sizeof(float));
                    ms.writeBytes(fbuf, sizeof(float));
                    std::memcpy(fbuf, &delta.position->y, sizeof(float));
                    ms.writeBytes(fbuf, sizeof(float));
                    std::memcpy(fbuf, &delta.position->z, sizeof(float));
                    ms.writeBytes(fbuf, sizeof(float));
                }
                auto encoded = PropertyReplication::encodeDelta(delta.properties);
                ms.writeUint32(static_cast<std::uint32_t>(encoded.size()));
                if (!encoded.empty()) {
                    ms.writeBytes(encoded.data(), encoded.size());
                }
            }

            RuntimeInvocation inv;
            inv.entityId = od.observerId;
            inv.targetComponent = baseCall->targetComponent();
            inv.entityType = observer->entityType();
            inv.method = "witness.propertySync";
            inv.deliveryClass = DeliveryClass::UNORDERED_LOSSY;
            inv.payload = std::vector<std::byte>(ms.data(), ms.data() + ms.size());
            pendingRuntimeSync_.push_back(std::move(inv));
        }
    }
}

std::vector<Entity*> CellRuntime::findEntitiesByTag(const std::string& tag) const {
    std::vector<Entity*> result;
    for (auto& [id, entity] : ownedEntities_) {
        if (entity->hasTag(tag)) {
            result.push_back(entity.get());
        }
    }
    return result;
}

std::vector<Entity*> CellRuntime::queryEntities(std::function<bool(const Entity&)> predicate) const {
    std::vector<Entity*> result;
    for (auto& [id, entity] : ownedEntities_) {
        if (predicate(*entity)) {
            result.push_back(entity.get());
        }
    }
    return result;
}

void CellRuntime::setEntityFactoryHook(EntityFactoryHook hook) {
    factoryHook_ = std::move(hook);
}

GroupManager& CellRuntime::groupManager() {
    return groupManager_;
}

const GroupManager& CellRuntime::groupManager() const {
    return groupManager_;
}

void CellRuntime::broadcastEvent(std::string_view event, std::span<const std::byte> data) {
    for (auto& [id, sr] : spaceRuntimes_) {
        for (auto* entity : sr->space().entities()) {
            if (entity != nullptr && entity->state() == EntityState::Active) {
                entity->emit(event, data);
            }
        }
    }
}

void CellRuntime::broadcastEventInRange(std::string_view event, const Vector3& center, float range,
                                         std::span<const std::byte> data) {
    for (auto& [id, sr] : spaceRuntimes_) {
        for (auto* entity : sr->space().queryRange(center, range)) {
            if (entity != nullptr && entity->state() == EntityState::Active) {
                entity->emit(event, data);
            }
        }
    }
}

}  // namespace theseed::runtime
