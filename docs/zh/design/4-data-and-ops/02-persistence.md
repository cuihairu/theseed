# Persistence — Persistence and Query

> English | **中文**

> theseed's persistence layer: entity storage, query extension, Schema management.
>
> Origin: BigWorld `Archiver / SecondaryDB / consolidate_dbs`.
> Reference implementation: KBEngine's EntityTable CRUD main path.
> The current implementation baseline follows [../0-foundation/01-mvp-architecture-baseline](../0-foundation/01-mvp-architecture-baseline.md).
> This document only discusses primary persistence and query, and does not expand on the local archive staging layer; for the latter see [03-local-archive-and-secondary-db](03-local-archive-and-secondary-db.md).

---

## 0.5 Engine Implementation Comparison and Trade-offs

### How BigWorld implements it

```
BigWorld's persistence is not single-table CRUD:
  - Archiver handles archiving
  - SecondaryDB handles local staging and later consolidation
  - There is also a server-merge / sync / repair toolchain
```

### How KBEngine implements it

```
KBEngine is mainly EntityTable-level loading and saving,
leaning overall toward basic CRUD rather than a complete data ops system.
```

### Pros and cons

```
BigWorld's pros:
  - A complete data hierarchy
  - Can support more complex operational actions

KBEngine's pros:
  - Simple
  - Easy to land

Shared cons:
  - Once archiving, sync and repair are introduced, the boundaries must be very clear
```

### theseed's trade-off

```
theseed splits primary persistence and the local archive staging layer apart,
avoiding mistaking SecondaryDB for the primary database,
and avoiding stuffing toolchain capabilities into the online read/write interface.
```

---

## 0. MVP boundary

```
The MVP solves first:
  - Entity load / save / remove
  - Basic structured queries
  - MySQL backend
  - JSON column queries

The MVP is not required to solve all at once:
  - Unified advanced query semantics across all backends
  - Cross-backend equivalent abstraction of aggregation and raw SQL
  - Schema migration, server merge, and query platform capabilities all stuffed into one interface
  - BaseApp local SecondaryDB / LocalArchiveStore
```

---

## 1. Native JSON support

### 1.1 Why not store ARRAY/FIXED_DICT as BLOB

```
Problems with BLOB:
  1. Cannot be queried in SQL
  2. Cannot be indexed
  3. Cannot do statistics/aggregation
  4. Hard to debug (SELECT * shows garbage)
  5. Hard to access across languages

Advantages of JSON:
  1. Queryable: JSON_EXTRACT(equipment, '$.weapon') = 500
  2. Indexable: MySQL 8.0 multi-valued indexes, PostgreSQL GIN indexes
  3. Readable
  4. Statistics-friendly
  5. Tool-friendly
```

### 1.2 JSON query interface

```python
import theseed

# Structured query
results = theseed.db.query("Player") \
    .filter(theseed.db.json_path("equipment.weapon") == 500) \
    .filter(theseed.db.field("level") >= 50) \
    .limit(100) \
    .execute()

# Custom SQL (only applicable to MySQL/PG)
results = theseed.db.execute("""
    SELECT id, name, JSON_EXTRACT(equipment, '$.weapon') as weapon_id
    FROM tbl_Player
    WHERE level > :min_level
    ORDER BY level DESC
    LIMIT :limit
""", min_level=50, limit=100)
```

---

## 2. Minimal-responsibility interfaces

> Core principle: the runtime depends only on minimal interfaces, and does not let `IStorageBackend` become a "general entry point to the database platform".

### 2.1 Entity storage interface

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

### 2.2 Structured query interface

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

### 2.3 Backend-specific query interface

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

### 2.4 Schema management interface

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

### 2.5 Server merge interface (not on the MVP main path)

```cpp
// storage/IMergeBackend.h

class IMergeBackend {
public:
    virtual ~IMergeBackend() = default;

    virtual Future<MergeReport> mergeFrom(const IMergeBackend& source,
                                          const MergeConfig& config) = 0;
};
```

