# Data Definition — Data Definition and XML/XSD

> **English** | [中文](01-data-definition.zh.md)

> theseed's data definition system: one XML definition → three-layer mapping across memory/network/storage.
>
> Format: XML + XSD Schema validation + XInclude file splitting.
> Origin: BigWorld's property-level mapping (15+ kinds of PropertyMapping).
> Reference implementation: KBEngine simplifies the mapping to 5 kinds of TABLE_ITEM_TYPE.
> theseed performs a modern convergence while preserving property-level expressiveness.

---

## 0.5 Engine Implementation Comparison and Trade-offs

### How BigWorld implements it

```
BigWorld's data definition is very fine-grained:
  - Each property can be mapped independently
  - The data definition simultaneously affects memory, network and database
  - PropertyMapping has a rich variety of kinds
```

### How KBEngine implements it

```
KBEngine retains the data definition mainline,
but shrinks the mapping types to something simpler,
emphasizing more that "the definition can run".
```

### Pros and cons

```
BigWorld's pros:
  - Strong expressiveness
  - Suited to complex data models and query requirements

KBEngine's pros:
  - Fewer rules
  - Lower cost to learn and implement

Shared cons:
  - Once the definition layer spirals out of control, it pollutes memory, network and storage all at once
```

### theseed's trade-off

```
theseed inherits BigWorld's idea of "property-level mapping",
but avoids unbounded expansion of mapping kinds,
prioritizing the expressiveness that is actually needed today.
```

---

## 1. BigWorld PropertyMapping (the origin)

BigWorld is the origin of these designs. Its core idea is: **each property can have an independent database mapping strategy**, rather than the whole Entity being treated uniformly.

BigWorld defines a `PropertyMapping` base class, from which all mapping types inherit. The source is in `bigworld/src/server/db/db_mgr.hpp`.

### 1.1 Scalar mappings — one property corresponds to one column

```
IntMapping       INT8/16/32/64 → TINYINT / SMALLINT / INT / BIGINT
FloatMapping     FLOAT32/64    → FLOAT / DOUBLE
StringMapping    STRING        → VARCHAR(N) or TEXT
BoolMapping      BOOL          → TINYINT(1)
BlobMapping      BLOB          → BLOB
```

This is the simplest case: whatever type the property is, the database stores a column of that type, with no conversion.

### 1.2 Expansion mappings — one property expands into multiple columns or rows

This is BigWorld's most valuable design:

**VectorMapping** — Vector3 expands into 3 columns:

```
Entity property:  position = Vector3(100.0, 0.0, 50.0)
                    ↓ VectorMapping
MySQL table:      positionX = 100.0, positionY = 0.0, positionZ = 50.0
```

Why expand? Because 3 independent FLOAT columns can each be indexed and support range queries (`WHERE positionX > 90 AND positionX < 110`). If it were stored as a single BLOB, none of this would be possible.

**CompositeMapping** — FIXED_DICT expands into multiple columns:

```
Entity property:  stats = FIXED_DICT { kill_count=100, death_count=20, play_time=3600 }
                    ↓ CompositeMapping
MySQL table:      stats_kill_count = 100
                  stats_death_count = 20
                  stats_play_time = 3600
```

The field names are prefixed with the property name to guarantee no collision. Each field is an independent column, so it can be indexed on its own and support `WHERE stats_kill_count > 50`.

**ClassMapping** — nested FIXED_DICT expands recursively:

```
Entity property:  equipment = FIXED_DICT {
                      weapon = FIXED_DICT { id=500, level=3 },
                      armor  = FIXED_DICT { id=300, level=2 }
                  }
                    ↓ ClassMapping (recursive)
MySQL table:      equipment_weapon_id = 500
                  equipment_weapon_level = 3
                  equipment_armor_id = 300
                  equipment_armor_level = 2
```

It uses a dot-separated path as the column name prefix, supporting nesting of arbitrary depth. In essence it is the recursive version of CompositeMapping.

**SequenceMapping** — ARRAY expands into a subtable:

