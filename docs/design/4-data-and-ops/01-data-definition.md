# Data Definition — 数据定义与 XML/XSD

> theseed 的数据定义系统：一份 XML 定义 → 内存/网络/存储三层映射。
>
> 格式：XML + XSD Schema 校验 + XInclude 文件拆分。
> 来源源头：BigWorld 属性级映射（15+ 种 PropertyMapping）。
> 参考实现：KBEngine 将映射简化为 5 种 TABLE_ITEM_TYPE。
> theseed 在保留属性级表达力的前提下做现代化收敛。

---

## 0.5 引擎实现对照与取舍

### BigWorld 是怎么实现的

```
BigWorld 的数据定义非常细：
  - 每个属性可以独立映射
  - 数据定义同时影响内存、网络和数据库
  - PropertyMapping 种类丰富
```

### KBEngine 是怎么实现的

```
KBEngine 保留了数据定义主线，
但把映射类型收缩得更简单，
更强调“定义能跑起来”。
```

### 优缺点

```
BigWorld 的优点：
  - 表达力强
  - 适合复杂数据模型和查询需求

KBEngine 的优点：
  - 规则更少
  - 上手和实现成本更低

共同缺点：
  - 定义层一旦失控，会同时污染内存、网络和存储三层
```

### theseed 的取舍

```
theseed 继承 BigWorld 的“属性级映射”思想，
但避免无节制扩张映射种类，
优先保留当前确实需要的表达力。
```

---

## 1. BigWorld PropertyMapping（源头）

BigWorld 是这些设计的源头。它的核心思想是：**每个属性可以有独立的数据库映射策略**，而不是整个 Entity 一刀切。

BigWorld 定义了 `PropertyMapping` 基类，所有映射类型继承自它。源码在 `bigworld/src/server/db/db_mgr.hpp`。

### 1.1 标量映射—— 一个属性对应一列

```
IntMapping       INT8/16/32/64 → TINYINT / SMALLINT / INT / BIGINT
FloatMapping     FLOAT32/64    → FLOAT / DOUBLE
StringMapping    STRING        → VARCHAR(N) 或 TEXT
BoolMapping      BOOL          → TINYINT(1)
BlobMapping      BLOB          → BLOB
```

这是最简单的，属性是什么类型，数据库就存什么类型的列，没有转换。

### 1.2 展开映射—— 一个属性展开为多列或多行

这是 BigWorld 最有价值的设计：

**VectorMapping** — Vector3 展开为 3 列：

```
Entity 属性：  position = Vector3(100.0, 0.0, 50.0)
                    ↓ VectorMapping
MySQL 表：     positionX = 100.0, positionY = 0.0, positionZ = 50.0
```

为什么展开？因为 3 个独立 FLOAT 列可以分别建索引、做范围查询（`WHERE positionX > 90 AND positionX < 110`）。如果存成一个 BLOB，这些都做不了。

**CompositeMapping** — FIXED_DICT 展开为多列：

```
Entity 属性：  stats = FIXED_DICT { kill_count=100, death_count=20, play_time=3600 }
                    ↓ CompositeMapping
MySQL 表：     stats_kill_count = 100
               stats_death_count = 20
               stats_play_time = 3600
```

字段名前缀是属性名，保证不冲突。每个字段是独立列，可以单独建索引、做 `WHERE stats_kill_count > 50`。

**ClassMapping** — 嵌套 FIXED_DICT 递归展开：

```
Entity 属性：  equipment = FIXED_DICT {
                   weapon = FIXED_DICT { id=500, level=3 },
                   armor  = FIXED_DICT { id=300, level=2 }
               }
                    ↓ ClassMapping（递归）
MySQL 表：     equipment_weapon_id = 500
               equipment_weapon_level = 3
               equipment_armor_id = 300
               equipment_armor_level = 2
```

用点分路径做列名前缀，支持任意深度嵌套。本质上是 CompositeMapping 的递归版本。

**SequenceMapping** — ARRAY 展开为子表：

