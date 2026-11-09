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

// 仅由 dispatchInvocation 调用（method 已保证为 "ghost.sync"）
bool CellRuntime::applyGhostSync(const RuntimeInvocation& invocation) {
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

void CellRuntime::syncRealGhosts() {
    for (auto& [entityId, binding] : ghostBindings_) {
        static_cast<void>(entityId);
        if (binding.manager == nullptr || !binding.manager->isReal() || !binding.manager->hasGhost()) {  // LCOV_EXCL_BR_LINE binding.manager 于 ensureRealGhost/ensureGhostProxy 恒即建即挂、条目仅整体 erase，null 短路臂不可达
            continue;
        }

        auto* entity = binding.manager->owner();
        if (entity == nullptr || entity->state() != EntityState::Active) {  // LCOV_EXCL_BR_LINE owner 由 attach(Entity&) 即设且 GhostManager::detach 全库无调用点，owner null 臂不可达
            continue;
        }

        auto spaceIt = entitySpaceMap_.find(entity->id());
        if (spaceIt == entitySpaceMap_.end()) {
            continue;
        }
        auto* sr = findSpaceRuntime(spaceIt->second);
        if (!sr) {
            continue;
        }
        const auto* staged = sr->findStagedDelta(entity->id());
        if (staged == nullptr || staged->empty()) {  // LCOV_EXCL_BR_LINE stageDirtyEntities 仅在 viewDelta 非空时 emplace 且每轮先 clear，非 null 蕴含非空
            continue;
        }

        RuntimeInvocation invocation;
        invocation.entityId = entity->id();
        invocation.targetComponent = binding.manager->ghostTarget();
        invocation.entityType = entity->entityType();
        invocation.method = "ghost.sync";
        invocation.deliveryClass = DeliveryClass::ORDERED_RELIABLE;
        invocation.payload = PropertyReplication::encodeDelta(*staged);
        pendingRuntimeSync_.push_back(std::move(invocation));
    }
}

}  // namespace theseed::runtime
