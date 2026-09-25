#include "theseed/runtime/EntityDef.h"
#include "theseed/runtime/PropertyBlock.h"
#include "theseed/runtime/PropertyReplication.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <span>
#include <string>
#include <stdexcept>
#include <vector>

using theseed::runtime::EntityDef;
using theseed::runtime::PropertyBlock;
using theseed::runtime::PropertyDirtyTarget;
using theseed::runtime::PropertyFlag;
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

        // 空 delta 集：早退且不要求 storage（零属性实体场景）
        PropertyReplication::applyDelta(def, std::span<const PropertyDelta>{}, nullptr);

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

        // 单条 value 截断由下方 truncated 场景覆盖；header 截断已被
        // count 一致性闸蕴含（每条最少 8 字节），不再单测。

        auto truncated = encoded2;
        truncated.resize(encoded2.size() - 1);
        threw = false;
        try {
            PropertyReplication::decodeDelta(truncated);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        if (!threw) return fail("decode_truncated_value");

        // count 超限：声称的条数超出 payload 剩余字节能容纳的最小条目数
        //（防畸形 count 触发 reserve 内存放大）
        std::vector<std::byte> evilCount(sizeof(std::uint32_t));
        const std::uint32_t huge = 0xFFFFFFF0u;
        std::memcpy(evilCount.data(), &huge, sizeof(huge));
        threw = false;
        try {
            PropertyReplication::decodeDelta(evilCount);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        if (!threw) return fail("decode_count_exceeds_payload");

        // 边界合法：count=1 的最小 payload（propertyId + valueSize=0）
        std::vector<std::byte> minimal;
        auto appendU32 = [&minimal](std::uint32_t v) {
            auto* p = reinterpret_cast<const std::byte*>(&v);
            minimal.insert(minimal.end(), p, p + 4);
        };
        appendU32(1);
        appendU32(hpId);
        appendU32(0);
        const auto decodedMinimal = PropertyReplication::decodeDelta(minimal);
        if (decodedMinimal.size() != 1 || decodedMinimal[0].propertyId != hpId
            || !decodedMinimal[0].value.empty()) {
            return fail("decode_minimal_entry");
        }

        // 空 value 的 delta 再编码：value 空臂（跳过 payload 拷贝）
        const auto reEncoded = PropertyReplication::encodeDelta(decodedMinimal);
        if (reEncoded.size() != sizeof(std::uint32_t) + 8) {
            return fail("encode_empty_value_size");
        }
    }

    // 变长属性 def 的 init：定长带默认值 memcpy / 变长默认值 emplace / 变长无默认值
    {
        EntityDef varDef("Mixed");
        std::vector<std::byte> fourBytes(4, std::byte{0x2A});
        varDef.addProperty("answer", PropertyType::Int32, sizeof(std::int32_t),
                           PropertyFlag::None, fourBytes);
        varDef.addProperty("title", PropertyType::String, 0, PropertyFlag::None, fourBytes);
        varDef.addProperty("memo", PropertyType::Blob);

        PropertyBlock mixed;
        mixed.init(varDef);
        if (mixed.get<std::int32_t>(varDef.property(0).id) != 0x2A2A2A2A) {
            return fail("fixed_default_value_memcpy");
        }
        if (std::string(mixed.getString(varDef.property(1).id)) != "****") {
            return fail("variable_default_value_emplace");
        }
        if (!mixed.getBlob(varDef.property(2).id).empty()) {
            return fail("variable_no_default_empty");
        }

        // 定长默认值防御矩阵：空 defaultValue 跳过 memcpy / 超长 defaultValue 拒绝写入
        {
            EntityDef edgeDef("EdgeDefaults");
            edgeDef.addProperty("novalue", PropertyType::Int32, sizeof(std::int32_t));
            edgeDef.addProperty("toolong", PropertyType::Int32, sizeof(std::int32_t),
                                PropertyFlag::None, std::vector<std::byte>(9, std::byte{0x07}));
            PropertyBlock edge;
            edge.init(edgeDef);
            if (edge.get<std::int32_t>(edgeDef.property(0).id) != 0) {
                return fail("fixed_no_default_stays_zero");
            }
            if (edge.get<std::int32_t>(edgeDef.property(1).id) != 0) {
                return fail("fixed_oversize_default_rejected");
            }
        }

        // applyDelta 定向 markDirty 矩阵：各 target 假臂（不误标其它 mask）
        PropertyBlock marked;
        marked.init(varDef);
        marked.clearDirty();
        const auto pid = varDef.property(0).id;
        std::vector<theseed::runtime::PropertyDelta> oneDelta(1);
        oneDelta[0].propertyId = pid;
        oneDelta[0].value = fourBytes;
        marked.applyDelta(oneDelta, PropertyDirtyTarget::Runtime);
        if (marked.clientDirtyMask().any() || marked.viewDirtyMask().any()
            || marked.persistenceDirtyMask().any()) {
            return fail("mark_runtime_only");
        }
        marked.clearDirty();
        marked.applyDelta(oneDelta, PropertyDirtyTarget::Client);
        if (!marked.clientDirtyMask().any() || marked.dirtyMask().any()
            || marked.viewDirtyMask().any()) {
            return fail("mark_client_only");
        }
        marked.clearDirty();
        marked.applyDelta(oneDelta, PropertyDirtyTarget::View);
        if (!marked.viewDirtyMask().any() || marked.persistenceDirtyMask().any()
            || marked.clientDirtyMask().any()) {
            return fail("mark_view_only");
        }
    }

    return EXIT_SUCCESS;
}