```
Entity 属性：  inventory = ARRAY [ {item_id=1, count=5}, {item_id=2, count=3} ]
                    ↓ SequenceMapping
MySQL 子表：   player_inventory (parent_id FK → Entity 表)
               row 1: parent_id=42, idx=0, item_id=1, count=5
               row 2: parent_id=42, idx=1, item_id=2, count=3
```

每个元素一行，关联到父 Entity。子表每列可以独立查询/索引——"找出所有持有 item_id=1 的玩家"一条 SQL 就搞定。

### 1.3 特殊映射

```
EnumMapping       枚举 → INT 列 + 字符串映射表
                  内存中是枚举名（"WARRIOR"），数据库存 INT (1)
                  读取时做反向映射

MailboxMapping    Entity 引用 → 存 EntityID + 进程地址
                  用于持久化 EntityCall/Mailbox

UserTypeMapping   用户自定义类型 → BLOB 兜底
                  引擎不认识的类型，序列化为二进制存 BLOB

XmlDataSource     不是映射到 SQL，而是映射到 XML 文件
                  BigWorld 的二级存储方案（适合低频访问数据）
                  不需要数据库表，直接读写 XML
```

### 1.4 BigWorld 映射总表

```
映射类型            │ 做什么                          │ DB 结果
────────────────────┼─────────────────────────────────┼────────────────────
IntMapping          │ 整数 → 整数列                    │ 1 列 INT
FloatMapping        │ 浮点 → 浮点列                    │ 1 列 FLOAT/DOUBLE
StringMapping       │ 字符串 → VARCHAR/TEXT             │ 1 列
BoolMapping         │ 布尔 → TINYINT                   │ 1 列
BlobMapping         │ 二进制 → BLOB                    │ 1 列 BLOB
VectorMapping       │ Vector3 → 3 个 FLOAT 列          │ 3 列
EnumMapping         │ 枚举名 → INT + 映射表             │ 1 列 INT
CompositeMapping    │ FIXED_DICT → 多列（一层展开）     │ N 列
ClassMapping        │ 嵌套 FIXED_DICT → 多列（递归展开）│ N 列（点分前缀）
SequenceMapping     │ ARRAY → 子表（每元素一行）        │ 子表 + FK
MailboxMapping      │ Entity 引用 → ID + 地址          │ 2 列
UserTypeMapping     │ 自定义类型 → BLOB 兜底            │ 1 列 BLOB
XmlDataSource       │ 整个 Entity → XML 文件            │ 不走 SQL
```

**BigWorld 的局限**：只支持 MySQL（+ XML 作为二级存储）。不支持 JSON（2005 年 MySQL 没有 JSON 类型）。不支持 PostgreSQL / MongoDB。

---

## 2. KBEngine 的取舍

KBEngine 参考了 BigWorld 的 Entity 模型，但在数据库映射上做了大幅简化。

### 2.1 KBEngine 只保留 5 种 TABLE_ITEM_TYPE

```
TABLE_ITEM_TYPE_STRING   → VARCHAR / TEXT
TABLE_ITEM_TYPE_DIGIT    → INT / BIGINT（根据位数自动选择）
TABLE_ITEM_TYPE_VECTOR3  → 3 × FLOAT（保留了这个展开）
TABLE_ITEM_TYPE_BLOB     → BLOB
TABLE_ITEM_TYPE_UNICODE  → UTF8 VARCHAR / TEXT
```

### 2.2 砍掉了什么，为什么

```
砍掉的                    │ BigWorld 原设计          │ KBEngine 的替代
──────────────────────────┼──────────────────────────┼──────────────────────
CompositeMapping          │ FIXED_DICT → 多列展开    │ 全部 BLOB（pickle 序列化）
ClassMapping              │ 嵌套 FIXED_DICT → 递归   │ 同上，BLOB
SequenceMapping           │ ARRAY → 子表             │ 同上，BLOB
EnumMapping               │ 枚举名 ↔ INT 映射        │ 直接用 INT，没有映射层
MailboxMapping            │ Entity 引用持久化        │ 不支持持久化 EntityCall
XmlDataSource             │ XML 文件存储             │ 不支持
```

### 2.3 砍掉的后果

