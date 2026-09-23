#include "theseed/runtime/Entity.h"
#include "theseed/runtime/EntityMigration.h"

#include <cstddef>
#include <cstdlib>
#include <iostream>

using theseed::runtime::DeliveryClass;
using theseed::runtime::Entity;
using theseed::runtime::EntityDef;
using theseed::runtime::EntityMigration;
using theseed::runtime::EntityMigrationSnapshot;
using theseed::runtime::EntitySide;
using theseed::runtime::EntityState;
using theseed::runtime::PropertyType;

namespace {

int fail(const char* stage) {
    std::cerr << "entity_migration_test_failed_at=" << stage << '\n';
    return EXIT_FAILURE;
}

}  // namespace

int main() {
    EntityDef def("Avatar");
    const auto hpId = def.addProperty("hp", PropertyType::Int32, sizeof(std::int32_t));
    const auto speedId = def.addProperty("speed", PropertyType::Float32, sizeof(float));

    Entity source(1001, EntitySide::Cell, def);
    source.setProperty<std::int32_t>(hpId, 66);
    source.setProperty<float>(speedId, 4.5F);
    source.bindBaseEntityCall(11);
    source.bindCellEntityCall(22, DeliveryClass::UNORDERED_LOSSY);
    source.beginMigration();

    const auto snapshot = EntityMigration::capture(source, 7, 11, 22);
    if (snapshot.entityId != 1001 || snapshot.epoch != 7) {
        return fail("snapshot_identity");
    }
    if (!snapshot.baseCall.has_value() || !snapshot.cellCall.has_value()) {
        return fail("snapshot_route_presence");
    }
    if (snapshot.baseCall->targetComponent != 11 ||
        snapshot.cellCall->targetComponent != 22) {
        return fail("snapshot_route_values");
    }

    Entity target(1001, EntitySide::Cell, def);
    EntityMigration::restore(target, snapshot);

    if (target.state() != EntityState::Active) {
        return fail("restore_state");
    }
    if (target.getProperty<std::int32_t>(hpId) != 66) {
        return fail("restore_hp");
    }
    if (target.getProperty<float>(speedId) != 4.5F) {
        return fail("restore_speed");
    }
    if (target.baseEntityCall() == nullptr || target.cellEntityCall() == nullptr) {
        return fail("restore_route_presence");
    }
    if (target.baseEntityCall()->targetComponent() != 11 ||
        target.cellEntityCall()->targetComponent() != 22) {
        return fail("restore_route_values");
    }
    if (target.cellEntityCall()->deliveryClass() != DeliveryClass::UNORDERED_LOSSY) {
        return fail("restore_delivery");
    }

    // --- encode/decode 往返（带 position）与错误路径 ---
    {
        auto routed = snapshot;
        routed.position = theseed::runtime::Vector3{1.5F, 2.5F, 3.5F};
        const auto payload = EntityMigration::encode(routed);
        auto decoded = EntityMigration::decode(payload);
        bool ok = decoded.entityId == routed.entityId && decoded.epoch == routed.epoch;
        ok = ok && decoded.entityType == "Avatar" && decoded.side == routed.side;
        ok = ok && decoded.spaceId == routed.spaceId;
        ok = ok && decoded.position.has_value() && decoded.position->x == 1.5F;
        ok = ok && decoded.baseCall.has_value() && decoded.cellCall.has_value();
        ok = ok && decoded.baseCall->targetComponent == 11 &&
             decoded.cellCall->targetComponent == 22;
        if (!ok) return fail("encode_decode_roundtrip");

        // 空字符串字段的编码路径（appendString 的 empty 提前返回）
        {
            auto anon = routed;
            anon.entityType = "";
            const auto anonPayload = EntityMigration::encode(anon);
            auto anonDecoded = EntityMigration::decode(anonPayload);
            if (!anonDecoded.entityType.empty()) return fail("encode_empty_string");
        }

        // 截断：任意前缀都必须被拒绝（readValue/readBytes/readString 截断 throw）
        bool truncatedOk = true;
        for (std::size_t cut = 1; cut < payload.size() && truncatedOk; ++cut) {
            bool threw = false;
            try {
                static_cast<void>(EntityMigration::decode(
                    std::span<const std::byte>(payload.data(), cut)));
            } catch (const std::invalid_argument&) {
                threw = true;
            }
            truncatedOk = truncatedOk && threw;
        }
        if (!truncatedOk) return fail("truncated_payload_rejected");

        // 尾部多余字节拒绝
        bool threw = false;
        {
            std::vector<std::byte> trailing = payload;
            trailing.push_back(std::byte{0});
            try {
                static_cast<void>(EntityMigration::decode(trailing));
            } catch (const std::invalid_argument&) {
                threw = true;
            }
        }
        if (!threw) return fail("trailing_bytes_rejected");

        // restore：实体 id 不匹配
        Entity other(2002, EntitySide::Cell, def);
        threw = false;
        try {
            EntityMigration::restore(other, snapshot);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        if (!threw) return fail("restore_id_mismatch");

        // restore：实体类型不匹配
        EntityDef otherDef("Monster");
        static_cast<void>(
            otherDef.addProperty("hp", PropertyType::Int32, sizeof(std::int32_t)));
        Entity otherType(1001, EntitySide::Cell, otherDef);
        threw = false;
        try {
            EntityMigration::restore(otherType, snapshot);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        if (!threw) return fail("restore_type_mismatch");

        // restore：属性块尺寸不匹配（3×Int32=12 字节 ≠ 源 8 字节）
        EntityDef sizedDef("Avatar");
        static_cast<void>(
            sizedDef.addProperty("hp", PropertyType::Int32, sizeof(std::int32_t)));
        static_cast<void>(
            sizedDef.addProperty("mana", PropertyType::Int32, sizeof(std::int32_t)));
        static_cast<void>(
            sizedDef.addProperty("rage", PropertyType::Int32, sizeof(std::int32_t)));
        Entity otherSize(1001, EntitySide::Cell, sizedDef);
        threw = false;
        try {
            EntityMigration::restore(otherSize, snapshot);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        if (!threw) return fail("restore_size_mismatch");
    }

    return EXIT_SUCCESS;
}
