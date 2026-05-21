# Persistence — 持久化与查询

> theseed 持久化层：实体存储、查询扩展、Schema 管理。
>
> 来源：KBEngine 只有 EntityTable CRUD，BigWorld 有 Archiver + SecondaryDB。
> 当前实现基线以 [../0-foundation/01-mvp-architecture-baseline](../0-foundation/01-mvp-architecture-baseline.md) 为准。
> 本篇只讨论主持久化与查询，不展开本地归档暂存层；后者见 [04-secondary-db](04-secondary-db.md)。

---

## 0. MVP 边界

```
MVP 先解决：
  - Entity load / save / remove
  - 基础结构化查询
  - MySQL 后端
  - JSON 列查询

MVP 不要求一次解决：
  - 所有后端的统一高级查询语义
  - 聚合与原始 SQL 的跨后端等价抽象
  - Schema 迁移、合服、查询平台能力全部塞进一个接口
  - BaseApp 本地 SecondaryDB / LocalArchiveStore
```

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

# 自定义 SQL（仅 MySQL/PG 适用）
results = theseed.db.execute("""
    SELECT id, name, JSON_EXTRACT(equipment, '$.weapon') as weapon_id
    FROM tbl_Player
    WHERE level > :min_level
    ORDER BY level DESC
    LIMIT :limit
""", min_level=50, limit=100)
```

---

## 2. 最小职责接口

> 核心原则：运行时只依赖最小接口，不让 `IStorageBackend` 变成“数据库平台总入口”。

### 2.1 Entity 存储接口

```cpp
// storage/IEntityStore.h

class IEntityStore {
public:
    virtual ~IEntityStore() = default;

    virtual Future<EntityData> load(EntityId id, const EntityDef& def) = 0;
    virtual Future<void> save(EntityId id,
                              const EntityData& data,
                              const EntityDef& def) = 0;
    virtual Future<void> remove(EntityId id) = 0;
};
```

### 2.2 结构化查询接口

```cpp
// storage/IEntityQueryStore.h

class IEntityQueryStore {
public:
    virtual ~IEntityQueryStore() = default;

    virtual Future<std::vector<EntityId>> query(const StorageQuery& q) = 0;
    virtual Future<std::vector<EntityId>> queryJsonPath(
        const std::string& entityType,
        const std::string& jsonPath,
        const std::string& op,
        const QueryParam& value) = 0;
};
```

### 2.3 后端特定查询接口

```cpp
// storage/IRawQueryExecutor.h

class IRawQueryExecutor {
public:
    virtual ~IRawQueryExecutor() = default;

    virtual Future<QueryResult> executeRaw(const std::string& sql,
                                           const QueryParams& params) = 0;
    virtual Future<QueryResult> executeNamed(const std::string& queryName,
                                             const QueryParams& params) = 0;
};
```

### 2.4 Schema 管理接口

```cpp
// storage/ISchemaMigrator.h

class ISchemaMigrator {
public:
    virtual ~ISchemaMigrator() = default;

    virtual Future<void> createTable(const EntityDef& def) = 0;
    virtual Future<MigrationPlan> planMigration(const EntityDef& oldDef,
                                                const EntityDef& newDef) = 0;
    virtual Future<void> executeMigration(const MigrationPlan& plan) = 0;
};
```

### 2.5 合服接口（非 MVP 主路径）

```cpp
// storage/IMergeBackend.h

class IMergeBackend {
public:
    virtual ~IMergeBackend() = default;

    virtual Future<MergeReport> mergeFrom(const IMergeBackend& source,
                                          const MergeConfig& config) = 0;
};
```

### 2.6 本地归档暂存接口（非 Runtime Core 主依赖）

```cpp
// storage/ILocalArchiveStore.h

class ILocalArchiveStore {
public:
    virtual ~ILocalArchiveStore() = default;

    virtual Future<void> append(const ArchiveSnapshot& snapshot) = 0;
    virtual Future<void> flush() = 0;
    virtual Future<ArchiveGenerationMeta> rotate() = 0;
};
```

说明：

```
ILocalArchiveStore 属于：
  - Archiver 增强
  - 数据运维工具链

