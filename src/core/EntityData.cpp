#include "theseed/core/EntityData.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>

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
        default:
            // String/Blob 是变长类型，定长记 0；非法枚举值同样视为无定长。
            return 0;
    }
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

const PropertyData* EntityData::findPropertyByName(const std::string& name) const {
    for (const auto& prop : properties) {
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
    // 反序列化边界：截断/损坏的输入（网络载荷或磁盘文件）只允许返回 false，
    // 不允许异常穿越到调用方的 tick 循环——MemoryStream 的读溢出抛
    // std::runtime_error，曾让带畸形载荷的请求直接 terminate 服务进程。
    try {
        data.id = stream.readUint64();
        data.entityType = stream.readString();
        const auto count = stream.readUint32();
        // 合理上限防御：垃圾 count 不允许触发超大分配。
        constexpr std::uint32_t kMaxPropertyCount = 1u << 20;
        if (count > kMaxPropertyCount) return false;

        data.properties.resize(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            // decodeProperty 只在流截断时经 MemoryStream 异常报告失败（外层
            // catch 统一转 false），返回值恒为 true，无需逐项检查。
            decodeProperty(stream, data.properties[i]);
        }
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

}  // namespace theseed::core