### 2.6 Local archive staging interface (not a main Runtime Core dependency)

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

Notes:

```
ILocalArchiveStore belongs to:
  - Archiver enhancement
  - The data ops toolchain

ILocalArchiveStore does not belong to:
  - Runtime Core's primary persistence dependencies
  - The responsibility scope of IEntityStore
```

---

## 3. Why split the interfaces

```
If CRUD, raw SQL, named queries, JSON Path, aggregation, Schema migration, server merge,
local archive staging and consolidate were all stuffed into a single IStorageBackend, three problems would arise:

1. SRP is violated
   - One interface carries too many responsibilities
   - Runtime, the toolchain and the ops platform are all forced to depend on the same "big interface"

2. ISP is violated
   - The kernel only wants load/save, but is forced to know about mergeFrom / executeRaw / aggregate

3. Least-common-denominator design
   - To stay compatible with all backends, the abstraction becomes ever more hollow
   - In the end it is either everywhere if(capabilities), or a pile of unverifiable promises
```

Dependencies after splitting:

```
Runtime Core
  → IEntityStore

Business query layer
  → IEntityQueryStore

Ops / data tools
  → IRawQueryExecutor / ISchemaMigrator / IMergeBackend

Archive enhancement
  → ILocalArchiveStore / IArchiveConsolidator
```

---

## 4. MVP backend strategy

### 4.1 MySQL backend (preferred)

```cpp
class MySQLEntityStore : public IEntityStore,
                         public IEntityQueryStore,
                         public IRawQueryExecutor,
                         public ISchemaMigrator {
    // The MVP's default combination
};
```

```
Reasons MySQL is the MVP default backend:
  - Close to the habits of existing BigWorld / KBEngine users
  - JSON support is already practical enough
  - Low ops cost
  - Suited to getting the entity lifecycle loop working end to end first
```

### 4.2 PostgreSQL / MongoDB (Phase 2)

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
Notes:
  - PG and MongoDB remain sensible directions
  - But they should not block the MySQL MVP
  - Nor should all three be required to have fully equivalent advanced capabilities in the first release
```

---

## 5. Capability layering

```
Level 1: The core runtime must depend on it
  - load
  - save
  - remove

Level 2: High-frequency use in business development
  - query
  - queryJsonPath

Level 3: Platform and ops enhancement
  - executeRaw
  - executeNamed
  - schema migration
  - merge
  - local archive store
  - consolidate / transfer / sync
```

```
Design requirements:
  - Level 1 must be stable, simple and testable
  - Level 2 can be enhanced incrementally per backend
  - Level 3 must not pollute Runtime Core in reverse
```

---

## 6. Validation during development

### 6.1 XSD structural validation (first layer)

```
XSD automatic validation (when the editor saves):
  ✅ Element structure: Properties must contain property child elements
  ✅ Type enumeration: type can only be UINT8/UINT32/STRING/FIXED_DICT etc.
  ✅ Required checks: name and type are required attributes
  ✅ Value-domain constraints: size a positive integer, persistent a boolean
  ✅ Flags enumeration: ALL_CLIENTS | BASE | OWN_CLIENT etc.
  ✅ column_type enumeration: VARCHAR | JSON | SUBTABLE etc.
```

### 6.2 defcheck semantic validation (second layer)

```
Typical validation rules:
  - string_without_size (error): STRING has no size limit
  - string_size_exceeds_column (error): STRING size exceeds the column length
  - blob_without_max (warning): BLOB has no max limit
  - array_too_large_for_blob (warning): ARRAY may exceed the BLOB limit
  - json_without_path_index (warning): a JSON column is advised to add an index
  - type_change_breaks_storage (error): a type change requires data migration
```

### 6.3 Validation flow

```
Automatically executed when a def changes:
  1. XSD validation → structural errors highlighted in red
  2. defcheck → semantic validation report
  3. XInclude merge → conflict detection
  4. Generate IDE hints (.pyi / .d.ts)
  5. Schema diff → migration plan
  6. CI integration → error blocks the commit
```
