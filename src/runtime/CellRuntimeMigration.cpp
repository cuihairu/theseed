#include "theseed/runtime/CellRuntime.h"
#include "theseed/foundation/Logger.h"
#include "theseed/foundation/MemoryStream.h"
#include "theseed/foundation/Metrics.h"
#include "theseed/foundation/TimerWheel.h"

#include "CellRuntimeDetail.h"

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

}  // namespace

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

    // findEntity 非空已蕴含 entitySpaceMap_ 有记录且空间运行时存在（L243-247
    // 同一查找链），单线程 dispatch 内两条防御臂均不可达。
    auto spaceIt = entitySpaceMap_.find(entityId);
    if (spaceIt == entitySpaceMap_.end()) return false;  // LCOV_EXCL_BR_LINE 前置 findEntity 已保证命中
    auto* sr = findSpaceRuntime(spaceIt->second);
    if (!sr) return false;  // LCOV_EXCL_BR_LINE 前置 findEntity 已保证空间存在

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
        .expiry = Clock::now() + migrationRouteTtl_,
    };
    return true;
}

bool CellRuntime::clearMigrationRoute(EntityId entityId) {
    return migrationRoutes_.erase(entityId) > 0;
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
        cellReady.payload = detail::encodeCellReady(snapshot.entityId, localComponentId_);
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
    // (constructor-configurable TTL, default 10s) keeps catching straggler
    // messages addressed to the old CellApp until either clearMigrationRoute()
    // is called or the TTL expires.
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
        std::vector<theseed::foundation::LogAttribute> attrs;
        attrs.push_back({"entity_id", static_cast<std::int64_t>(invocation.entityId)});
        attrs.push_back({"epoch", static_cast<std::int64_t>(iter->second.epoch)});
        attrs.push_back({"method", invocation.method});
        theseed::foundation::logWarn("migration route window expired but message arrived",
                                     std::move(attrs));
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
}  // LCOV_EXCL_BR_LINE 函数尾聚合：闭括号上的 RuntimeInvocation 析构/汇合边为 gcc 布局伪影

}  // namespace theseed::runtime
