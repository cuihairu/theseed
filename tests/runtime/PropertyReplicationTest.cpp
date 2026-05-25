#include "theseed/runtime/EntityDef.h"
#include "theseed/runtime/PropertyBlock.h"
#include "theseed/runtime/PropertyReplication.h"

#include <cstdlib>
#include <iostream>

using theseed::runtime::EntityDef;
using theseed::runtime::PropertyBlock;
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

    const auto encoded = theseed::runtime::PropertyReplication::encodeDelta(deltas);
    const auto decoded = theseed::runtime::PropertyReplication::decodeDelta(encoded);
    if (decoded.size() != deltas.size()) {
        return fail("encode_decode_size");
    }
    if (decoded[0].propertyId != hpId || decoded[1].propertyId != speedId) {
        return fail("encode_decode_order");
    }

    return EXIT_SUCCESS;
}
