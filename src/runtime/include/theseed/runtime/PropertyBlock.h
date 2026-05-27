#pragma once

#include "theseed/runtime/DirtyMask.h"
#include "theseed/runtime/EntityDef.h"
#include "theseed/runtime/PropertyReplication.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace theseed::runtime {

class PropertyBlock final {
public:
    using PropertyChangeCallback = std::function<void(PropertyId, const std::byte* oldVal,
                                                      const std::byte* newVal, std::size_t size)>;

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
        auto* dst = reinterpret_cast<T*>(storage_.data() + descriptor.offset);

        T oldValue = *dst;
        *dst = value;
        dirtyMask_.mark(id);

        fireCallback(id, &oldValue, &value, sizeof(T));
    }

    void setChangeCallback(PropertyId id, PropertyChangeCallback callback);
    void clearChangeCallbacks();

    const EntityDef& def() const;
    const DirtyMask& dirtyMask() const;
    bool isDirty(PropertyId id) const;
    void clearDirty();
    std::vector<PropertyDelta> buildDirtyDelta() const;
    std::vector<PropertyDelta> buildFullSnapshot() const;
    void applyDelta(std::span<const PropertyDelta> deltas, bool markDirty = false);

    const std::byte* data() const;
    std::byte* data();
    std::size_t size() const;

private:
    void fireCallback(PropertyId id, const void* oldVal, const void* newVal, std::size_t size);

    const EntityDef* def_ = nullptr;
    DirtyMask dirtyMask_{};
    std::vector<std::byte> storage_;
    std::unordered_map<PropertyId, PropertyChangeCallback> changeCallbacks_;
};

}  // namespace theseed::runtime
