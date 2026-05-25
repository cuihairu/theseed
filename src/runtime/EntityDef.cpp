#include "theseed/runtime/EntityDef.h"

#include <stdexcept>
#include <string_view>
#include <utility>

namespace theseed::runtime {

EntityDef::EntityDef(std::string entityType) : entityType_(std::move(entityType)) {}

const std::string& EntityDef::entityType() const {
    return entityType_;
}

PropertyId EntityDef::addProperty(std::string name, PropertyType type, std::size_t size) {
    PropertyDescriptor descriptor;
    descriptor.id = static_cast<PropertyId>(properties_.size());
    descriptor.name = std::move(name);
    descriptor.type = type;
    descriptor.offset = storageSize_;
    descriptor.size = size;

    storageSize_ += size;
    properties_.push_back(std::move(descriptor));
    return properties_.back().id;
}

MethodId EntityDef::addMethod(std::string name, MethodSide side) {
    if (name.empty()) {
        throw std::invalid_argument("method name is empty");
    }

    if (findMethod(name) != nullptr) {
        throw std::invalid_argument("duplicate method name");
    }

    MethodDescriptor descriptor;
    descriptor.id = static_cast<MethodId>(methods_.size());
    descriptor.name = std::move(name);
    descriptor.side = side;

    methods_.push_back(std::move(descriptor));
    return methods_.back().id;
}

std::size_t EntityDef::propertyCount() const {
    return properties_.size();
}

std::size_t EntityDef::storageSize() const {
    return storageSize_;
}

std::size_t EntityDef::methodCount() const {
    return methods_.size();
}

const PropertyDescriptor& EntityDef::property(PropertyId id) const {
    if (id >= properties_.size()) {
        throw std::out_of_range("property id out of range");
    }

    return properties_[id];
}

const PropertyDescriptor* EntityDef::findProperty(const std::string& name) const {
    for (const auto& descriptor : properties_) {
        if (descriptor.name == name) {
            return &descriptor;
        }
    }

    return nullptr;
}

const std::vector<PropertyDescriptor>& EntityDef::properties() const {
    return properties_;
}

const MethodDescriptor& EntityDef::method(MethodId id) const {
    if (id >= methods_.size()) {
        throw std::out_of_range("method id out of range");
    }

    return methods_[id];
}

const MethodDescriptor* EntityDef::findMethod(std::string_view name) const {
    for (const auto& descriptor : methods_) {
        if (descriptor.name == name) {
            return &descriptor;
        }
    }

    return nullptr;
}

const std::vector<MethodDescriptor>& EntityDef::methods() const {
    return methods_;
}

}  // namespace theseed::runtime