ILocalArchiveStore 不属于：
  - Runtime Core 的主持久化依赖
  - IEntityStore 的职责范围
```

---

## 3. 为什么要拆接口

```
如果把 CRUD、原始 SQL、命名查询、JSON Path、聚合、Schema 迁移、合服、
本地归档暂存、consolidate 全部塞进一个 IStorageBackend，会出现三个问题：

1. SRP 被破坏
   - 一个接口承载太多职责
   - Runtime、工具链、运维平台都被迫依赖同一个“大接口”

2. ISP 被破坏
   - 内核只想 load/save，却被迫知道 mergeFrom / executeRaw / aggregate

3. 最小公分母设计
   - 为了兼容所有后端，抽象会越来越虚
   - 最后不是 everywhere if(capabilities)，就是一堆不可验证的承诺
```

拆分后的依赖关系：

```
Runtime Core
  → IEntityStore

业务查询层
  → IEntityQueryStore

运维/数据工具
  → IRawQueryExecutor / ISchemaMigrator / IMergeBackend

归档增强
  → ILocalArchiveStore / IArchiveConsolidator
```

---

## 4. MVP 后端策略

### 4.1 MySQL 后端（首选）

```cpp
class MySQLEntityStore : public IEntityStore,
                         public IEntityQueryStore,
                         public IRawQueryExecutor,
                         public ISchemaMigrator {
    // MVP 的默认组合
};
```

```
MySQL 作为 MVP 默认后端的原因：
  - 和现有 BigWorld / KBEngine 用户习惯接近
  - JSON 支持已经足够实用
  - 运维成本低
  - 适合先跑通实体生命周期闭环
```

### 4.2 PostgreSQL / MongoDB（Phase 2）

```cpp
class PostgreSQLEntityStore : public IEntityStore,
                              public IEntityQueryStore,
                              public IRawQueryExecutor,
                              public ISchemaMigrator {
    // Phase 2
};

class MongoEntityStore : public IEntityStore,
                         public IEntityQueryStore {
    // Phase 2
};
```

```
说明：
  - PG 和 MongoDB 仍然是合理方向
  - 但不应该阻塞 MySQL MVP
  - 也不应该要求三者首版就具备完全等价的高级能力
```

---

## 5. 能力分层

```
Level 1: 核心运行时必须依赖
  - load
  - save
  - remove

Level 2: 业务开发高频使用
  - query
  - queryJsonPath

Level 3: 平台和运维增强
  - executeRaw
  - executeNamed
  - schema migration
  - merge
  - local archive store
  - consolidate / transfer / sync
```

```
设计要求：
  - Level 1 必须稳定、简单、可测
  - Level 2 可以按后端逐步增强
  - Level 3 不得反向污染 Runtime Core
```

---

## 6. 开发时校验

### 6.1 XSD 结构校验（第一层）

```
XSD 自动校验（编辑器保存时）：
  ✅ 元素结构：Properties 必须包含 property 子元素
  ✅ 类型枚举：type 只能是 UINT8/UINT32/STRING/FIXED_DICT 等
  ✅ 必填检查：name 和 type 是 required 属性
  ✅ 值域约束：size 正整数，persistent 布尔值
  ✅ Flags 枚举：ALL_CLIENTS | BASE | OWN_CLIENT 等
  ✅ column_type 枚举：VARCHAR | JSON | SUBTABLE 等
```

### 6.2 defcheck 语义校验（第二层）

```
典型校验规则：
  - string_without_size (error): STRING 没有 size 限制
  - string_size_exceeds_column (error): STRING size 超过列长度
  - blob_without_max (warning): BLOB 没有 max 限制
  - array_too_large_for_blob (warning): ARRAY 可能超 BLOB 限制
  - json_without_path_index (warning): JSON 列建议添加索引
  - type_change_breaks_storage (error): 类型变更需要数据迁移
```

### 6.3 校验流程

```
def 变更时自动执行：
  1. XSD 校验 → 结构错误标红
  2. defcheck → 语义校验报告
  3. XInclude 合并 → 冲突检测
  4. 生成 IDE 提示（.pyi / .d.ts）
  5. Schema diff → 迁移计划
  6. CI 集成 → error 阻断提交
```