```python
# KBEngine 的 FIXED_DICT 在数据库里是什么样：
# 玩家 stats = FIXED_DICT { kill_count=100, death_count=20 }

# 数据库中实际存储：
# stats = b'\x80\x04\x95...\x94\x8c\x0bkill_count...'  (pickle 序列化二进制)

# 你想做 "找出 kill_count > 50 的玩家"？
# 做不到。必须把所有玩家加载到内存，反序列化，再过滤。

# 你想在 kill_count 上建索引？
# 做不到。它是一个 BLOB 列。

# 运维想看某个玩家的 stats？
# SELECT stats FROM tbl_Avatar WHERE id=42;
# 返回乱码。
```

KBEngine 选择 BLOB 的原因可以理解：
- 实现简单（`pickle.dumps` / `pickle.loads` 搞定）
- 不需要处理 Schema 迁移（展开列的增删改是复杂的 DDL）
- Python 的 pickle 天然支持任意 Python 对象

但代价是**丧失了数据库的所有查询能力**。排行榜、统计、搜索——全部要绕过数据库在内存里做，或者直接写裸 SQL 绕过引擎。

### 2.4 KBEngine 保留的少数展开

```
VECTOR3 → 3 × FLOAT 列     ← 唯一保留的展开映射

原因：位置查询是游戏服务器最频繁的查询之一。
  - "谁在这个坐标附近" → 需要范围查询
  - 如果 position 存成 BLOB → 不可能做范围查询
  - 所以 KBEngine 保留了这个展开
```

连这个都保留，说明 BigWorld 的展开设计是对的——KBEngine 只是在实现复杂度上做了妥协，不是不想要这个能力。

---

## 3. theseed 的设计

### 3.1 设计原则

```
1. 继承 BigWorld 属性级映射思想（不是 KBEngine 的表级）
2. 保留 BigWorld 的展开能力（CompositeMapping / SequenceMapping）
3. 新增 JSON 作为"可查询的整存"方案（BigWorld 时代没有这个选项）
4. 用户自选整存还是展开，不强制
5. 提供合理默认值，简单场景零配置
```

### 3.2 映射对应关系

```
BigWorld 映射             │ theseed 对应           │ 说明
──────────────────────────┼────────────────────────┼──────────────────────
IntMapping                │ 自动推断               │ 标量类型不需要显式声明，根据 DataType 自动选列类型
FloatMapping              │ 自动推断               │ 同上
StringMapping             │ VARCHAR / TEXT         │ size <= 255 → VARCHAR，否则 TEXT
BoolMapping               │ 自动 → TINYINT         │ 同上
BlobMapping               │ BLOB                   │ 保留
VectorMapping             │ 自动 → 3×FLOAT 列     │ 继承 BigWorld 展开，保留位置查询能力
EnumMapping               │ INT 列                 │ 简化，不做字符串映射表
CompositeMapping          │ EXPAND                 │ 保留 BigWorld 展开，FIXED_DICT → 多列
ClassMapping              │ EXPAND（递归）          │ 保留，嵌套 FIXED_DICT 递归展开为点分前缀列
SequenceMapping           │ SUBTABLE               │ 保留 BigWorld 子表映射，ARRAY → 关联子表
MailboxMapping            │ ENTITY_ID 列           │ 简化，只存 EntityID
UserTypeMapping           │ BLOB 兜底              │ 保留
XmlDataSource             │ 不支持                 │ 用 MongoDB 文档模式替代
（无）                    │ JSON                   │ 新增：可查询的整存（BigWorld/KBEngine 都没有）
（无）                    │ COMMA_SEP              │ 新增：简单枚举的轻量方案
（无）                    │ BITMAP                 │ 新增：标签/权限位图
```

### 3.3 整存 vs 展开——用户自选

BigWorld 只有一种选择（展开），KBEngine 也只有一种选择（BLOB 整存）。theseed 两种都支持。

**场景：玩家 stats 属性**

