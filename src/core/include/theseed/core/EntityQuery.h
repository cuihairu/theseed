#pragma once

#include "theseed/core/EntityData.h"
#include "theseed/core/IEntityStore.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace theseed::core {

// MVP Phase B-5: Level 2 结构化查询接口。
//
// 与 docs/design/4-data-and-ops/02-persistence.md §2.2 对齐：
//   - IEntityQueryStore.query(StorageQuery) 返回匹配的 EntityId 列表
//   - 仅承诺基于属性的结构化比较（EQ/NE/LT/LE/GT/GE）
//   - 多 filter 之间是 AND（YAGNI：OR / 嵌套表达式留给 Level 3 IRawQueryExecutor）
//
// 不在本接口范围内（§2.3 / §2.4）：
//   - queryJsonPath（需要 JSON 列存储，当前 EntityData 用二进制 MemoryStream）
//   - executeRaw（仅 MySQL/PG 后端）
//   - schema migration / merge / local archive

enum class QueryOp : std::uint8_t {
    Eq = 0,
    Ne,
    Lt,
    Le,
    Gt,
    Ge,
};

// 单个过滤条件：propertyName op rawValue(type)
// rawValue 与 PropertyData.rawValue 同格式（小端 native 字节）。
struct QueryFilter {
    std::string propertyName;
    QueryOp op = QueryOp::Eq;
    DataType type = DataType::Int32;
    std::vector<std::byte> rawValue;

    // 类型安全的构造器：避免调用方手写 byte 编码。
    static QueryFilter of(std::string name, QueryOp op, std::int32_t v);
    static QueryFilter of(std::string name, QueryOp op, std::int64_t v);
    static QueryFilter of(std::string name, QueryOp op, std::uint32_t v);
    static QueryFilter of(std::string name, QueryOp op, std::uint64_t v);
    static QueryFilter of(std::string name, QueryOp op, float v);
    static QueryFilter of(std::string name, QueryOp op, double v);
    static QueryFilter of(std::string name, QueryOp op, bool v);
    static QueryFilter of(std::string name, QueryOp op, std::string v);
};

struct StorageQuery {
    std::string entityType;
    std::vector<QueryFilter> filters;   // AND
    std::size_t limit = 100;
    std::size_t offset = 0;
};

class IEntityQueryStore {
public:
    virtual ~IEntityQueryStore() = default;
    virtual std::vector<EntityId> query(const StorageQuery& q) = 0;
};

// 包装任意 IEntityStore（如 InMemoryEntityStore / FileEntityStore），
// 在其上提供 Level 2 结构化查询。
class InMemoryQueryStore final : public IEntityQueryStore {
public:
    explicit InMemoryQueryStore(std::shared_ptr<IEntityStore> store);

    std::vector<EntityId> query(const StorageQuery& q) override;

private:
    std::shared_ptr<IEntityStore> store_;
};

// 把两个 PropertyData 按其 DataType 比较。返回 std::partial_ordering::unordered 表示
// 类型不兼容或类型不支持大小比较（Vector3 / Blob 不同长 / 类型不匹配）。
// 公开以便测试与未来 RawQueryExecutor 复用。
std::partial_ordering compareProperty(const PropertyData& lhs,
                                      const PropertyData& rhs);

// 单条 filter 对单条 EntityData 求值。
bool matchesFilter(const EntityData& entity, const QueryFilter& filter);

}  // namespace theseed::core
