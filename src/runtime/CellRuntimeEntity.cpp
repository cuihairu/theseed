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

bool CellRuntime::requestSpawnEntity(const std::string& entityType,
                                      const Vector3& position,
                                      EntityId creatorEntityId) {
    auto* entity = findEntity(creatorEntityId);
    if (!entity) return false;

    auto* baseCall = entity->baseEntityCall();
    if (!baseCall || !baseCall->isValid()) return false;  // LCOV_EXCL_BR_LINE isValid 假臂不可达（bind 恒有 target，clear 即空指针）

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

}  // namespace theseed::runtime
