#include "theseed/runtime/PropertyBlock.h"

#include <cstring>
#include <stdexcept>

namespace theseed::runtime {

namespace {

bool isPersistableProperty(const PropertyDescriptor& desc) {
    return desc.flags == PropertyFlag::None || hasFlag(desc.flags, PropertyFlag::Persistent);
}

}  // namespace

void PropertyBlock::init(const EntityDef& def) {
    def_ = &def;
    dirtyMask_.resize(def.propertyCount());
    viewDirtyMask_.resize(def.propertyCount());
    clientDirtyMask_.resize(def.propertyCount());
    persistenceDirtyMask_.resize(def.propertyCount());
    storage_.assign(def.storageSize(), std::byte{0});

    for (const auto& desc : def.properties()) {
        if (!EntityDef::isVariableSized(desc.type)) {
            if (!desc.defaultValue.empty() && desc.size > 0
                && desc.defaultValue.size() <= desc.size) {
                std::memcpy(storage_.data() + desc.offset,
                            desc.defaultValue.data(),
                            desc.defaultValue.size());
            }
        } else if (!desc.defaultValue.empty()) {
            varStorage_.emplace(desc.id, desc.defaultValue);
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

const DirtyMask& PropertyBlock::viewDirtyMask() const {
    return viewDirtyMask_;
}

const DirtyMask& PropertyBlock::clientDirtyMask() const {
    return clientDirtyMask_;
}

const DirtyMask& PropertyBlock::persistenceDirtyMask() const {
    return persistenceDirtyMask_;
}

bool PropertyBlock::isDirty(PropertyId id) const {
    return dirtyMask_.isDirty(id);
}

void PropertyBlock::clearDirty() {
    dirtyMask_.clear();
    viewDirtyMask_.clear();
    clientDirtyMask_.clear();
    persistenceDirtyMask_.clear();
}

void PropertyBlock::clearRuntimeDirty() {
    dirtyMask_.clear();
}

void PropertyBlock::clearViewDirty() {
    viewDirtyMask_.clear();
}

void PropertyBlock::clearClientDirty() {
    clientDirtyMask_.clear();
}

void PropertyBlock::clearPersistenceDirty() {
    persistenceDirtyMask_.clear();
}

std::string_view PropertyBlock::getString(PropertyId id) const {
    auto it = varStorage_.find(id);
    if (it == varStorage_.end()) return {};
    return std::string_view(reinterpret_cast<const char*>(it->second.data()),
                            it->second.size());
}

void PropertyBlock::setString(PropertyId id, std::string_view value) {
    std::vector<std::byte> oldVal;
    auto it = varStorage_.find(id);
    if (it != varStorage_.end()) {
        oldVal = it->second;
    }

    std::vector<std::byte> newData(value.size());
    std::memcpy(newData.data(), value.data(), value.size());
    varStorage_[id] = std::move(newData);
    markDirty(id, PropertyDirtyTarget::All);

    auto& cur = varStorage_[id];
    fireVarCallback(id, oldVal, cur);
}

std::span<const std::byte> PropertyBlock::getBlob(PropertyId id) const {
    auto it = varStorage_.find(id);
    if (it == varStorage_.end()) return {};
    return it->second;
}

void PropertyBlock::setBlob(PropertyId id, std::span<const std::byte> value) {
    std::vector<std::byte> oldVal;
    auto it = varStorage_.find(id);
    if (it != varStorage_.end()) {
        oldVal = it->second;
    }

    std::vector<std::byte> newData(value.begin(), value.end());
    varStorage_[id] = std::move(newData);
    markDirty(id, PropertyDirtyTarget::All);

    auto& cur = varStorage_[id];
    fireVarCallback(id, oldVal, cur);
}

std::vector<PropertyDelta> PropertyBlock::buildDirtyDelta(PropertyFlag excludeFlags) const {
    return buildDeltaFromMask(dirtyMask_, excludeFlags);
}

std::vector<PropertyDelta> PropertyBlock::buildViewDirtyDelta(PropertyFlag excludeFlags) const {
    return buildDeltaFromMask(viewDirtyMask_, excludeFlags);
}

std::vector<PropertyDelta> PropertyBlock::buildClientDirtyDelta(PropertyFlag excludeFlags) const {
    return buildDeltaFromMask(clientDirtyMask_, excludeFlags);
}

std::vector<PropertyDelta> PropertyBlock::buildDeltaFromMask(const DirtyMask& mask,
                                                             PropertyFlag excludeFlags) const {
    auto deltas = PropertyReplication::buildDirtyDelta(def(), data(), mask, excludeFlags);

    for (const auto& desc : def_->properties()) {
        if (!EntityDef::isVariableSized(desc.type)) continue;
        if (static_cast<std::uint32_t>(desc.flags) & static_cast<std::uint32_t>(excludeFlags)) continue;
        if (!mask.isDirty(desc.id)) continue;

        auto it = varStorage_.find(desc.id);
        if (it == varStorage_.end()) continue;

        PropertyDelta delta;
        delta.propertyId = desc.id;
        delta.value = it->second;
        deltas.push_back(std::move(delta));
    }

    return deltas;
}

std::vector<PropertyDelta> PropertyBlock::buildFullSnapshot(PropertyFlag excludeFlags) const {
    std::vector<PropertyDelta> deltas;
    for (const auto& desc : def_->properties()) {
        if (static_cast<std::uint32_t>(desc.flags) & static_cast<std::uint32_t>(excludeFlags)) continue;

        if (EntityDef::isVariableSized(desc.type)) {
            auto it = varStorage_.find(desc.id);
            if (it != varStorage_.end()) {
                PropertyDelta delta;
                delta.propertyId = desc.id;
                delta.value = it->second;
                deltas.push_back(std::move(delta));
            }
        } else {
            PropertyDelta delta;
            delta.propertyId = desc.id;
            delta.value.resize(desc.size);
            std::memcpy(delta.value.data(), storage_.data() + desc.offset, desc.size);
            deltas.push_back(std::move(delta));
        }
    }
    return deltas;
}

void PropertyBlock::applyDelta(std::span<const PropertyDelta> deltas, bool markDirty) {
    applyDelta(deltas, markDirty ? PropertyDirtyTarget::All : PropertyDirtyTarget::None);
}

void PropertyBlock::applyDelta(std::span<const PropertyDelta> deltas,
                               PropertyDirtyTarget markTargets) {
    for (const auto& delta : deltas) {
        const auto& descriptor = def_->property(delta.propertyId);

        if (EntityDef::isVariableSized(descriptor.type)) {
            std::vector<std::byte> oldVal;
            auto it = varStorage_.find(delta.propertyId);
            if (it != varStorage_.end()) {
                oldVal = it->second;
            }

            varStorage_[delta.propertyId] = delta.value;

            auto& cur = varStorage_[delta.propertyId];
            fireVarCallback(delta.propertyId, oldVal, cur);
        } else {
            if (delta.value.size() != descriptor.size) continue;

            if (!changeCallbacks_.empty()) {
                std::vector<std::byte> oldValue(descriptor.size);
                std::memcpy(oldValue.data(), storage_.data() + descriptor.offset, descriptor.size);
                std::memcpy(storage_.data() + descriptor.offset, delta.value.data(), descriptor.size);
                fireCallback(delta.propertyId, oldValue.data(), delta.value.data(), descriptor.size);
            } else {
                std::memcpy(storage_.data() + descriptor.offset, delta.value.data(), descriptor.size);
            }
        }

        if (markTargets != PropertyDirtyTarget::None) {
            markDirty(delta.propertyId, markTargets);
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

void PropertyBlock::fireVarCallback(PropertyId id, const std::vector<std::byte>& oldVal,
                                     const std::vector<std::byte>& newVal) {
    auto it = changeCallbacks_.find(id);
    if (it == changeCallbacks_.end()) return;

    it->second(id, oldVal.data(), newVal.data(), newVal.size());
}

void PropertyBlock::markDirty(PropertyId id, PropertyDirtyTarget targets) {
    if (hasDirtyTarget(targets, PropertyDirtyTarget::Runtime)) {
        dirtyMask_.mark(id);
    }
    if (hasDirtyTarget(targets, PropertyDirtyTarget::View)) {
        viewDirtyMask_.mark(id);
    }
    if (hasDirtyTarget(targets, PropertyDirtyTarget::Client)) {
        clientDirtyMask_.mark(id);
    }
    if (hasDirtyTarget(targets, PropertyDirtyTarget::Persistence)) {
        if (isPersistableProperty(def().property(id))) {
            persistenceDirtyMask_.mark(id);
        }
    }
}

}  // namespace theseed::runtime
