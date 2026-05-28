#include "theseed/runtime/PropertyReplication.h"

#include <bit>
#include <cstring>
#include <stdexcept>

namespace theseed::runtime {

std::vector<PropertyDelta> PropertyReplication::buildDirtyDelta(const EntityDef& def,
                                                                const std::byte* storage,
                                                                const DirtyMask& dirtyMask,
                                                                PropertyFlag excludeFlags) {
    if (storage == nullptr) {
        throw std::invalid_argument("property storage is null");
    }

    std::vector<PropertyDelta> deltas;
    dirtyMask.forEachDirty([&](PropertyId propertyId) {
        const auto& descriptor = def.property(propertyId);
        if (static_cast<std::uint32_t>(descriptor.flags) & static_cast<std::uint32_t>(excludeFlags)) {
            return;
        }

        PropertyDelta delta;
        delta.propertyId = propertyId;
        delta.value.resize(descriptor.size);
        std::memcpy(delta.value.data(), storage + descriptor.offset, descriptor.size);
        deltas.push_back(std::move(delta));
    });
    return deltas;
}

void PropertyReplication::applyDelta(const EntityDef& def,
                                     std::span<const PropertyDelta> deltas,
                                     std::byte* storage) {
    if (storage == nullptr) {
        throw std::invalid_argument("property storage is null");
    }

    for (const auto& delta : deltas) {
        const auto& descriptor = def.property(delta.propertyId);
        if (delta.value.size() != descriptor.size) {
            throw std::invalid_argument("property delta size mismatch");
        }

        std::memcpy(storage + descriptor.offset, delta.value.data(), descriptor.size);
    }
}

std::vector<std::byte> PropertyReplication::encodeDelta(std::span<const PropertyDelta> deltas) {
    std::vector<std::byte> payload;
    const auto count = static_cast<std::uint32_t>(deltas.size());
    payload.resize(sizeof(count));
    std::memcpy(payload.data(), &count, sizeof(count));

    for (const auto& delta : deltas) {
        const auto valueSize = static_cast<std::uint32_t>(delta.value.size());
        const auto offset = payload.size();
        payload.resize(offset + sizeof(delta.propertyId) + sizeof(valueSize) + delta.value.size());

        std::memcpy(payload.data() + offset, &delta.propertyId, sizeof(delta.propertyId));
        std::memcpy(payload.data() + offset + sizeof(delta.propertyId), &valueSize, sizeof(valueSize));
        if (!delta.value.empty()) {
            std::memcpy(payload.data() + offset + sizeof(delta.propertyId) + sizeof(valueSize),
                        delta.value.data(),
                        delta.value.size());
        }
    }

    return payload;
}

std::vector<PropertyDelta> PropertyReplication::decodeDelta(std::span<const std::byte> payload) {
    if (payload.size() < sizeof(std::uint32_t)) {
        throw std::invalid_argument("property delta payload is truncated");
    }

    std::size_t cursor = 0;
    std::uint32_t count = 0;
    std::memcpy(&count, payload.data() + cursor, sizeof(count));
    cursor += sizeof(count);

    std::vector<PropertyDelta> deltas;
    deltas.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        if (cursor + sizeof(PropertyId) + sizeof(std::uint32_t) > payload.size()) {
            throw std::invalid_argument("property delta payload header is truncated");
        }

        PropertyDelta delta;
        std::uint32_t valueSize = 0;
        std::memcpy(&delta.propertyId, payload.data() + cursor, sizeof(delta.propertyId));
        cursor += sizeof(delta.propertyId);
        std::memcpy(&valueSize, payload.data() + cursor, sizeof(valueSize));
        cursor += sizeof(valueSize);

        if (cursor + valueSize > payload.size()) {
            throw std::invalid_argument("property delta payload value is truncated");
        }

        delta.value.resize(valueSize);
        if (valueSize > 0) {
            std::memcpy(delta.value.data(), payload.data() + cursor, valueSize);
        }
        cursor += valueSize;
        deltas.push_back(std::move(delta));
    }

    return deltas;
}

}  // namespace theseed::runtime
