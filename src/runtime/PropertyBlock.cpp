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

void PropertyBlock::applyDelta(std::span<const PropertyDelta> deltas, bool markDirty) {
    PropertyReplication::applyDelta(def(), deltas, data());
    if (!markDirty) {
        return;
    }

    for (const auto& delta : deltas) {
        dirtyMask_.mark(delta.propertyId);
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

}  // namespace theseed::runtime
