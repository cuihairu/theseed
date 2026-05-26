#pragma once

#include "theseed/foundation/MemoryStream.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace theseed::core {

using foundation::MemoryStream;

using EntityId = std::uint64_t;
using PropertyId = std::uint32_t;

enum class DataType : std::uint8_t {
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

struct PropertyData {
    PropertyId id = 0;
    std::string name;
    DataType type = DataType::Int32;
    std::vector<std::byte> rawValue;

    static std::size_t fixedSizeOfType(DataType type);
    static bool isVariableSized(DataType type);
};

struct EntityData {
    EntityId id = 0;
    std::string entityType;
    std::vector<PropertyData> properties;

    PropertyData* findProperty(PropertyId id);
    const PropertyData* findProperty(PropertyId id) const;
    PropertyData* findPropertyByName(const std::string& name);
};

void encodeProperty(MemoryStream& stream, const PropertyData& prop);
bool decodeProperty(MemoryStream& stream, PropertyData& prop);

void encodeEntityData(MemoryStream& stream, const EntityData& data);
bool decodeEntityData(MemoryStream& stream, EntityData& data);

}  // namespace theseed::core