```xml
<!-- 方案 A：整存为 JSON（简单，推荐默认） -->
<property name="stats" type="FIXED_DICT" flags="BASE" persistent="true">
    <Storage columnType="JSON">
        <jsonIndex path="$.kill_count" name="idx_kills"/>
    </Storage>
</property>
<!-- 结果：stats = {"kill_count":100, "death_count":20, "play_time":3600} -->
<!-- 可查询：SELECT * WHERE JSON_EXTRACT(stats, '$.kill_count') > 50 -->
<!-- 简单：一列搞定，增删字段不需要 DDL -->
```

```xml
<!-- 方案 B：展开为多列（BigWorld CompositeMapping 风格） -->
<property name="stats" type="FIXED_DICT" flags="BASE" persistent="true">
    <Storage columnType="EXPAND">
        <column name="kill_count" type="UINT32" index="btree"/>
        <column name="death_count" type="UINT32"/>
        <column name="play_time" type="UINT64"/>
    </Storage>
</property>
<!-- 结果：stats_kill_count=100, stats_death_count=20, stats_play_time=3600 -->
<!-- 可查询：SELECT * WHERE stats_kill_count > 50（原生 SQL，更快） -->
<!-- 索引友好：每列独立索引，查询优化器能用 -->
```

**方案 A 适合**：字段经常变动的配置型数据（装备属性、成就列表、活动数据）
**方案 B 适合**：需要高频查询/排序的核心字段（战力、等级、击杀数）

**场景：玩家背包**

```xml
<!-- 方案 A：整存为 JSON -->
<property name="inventory" type="ARRAY" flags="BASE" persistent="true">
    <Storage columnType="JSON"/>
</property>
<!-- 背包不大（<200 格），整存 JSON 足够，JSON_EXTRACT 也能查 -->

<!-- 方案 B：展开为子表（BigWorld SequenceMapping 风格） -->
<property name="inventory" type="ARRAY" flags="BASE" persistent="true">
    <Storage columnType="SUBTABLE" table="player_inventory" foreignKey="player_dbid">
        <column name="item_id" type="UINT32" index="btree"/>
        <column name="count" type="UINT32"/>
        <column name="expire_at" type="UINT64"/>
    </Storage>
</property>
<!-- "找出所有持有 item_id=500 的玩家" → 一条 SQL 搞定 -->
<!-- 适合跨玩家查询、排行榜、统计 -->
```

### 3.4 默认策略（不指定 Storage 时）

```
类型          │ 关系型默认       │ 原因
──────────────┼──────────────────┼──────────────────────
标量类型       │ 自动推断列类型     │ INT→INT, FLOAT→FLOAT, STRING→VARCHAR(N)
VECTOR3       │ 3×FLOAT 列       │ 继承 BigWorld 展开，位置必须可查询
FIXED_DICT    │ JSON              │ 现代数据库 JSON 可查询，不需要展开
ARRAY         │ JSON              │ 同上
BLOB          │ BLOB              │ 二进制没有其他选择
```

如果用户的 FIXED_DICT/ARRAY 需要展开，显式写 `columnType="EXPAND"` 或 `columnType="SUBTABLE"` 即可。默认选 JSON 是因为现在 MySQL/PG 的 JSON 查询能力已经很成熟，大多数场景够用。

### 3.5 MongoDB 后端

```
MongoDB 是文档数据库，Entity 本身就是文档。
上面关系型的"整存 vs 展开"差异在 MongoDB 上不存在：

  标量     → 字段
  FIXED_DICT → 嵌入子文档
  ARRAY    → 嵌入数组
  VECTOR3  → [x, y, z] 数组
  BLOB     → BinData
  索引     → createIndex on field path

BigWorld 只支持 MySQL，MongoDB 是 theseed 自己加的后端。
MongoDB 的优势：不需要设计映射策略，天然文档结构。
MongoDB 的劣势：跨 Entity 关联查询弱，事务支持晚（4.0 才有）。
```

---

## 4. XSD Schema 定义

### 4.1 entity.xsd