```
Entity property:  inventory = ARRAY [ {item_id=1, count=5}, {item_id=2, count=3} ]
                    ↓ SequenceMapping
MySQL subtable:   player_inventory (parent_id FK → Entity table)
                  row 1: parent_id=42, idx=0, item_id=1, count=5
                  row 2: parent_id=42, idx=1, item_id=2, count=3
```

Each element is a row, associated with the parent Entity. Each column of the subtable can be queried/indexed independently — "find all players holding item_id=1" is done with a single SQL statement.

### 1.3 Special mappings

```
EnumMapping       Enum → INT column + string mapping table
                  In memory it is the enum name ("WARRIOR"), in the database an INT (1)
                  A reverse mapping is applied when reading

MailboxMapping    Entity reference → store EntityID + process address
                  Used to persist EntityCall/Mailbox

UserTypeMapping   User-defined type → BLOB fallback
                  For types the engine does not recognize, serialize to binary and store as BLOB

XmlDataSource     Not mapped to SQL, but mapped to an XML file
                  BigWorld's secondary storage scheme (suited to low-frequency-access data)
                  No database table needed, read and write XML directly
```

### 1.4 BigWorld mapping master table

```
Mapping type        │ What it does                    │ DB result
────────────────────┼─────────────────────────────────┼────────────────────
IntMapping          │ Integer → integer column         │ 1 INT column
FloatMapping        │ Float → float column             │ 1 FLOAT/DOUBLE column
StringMapping       │ String → VARCHAR/TEXT            │ 1 column
BoolMapping         │ Bool → TINYINT                   │ 1 column
BlobMapping         │ Binary → BLOB                    │ 1 BLOB column
VectorMapping       │ Vector3 → 3 FLOAT columns        │ 3 columns
EnumMapping         │ Enum name → INT + mapping table  │ 1 INT column
CompositeMapping    │ FIXED_DICT → multiple columns (one-level expansion) │ N columns
ClassMapping        │ Nested FIXED_DICT → multiple columns (recursive expansion) │ N columns (dot-separated prefix)
SequenceMapping     │ ARRAY → subtable (one row per element) │ Subtable + FK
MailboxMapping      │ Entity reference → ID + address  │ 2 columns
UserTypeMapping     │ Custom type → BLOB fallback      │ 1 BLOB column
XmlDataSource       │ Whole Entity → XML file          │ No SQL involved
```

**BigWorld's limitation**: it only supports MySQL (+ XML as secondary storage). It does not support JSON (MySQL had no JSON type in 2005). It does not support PostgreSQL / MongoDB.

---

## 2. KBEngine's trade-off

KBEngine referenced BigWorld's Entity model, but greatly simplified the database mapping.

### 2.1 KBEngine keeps only 5 kinds of TABLE_ITEM_TYPE

```
TABLE_ITEM_TYPE_STRING   → VARCHAR / TEXT
TABLE_ITEM_TYPE_DIGIT    → INT / BIGINT (chosen automatically by digit count)
TABLE_ITEM_TYPE_VECTOR3  → 3 × FLOAT (this expansion was kept)
TABLE_ITEM_TYPE_BLOB     → BLOB
TABLE_ITEM_TYPE_UNICODE  → UTF8 VARCHAR / TEXT
```

### 2.2 What was cut, and why

```
Cut                       │ BigWorld's original design │ KBEngine's replacement
──────────────────────────┼──────────────────────────┼──────────────────────
CompositeMapping          │ FIXED_DICT → multi-column expansion │ All BLOB (pickle serialization)
ClassMapping              │ Nested FIXED_DICT → recursive │ Same as above, BLOB
SequenceMapping           │ ARRAY → subtable           │ Same as above, BLOB
EnumMapping               │ Enum name ↔ INT mapping    │ Use INT directly, no mapping layer
MailboxMapping            │ Entity reference persistence │ Persisting EntityCall not supported
XmlDataSource             │ XML file storage           │ Not supported
```

### 2.3 The consequences of cutting

