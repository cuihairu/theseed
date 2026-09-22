#include "theseed/runtime/EntityDef.h"
#include "theseed/runtime/PropertyBlock.h"
#include "theseed/runtime/PropertyReplication.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <span>
#include <stdexcept>
#include <vector>

using theseed::runtime::EntityDef;
using theseed::runtime::PropertyBlock;
using theseed::runtime::PropertyDirtyTarget;
using theseed::runtime::PropertyType;

namespace {

int fail(const char* stage) {
    std::cerr << "property_replication_test_failed_at=" << stage << '\n';
    return EXIT_FAILURE;
}

}  // namespace

int main() {
    EntityDef def("Avatar");
    const auto hpId = def.addProperty("hp", PropertyType::Int32, sizeof(std::int32_t));
    const auto manaId = def.addProperty("mana", PropertyType::UInt32, sizeof(std::uint32_t));
    const auto speedId = def.addProperty("speed", PropertyType::Float32, sizeof(float));

    PropertyBlock source;
    source.init(def);
    source.set<std::int32_t>(hpId, 100);
    source.set<float>(speedId, 2.5F);

    if (!source.dirtyMask().any() || !source.viewDirtyMask().any()
        || !source.clientDirtyMask().any() || !source.persistenceDirtyMask().any()) {
        return fail("set_marks_all_dirty_targets");
    }

    source.clearPersistenceDirty();
    if (!source.dirtyMask().any() || !source.viewDirtyMask().any()
        || !source.clientDirtyMask().any() || source.persistenceDirtyMask().any()) {
        return fail("clear_persistence_keeps_sync_dirty");
    }

    const auto deltas = source.buildDirtyDelta();
    if (deltas.size() != 2) {
        return fail("dirty_delta_size");
    }
    if (deltas[0].propertyId != hpId || deltas[1].propertyId != speedId) {
        return fail("dirty_delta_order");
    }

    PropertyBlock target;
    target.init(def);
    target.set<std::uint32_t>(manaId, 77);
    target.clearDirty();
    target.applyDelta(deltas);

    if (target.get<std::int32_t>(hpId) != 100) {
        return fail("apply_hp");
    }
    if (target.get<float>(speedId) != 2.5F) {
        return fail("apply_speed");
    }
    if (target.get<std::uint32_t>(manaId) != 77) {
        return fail("apply_keep_other_value");
    }
    if (target.dirtyMask().any()) {
        return fail("apply_no_dirty");
    }

    target.applyDelta(deltas, true);
    if (!target.isDirty(hpId) || !target.isDirty(speedId) || target.isDirty(manaId)) {
        return fail("apply_mark_dirty");
    }

    target.clearDirty();
    target.applyDelta(deltas, PropertyDirtyTarget::Client | PropertyDirtyTarget::Persistence);
    if (target.dirtyMask().any() || target.viewDirtyMask().any()) {
        return fail("apply_targeted_no_runtime_or_view_dirty");
    }
    if (!target.clientDirtyMask().isDirty(hpId) || !target.clientDirtyMask().isDirty(speedId)
        || !target.persistenceDirtyMask().isDirty(hpId)
        || !target.persistenceDirtyMask().isDirty(speedId)) {
        return fail("apply_targeted_marks_selected_dirty");
    }

    const auto encoded = theseed::runtime::PropertyReplication::encodeDelta(deltas);
    const auto decoded = theseed::runtime::PropertyReplication::decodeDelta(encoded);
    if (decoded.size() != deltas.size()) {
        return fail("encode_decode_size");
    }
    if (decoded[0].propertyId != hpId || decoded[1].propertyId != speedId) {
        return fail("encode_decode_order");
    }

    // 静态 API 防御分支：null storage / 尺寸不匹配 / decode 截断
    {
        using theseed::runtime::PropertyDelta;
        using theseed::runtime::PropertyFlag;
        using theseed::runtime::PropertyReplication;

        bool threw = false;
        try {
            PropertyReplication::buildDirtyDelta(def, nullptr, source.dirtyMask(),
                                                 PropertyFlag::None);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        if (!threw) return fail("static_build_null_storage");

        threw = false;
        try {
            PropertyReplication::applyDelta(def, deltas, nullptr);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        if (!threw) return fail("static_apply_null_storage");

        // delta 值尺寸与描述符不符
        std::vector<PropertyDelta> bad = deltas;
        bad[0].value.resize(2);
        std::vector<std::byte> storage(def.storageSize(), std::byte{0});
        threw = false;
        try {
            PropertyReplication::applyDelta(def, bad, storage.data());
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        if (!threw) return fail("static_size_mismatch");

        // 正常 apply 写入裸 storage
        PropertyReplication::applyDelta(def, deltas, storage.data());
        std::int32_t hp = 0;
        std::memcpy(&hp, storage.data() + def.property(hpId).offset, sizeof(hp));
        if (hp != 100) return fail("static_apply_value");

        // decode：payload 连 count 都放不下 / 单条 header 截断 / value 截断
        const auto encoded2 = PropertyReplication::encodeDelta(deltas);
        threw = false;
        try {
            PropertyReplication::decodeDelta(std::span<const std::byte>(encoded2.data(), 2));
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        if (!threw) return fail("decode_truncated_count");

        threw = false;
        try {
            PropertyReplication::decodeDelta(std::span<const std::byte>(encoded2.data(), 6));
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        if (!threw) return fail("decode_truncated_header");

        auto truncated = encoded2;
        truncated.resize(encoded2.size() - 1);
        threw = false;
        try {
            PropertyReplication::decodeDelta(truncated);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        if (!threw) return fail("decode_truncated_value");
    }

    return EXIT_SUCCESS;
}
