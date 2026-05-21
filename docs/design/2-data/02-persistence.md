# Persistence — 持久化与查询

> theseed 持久化层：多后端存储抽象、JSON 查询、聚合统计。
>
> 来源：KBEngine 只有 EntityTable CRUD，BigWorld 有 Archiver + SecondaryDB。

---

## 1. JSON 原生支持

### 1.1 为什么不用 BLOB 存 ARRAY/FIXED_DICT

```
BLOB 的问题：
  1. 无法在 SQL 中查询
  2. 无法建索引
  3. 无法做统计/聚合
  4. 调试困难（SELECT * 看到乱码）
  5. 跨语言访问困难

JSON 的优势：
  1. 可查询：JSON_EXTRACT(equipment, '$.weapon') = 500
  2. 可索引：MySQL 8.0 多值索引、PostgreSQL GIN 索引
  3. 可读
  4. 可统计
  5. 工具友好
```

### 1.2 JSON 查询接口

```python
import theseed

# 结构化查询
results = theseed.db.query("Player") \
    .filter(theseed.db.json_path("equipment.weapon") == 500) \
    .filter(theseed.db.field("level") >= 50) \
    .limit(100) \
    .execute()

# 自定义 SQL
results = theseed.db.execute("""
    SELECT id, name, JSON_EXTRACT(equipment, '$.weapon') as weapon_id
    FROM tbl_Player
    WHERE level > :min_level
    ORDER BY level DESC
    LIMIT :limit
""", min_level=50, limit=100)

# 聚合查询
count = theseed.db.query("Player") \
    .filter(theseed.db.json_path("stats.guild_id") == guild_id) \
    .count()
```

### 1.3 自定义查询的安全机制

```cpp
class CustomQuery {
public:
    // 参数化查询（防 SQL 注入）
    QueryResult execute(const std::string& entity,
                        const std::string& sql,
                        const QueryParams& params);

    // 查询白名单
    void registerNamedQuery(const std::string& name,
                            const std::string& sqlTemplate);

    QueryResult executeNamed(const std::string& name,
                             const QueryParams& params);
};
```

---

## 2. 存储后端抽象

```cpp
// storage/IStorageBackend.h

class IStorageBackend {
public:
    // 基本 CRUD
    virtual Future<EntityData> load(EntityId id, const EntityDef& def) = 0;
    virtual Future<void> save(EntityId id, const EntityData& data, const EntityDef& def) = 0;
    virtual Future<void> remove(EntityId id) = 0;

    // 查询
    virtual Future<std::vector<EntityId>> query(const StorageQuery& q) = 0;

    // 自定义查询
    virtual Future<QueryResult> executeRaw(const std::string& sql,
                                           const QueryParams& params) = 0;
    virtual Future<QueryResult> executeNamed(const std::string& queryName,
                                             const QueryParams& params) = 0;

    // JSON 查询
    virtual Future<std::vector<EntityId>> queryJsonPath(
        const std::string& entityType,
        const std::string& jsonPath,
        const std::string& op,
        const QueryParam& value) = 0;

    // 聚合
    virtual Future<AggregateResult> aggregate(
        const std::string& entityType,
        const std::string& field,
        AggregateOp op,
        const StorageQuery& filter) = 0;

    // Schema 管理
    virtual Future<void> createTable(const EntityDef& def) = 0;
    virtual Future<MigrationPlan> planMigration(const EntityDef& oldDef,
                                                 const EntityDef& newDef) = 0;
    virtual Future<void> executeMigration(const MigrationPlan& plan) = 0;

    // 合服
    virtual Future<MergeReport> mergeFrom(const IStorageBackend& source,
                                          const MergeConfig& config) = 0;

    // 后端能力
    virtual std::string backendName() const = 0;
    virtual std::vector<std::string> capabilities() const = 0;
};

enum class AggregateOp { COUNT, SUM, AVG, MIN, MAX };
```

---

## 3. 多后端实现

### 3.1 MySQL 后端

```cpp
Future<std::vector<EntityId>>
MySQLBackend::queryJsonPath(const std::string& entityType,
                             const std::string& jsonPath,
                             const std::string& op,
                             const QueryParam& value) {
    // MySQL 5.7+: JSON_EXTRACT
    // MySQL 8.0+: -> 操作符 + 多值索引
}
```

### 3.2 PostgreSQL 后端

```cpp
// JSONB: @> 包含、? key存在、->/->> 路径、GIN 索引
Future<std::vector<EntityId>>
PostgreSQLBackend::queryJsonPath(...) {
    // WHERE {} @> '{"weapon": 500}'
}
```

### 3.3 MongoDB 后端

```cpp
// 天然文档模型：FIXED_DICT → 嵌套文档，ARRAY → 数组
Future<std::vector<EntityId>>
MongoDBBackend::queryJsonPath(...) {
    // db.tbl_Player.find({"equipment.weapon": 500})
}
```

---

## 4. 开发时校验

### 4.1 XSD 结构校验（第一层）

```
XSD 自动校验（编辑器保存时）：
  ✅ 元素结构：Properties 必须包含 property 子元素
  ✅ 类型枚举：type 只能是 UINT8/UINT32/STRING/FIXED_DICT 等
  ✅ 必填检查：name 和 type 是 required 属性
  ✅ 值域约束：size 正整数，persistent 布尔值
  ✅ Flags 枚举：ALL_CLIENTS | BASE | OWN_CLIENT 等
  ✅ column_type 枚举：VARCHAR | JSON | SUBTABLE 等
```

### 4.2 defcheck 语义校验（第二层）

```
典型校验规则：
  - string_without_size (error): STRING 没有 size 限制
  - string_size_exceeds_column (error): STRING size 超过列长度
  - blob_without_max (warning): BLOB 没有 max 限制
  - array_too_large_for_blob (warning): ARRAY 可能超 BLOB 限制
  - json_without_path_index (warning): JSON 列建议添加索引
  - type_change_breaks_storage (error): 类型变更需要数据迁移
```

### 4.3 校验流程

```
def 变更时自动执行：
  1. XSD 校验 → 结构错误标红
  2. defcheck → 语义校验报告
  3. XInclude 合并 → 冲突检测
  4. 生成 IDE 提示（.pyi / .d.ts）
  5. Schema diff → 迁移计划
  6. CI 集成 → error 阻断提交
```