```python
# What KBEngine's FIXED_DICT looks like in the database:
# Player stats = FIXED_DICT { kill_count=100, death_count=20 }

# What is actually stored in the database:
# stats = b'\x80\x04\x95...\x94\x8c\x0bkill_count...'  (pickle-serialized binary)

# You want to do "find players with kill_count > 50"?
# Impossible. You must load all players into memory, deserialize, then filter.

# You want to build an index on kill_count?
# Impossible. It is a BLOB column.

# Ops wants to look at some player's stats?
# SELECT stats FROM tbl_Avatar WHERE id=42;
# Returns garbage.
```

KBEngine's reason for choosing BLOB is understandable:
- Simple to implement (`pickle.dumps` / `pickle.loads` does it)
- No need to handle Schema migration (adding/removing/changing expanded columns is complex DDL)
- Python's pickle natively supports arbitrary Python objects

But the cost is **losing all of the database's query capabilities**. Leaderboards, statistics, search — everything must bypass the database and be done in memory, or raw SQL must be written to bypass the engine.

### 2.4 The few expansions KBEngine kept

```
VECTOR3 → 3 × FLOAT columns     ← the only expansion mapping kept

Reason: position queries are among the most frequent queries on a game server.
  - "Who is near this coordinate" → requires a range query
  - If position is stored as a BLOB → range queries become impossible
  - So KBEngine kept this expansion
```

Keeping even this shows that BigWorld's expansion design is correct — KBEngine merely compromised on implementation complexity, not on wanting the capability.

---

## 3. theseed's design

### 3.1 Design principles

```
1. Inherit BigWorld's property-level mapping idea (not KBEngine's table-level)
2. Keep BigWorld's expansion capabilities (CompositeMapping / SequenceMapping)
3. Add JSON as a "queryable whole-store" scheme (this option did not exist in BigWorld's era)
4. Let the user choose whole-store or expansion, no forcing
5. Provide sensible defaults, zero configuration for simple scenarios
```

### 3.2 Mapping correspondence

```
BigWorld mapping          │ theseed equivalent      │ Notes
──────────────────────────┼────────────────────────┼──────────────────────
IntMapping                │ Auto-inferred           │ Scalar types need no explicit declaration; the column type is chosen automatically from DataType
FloatMapping              │ Auto-inferred           │ Same as above
StringMapping             │ VARCHAR / TEXT          │ size <= 255 → VARCHAR, otherwise TEXT
BoolMapping               │ Auto → TINYINT          │ Same as above
BlobMapping               │ BLOB                    │ Kept
VectorMapping             │ Auto → 3×FLOAT columns  │ Inherits BigWorld's expansion, keeps position query capability
EnumMapping               │ INT column              │ Simplified, no string mapping table
CompositeMapping          │ EXPAND                  │ Keeps BigWorld's expansion, FIXED_DICT → multiple columns
ClassMapping              │ EXPAND (recursive)      │ Kept, nested FIXED_DICT recursively expands into dot-separated prefix columns
SequenceMapping           │ SUBTABLE                │ Keeps BigWorld's subtable mapping, ARRAY → associated subtable
MailboxMapping            │ ENTITY_ID column        │ Simplified, stores only EntityID
UserTypeMapping           │ BLOB fallback           │ Kept
XmlDataSource             │ Not supported           │ Replaced by MongoDB document mode
(none)                    │ JSON                    │ New: queryable whole-store (neither BigWorld nor KBEngine has it)
(none)                    │ COMMA_SEP               │ New: lightweight scheme for simple enums
(none)                    │ BITMAP                  │ New: tag/permission bitmap
```

### 3.3 Whole-store vs expansion — user's choice

BigWorld had only one choice (expansion), and KBEngine also had only one choice (BLOB whole-store). theseed supports both.

**Scenario: the player stats property**

```xml
<!-- Option A: whole-store as JSON (simple, recommended default) -->
<property name="stats" type="FIXED_DICT" flags="BASE" persistent="true">
    <Storage columnType="JSON">
        <jsonIndex path="$.kill_count" name="idx_kills"/>
    </Storage>
</property>
<!-- Result: stats = {"kill_count":100, "death_count":20, "play_time":3600} -->
<!-- Queryable: SELECT * WHERE JSON_EXTRACT(stats, '$.kill_count') > 50 -->
<!-- Simple: one column does it, adding/removing fields needs no DDL -->
```

