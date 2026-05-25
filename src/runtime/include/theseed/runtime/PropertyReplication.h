#pragma once

#include "theseed/runtime/EntityDef.h"

#include <cstddef>
#include <span>
#include <vector>

namespace theseed::runtime {

struct PropertyDelta final {
    PropertyId propertyId = 0;
    std::vector<std::byte> value;
};

class PropertyReplication final {
public:
    static std::vector<PropertyDelta> buildDirtyDelta(const EntityDef& def,
                                                      const std::byte* storage,
                                                      const DirtyMask& dirtyMask);

    static void applyDelta(const EntityDef& def,
                           std::span<const PropertyDelta> deltas,
                           std::byte* storage);

    static std::vector<std::byte> encodeDelta(std::span<const PropertyDelta> deltas);
    static std::vector<PropertyDelta> decodeDelta(std::span<const std::byte> payload);
};

}  // namespace theseed::runtime
