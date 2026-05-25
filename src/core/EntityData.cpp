#include "theseed/core/EntityData.h"

#include <cstring>

namespace theseed::core {

std::size_t PropertyData::fixedSizeOfType(DataType type) {
    switch (type) {
        case DataType::Int8:    return 1;
        case DataType::Int16:   return 2;
        case DataType::Int32:   return 4;
        case DataType::Int64:   return 8;
        case DataType::UInt8:   return 1;
        case DataType::UInt16:  return 2;
        case DataType::UInt32:  return 4;
        case DataType::UInt64:  return 8;
        case DataType::Float32: return 4;
        case DataType::Float64: return 8;
        case DataType::Bool:    return 1;
        case DataType::Vector3: return 12;
        case DataType::String:  return 0;
        case DataType::Blob:    return 0;
    }
    return 0;
}

bool PropertyData::isVariableSized(DataType type) {
    return type == DataType::String || type == DataType::Blob;
}

PropertyData* EntityData::findProperty(PropertyId id) {
    for (auto& prop : properties) {
        if (prop.id == id) return &prop;
    }
    return nullptr;
}

const PropertyData* EntityData::findProperty(PropertyId id) const {
    for (const auto& prop : properties) {
        if (prop.id == id) return &prop;
    }
    return nullptr;
}

PropertyData* EntityData::findPropertyByName(const std::string& name) {
    for (auto& prop : properties) {
        if (prop.name == name) return &prop;
    }
    return nullptr;
}

void encodeProperty(MemoryStream& stream, const PropertyData& prop) {
    stream.writeUint32(prop.id);
    stream.writeString(prop.name);
    stream.writeUint8(static_cast<std::uint8_t>(prop.type));

    if (PropertyData::isVariableSized(prop.type)) {
        stream.writeUint32(static_cast<std::uint32_t>(prop.rawValue.size()));
    }
    if (!prop.rawValue.empty()) {
        stream.writeBytes(prop.rawValue.data(), prop.rawValue.size());
    }
}

bool decodeProperty(MemoryStream& stream, PropertyData& prop) {
    prop.id = stream.readUint32();
    prop.name = stream.readString();
    prop.type = static_cast<DataType>(stream.readUint8());

    std::size_t size = 0;
    if (PropertyData::isVariableSized(prop.type)) {
        size = static_cast<std::size_t>(stream.readUint32());
    } else {
        size = PropertyData::fixedSizeOfType(prop.type);
    }

    prop.rawValue.resize(size);
    if (size > 0) {
        stream.readBytes(prop.rawValue.data(), size);
    }
    return true;
}

void encodeEntityData(MemoryStream& stream, const EntityData& data) {
    stream.writeUint64(data.id);
    stream.writeString(data.entityType);
    stream.writeUint32(static_cast<std::uint32_t>(data.properties.size()));

    for (const auto& prop : data.properties) {
        encodeProperty(stream, prop);
    }
}

bool decodeEntityData(MemoryStream& stream, EntityData& data) {
    data.id = stream.readUint64();
    data.entityType = stream.readString();
    const auto count = stream.readUint32();

    data.properties.resize(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        if (!decodeProperty(stream, data.properties[i])) {
            return false;
        }
    }
    return true;
}

}  // namespace theseed::core