```xml
<!-- Option B: expand into multiple columns (BigWorld CompositeMapping style) -->
<property name="stats" type="FIXED_DICT" flags="BASE" persistent="true">
    <Storage columnType="EXPAND">
        <column name="kill_count" type="UINT32" index="btree"/>
        <column name="death_count" type="UINT32"/>
        <column name="play_time" type="UINT64"/>
    </Storage>
</property>
<!-- Result: stats_kill_count=100, stats_death_count=20, stats_play_time=3600 -->
<!-- Queryable: SELECT * WHERE stats_kill_count > 50 (native SQL, faster) -->
<!-- Index-friendly: each column indexed independently, usable by the query optimizer -->
```

**Option A suits**: configuration-style data whose fields change often (equipment attributes, achievement lists, event data)
**Option B suits**: core fields that need high-frequency queries/sorting (combat power, level, kill count)

**Scenario: the player inventory**

```xml
<!-- Option A: whole-store as JSON -->
<property name="inventory" type="ARRAY" flags="BASE" persistent="true">
    <Storage columnType="JSON"/>
</property>
<!-- The inventory is not large (<200 slots), whole-store JSON is enough, and JSON_EXTRACT can query it -->

<!-- Option B: expand into a subtable (BigWorld SequenceMapping style) -->
<property name="inventory" type="ARRAY" flags="BASE" persistent="true">
    <Storage columnType="SUBTABLE" table="player_inventory" foreignKey="player_dbid">
        <column name="item_id" type="UINT32" index="btree"/>
        <column name="count" type="UINT32"/>
        <column name="expire_at" type="UINT64"/>
    </Storage>
</property>
<!-- "Find all players holding item_id=500" → done with a single SQL statement -->
<!-- Suited to cross-player queries, leaderboards, statistics -->
```

### 3.4 Default strategy (when Storage is not specified)

```
Type          │ Relational default │ Reason
──────────────┼────────────────────┼──────────────────────
Scalar type    │ Auto-inferred column type │ INT→INT, FLOAT→FLOAT, STRING→VARCHAR(N)
VECTOR3        │ 3×FLOAT columns    │ Inherits BigWorld's expansion, position must be queryable
FIXED_DICT     │ JSON               │ Modern databases can query JSON, no expansion needed
ARRAY          │ JSON               │ Same as above
BLOB           │ BLOB               │ Binary has no other choice
```

If the user's FIXED_DICT/ARRAY needs expansion, they simply write `columnType="EXPAND"` or `columnType="SUBTABLE"` explicitly. JSON is the default because MySQL/PG JSON querying is now mature enough for most scenarios.

### 3.5 MongoDB backend

```
MongoDB is a document database, and an Entity is itself a document.
The "whole-store vs expansion" difference of the relational case does not exist in MongoDB:

  Scalar     → field
  FIXED_DICT → embedded subdocument
  ARRAY      → embedded array
  VECTOR3    → [x, y, z] array
  BLOB       → BinData
  Index      → createIndex on field path

BigWorld only supported MySQL; MongoDB is a backend theseed added itself.
MongoDB's advantage: no mapping strategy needs to be designed, the document structure is natural.
MongoDB's disadvantage: weak cross-Entity join queries, late transaction support (only from 4.0).
```

---

## 4. XSD Schema definition

### 4.1 entity.xsd

