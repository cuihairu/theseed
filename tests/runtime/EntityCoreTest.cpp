#include "theseed/runtime/Controller.h"
#include "theseed/runtime/DirtyMask.h"
#include "theseed/runtime/Entity.h"
#include "theseed/runtime/EntityCall.h"
#include "theseed/runtime/EntityDef.h"
#include "theseed/runtime/PropertyBlock.h"
#include "theseed/runtime/StateMachine.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <vector>

using theseed::foundation::MemoryStream;
using theseed::runtime::DirtyMask;
using theseed::runtime::Entity;
using theseed::runtime::EntityCall;
using theseed::runtime::EntityDef;
using theseed::runtime::EntityId;
using theseed::runtime::EntitySide;
using theseed::runtime::EntityState;
using theseed::runtime::DeliveryClass;
using theseed::runtime::MethodSide;
using theseed::runtime::PropertyId;
using theseed::runtime::PropertyType;
using theseed::runtime::RuntimeInvocation;
using theseed::runtime::SendResult;
using theseed::runtime::Vector3;

namespace {

int fail(const char* stage) {
    std::cerr << "entity_core_test_failed_at=" << stage << '\n';
    return EXIT_FAILURE;
}

}  // namespace

int main() {
    EntityDef def("Avatar");
    const auto hpId = def.addProperty("hp", PropertyType::Int32, sizeof(std::int32_t));
    const auto manaId = def.addProperty("mana", PropertyType::UInt32, sizeof(std::uint32_t));
    const auto speedId = def.addProperty("speed", PropertyType::Float32, sizeof(float));
    const auto castSpellId = def.addMethod("castSpell", MethodSide::Cell);
    const auto basePingId = def.addMethod("basePing", MethodSide::Base);
    static_cast<void>(basePingId);

    if (def.propertyCount() != 3 || def.methodCount() != 2) {
        return fail("property_count");
    }

    if (def.storageSize() != sizeof(std::int32_t) + sizeof(std::uint32_t) + sizeof(float)) {
        return fail("storage_size");
    }

    if (def.findProperty("speed") == nullptr) {
        return fail("find_property");
    }
    if (def.method(castSpellId).name != "castSpell" || def.findMethod("basePing") == nullptr) {
        return fail("find_method");
    }
    if (def.method(basePingId).side != MethodSide::Base) {
        return fail("find_method_side");
    }

    std::vector<std::byte> spellPayload;
    int spellCount = 0;

    DirtyMask mask(3);
    mask.mark(hpId);
    mask.mark(speedId);
    if (!mask.any()) {
        return fail("mask_any");
    }
    if (!mask.isDirty(hpId) || mask.isDirty(manaId) || !mask.isDirty(speedId)) {
        return fail("mask_bits");
    }

    std::vector<PropertyId> dirtyIds;
    mask.forEachDirty([&](PropertyId id) {
        dirtyIds.push_back(id);
    });
    if (dirtyIds.size() != 2 || dirtyIds[0] != hpId || dirtyIds[1] != speedId) {
        return fail("mask_foreach");
    }

    Entity entity(1001, EntitySide::Cell, def);
    if (entity.id() != 1001 || entity.side() != EntitySide::Cell) {
        return fail("entity_identity");
    }

    if (entity.entityType() != "Avatar") {
        return fail("entity_type");
    }

    if (entity.baseEntityCall() != nullptr || entity.cellEntityCall() != nullptr) {
        return fail("entity_call_initial");
    }

    EntityCall call(1001, 7, "Avatar");
    if (!call.isValid()) {
        return fail("call_valid");
    }
    if (call.entityId() != 1001 || call.targetComponent() != 7) {
        return fail("call_identity");
    }
    if (call.entityType() != "Avatar") {
        return fail("call_entity_type");
    }
    if (call.deliveryClass() != DeliveryClass::ORDERED_RELIABLE) {
        return fail("call_delivery_default");
    }

    call.updateTarget(9);
    call.setDeliveryClass(DeliveryClass::UNORDERED_LOSSY);
    if (call.targetComponent() != 9 || call.deliveryClass() != DeliveryClass::UNORDERED_LOSSY) {
        return fail("call_update");
    }

    call.invalidate();
    if (call.isValid()) {
        return fail("call_invalidate");
    }

    if (entity.state() != EntityState::Creating) {
        return fail("entity_initial_state");
    }

    entity.activate();
    if (entity.state() != EntityState::Active) {
        return fail("entity_active");
    }

    entity.bindBaseEntityCall(21);
    entity.bindCellEntityCall(22, DeliveryClass::UNORDERED_LOSSY);
    if (entity.baseEntityCall() == nullptr || entity.cellEntityCall() == nullptr) {
        return fail("entity_call_bind");
    }
    if (entity.baseEntityCall()->entityId() != 1001 ||
        entity.baseEntityCall()->targetComponent() != 21) {
        return fail("base_call_values");
    }
    if (entity.cellEntityCall()->targetComponent() != 22 ||
        entity.cellEntityCall()->deliveryClass() != DeliveryClass::UNORDERED_LOSSY) {
        return fail("cell_call_values");
    }

    if (!entity.bindMethodHandler("castSpell",
                                  [&](Entity&, std::span<const std::byte> payload) {
                                      spellPayload.assign(payload.begin(), payload.end());
                                      spellCount += 1;
                                  })) {
        return fail("bind_method");
    }
    if (entity.bindMethodHandler("basePing", [&](Entity&, std::span<const std::byte>) {})) {
        return fail("bind_method_side_mismatch");
    }

    std::array<std::byte, 3> rawPayload{std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};
    RuntimeInvocation invocation;
    invocation.entityId = entity.id();
    invocation.entityType = entity.entityType();
    invocation.method = "castSpell";
    invocation.payload.assign(rawPayload.begin(), rawPayload.end());

    if (!entity.dispatchInvocation(invocation)) {
        return fail("entity_dispatch");
    }
    if (spellCount != 1 || spellPayload != invocation.payload) {
        return fail("entity_dispatch_payload");
    }

    if (entity.dispatchMethod("missingSpell", invocation.payload)) {
        return fail("entity_dispatch_missing");
    }

    // --- 错误路径与惰性初始化 ---
    {
        // bindMethodHandler：空名 / 空 handler / 未知方法 → false
        if (entity.bindMethodHandler("", [](Entity&, std::span<const std::byte>) {})) {
            return fail("bind_method_empty_name");
        }
        if (entity.bindMethodHandler("castSpell", nullptr)) {
            return fail("bind_method_null_handler");
        }
        if (entity.bindMethodHandler("noSuchSpell", [](Entity&, std::span<const std::byte>) {})) {
            return fail("bind_method_unknown");
        }

        // bindStreamMethodHandler：空名 / 空 handler / side 不匹配 → false
        if (entity.bindStreamMethodHandler("", [](Entity&, MemoryStream&) {})) {
            return fail("bind_stream_empty_name");
        }
        if (entity.bindStreamMethodHandler("castSpell", nullptr)) {
            return fail("bind_stream_null_handler");
        }
        if (entity.bindStreamMethodHandler("basePing", [](Entity&, MemoryStream&) {})) {
            return fail("bind_stream_side_mismatch");
        }

        // dispatchInvocation：id / 类型不匹配 → false
        RuntimeInvocation wrongId = invocation;
        wrongId.entityId = 9999;
        if (entity.dispatchInvocation(wrongId)) {
            return fail("dispatch_wrong_id");
        }
        RuntimeInvocation wrongType = invocation;
        wrongType.entityType = "Monster";
        if (entity.dispatchInvocation(wrongType)) {
            return fail("dispatch_wrong_type");
        }

        // queryEntityPosition：无 provider → nullopt；有 provider → 透传
        if (entity.queryEntityPosition(42).has_value()) {
            return fail("query_position_no_provider");
        }
        entity.setPositionProvider([](EntityId) {
            return std::optional<Vector3>{Vector3{1.0F, 2.0F, 3.0F}};
        });
        const auto provided = entity.queryEntityPosition(42);
        if (!provided.has_value() || provided->x != 1.0F) {
            return fail("query_position_provider");
        }

        // transport()：默认 nullptr → callCell/callBase 均 NotConnected
        if (entity.transport() != nullptr) {
            return fail("transport_initial");
        }
        if (entity.callCell("castSpell") != SendResult::NotConnected ||
            entity.callBase("basePing") != SendResult::NotConnected) {
            return fail("call_not_connected");
        }

        // 定时器：未注入调度函数或回调为空 → 空 handle
        if (entity.addTimer(std::chrono::milliseconds(5), nullptr)) {
            return fail("timer_null_callback");
        }
        if (entity.addPeriodicTimer(std::chrono::milliseconds(5), nullptr)) {
            return fail("periodic_null_callback");
        }

        // const 懒初始化：fsm() / controllers() 的 const 版本各自惰性建实例。
        // 先单独触发 const 版本——比较表达式两侧的求值顺序未指定，若 gcc
        // 先求右侧的非 const 调用，const 分支会因实例已存在而被跳过。
        const Entity& frozen = entity;
        static_cast<void>(frozen.fsm().state());
        static_cast<void>(frozen.controllers().count());
        if (frozen.fsm().state() != entity.fsm().state()) {
            return fail("fsm_lazy_init");
        }
        if (frozen.controllers().count() != entity.controllers().count()) {
            return fail("controllers_lazy_init");
        }
    }

    entity.setProperty<std::int32_t>(hpId, 75);
    entity.setProperty<std::uint32_t>(manaId, 30);
    entity.setProperty<float>(speedId, 3.5F);

    if (entity.getProperty<std::int32_t>(hpId) != 75) {
        return fail("entity_hp");
    }
    if (entity.getProperty<std::uint32_t>(manaId) != 30) {
        return fail("entity_mana");
    }
    if (entity.getProperty<float>(speedId) != 3.5F) {
        return fail("entity_speed");
    }

    if (!entity.isPropertyDirty(hpId) || !entity.isPropertyDirty(manaId) ||
        !entity.isPropertyDirty(speedId)) {
        return fail("entity_dirty");
    }

    entity.clearDirtyFlags();
    if (entity.isPropertyDirty(hpId) || entity.isPropertyDirty(manaId) ||
        entity.isPropertyDirty(speedId)) {
        return fail("entity_clear_dirty");
    }

    entity.setProperty<std::int32_t>(hpId, 88);
    entity.setProperty<float>(speedId, 5.0F);
    const auto delta = entity.buildDirtyPropertyDelta();
    if (delta.size() != 2) {
        return fail("entity_delta_size");
    }
    if (delta[0].propertyId != hpId || delta[1].propertyId != speedId) {
        return fail("entity_delta_order");
    }

    Entity replica(1001, EntitySide::Base, def);
    replica.applyPropertyDelta(delta);
    if (replica.getProperty<std::int32_t>(hpId) != 88) {
        return fail("entity_delta_apply_hp");
    }
    if (replica.getProperty<float>(speedId) != 5.0F) {
        return fail("entity_delta_apply_speed");
    }
    if (replica.isPropertyDirty(hpId) || replica.isPropertyDirty(speedId)) {
        return fail("entity_delta_apply_dirty");
    }

    entity.beginMigration();
    if (entity.state() != EntityState::Migrating) {
        return fail("entity_migrating");
    }

    entity.beginDestroy();
    if (entity.state() != EntityState::Destroying) {
        return fail("entity_destroying");
    }

    entity.destroy();
    if (entity.state() != EntityState::Destroyed) {
        return fail("entity_destroyed");
    }

    if (entity.baseEntityCall() != nullptr || entity.cellEntityCall() != nullptr) {
        return fail("entity_call_destroy_clear");
    }

    return EXIT_SUCCESS;
}
