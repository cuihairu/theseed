#include "theseed/runtime/CellRuntime.h"
#include "theseed/foundation/TimerWheel.h"

#include <array>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace theseed::runtime {

namespace {

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

}  // namespace

CellRuntime::IngressPump::IngressPump(CellRuntime& owner) : owner_(&owner) {}

void CellRuntime::IngressPump::tick(TickContext& context) {
    static_cast<void>(context);
    owner_->pumpInbound();
}

CellRuntime::FlushPump::FlushPump(CellRuntime& owner) : owner_(&owner) {}

void CellRuntime::FlushPump::tick(TickContext& context) {
    owner_->tick(context);
}

CellRuntime::CellRuntime(std::unique_ptr<SpaceRuntime> spaceRuntime,
                         std::shared_ptr<IRuntimeTransport> transport,
                         ComponentId localComponentId)
    : spaceRuntime_(std::move(spaceRuntime)),
      transport_(std::move(transport)),
      localComponentId_(localComponentId),
      ingressPump_(*this),
      flushPump_(*this),
      timerWheel_(std::make_unique<foundation::TimerWheel>()) {
    if (!spaceRuntime_) {
        throw std::invalid_argument("cell runtime requires space runtime");
    }
    if (!transport_) {
        throw std::invalid_argument("cell runtime requires transport");
    }
    if (localComponentId_ == 0) {
        throw std::invalid_argument("cell runtime requires non-zero local component id");
    }
}

CellRuntime::~CellRuntime() = default;

SpaceRuntime& CellRuntime::spaceRuntime() {
    return *spaceRuntime_;
}

const SpaceRuntime& CellRuntime::spaceRuntime() const {
    return *spaceRuntime_;
}

IRuntimeTransport& CellRuntime::transport() {
    return *transport_;
}

ComponentId CellRuntime::localComponentId() const {
    return localComponentId_;
}

void CellRuntime::attach(TickScheduler& scheduler) {
    spaceRuntime_->attach(scheduler);
    scheduler.registerTickable(TickPhase::Network, ingressPump_);
    scheduler.registerTickable(TickPhase::Flush, flushPump_);
}

void CellRuntime::detach(TickScheduler& scheduler) {
    static_cast<void>(scheduler.unregisterTickable(TickPhase::Network, ingressPump_));
    static_cast<void>(scheduler.unregisterTickable(TickPhase::Flush, flushPump_));
    spaceRuntime_->detach(scheduler);
}

void CellRuntime::addEntity(Entity& entity, const Vector3& position) {
    spaceRuntime_->addEntity(entity, position);
    entity.notifyEnterSpace(spaceRuntime_->space().id());
}

void CellRuntime::removeEntity(EntityId entityId) {
    auto* entity = findEntity(entityId);
    if (entity != nullptr) {
        entity->notifyLeaveSpace(spaceRuntime_->space().id());
    }
    ghostBindings_.erase(entityId);
    ownedEntities_.erase(entityId);
    spaceRuntime_->removeEntity(entityId);
}

Entity* CellRuntime::findEntity(EntityId entityId) const {
    return spaceRuntime_->space().findEntity(entityId);
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

    const auto position = spaceRuntime_->space().entityPosition(entityId);
    if (!position.has_value()) {
        return false;
    }

    const auto snapshot = EntityMigration::capture(*entity,
                                                   epoch,
                                                   localComponentId_,
                                                   targetComponent,
                                                   position,
                                                   spaceRuntime_->space().id());

    RuntimeInvocation invocation;
    invocation.entityId = entityId;
    invocation.targetComponent = targetComponent;
    invocation.entityType = entity->entityType();
    invocation.method = "migration.transfer";
    invocation.deliveryClass = DeliveryClass::ORDERED_RELIABLE;
    invocation.payload = EntityMigration::encode(snapshot);
    if (!transport_->send(std::move(invocation))) {
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

    return transport_->send(*invocation);
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

        return transport_->send(*forwarded);
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
    addEntity(*entityPtr, *snapshot.position);
    ownedEntities_.emplace(snapshot.entityId, std::move(entity));

    RuntimeInvocation commit;
    commit.entityId = snapshot.entityId;
    commit.targetComponent = snapshot.sourceComponent;
    commit.entityType = snapshot.entityType;
    commit.method = "migration.commit";
    commit.deliveryClass = DeliveryClass::ORDERED_RELIABLE;
    commit.payload = encodeMigrationEpoch(snapshot.epoch);
    return transport_->send(std::move(commit));
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
    return true;
}

bool CellRuntime::routeMigratingInvocation(const RuntimeInvocation& invocation) {
    const auto iter = migrationRoutes_.find(invocation.entityId);
    if (iter == migrationRoutes_.end()) {
        return false;
    }

    if (Clock::now() >= iter->second.expiry) {
        migrationRoutes_.erase(iter);
        return false;
    }

    RuntimeInvocation forwarded = invocation;
    forwarded.targetComponent = iter->second.targetComponent;
    return transport_->send(std::move(forwarded));
}

bool CellRuntime::applyGhostSync(const RuntimeInvocation& invocation) {
    if (invocation.method != "ghost.sync") {
        return false;
    }

    if (invocation.targetComponent != localComponentId_) {
        return false;
    }

    auto* entity = spaceRuntime_->space().findEntity(invocation.entityId);
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

void CellRuntime::tick(TickContext& context) {
    timerWheel_->advance(context.deltaTime);
    syncRealGhosts();
}

foundation::TimerHandle CellRuntime::addTimer(Duration delay, TimerCallback callback) {
    return timerWheel_->addTimer(delay, std::move(callback));
}

bool CellRuntime::cancelTimer(foundation::TimerHandle handle) {
    return timerWheel_->cancel(handle);
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

        const auto* delta = spaceRuntime_->findStagedDelta(entity->id());
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
        transport_->send(std::move(invocation));
    }
}

void CellRuntime::broadcastEvent(std::string_view event, std::span<const std::byte> data) {
    for (auto* entity : spaceRuntime_->space().entities()) {
        if (entity != nullptr && entity->state() == EntityState::Active) {
            entity->emit(event, data);
        }
    }
}

void CellRuntime::broadcastEventInRange(std::string_view event, const Vector3& center, float range,
                                         std::span<const std::byte> data) {
    for (auto* entity : spaceRuntime_->space().queryRange(center, range)) {
        if (entity != nullptr && entity->state() == EntityState::Active) {
            entity->emit(event, data);
        }
    }
}

}  // namespace theseed::runtime
