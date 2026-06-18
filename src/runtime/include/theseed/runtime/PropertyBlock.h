#pragma once

#include "theseed/runtime/DirtyMask.h"
#include "theseed/runtime/EntityDef.h"
#include "theseed/runtime/PropertyReplication.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace theseed::runtime {

enum class PropertyDirtyTarget : std::uint8_t {
    None = 0,
    Runtime = 1 << 0,
    View = 1 << 1,
    Client = 1 << 2,
    Persistence = 1 << 3,
    All = (1 << 0) | (1 << 1) | (1 << 2) | (1 << 3),
};

inline PropertyDirtyTarget operator|(PropertyDirtyTarget a, PropertyDirtyTarget b) {
    return static_cast<PropertyDirtyTarget>(static_cast<std::uint8_t>(a)
                                            | static_cast<std::uint8_t>(b));
}

inline bool hasDirtyTarget(PropertyDirtyTarget targets, PropertyDirtyTarget target) {
    return (static_cast<std::uint8_t>(targets) & static_cast<std::uint8_t>(target)) != 0;
}

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
        markDirty(id, PropertyDirtyTarget::All);

        fireCallback(id, &oldValue, &value, sizeof(T));
    }

    std::string_view getString(PropertyId id) const;
    void setString(PropertyId id, std::string_view value);

    std::span<const std::byte> getBlob(PropertyId id) const;
    void setBlob(PropertyId id, std::span<const std::byte> value);

    void setChangeCallback(PropertyId id, PropertyChangeCallback callback);
    void clearChangeCallbacks();

    const EntityDef& def() const;
    const DirtyMask& dirtyMask() const;
    const DirtyMask& viewDirtyMask() const;
    const DirtyMask& clientDirtyMask() const;
    const DirtyMask& persistenceDirtyMask() const;
    bool isDirty(PropertyId id) const;
    void clearDirty();
    void clearRuntimeDirty();
    void clearViewDirty();
    void clearClientDirty();
    void clearPersistenceDirty();
    std::vector<PropertyDelta> buildDirtyDelta(PropertyFlag excludeFlags = PropertyFlag::None) const;
    std::vector<PropertyDelta> buildViewDirtyDelta(PropertyFlag excludeFlags = PropertyFlag::None) const;
    std::vector<PropertyDelta> buildClientDirtyDelta(PropertyFlag excludeFlags = PropertyFlag::None) const;
    std::vector<PropertyDelta> buildFullSnapshot(PropertyFlag excludeFlags = PropertyFlag::None) const;
    void applyDelta(std::span<const PropertyDelta> deltas, bool markDirty = false);
    void applyDelta(std::span<const PropertyDelta> deltas, PropertyDirtyTarget markTargets);

    const std::byte* data() const;
    std::byte* data();
    std::size_t size() const;

private:
    void fireCallback(PropertyId id, const void* oldVal, const void* newVal, std::size_t size);
    void fireVarCallback(PropertyId id, const std::vector<std::byte>& oldVal,
                         const std::vector<std::byte>& newVal);
    void markDirty(PropertyId id, PropertyDirtyTarget targets);
    std::vector<PropertyDelta> buildDeltaFromMask(const DirtyMask& mask,
                                                  PropertyFlag excludeFlags) const;

    const EntityDef* def_ = nullptr;
    DirtyMask dirtyMask_{};
    DirtyMask viewDirtyMask_{};
    DirtyMask clientDirtyMask_{};
    DirtyMask persistenceDirtyMask_{};
    std::vector<std::byte> storage_;
    std::unordered_map<PropertyId, std::vector<std::byte>> varStorage_;
    std::unordered_map<PropertyId, PropertyChangeCallback> changeCallbacks_;
};

}  // namespace theseed::runtime
