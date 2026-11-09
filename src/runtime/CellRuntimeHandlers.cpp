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

// entity.createCell payload: spaceId(8, SpaceId) + entityId(8) + baseComponentId(4) + posX(4) + posY(4) + posZ(4)
struct CellCreationPayload {
    SpaceId spaceId = 0;
    EntityId entityId = 0;
    ComponentId baseComponentId = 0;
    float posX = 0;
    float posY = 0;
    float posZ = 0;
};

bool decodeCellCreation(std::span<const std::byte> payload, CellCreationPayload& out) {
    if (payload.size() < sizeof(CellCreationPayload)) return false;
    std::memcpy(&out, payload.data(), sizeof(out));
    return true;
}

}  // namespace

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
            return addEntityTimer(params.entityId, delay, [cb = std::move(cb), this, params]() {  // LCOV_EXCL_BR_LINE lambda 闭包经 std::function 装箱转发的 gcc 内联副本边，业务臂（L593 if(e)）已双臂覆盖
                auto* e = findEntity(params.entityId);
                if (e) cb(*e);
            });
        },
        [this, params](Duration interval, Entity::EntityTimerCallback cb) {
            return addEntityPeriodicTimer(params.entityId, interval, [cb = std::move(cb), this, params]() {  // LCOV_EXCL_BR_LINE 同上：定时器闭包装箱内联副本边，业务臂已双臂覆盖
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
    ready.payload = detail::encodeCellReady(params.entityId, localComponentId_);
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

    entity->pushInput(Entity::InputAction{  // LCOV_EXCL_BR_LINE 聚合初始化成员移动构造的 noexcept 分支副本边（std::string/vector 内联），非业务条件
        .name = std::move(actionName),
        .payload = std::move(actionData),
    });
    return true;
}

bool CellRuntime::handleTeleport(const RuntimeInvocation& invocation) {
    // Payload: entityId(8) + spaceId(8, SpaceId) + posX(4) + posY(4) + posZ(4)
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

}  // namespace theseed::runtime
