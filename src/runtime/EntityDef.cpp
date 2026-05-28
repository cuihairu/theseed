#include "theseed/runtime/EntityDef.h"

#include <stdexcept>
#include <string_view>
#include <utility>

namespace theseed::runtime {

EntityDef::EntityDef(std::string entityType) : entityType_(std::move(entityType)) {}

const std::string& EntityDef::entityType() const {
    return entityType_;
}

void EntityDef::setParentType(std::string parentType) {
    parentType_ = std::move(parentType);
}

const std::string& EntityDef::parentType() const {
    return parentType_;
}

bool EntityDef::mergeFrom(const EntityDef& parent) {
    if (inherited_) return false;

    for (const auto& prop : parent.properties_) {
        if (findProperty(prop.name) != nullptr) {
            return false;
        }
        auto descriptor = prop;
        descriptor.id = static_cast<PropertyId>(properties_.size());
        descriptor.offset = storageSize_;
        properties_.push_back(std::move(descriptor));
        storageSize_ += prop.size;
    }

    for (const auto& meth : parent.methods_) {
        if (findMethod(meth.name) != nullptr) {
            continue;
        }
        auto descriptor = meth;
        descriptor.id = static_cast<MethodId>(methods_.size());
        methods_.push_back(std::move(descriptor));
    }

    inherited_ = true;
    return true;
}

std::size_t EntityDef::fixedSizeOfType(PropertyType type) {
    switch (type) {
        case PropertyType::Int8:    return 1;
        case PropertyType::Int16:   return 2;
        case PropertyType::Int32:   return 4;
        case PropertyType::Int64:   return 8;
        case PropertyType::UInt8:   return 1;
        case PropertyType::UInt16:  return 2;
        case PropertyType::UInt32:  return 4;
        case PropertyType::UInt64:  return 8;
        case PropertyType::Float32: return 4;
        case PropertyType::Float64: return 8;
        case PropertyType::Bool:    return 1;
        case PropertyType::Vector3: return 12; // 3 * float
        case PropertyType::String:  return 0;
        case PropertyType::Blob:    return 0;
    }
    return 0;
}

bool EntityDef::isVariableSized(PropertyType type) {
    return type == PropertyType::String || type == PropertyType::Blob;
}

PropertyId EntityDef::addProperty(std::string name, PropertyType type, std::size_t size,
                                   PropertyFlag flags, std::vector<std::byte> defaultValue) {
    if (name.empty()) {
        throw std::invalid_argument("property name is empty");
    }

    if (findProperty(name) != nullptr) {
        throw std::invalid_argument("duplicate property name: " + name);
    }

    PropertyDescriptor descriptor;
    descriptor.id = static_cast<PropertyId>(properties_.size());
    descriptor.name = std::move(name);
    descriptor.type = type;
    descriptor.offset = storageSize_;
    descriptor.flags = flags;
    descriptor.defaultValue = std::move(defaultValue);

    if (size == 0 && !isVariableSized(type)) {
        descriptor.size = fixedSizeOfType(type);
    } else {
        descriptor.size = size;
    }

    storageSize_ += descriptor.size;
    properties_.push_back(std::move(descriptor));
    return properties_.back().id;
}

MethodId EntityDef::addMethod(std::string name, MethodSide side,
                               std::vector<ArgDescriptor> args) {
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
    descriptor.args = std::move(args);

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