```xml
<!-- schemas/entity.xsd -->
<xsd:schema xmlns:xsd="http://www.w3.org/2001/XMLSchema"
            targetNamespace="https://theseed.dev/schema/entity"
            xmlns:t="https://theseed.dev/schema/entity"
            elementFormDefault="qualified">

    <!-- Base type enumeration -->
    <xsd:simpleType name="DataType">
        <xsd:restriction base="xsd:string">
            <xsd:enumeration value="UINT8"/>
            <xsd:enumeration value="UINT16"/>
            <xsd:enumeration value="UINT32"/>
            <xsd:enumeration value="UINT64"/>
            <xsd:enumeration value="INT8"/>
            <xsd:enumeration value="INT16"/>
            <xsd:enumeration value="INT32"/>
            <xsd:enumeration value="INT64"/>
            <xsd:enumeration value="FLOAT32"/>
            <xsd:enumeration value="FLOAT64"/>
            <xsd:enumeration value="STRING"/>
            <xsd:enumeration value="BLOB"/>
            <xsd:enumeration value="BOOL"/>
            <xsd:enumeration value="VECTOR2"/>
            <xsd:enumeration value="VECTOR3"/>
            <xsd:enumeration value="VECTOR4"/>
            <xsd:enumeration value="ENTITY_ID"/>
            <xsd:enumeration value="FIXED_DICT"/>
            <xsd:enumeration value="ARRAY"/>
            <xsd:enumeration value="PYTHON"/>
        </xsd:restriction>
    </xsd:simpleType>

    <!-- Storage column type -->
    <xsd:simpleType name="ColumnType">
        <xsd:restriction base="xsd:string">
            <xsd:enumeration value="VARCHAR"/>
            <xsd:enumeration value="TEXT"/>
            <xsd:enumeration value="BLOB"/>
            <xsd:enumeration value="JSON"/>
            <xsd:enumeration value="EXPAND"/>        <!-- BigWorld CompositeMapping/ClassMapping -->
            <xsd:enumeration value="SUBTABLE"/>       <!-- BigWorld SequenceMapping -->
            <xsd:enumeration value="COMMA_SEP"/>
            <xsd:enumeration value="BITMAP"/>
        </xsd:restriction>
    </xsd:simpleType>

    <!-- Type definitions such as Flags, IndexType, JsonIndex, StorageConfig, PropertyDef, MethodDef, NamedQuery -->
    <!-- See the original document for the complete Schema -->

    <xsd:element name="Entity">
        <xsd:complexType>
            <xsd:sequence>
                <xsd:element name="Properties" minOccurs="0">...</xsd:element>
                <xsd:element name="ClientMethods" minOccurs="0">...</xsd:element>
                <xsd:element name="CellMethods" minOccurs="0">...</xsd:element>
                <xsd:element name="BaseMethods" minOccurs="0">...</xsd:element>
                <xsd:element name="Queries" minOccurs="0">...</xsd:element>
            </xsd:sequence>
            <xsd:attribute name="name" type="xsd:string" use="required"/>
            <xsd:attribute name="sides" type="xsd:string" default="base,cell"/>
        </xsd:complexType>
    </xsd:element>

</xsd:schema>
```

---

## 5. XML definition examples

### 5.1 XInclude file splitting

```xml
<!-- entities/Avatar/Avatar.def — main file -->
<Entity name="Avatar" sides="base,cell"
        xmlns="https://theseed.dev/schema/entity"
        xmlns:xi="http://www.w3.org/2001/XInclude">

    <Properties>
        <xi:include href="properties.xml"/>
        <xi:include href="db_mapping.xml"/>
        <xi:include href="client_sync.xml"/>
    </Properties>

    <CellMethods>
        <xi:include href="cell_methods.xml"/>
    </CellMethods>

    <BaseMethods>
        <xi:include href="base_methods.xml"/>
    </BaseMethods>

    <Queries>
        <xi:include href="queries.xml"/>
    </Queries>
</Entity>
```

### 5.2 Database mapping file

