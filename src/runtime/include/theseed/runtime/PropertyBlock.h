#pragma once

#include "theseed/runtime/DirtyMask.h"
#include "theseed/runtime/EntityDef.h"
#include "theseed/runtime/PropertyReplication.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <vector>

namespace theseed::runtime {

class PropertyBlock final {
public:
    PropertyBlock() = default;

    void init(const EntityDef& def);

    template <typename T>
    const T& get(PropertyId id) const {
        static_assert(std::is_trivially_copyable_v<T>);
        const auto& descriptor = def_->property(id);
        return *reinterpret_cast<const T*>(storage_.data() + descriptor.offset);
    }

    template <typename T>
    void set(PropertyId id, const T& value) {
        static_assert(std::is_trivially_copyable_v<T>);
        const auto& descriptor = def_->property(id);
        *reinterpret_cast<T*>(storage_.data() + descriptor.offset) = value;
        dirtyMask_.mark(id);
    }

    const EntityDef& def() const;
    const DirtyMask& dirtyMask() const;
    bool isDirty(PropertyId id) const;
    void clearDirty();
    std::vector<PropertyDelta> buildDirtyDelta() const;
    void applyDelta(std::span<const PropertyDelta> deltas, bool markDirty = false);

    const std::byte* data() const;
    std::byte* data();
    std::size_t size() const;

private:
    const EntityDef* def_ = nullptr;
    DirtyMask dirtyMask_{};
    std::vector<std::byte> storage_;
};

}  // namespace theseed::runtime
