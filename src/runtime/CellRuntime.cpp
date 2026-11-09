#include "theseed/runtime/CellRuntime.h"
#include "theseed/foundation/Logger.h"
#include "theseed/foundation/Metrics.h"
#include "theseed/foundation/TimerWheel.h"

#include <array>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <utility>

namespace theseed::runtime {

namespace {

theseed::foundation::Counter& dispatchExceptionCounter() {
    return theseed::foundation::MetricsRegistry::instance().counter(
        "cell_dispatch_exceptions_total",
        "invocations whose handler threw and was isolated");
}

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
    // Phase C-2：实体级负载剖面在 Flush 阶段推进 EMA（每 tick 一次）。
    // 放在 FlushPump（scheduler 注册的 tickable）里，确保生产 app 主循环
    // scheduler.runOnce() 真实驱动剖面，而非依赖手动 runtime.tick() 约定。
    owner_->advanceLoadProfiler();
}

CellRuntime::CellRuntime(std::unique_ptr<SpaceRuntime> spaceRuntime,
                         std::shared_ptr<IRuntimeTransport> transport,
                         ComponentId localComponentId,
                         Duration migrationRouteTtl)
    : defaultSpaceId_(spaceRuntime ? spaceRuntime->space().id() : 0),
      transport_(std::move(transport)),
      localComponentId_(localComponentId),
      ingressPump_(*this),
      timerPump_(*this),
      syncBuildPump_(*this),
      flushPump_(*this),
      timerWheel_(std::make_unique<foundation::TimerWheel>()) {
    migrationRouteTtl_ = migrationRouteTtl;
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

GroupManager& CellRuntime::groupManager() {
    return groupManager_;
}

const GroupManager& CellRuntime::groupManager() const {
    return groupManager_;
}

EntityLoadProfiler& CellRuntime::loadProfiler() noexcept {
    return loadProfiler_;
}

const EntityLoadProfiler& CellRuntime::loadProfiler() const noexcept {
    return loadProfiler_;
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

std::size_t CellRuntime::pumpInbound() {
    std::array<RuntimeInvocation, 32> batch{};  // LCOV_EXCL_BR_LINE std::array 聚合初始化的 gcc 内联副本边，与运行时取值无关
    std::size_t total = 0;
    while (true) {
        const auto count =
            transport_->receive(localComponentId_, batch.data(), batch.size());
        if (count == 0) {
            break;
        }

        for (std::size_t index = 0; index < count; ++index) {
            // P2 深审：handler 可抛异常（用户方法/流回调）。无隔离时异常
            // 逃逸 pumpInbound，杀死整个 tick 循环/进程。逐条隔离：坏 handler
            // 只丢本条，其余消息照常分发，计数上抛给观测。
            try {
                static_cast<void>(dispatchInvocation(batch[index]));
            } catch (const std::exception& e) {
                dispatchExceptionCounter().increment();
                foundation::logWarn("cell dispatch isolated: " + std::string(e.what()));
            } catch (...) {
                dispatchExceptionCounter().increment();
                foundation::logWarn("cell dispatch isolated: unknown exception");
            }
        }
        total += count;
    }

    return total;
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
        // forwardToReal 仅在 !isGhost() 时返回 nullopt，此处恒有值
        const auto forwarded = manager->forwardToReal(invocation.method, invocation.payload);
        return transport_->send(*forwarded) == SendResult::Accepted;
    }

    // 实体级负载剖面（Phase C-2）：用 Scope 包裹实际分发，把该实体本 tick
    // 的分发耗时记入 EntityLoadProfiler。findEntity 已保证实体存在，故
    // scope 用 entity->entityType() 作为类型标签。
    const auto profilerScope = loadProfiler_.scope(invocation.entityId, entity->entityType());
    return entity->dispatchInvocation(invocation);
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

void CellRuntime::advanceLoadProfiler() {
    // 每个 tick 末尾推进 EMA：把刚完成的分发负载从 currentRawLoad 提升为
    // lastRawLoad 并平滑，供 snapshot()/all() 读取（见 EntityLoadProfiler::tick）。
    loadProfiler_.tick();
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

}  // namespace theseed::runtime