```xml
<!-- schemas/entity.xsd -->
<xsd:schema xmlns:xsd="http://www.w3.org/2001/XMLSchema"
            targetNamespace="https://theseed.dev/schema/entity"
            xmlns:t="https://theseed.dev/schema/entity"
            elementFormDefault="qualified">

    <!-- 基础类型枚举 -->
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

    <!-- 存储列类型 -->
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

    <!-- Flags, IndexType, JsonIndex, StorageConfig, PropertyDef, MethodDef, NamedQuery 等类型定义 -->
    <!-- 完整 Schema 见原始文档 -->

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

## 5. XML 定义示例

### 5.1 XInclude 文件拆分

```xml
<!-- entities/Avatar/Avatar.def — 主文件 -->
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

### 5.2 数据库映射文件

```xml
<!-- entities/Avatar/db_mapping.xml -->
<xi:include xmlns:xi="http://www.w3.org/2001/XInclude">

    <property name="playerName">
        <Storage columnType="VARCHAR" length="64" index="unique"/>
    </property>

    <property name="level">
        <Storage index="btree"/>
    </property>

    <!-- JSON 整存 -->
    <property name="equipment">
        <Storage columnType="JSON">
            <jsonIndex path="$.weapon" name="idx_weapon"/>
        </Storage>
    </property>

    <!-- EXPAND 展开（BigWorld CompositeMapping） -->
    <property name="stats">
        <Storage columnType="EXPAND">
            <column name="kill_count" type="UINT32" index="btree"/>
            <column name="death_count" type="UINT32"/>
            <column name="play_time" type="UINT64"/>
        </Storage>
    </property>

    <!-- SUBTABLE 子表（BigWorld SequenceMapping） -->
    <property name="inventory">
        <Storage columnType="SUBTABLE" table="player_inventory" foreignKey="player_dbid">
            <column name="item_id" type="UINT32" index="btree"/>
            <column name="count" type="UINT32"/>
        </Storage>
    </property>

</xi:include>
```

### 5.3 共享属性片段

```xml
<!-- entities/shared/combat_props.xml — 战斗属性（多个实体复用） -->
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
<!-- entities/Monster/Monster.def — 复用战斗属性 -->
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

### 5.4 XInclude 合并规则

```
同名 property = 合并属性，不是覆盖：

  properties.xml: playerName { type=STRING, size=64, flags=BASE_AND_CLIENT }
  db_mapping.xml: playerName { Storage { columnType=VARCHAR, index=unique } }

  → 合并结果: playerName { type=STRING, size=64, flags=BASE_AND_CLIENT,
                           Storage { columnType=VARCHAR, length=64, index=unique } }

冲突检测:
  - 不同 type → 编译报错
  - 不同 flags → 编译报错
  - Storage 与类型不兼容 → 警告
```

---

## 6. 与 KBEngine/BigWorld 的对比

| 能力 | KBEngine | BigWorld | theseed |
|------|---------|---------|---------|
| 定义格式 | XML (.def) 无 XSD | XML (.def) 无 XSD | XML + XSD + XInclude |
| 映射粒度 | 表级（5 种） | 属性级（15+ 种） | 属性级（整存 + 展开双策略） |
| ARRAY 存储 | BLOB | 子表（SequenceMapping） | JSON / SUBTABLE |
| FIXED_DICT 存储 | BLOB | 多列（CompositeMapping） | JSON / EXPAND |
| 嵌套 FIXED_DICT | BLOB | 递归展开（ClassMapping） | JSON / EXPAND（递归） |
| VECTOR3 | 3×FLOAT 列 | 3×FLOAT 列 | 3×FLOAT 列（一致） |
| 整存 vs 展开 | 只有 BLOB | 只有展开 | 两种都支持，用户自选 |
| JSON 支持 | 无 | 无 | 原生 |
| JSON 索引 | 无 | 无 | json_path 索引 |
| 自定义查询 | 无 | 无 | 参数化 SQL + 命名查询 |
| 合服工具 | 无 | consolidate_dbs | theseed-merge |
| 结构校验 | 无 | 无 | XSD 实时校验 |
| 语义校验 | 无 | 无 | defcheck |
| 文件拆分 | 无 | 无 | XInclude |
| 存储后端 | MySQL + Redis | MySQL + XML | MySQL/PG/MongoDB/Redis/Memory |
| Schema 迁移 | syncToDB（有限） | 手动 | 自动 diff + 迁移计划 |
