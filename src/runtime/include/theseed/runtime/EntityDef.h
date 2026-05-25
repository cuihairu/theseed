#pragma once

#include "theseed/runtime/DirtyMask.h"
#include "theseed/runtime/RuntimeTypes.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace theseed::runtime {

enum class MethodSide : std::uint8_t {
    Base = 0,
    Cell,
    Client,
};

enum class PropertyType : std::uint8_t {
    Int32 = 0,
    UInt32,
    Float32,
    Float64,
    Bool,
};

struct PropertyDescriptor {
    PropertyId id = 0;
    std::string name;
    PropertyType type = PropertyType::Int32;
    std::size_t offset = 0;
    std::size_t size = 0;
};

struct MethodDescriptor {
    MethodId id = 0;
    std::string name;
    MethodSide side = MethodSide::Cell;
};

class EntityDef final {
public:
    explicit EntityDef(std::string entityType = {});

    const std::string& entityType() const;

    PropertyId addProperty(std::string name, PropertyType type, std::size_t size);
    std::size_t propertyCount() const;
    std::size_t storageSize() const;

    MethodId addMethod(std::string name, MethodSide side = MethodSide::Cell);
    std::size_t methodCount() const;

    const PropertyDescriptor& property(PropertyId id) const;
    const PropertyDescriptor* findProperty(const std::string& name) const;
    const std::vector<PropertyDescriptor>& properties() const;

    const MethodDescriptor& method(MethodId id) const;
    const MethodDescriptor* findMethod(std::string_view name) const;
    const std::vector<MethodDescriptor>& methods() const;

private:
    std::string entityType_;
    std::size_t storageSize_ = 0;
    std::vector<PropertyDescriptor> properties_;
    std::vector<MethodDescriptor> methods_;
};

}  // namespace theseed::runtime
