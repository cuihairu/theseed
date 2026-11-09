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
    // baseEntityCall 经 bind 恒有 target（isValid 恒真）、经 clearBaseEntityCall
    // 恒为空指针：isValid 假臂经公开 API 不可达，null 臂由测试覆盖。
    if (entity->baseEntityCall() && entity->baseEntityCall()->isValid()) {  // LCOV_EXCL_BR_LINE isValid 假臂不可达
        RuntimeInvocation spaceChanged;
        spaceChanged.entityId = entityId;
        spaceChanged.targetComponent = entity->baseEntityCall()->targetComponent();
        spaceChanged.entityType = entity->entityType();
        spaceChanged.method = "entity.spaceChanged";
        spaceChanged.deliveryClass = DeliveryClass::ORDERED_RELIABLE;
        // Payload: entityId(8) + spaceId(8, SpaceId) + posX(4) + posY(4) + posZ(4)
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
        if (!entity) continue;  // LCOV_EXCL_BR_LINE Space 名册成员取自 Entity& 引用，恒非空
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

}  // namespace theseed::runtime
