#include "theseed/runtime/PropertyReplication.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace theseed::runtime {

std::vector<PropertyDelta> PropertyReplication::buildDirtyDelta(const EntityDef& def,
                                                                const std::byte* storage,
                                                                const DirtyMask& dirtyMask,
                                                                PropertyFlag excludeFlags) {
    // 零属性实体（空 def）的 storage vector data() 为 nullptr：无脏位时
    // 无需触碰 storage，先于 null 防御返回空集，避免泵阶段误伤合法空实体。
    if (!dirtyMask.any()) return {};
    if (storage == nullptr) {
        throw std::invalid_argument("property storage is null");
    }

    std::vector<PropertyDelta> deltas;
    dirtyMask.forEachDirty([&](PropertyId propertyId) {
        const auto& descriptor = def.property(propertyId);
        if (static_cast<std::uint32_t>(descriptor.flags) & static_cast<std::uint32_t>(excludeFlags)) {
            return;
        }
        if (EntityDef::isVariableSized(descriptor.type)) return;

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
    // 与 buildDirtyDelta 对称：空 delta 集无需 storage（零属性实体场景）。
    if (deltas.empty()) return;
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

    // count 与 payload 长度的一致性校验：每条 delta 在 wire 上至少占
    // propertyId(4) + valueSize(4) = 8 字节，超出的 count 必为畸形消息。
    // 没有这道闸，损坏的 count 会让下方 reserve 请求 GB 级分配（内存放大）。
    constexpr std::size_t kMinEntryBytes = sizeof(PropertyId) + sizeof(std::uint32_t);
    if (count > (payload.size() - cursor) / kMinEntryBytes) {
        throw std::invalid_argument("property delta count exceeds payload size");
    }

    std::vector<PropertyDelta> deltas;
    deltas.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        // count 一致性闸已保证每条的 header（propertyId+valueSize 共 8 字节）
        // 完整在界内，无需再单查 header 截断；value 部分仍需单独校验。
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
