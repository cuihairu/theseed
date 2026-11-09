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

void CellRuntime::syncToBases() {
    for (auto& [entityId, entityPtr] : ownedEntities_) {
        auto* entity = entityPtr.get();
        if (entity == nullptr || entity->state() != EntityState::Active) continue;  // LCOV_EXCL_BR_LINE ownedEntities_ 存 unique_ptr 且移除即整条擦除，get() null 臂不可达

        auto* baseCall = entity->baseEntityCall();
        if (baseCall == nullptr || !baseCall->isValid()) continue;  // LCOV_EXCL_BR_LINE isValid 假臂不可达

        auto* sr = findSpaceRuntime(findEntitySpace(entityId));
        // ownedEntities_ 与 entitySpaceMap_ 同步维护（addEntity/迁移先入空间，
        // destroySpace 连带清除两表），owned 在册时空间必存在。
        if (sr == nullptr) continue;  // LCOV_EXCL_BR_LINE 同步不变式保证空间存在

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
        if (!observer) continue;  // LCOV_EXCL_BR_LINE AoI 事件产自 witnesses_ 名册，binding 与 ownedEntities_ 经 removeEntity 同步清除，观察者悬垂不可构造
        auto* baseCall = observer->baseEntityCall();
        if (!baseCall || !baseCall->isValid()) continue;  // LCOV_EXCL_BR_LINE isValid 假臂不可达

        if (event.type == AoIEventType::Enter) {
            auto* target = findEntity(event.targetId);
            if (!target) continue;  // LCOV_EXCL_BR_LINE findEntity 内联副本边伪影归因本行：Enter 命中臂（P7 aoi.enter）与 target 缺失早退臂（Q 场景）均已覆盖

            foundation::MemoryStream ms;
            ms.writeUint64(event.observerId);
            ms.writeUint64(event.targetId);
            auto& typeName = target->entityType();
            ms.writeUint32(static_cast<std::uint32_t>(typeName.size()));
            if (!typeName.empty()) {
                ms.writeBytes(reinterpret_cast<const std::byte*>(typeName.data()),
                              typeName.size());
            }
            // Position：AoI 事件的目标必然在 Space 中（addEntity 已 setPosition），
            // 因此 flag 恒为 1
            const auto pos = target->position();
            ms.writeUint8(1);
            std::byte fbuf[sizeof(float)];
            std::memcpy(fbuf, &pos.x, sizeof(float));
            ms.writeBytes(fbuf, sizeof(float));
            std::memcpy(fbuf, &pos.y, sizeof(float));
            ms.writeBytes(fbuf, sizeof(float));
            std::memcpy(fbuf, &pos.z, sizeof(float));
            ms.writeBytes(fbuf, sizeof(float));

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
        if (!entity || entity->state() != EntityState::Active) continue;  // LCOV_EXCL_BR_LINE ownedEntities_ 存 unique_ptr 且移除即整条擦除，get() null 臂不可达

        auto* baseCall = entity->baseEntityCall();
        if (!baseCall || !baseCall->isValid()) continue;  // LCOV_EXCL_BR_LINE isValid 假臂不可达

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
            if (!observer) continue;  // LCOV_EXCL_BR_LINE witness delta 同样产自 witnesses_ 名册，binding 与 ownedEntities_ 同步清除，观察者悬垂不可构造
            auto* baseCall = observer->baseEntityCall();
            if (!baseCall || !baseCall->isValid()) continue;  // LCOV_EXCL_BR_LINE isValid 假臂不可达

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
                // encodeDelta 至少写出 4 字节 count 前缀，payload 恒非空
                if (!encoded.empty()) {  // LCOV_EXCL_BR_LINE encoded 恒非空，防御臂
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

void CellRuntime::broadcastEvent(std::string_view event, std::span<const std::byte> data) {
    for (auto& [id, sr] : spaceRuntimes_) {
        for (auto* entity : sr->space().entities()) {
            if (entity != nullptr && entity->state() == EntityState::Active) {  // LCOV_EXCL_BR_LINE Space 名册 Member.entity 仅以非空指针写入且移除即整条擦除，null 短路臂不可达
                entity->emit(event, data);
            }
        }
    }
}

void CellRuntime::broadcastEventInRange(std::string_view event, const Vector3& center, float range,
                                         std::span<const std::byte> data) {
    for (auto& [id, sr] : spaceRuntimes_) {
        for (auto* entity : sr->space().queryRange(center, range)) {
            if (entity != nullptr && entity->state() == EntityState::Active) {  // LCOV_EXCL_BR_LINE 同 broadcastEvent：entitiesInRange 恒不返回空指针
                entity->emit(event, data);
            }
        }
    }
}

}  // namespace theseed::runtime
