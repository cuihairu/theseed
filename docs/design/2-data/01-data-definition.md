# Data Definition — 数据定义与 XML/XSD

> theseed 的数据定义系统：一份 XML 定义 → 内存/网络/存储三层映射。
> 解决 KBEngine 数据层的核心痛点。
>
> 格式：XML + XSD Schema 校验 + XInclude 文件拆分。
> 来源：KBEngine 表级映射（简单粗糙），BigWorld 属性级映射（15+ 种）。

---

## 1. KBEngine 数据层痛点

```
痛点 1：ARRAY 和 FIXED_DICT 只能存 BLOB
  → 无法在 SQL 中查询/过滤/索引
  → 必须加载到内存才能操作

痛点 2：不支持 JSON 类型
  MySQL 5.7+ / PostgreSQL / MongoDB 都原生支持 JSON
  KBEngine 完全没有利用

痛点 3：没有自定义查询
  所有操作都是预定义 CRUD
  复杂查询只能全加载到内存或绕过引擎直接写 SQL

痛点 4：合服时数据冲突
  EntityID 冲突、唯一索引冲突、关联数据断裂、自增 ID 重叠
  KBEngine 没有任何合服工具

痛点 5：开发时无校验
  .def 类型定义没有编译期/加载期警告
  运行时才发现存不进去
```

---

## 2. 设计目标

```
1. 声明式数据定义（一份 XML → 内存/网络/存储三层映射）
2. XSD Schema 校验（保存即校验，IDE 实时报错）
3. XInclude 文件拆分（按关注点分离，主文件简洁）
4. 属性级存储策略（BigWorld 的 15+ 种映射）
5. 原生 JSON 支持
6. 自定义查询 DSL
7. 合服工具链
8. 开发时校验（XSD + defcheck 双层）
```

---

## 3. XSD Schema 定义

### 3.1 entity.xsd

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

    <!-- Flags, IndexType, ColumnType, JsonIndex, StorageConfig, PropertyDef, MethodDef, NamedQuery 等类型定义 -->
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

## 4. XML 定义中的存储策略

### 4.1 属性级存储策略（来自 BigWorld PropertyMapping）

```xml
<!-- entities/Avatar/Avatar.def — 主文件（XInclude 组合入口） -->
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

```xml
<!-- entities/Avatar/db_mapping.xml — 数据库映射 -->
<xi:include xmlns:xi="http://www.w3.org/2001/XInclude">

    <property name="playerName">
        <Storage columnType="VARCHAR" length="64" index="unique"/>
    </property>

    <property name="level">
        <Storage index="btree"/>
    </property>

    <!-- JSON 列，不是 BLOB -->
    <property name="equipment">
        <Storage columnType="JSON">
            <jsonIndex path="$.weapon" name="idx_weapon"/>
        </Storage>
    </property>

    <!-- 关联子表 -->
    <property name="inventory">
        <Storage columnType="SUBTABLE" table="player_inventory" foreignKey="player_dbid">
            <jsonIndex path="item_id" name="idx_item"/>
        </Storage>
    </property>

</xi:include>
```

### 4.2 存储策略映射表

```
column_type    │ MySQL         │ PostgreSQL   │ MongoDB    │ 说明
───────────────┼───────────────┼──────────────┼────────────┼────────────
VARCHAR        │ VARCHAR(N)    │ VARCHAR(N)   │ String     │ 定长字符串
TEXT           │ TEXT          │ TEXT         │ String     │ 不限长文本
BLOB           │ BLOB          │ BYTEA        │ BinData    │ 二进制
JSON           │ JSON          │ JSONB        │ Object     │ 结构化 JSON
DOCUMENT       │ JSON          │ JSONB        │ 嵌套文档    │ MongoDB 原生
SUBTABLE       │ 关联表         │ 关联表        │ 嵌套数组    │ ARRAY 的关系映射
COMMA_SEP      │ VARCHAR       │ VARCHAR      │ Array      │ 逗号分隔
BITMAP         │ BLOB          │ BYTEA        │ BinData    │ 位图
```

### 4.3 XInclude 合并规则

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

### 4.4 与 KBEngine/BigWorld 的对比

| 能力 | KBEngine | BigWorld | theseed |
|------|---------|---------|---------|
| 定义格式 | XML (.def) 无 XSD | XML (.def) 无 XSD | XML + XSD + XInclude |
| 映射粒度 | 表级（5 种） | 属性级（15+ 种） | 属性级（8 种 column_type） |
| ARRAY 存储 | BLOB | 子表 | JSON / SUBTABLE |
| FIXED_DICT 存储 | BLOB | 多列映射 | JSON / 多列 |
| JSON 支持 | 无 | 无 | 原生 |
| JSON 索引 | 无 | 无 | json_path 索引 |
| 自定义查询 | 无 | 无 | 参数化 SQL + 命名查询 |
| 合服工具 | 无 | consolidate_dbs | theseed-merge |
| 结构校验 | 无 | 无 | XSD 实时校验 |
| 语义校验 | 无 | 无 | defcheck |
| 文件拆分 | 无 | 无 | XInclude |
| 存储后端 | MySQL + Redis | MySQL + XML | MySQL/PG/MongoDB/Redis/Memory |
| Schema 迁移 | syncToDB（有限） | 手动 | 自动 diff + 迁移计划 |
