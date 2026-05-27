#include "theseed/runtime/PropertyBlock.h"

#include <cstring>
#include <stdexcept>

namespace theseed::runtime {

void PropertyBlock::init(const EntityDef& def) {
    def_ = &def;
    dirtyMask_.resize(def.propertyCount());
    storage_.assign(def.storageSize(), std::byte{0});

    for (const auto& desc : def.properties()) {
        if (!desc.defaultValue.empty() && desc.size > 0
            && desc.defaultValue.size() <= desc.size) {
            std::memcpy(storage_.data() + desc.offset,
                        desc.defaultValue.data(),
                        desc.defaultValue.size());
        }
    }
}

const EntityDef& PropertyBlock::def() const {
    if (def_ == nullptr) {
        throw std::logic_error("property block is not initialized");
    }

    return *def_;
}

const DirtyMask& PropertyBlock::dirtyMask() const {
    return dirtyMask_;
}

bool PropertyBlock::isDirty(PropertyId id) const {
    return dirtyMask_.isDirty(id);
}

void PropertyBlock::clearDirty() {
    dirtyMask_.clear();
}

std::vector<PropertyDelta> PropertyBlock::buildDirtyDelta() const {
    return PropertyReplication::buildDirtyDelta(def(), data(), dirtyMask_);
}

std::vector<PropertyDelta> PropertyBlock::buildFullSnapshot() const {
    std::vector<PropertyDelta> deltas;
    for (const auto& desc : def_->properties()) {
        if (EntityDef::isVariableSized(desc.type)) continue;
        PropertyDelta delta;
        delta.propertyId = desc.id;
        delta.value.resize(desc.size);
        std::memcpy(delta.value.data(), storage_.data() + desc.offset, desc.size);
        deltas.push_back(std::move(delta));
    }
    return deltas;
}

void PropertyBlock::applyDelta(std::span<const PropertyDelta> deltas, bool markDirty) {
    if (changeCallbacks_.empty()) {
        PropertyReplication::applyDelta(def(), deltas, data());
    } else {
        for (const auto& delta : deltas) {
            const auto& descriptor = def_->property(delta.propertyId);
            if (delta.value.size() != descriptor.size) continue;

            std::vector<std::byte> oldValue(descriptor.size);
            std::memcpy(oldValue.data(), storage_.data() + descriptor.offset, descriptor.size);
            std::memcpy(storage_.data() + descriptor.offset, delta.value.data(), descriptor.size);

            fireCallback(delta.propertyId, oldValue.data(), delta.value.data(), descriptor.size);
        }
    }

    if (markDirty) {
        for (const auto& delta : deltas) {
            dirtyMask_.mark(delta.propertyId);
        }
    }
}

const std::byte* PropertyBlock::data() const {
    return storage_.data();
}

std::byte* PropertyBlock::data() {
    return storage_.data();
}

std::size_t PropertyBlock::size() const {
    return storage_.size();
}

void PropertyBlock::setChangeCallback(PropertyId id, PropertyChangeCallback callback) {
    if (callback) {
        changeCallbacks_[id] = std::move(callback);
    } else {
        changeCallbacks_.erase(id);
    }
}

void PropertyBlock::clearChangeCallbacks() {
    changeCallbacks_.clear();
}

void PropertyBlock::fireCallback(PropertyId id, const void* oldVal, const void* newVal, std::size_t size) {
    auto it = changeCallbacks_.find(id);
    if (it == changeCallbacks_.end()) return;

    it->second(id, static_cast<const std::byte*>(oldVal),
               static_cast<const std::byte*>(newVal), size);
}

}  // namespace theseed::runtime
