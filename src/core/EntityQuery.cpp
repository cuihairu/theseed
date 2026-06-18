#include "theseed/core/EntityQuery.h"
#include "theseed/core/IEntityStore.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace theseed::core {

namespace {

template <typename T>
std::vector<std::byte> encodeNative(T v) {
    std::vector<std::byte> buf(sizeof(T));
    std::memcpy(buf.data(), &v, sizeof(T));
    return buf;
}

template <typename T>
bool decodeNative(const std::vector<std::byte>& buf, T& out) {
    if (buf.size() != sizeof(T)) return false;
    std::memcpy(&out, buf.data(), sizeof(T));
    return true;
}

// 同类型数值 / Bool / String 比较；其他返回 unordered。
// String 走字典序（raw bytes 直接是字符内容，不需要 length prefix）。
std::partial_ordering compareSameType(const PropertyData& lhs, const PropertyData& rhs) {
    if (lhs.type != rhs.type) return std::partial_ordering::unordered;

    switch (lhs.type) {
        case DataType::Int8: {
            std::int8_t a = 0, b = 0;
            if (!decodeNative(lhs.rawValue, a) || !decodeNative(rhs.rawValue, b)) return std::partial_ordering::unordered;
            return a <=> b;
        }
        case DataType::Int16: {
            std::int16_t a = 0, b = 0;
            if (!decodeNative(lhs.rawValue, a) || !decodeNative(rhs.rawValue, b)) return std::partial_ordering::unordered;
            return a <=> b;
        }
        case DataType::Int32: {
            std::int32_t a = 0, b = 0;
            if (!decodeNative(lhs.rawValue, a) || !decodeNative(rhs.rawValue, b)) return std::partial_ordering::unordered;
            return a <=> b;
        }
        case DataType::Int64: {
            std::int64_t a = 0, b = 0;
            if (!decodeNative(lhs.rawValue, a) || !decodeNative(rhs.rawValue, b)) return std::partial_ordering::unordered;
            return a <=> b;
        }
        case DataType::UInt8: {
            std::uint8_t a = 0, b = 0;
            if (!decodeNative(lhs.rawValue, a) || !decodeNative(rhs.rawValue, b)) return std::partial_ordering::unordered;
            return a <=> b;
        }
        case DataType::UInt16: {
            std::uint16_t a = 0, b = 0;
            if (!decodeNative(lhs.rawValue, a) || !decodeNative(rhs.rawValue, b)) return std::partial_ordering::unordered;
            return a <=> b;
        }
        case DataType::UInt32: {
            std::uint32_t a = 0, b = 0;
            if (!decodeNative(lhs.rawValue, a) || !decodeNative(rhs.rawValue, b)) return std::partial_ordering::unordered;
            return a <=> b;
        }
        case DataType::UInt64: {
            std::uint64_t a = 0, b = 0;
            if (!decodeNative(lhs.rawValue, a) || !decodeNative(rhs.rawValue, b)) return std::partial_ordering::unordered;
            return a <=> b;
        }
        case DataType::Float32: {
            float a = 0, b = 0;
            if (!decodeNative(lhs.rawValue, a) || !decodeNative(rhs.rawValue, b)) return std::partial_ordering::unordered;
            return a <=> b;
        }
        case DataType::Float64: {
            double a = 0, b = 0;
            if (!decodeNative(lhs.rawValue, a) || !decodeNative(rhs.rawValue, b)) return std::partial_ordering::unordered;
            return a <=> b;
        }
        case DataType::Bool: {
            std::uint8_t a = 0, b = 0;
            if (!decodeNative(lhs.rawValue, a) || !decodeNative(rhs.rawValue, b)) return std::partial_ordering::unordered;
            return (a != 0) <=> (b != 0);
        }
        case DataType::String: {
            std::string a(lhs.rawValue.begin(), lhs.rawValue.end());
            std::string b(rhs.rawValue.begin(), rhs.rawValue.end());
            return a <=> b;
        }
        case DataType::Vector3:
        case DataType::Blob:
            // 大小比较无意义，本接口不支持。
            return std::partial_ordering::unordered;
    }
    return std::partial_ordering::unordered;
}

}  // namespace

std::partial_ordering compareProperty(const PropertyData& lhs, const PropertyData& rhs) {
    return compareSameType(lhs, rhs);
}