```xml
<!-- entities/Avatar/db_mapping.xml -->
<xi:include xmlns:xi="http://www.w3.org/2001/XInclude">

    <property name="playerName">
        <Storage columnType="VARCHAR" length="64" index="unique"/>
    </property>

    <property name="level">
        <Storage index="btree"/>
    </property>

    <!-- JSON whole-store -->
    <property name="equipment">
        <Storage columnType="JSON">
            <jsonIndex path="$.weapon" name="idx_weapon"/>
        </Storage>
    </property>

    <!-- EXPAND expansion (BigWorld CompositeMapping) -->
    <property name="stats">
        <Storage columnType="EXPAND">
            <column name="kill_count" type="UINT32" index="btree"/>
            <column name="death_count" type="UINT32"/>
            <column name="play_time" type="UINT64"/>
        </Storage>
    </property>

    <!-- SUBTABLE subtable (BigWorld SequenceMapping) -->
    <property name="inventory">
        <Storage columnType="SUBTABLE" table="player_inventory" foreignKey="player_dbid">
            <column name="item_id" type="UINT32" index="btree"/>
            <column name="count" type="UINT32"/>
        </Storage>
    </property>

</xi:include>
```

### 5.3 Shared property fragment

```xml
<!-- entities/shared/combat_props.xml — combat properties (reused by multiple entities) -->
<xi:include xmlns:xi="http://www.w3.org/2001/XInclude">

    <property name="hp" type="FLOAT32" flags="OWN_CLIENT" persistent="true">
        <Default>100.0</Default>
    </property>

    <property name="mp" type="FLOAT32" flags="OWN_CLIENT" persistent="true">
        <Default>50.0</Default>
    </property>

    <property name="attack" type="UINT32" flags="BASE" persistent="true">
        <Default>10</Default>
    </property>

    <property name="defense" type="UINT32" flags="BASE" persistent="true">
        <Default>5</Default>
    </property>

</xi:include>
```

```xml
<!-- entities/Monster/Monster.def — reuses the combat properties -->
<Entity name="Monster" sides="cell"
        xmlns="https://theseed.dev/schema/entity"
        xmlns:xi="http://www.w3.org/2001/XInclude">

    <Properties>
        <xi:include href="../shared/combat_props.xml"/>
        <xi:include href="properties.xml"/>
        <xi:include href="db_mapping.xml"/>
    </Properties>

    <CellMethods>
        <xi:include href="cell_methods.xml"/>
    </CellMethods>
</Entity>
```

### 5.4 XInclude merge rules

```
A property with the same name = merge the properties, not override:

  properties.xml: playerName { type=STRING, size=64, flags=BASE_AND_CLIENT }
  db_mapping.xml: playerName { Storage { columnType=VARCHAR, index=unique } }

  → merged result: playerName { type=STRING, size=64, flags=BASE_AND_CLIENT,
                                Storage { columnType=VARCHAR, length=64, index=unique } }

Conflict detection:
  - Different type → compile error
  - Different flags → compile error
  - Storage incompatible with the type → warning
```

---

## 6. Comparison with KBEngine/BigWorld

| Capability | KBEngine | BigWorld | theseed |
|------|---------|---------|---------|
| Definition format | XML (.def) without XSD | XML (.def) without XSD | XML + XSD + XInclude |
| Mapping granularity | Table-level (5 kinds) | Property-level (15+ kinds) | Property-level (dual strategy of whole-store + expansion) |
| ARRAY storage | BLOB | Subtable (SequenceMapping) | JSON / SUBTABLE |
| FIXED_DICT storage | BLOB | Multiple columns (CompositeMapping) | JSON / EXPAND |
| Nested FIXED_DICT | BLOB | Recursive expansion (ClassMapping) | JSON / EXPAND (recursive) |
| VECTOR3 | 3×FLOAT columns | 3×FLOAT columns | 3×FLOAT columns (identical) |
| Whole-store vs expansion | Only BLOB | Only expansion | Both supported, user's choice |
| JSON support | None | None | Native |
| JSON index | None | None | json_path index |
| Custom query | None | None | Parameterized SQL + named queries |
| Server merge tool | None | consolidate_dbs | theseed-merge |
| Structural validation | None | None | XSD real-time validation |
| Semantic validation | None | None | defcheck |
| File splitting | None | None | XInclude |
| Storage backend | MySQL + Redis | MySQL + XML | MySQL/PG/MongoDB/Redis/Memory |
| Schema migration | syncToDB (limited) | Manual | Automatic diff + migration plan |
