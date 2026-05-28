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
    Int8 = 0,
    Int16,
    Int32,
    Int64,
    UInt8,
    UInt16,
    UInt32,
    UInt64,
    Float32,
    Float64,
    Bool,
    String,
    Vector3,
    Blob,
};

enum class PropertyFlag : std::uint32_t {
    None = 0,
    Persistent = 1 << 0,
    ClientSync = 1 << 1,
    Base = 1 << 2,
    Cell = 1 << 3,
};

inline PropertyFlag operator|(PropertyFlag a, PropertyFlag b) {
    return static_cast<PropertyFlag>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}

inline bool hasFlag(PropertyFlag flags, PropertyFlag flag) {
    return (static_cast<std::uint32_t>(flags) & static_cast<std::uint32_t>(flag)) != 0;
}

struct PropertyDescriptor {
    PropertyId id = 0;
    std::string name;
    PropertyType type = PropertyType::Int32;
    std::size_t offset = 0;
    std::size_t size = 0;
    PropertyFlag flags = PropertyFlag::None;
    std::vector<std::byte> defaultValue;
};

struct ArgDescriptor {
    std::string name;
    PropertyType type = PropertyType::Int32;
};

struct MethodDescriptor {
    MethodId id = 0;
    std::string name;
    MethodSide side = MethodSide::Cell;
    std::vector<ArgDescriptor> args;
};

class EntityDef final {
public:
    explicit EntityDef(std::string entityType = {});

    const std::string& entityType() const;

    void setParentType(std::string parentType);
    const std::string& parentType() const;
    bool mergeFrom(const EntityDef& parent);

    PropertyId addProperty(std::string name, PropertyType type, std::size_t size = 0,
                           PropertyFlag flags = PropertyFlag::None,
                           std::vector<std::byte> defaultValue = {});

    static std::size_t fixedSizeOfType(PropertyType type);
    static bool isVariableSized(PropertyType type);
    std::size_t propertyCount() const;
    std::size_t storageSize() const;

    MethodId addMethod(std::string name, MethodSide side = MethodSide::Cell,
                       std::vector<ArgDescriptor> args = {});
    std::size_t methodCount() const;

    const PropertyDescriptor& property(PropertyId id) const;
    const PropertyDescriptor* findProperty(const std::string& name) const;
    const std::vector<PropertyDescriptor>& properties() const;

    const MethodDescriptor& method(MethodId id) const;
    const MethodDescriptor* findMethod(std::string_view name) const;
    const std::vector<MethodDescriptor>& methods() const;

private:
    std::string entityType_;
    std::string parentType_;
    bool inherited_ = false;
    std::size_t storageSize_ = 0;
    std::vector<PropertyDescriptor> properties_;
    std::vector<MethodDescriptor> methods_;
};

}  // namespace theseed::runtime