bool matchesFilter(const EntityData& entity, const QueryFilter& filter) {
    const auto* prop = entity.findPropertyByName(filter.propertyName);
    if (prop == nullptr) return false;

    PropertyData filterProp;
    filterProp.name = filter.propertyName;
    filterProp.type = filter.type;
    filterProp.rawValue = filter.rawValue;

    const auto cmp = compareSameType(*prop, filterProp);
    if (cmp == std::partial_ordering::unordered) return false;

    switch (filter.op) {
        case QueryOp::Eq: return cmp == 0;
        case QueryOp::Ne: return cmp != 0;
        case QueryOp::Lt: return cmp < 0;
        case QueryOp::Le: return cmp <= 0;
        case QueryOp::Gt: return cmp > 0;
        case QueryOp::Ge: return cmp >= 0;
    }
    return false;
}

// --- QueryFilter typed constructors ---

QueryFilter QueryFilter::of(std::string name, QueryOp op, std::int32_t v) {
    QueryFilter f;
    f.propertyName = std::move(name);
    f.op = op;
    f.type = DataType::Int32;
    f.rawValue = encodeNative(v);
    return f;
}

QueryFilter QueryFilter::of(std::string name, QueryOp op, std::int64_t v) {
    QueryFilter f;
    f.propertyName = std::move(name);
    f.op = op;
    f.type = DataType::Int64;
    f.rawValue = encodeNative(v);
    return f;
}

QueryFilter QueryFilter::of(std::string name, QueryOp op, std::uint32_t v) {
    QueryFilter f;
    f.propertyName = std::move(name);
    f.op = op;
    f.type = DataType::UInt32;
    f.rawValue = encodeNative(v);
    return f;
}

QueryFilter QueryFilter::of(std::string name, QueryOp op, std::uint64_t v) {
    QueryFilter f;
    f.propertyName = std::move(name);
    f.op = op;
    f.type = DataType::UInt64;
    f.rawValue = encodeNative(v);
    return f;
}

QueryFilter QueryFilter::of(std::string name, QueryOp op, float v) {
    QueryFilter f;
    f.propertyName = std::move(name);
    f.op = op;
    f.type = DataType::Float32;
    f.rawValue = encodeNative(v);
    return f;
}

QueryFilter QueryFilter::of(std::string name, QueryOp op, double v) {
    QueryFilter f;
    f.propertyName = std::move(name);
    f.op = op;
    f.type = DataType::Float64;
    f.rawValue = encodeNative(v);
    return f;
}

QueryFilter QueryFilter::of(std::string name, QueryOp op, bool v) {
    QueryFilter f;
    f.propertyName = std::move(name);
    f.op = op;
    f.type = DataType::Bool;
    const std::uint8_t b = v ? 1 : 0;
    f.rawValue = encodeNative(b);
    return f;
}

QueryFilter QueryFilter::of(std::string name, QueryOp op, std::string v) {
    QueryFilter f;
    f.propertyName = std::move(name);
    f.op = op;
    f.type = DataType::String;
    f.rawValue.assign(
        reinterpret_cast<const std::byte*>(v.data()),
        reinterpret_cast<const std::byte*>(v.data()) + v.size());
    return f;
}

// --- InMemoryQueryStore ---

InMemoryQueryStore::InMemoryQueryStore(std::shared_ptr<IEntityStore> store)
    : store_(std::move(store)) {
    if (!store_) throw std::invalid_argument("InMemoryQueryStore requires a non-null store");
}

namespace {

bool matchesAllFilters(const EntityData& data, const std::vector<QueryFilter>& filters) {
    for (const auto& f : filters) {
        if (!matchesFilter(data, f)) return false;
    }
    return true;
}

}  // namespace

std::vector<EntityId> InMemoryQueryStore::query(const StorageQuery& q) {
    std::vector<EntityId> result;
    if (q.entityType.empty()) return result;

    auto ids = store_->listIdsByType(q.entityType);
    std::sort(ids.begin(), ids.end());  // 保证 offset/limit 行为可预测

    std::size_t matched = 0;
    for (EntityId id : ids) {
        EntityData data;
        if (!store_->load(id, q.entityType, data)) continue;
        if (!matchesAllFilters(data, q.filters)) continue;

        if (matched < q.offset) {
            ++matched;
            continue;
        }
        result.push_back(id);
        ++matched;
        if (result.size() >= q.limit) break;
    }
    return result;
}

}  // namespace theseed::core
