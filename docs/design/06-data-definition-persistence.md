# Data Definition & Persistence Design — 数据定义与持久化

> 解决 KBEngine 数据层的五大痛点：
> 1. def 定义与数据库映射死板（ARRAY/FIXED_DICT 只能存 BLOB）
> 2. 不支持 JSON 类型（现代数据库已原生支持）
> 3. 没有自定义查询能力（不能写类似 MyBatis 的 SQL）
> 4. 合服时数据冲突无自动处理
> 5. 开发时无字段校验/警告（运行时才发现存不进去）
>
> 格式选择：XML + XSD Schema 校验 + XInclude 文件拆分。
> 理由：XML 有 XSD 免费结构校验 + IDE 自动补全，XInclude 原生支持文件拆分合并，
> 属性压缩后比同等内容 YAML 更短，且缩进无关不会静默出错。
> 来源对比：KBEngine 表级映射（简单但粗糙），BigWorld 属性级映射（15+ 种，更灵活）。
> theseed 取 BigWorld 的属性级映射 + 现代数据库能力。

---

## 1. 当前问题全景

```
KBEngine 数据层痛点（来自实际开发经验）：

痛点 1：ARRAY 和 FIXED_DICT 只能存 BLOB
  .def 中定义 ARRAY<UINT32> 或 FIXED_DICT，数据库列类型是 BLOB
  → 无法在 SQL 中查询/过滤/索引这些字段
  → 无法在数据库层面做统计（如"等级 > 50 的玩家有多少"）
  → 必须加载到内存才能操作

痛点 2：不支持 JSON 类型
  MySQL 5.7+ / PostgreSQL / MongoDB 都原生支持 JSON
  可以在 JSON 列上建索引、做路径查询、用 SQL 函数操作
  但 KBEngine 完全没有利用这个能力

痛点 3：没有自定义查询
  所有数据库操作都是 EntityTable 预定义的 CRUD
  没有类似 MyBatis 的"自定义 SQL + 结果映射"
  复杂查询（排行榜、统计、搜索）只能：
    A. 全加载到内存再过滤（慢）
    B. 绕过引擎直接写 SQL（破坏抽象）
    C. 用 Redis 做查询层（额外维护）

痛点 4：合服时数据冲突
  合服 = 把多个服的数据库合并
  冲突场景：
    - EntityID 冲突（两个服都有 id=10042 的玩家）
    - 唯一索引冲突（两个服都有叫 "Alice" 的玩家名）
    - 关联数据冲突（邮件、交易记录指向旧 ID）
    - 自增 ID 重叠
  KBEngine 没有任何合服工具
  BigWorld 有 consolidate_dbs / transfer_db / sync_db

痛点 5：开发时无校验
  .def 中定义 STRING 类型，没有声明 DatabaseLength
  → 运行时才发现 VARCHAR(255) 存不下
  .def 中定义 ARRAY，元素太多
  → 运行时 BLOB 超限
  没有任何编译期/加载期警告
```

---

## 2. 设计目标

```
1. 声明式数据定义（一份 XML → 内存/网络/存储三层映射）
2. XSD Schema 校验（保存即校验，IDE 实时报错）
3. XInclude 文件拆分（按关注点分离，主文件简洁）
4. 属性级存储策略（BigWorld 的 15+ 种映射，不是 KBEngine 的 5 种）
5. 原生 JSON 支持（MySQL JSON / PostgreSQL JSONB / MongoDB 文档）
6. 自定义查询 DSL（不是裸 SQL，但比纯 ORM 灵活）
7. 合服工具链（ID 重映射 + 冲突解决策略）
8. 开发时校验（XSD 结构校验 + defcheck 语义校验 + IDE 集成）
```

---

## 3. XSD Schema 定义

### 3.1 entity.xsd — 实体定义 Schema

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

    <!-- Flags 枚举 -->
    <xsd:simpleType name="PropertyFlags">
        <xsd:restriction base="xsd:string">
            <xsd:enumeration value="BASE"/>
            <xsd:enumeration value="CELL_PRIVATE"/>
            <xsd:enumeration value="CELL_PUBLIC"/>
            <xsd:enumeration value="OWN_CLIENT"/>
            <xsd:enumeration value="OTHER_CLIENTS"/>
            <xsd:enumeration value="ALL_CLIENTS"/>
            <xsd:enumeration value="BASE_AND_CLIENT"/>
            <xsd:enumeration value="CELL_PUBLIC_AND_OWN"/>
        </xsd:restriction>
    </xsd:simpleType>

    <!-- 索引类型 -->
    <xsd:simpleType name="IndexType">
        <xsd:restriction base="xsd:string">
            <xsd:enumeration value="unique"/>
            <xsd:enumeration value="btree"/>
            <xsd:enumeration value="hash"/>
        </xsd:restriction>
    </xsd:simpleType>

    <!-- 存储列类型 -->
    <xsd:simpleType name="ColumnType">
        <xsd:restriction base="xsd:string">
            <xsd:enumeration value="VARCHAR"/>
            <xsd:enumeration value="TEXT"/>
            <xsd:enumeration value="BLOB"/>
            <xsd:enumeration value="JSON"/>
            <xsd:enumeration value="DOCUMENT"/>
            <xsd:enumeration value="SUBTABLE"/>
            <xsd:enumeration value="COMMA_SEP"/>
            <xsd:enumeration value="BITMAP"/>
        </xsd:restriction>
    </xsd:simpleType>

    <!-- JSON 索引定义 -->
    <xsd:complexType name="JsonIndex">
        <xsd:attribute name="path" type="xsd:string" use="required"/>
        <xsd:attribute name="name" type="xsd:string"/>
        <xsd:attribute name="type" default="btree">
            <xsd:simpleType>
                <xsd:restriction base="xsd:string">
                    <xsd:enumeration value="btree"/>
                    <xsd:enumeration value="multivalue"/>
                    <xsd:enumeration value="hash"/>
                </xsd:restriction>
            </xsd:simpleType>
        </xsd:attribute>
    </xsd:complexType>

    <!-- 存储配置 -->
    <xsd:complexType name="StorageConfig">
        <xsd:sequence>
            <xsd:element name="jsonIndex" type="t:JsonIndex"
                         minOccurs="0" maxOccurs="unbounded"/>
        </xsd:sequence>
        <xsd:attribute name="columnType" type="t:ColumnType"/>
        <xsd:attribute name="length" type="xsd:positiveInteger"/>
        <xsd:attribute name="index" type="t:IndexType"/>
        <xsd:attribute name="table" type="xsd:string"/>
        <xsd:attribute name="foreignKey" type="xsd:string"/>
    </xsd:complexType>

    <!-- 属性定义 -->
    <xsd:complexType name="PropertyDef">
        <xsd:sequence>
            <xsd:element name="Default" type="xsd:string" minOccurs="0"/>
            <xsd:element name="Storage" type="t:StorageConfig" minOccurs="0"/>
            <xsd:element name="Fields" minOccurs="0">         <!-- FIXED_DICT 子字段 -->
                <xsd:complexType>
                    <xsd:sequence>
                        <xsd:any processContents="strict" minOccurs="0" maxOccurs="unbounded"/>
                    </xsd:sequence>
                </xsd:complexType>
            </xsd:element>
            <xsd:element name="Of" type="t:DataType" minOccurs="0"/>  <!-- ARRAY 元素类型 -->
        </xsd:sequence>
        <xsd:attribute name="name" type="xsd:string" use="required"/>
        <xsd:attribute name="type" type="t:DataType" use="required"/>
        <xsd:attribute name="flags" type="t:PropertyFlags" use="required"/>
        <xsd:attribute name="persistent" type="xsd:boolean" default="false"/>
        <xsd:attribute name="size" type="xsd:positiveInteger"/>
        <xsd:attribute name="max" type="xsd:positiveInteger"/>
        <xsd:attribute name="exposed" type="xsd:boolean" default="false"/>
        <xsd:attribute name="detailLevel" type="xsd:nonNegativeInteger"/>
        <xsd:attribute name="volatile" type="xsd:boolean" default="false"/>
        <xsd:attribute name="sendLatestOnly" type="xsd:boolean" default="false"/>
        <xsd:attribute name="interpolate" type="xsd:boolean" default="false"/>
        <xsd:attribute name="identifier" type="xsd:boolean" default="false"/>
    </xsd:complexType>

    <!-- 方法参数 -->
    <xsd:complexType name="MethodArg">
        <xsd:attribute name="name" type="xsd:string" use="required"/>
        <xsd:attribute name="type" type="t:DataType" use="required"/>
    </xsd:complexType>

    <!-- 方法定义 -->
    <xsd:complexType name="MethodDef">
        <xsd:sequence>
            <xsd:element name="Arg" type="t:MethodArg" minOccurs="0" maxOccurs="unbounded"/>
        </xsd:sequence>
        <xsd:attribute name="name" type="xsd:string" use="required"/>
        <xsd:attribute name="exposed" type="xsd:boolean" default="false"/>
        <xsd:attribute name="rateLimit" type="xsd:string"/>
    </xsd:complexType>

    <!-- 命名查询 -->
    <xsd:complexType name="NamedQuery">
        <xsd:sequence>
            <xsd:element name="Where" type="xsd:string" minOccurs="0"/>
            <xsd:element name="Sql" type="xsd:string" minOccurs="0"/>
            <xsd:element name="Order" type="xsd:string" minOccurs="0"/>
            <xsd:element name="Limit" type="xsd:positiveInteger" minOccurs="0"/>
        </xsd:sequence>
        <xsd:attribute name="name" type="xsd:string" use="required"/>
        <xsd:attribute name="cache" type="xsd:string"/>
    </xsd:complexType>

    <!-- 实体定义根元素 -->
    <xsd:element name="Entity">
        <xsd:complexType>
            <xsd:sequence>
                <xsd:element name="Properties" minOccurs="0">
                    <xsd:complexType>
                        <xsd:sequence>
                            <xsd:any processContents="lax" minOccurs="0" maxOccurs="unbounded"/>
                        </xsd:sequence>
                    </xsd:complexType>
                </xsd:element>
                <xsd:element name="ClientMethods" minOccurs="0">
                    <xsd:complexType>
                        <xsd:sequence>
                            <xsd:element name="Method" type="t:MethodDef" minOccurs="0" maxOccurs="unbounded"/>
                        </xsd:sequence>
                    </xsd:complexType>
                </xsd:element>
                <xsd:element name="CellMethods" minOccurs="0">
                    <xsd:complexType>
                        <xsd:sequence>
                            <xsd:element name="Method" type="t:MethodDef" minOccurs="0" maxOccurs="unbounded"/>
                        </xsd:sequence>
                    </xsd:complexType>
                </xsd:element>
                <xsd:element name="BaseMethods" minOccurs="0">
                    <xsd:complexType>
                        <xsd:sequence>
                            <xsd:element name="Method" type="t:MethodDef" minOccurs="0" maxOccurs="unbounded"/>
                        </xsd:sequence>
                    </xsd:complexType>
                </xsd:element>
                <xsd:element name="Queries" minOccurs="0">
                    <xsd:complexType>
                        <xsd:sequence>
                            <xsd:element name="Query" type="t:NamedQuery" minOccurs="0" maxOccurs="unbounded"/>
                        </xsd:sequence>
                    </xsd:complexType>
                </xsd:element>
            </xsd:sequence>
            <xsd:attribute name="name" type="xsd:string" use="required"/>
            <xsd:attribute name="sides" type="xsd:string" default="base,cell"/>
            <xsd:attribute name="description" type="xsd:string"/>
        </xsd:complexType>
    </xsd:element>

</xsd:schema>
```

### 3.2 defcheck-rules.xsd — 校验规则 Schema

```xml
<!-- schemas/defcheck-rules.xsd -->
<xsd:schema xmlns:xsd="http://www.w3.org/2001/XMLSchema"
            targetNamespace="https://theseed.dev/schema/defcheck"
            xmlns:dc="https://theseed.dev/schema/defcheck"
            elementFormDefault="qualified">

    <xsd:simpleType name="Severity">
        <xsd:restriction base="xsd:string">
            <xsd:enumeration value="error"/>
            <xsd:enumeration value="warning"/>
            <xsd:enumeration value="info"/>
        </xsd:restriction>
    </xsd:simpleType>

    <xsd:complexType name="Rule">
        <xsd:sequence>
            <xsd:element name="Check" type="xsd:string"/>
            <xsd:element name="Message" type="xsd:string"/>
        </xsd:sequence>
        <xsd:attribute name="name" type="xsd:string" use="required"/>
        <xsd:attribute name="severity" type="dc:Severity" use="required"/>
    </xsd:complexType>

    <xsd:element name="DefcheckRules">
        <xsd:complexType>
            <xsd:sequence>
                <xsd:element name="Rule" type="dc:Rule" minOccurs="0" maxOccurs="unbounded"/>
            </xsd:sequence>
        </xsd:complexType>
    </xsd:element>

</xsd:schema>
```

---

## 4. XML 定义中的存储策略

### 4.1 属性级存储策略（来自 BigWorld PropertyMapping）

```xml
<!-- entities/Avatar/Avatar.def — 主文件（组合入口） -->
<Entity name="Avatar" sides="base,cell"
        description="玩家实体"
        xmlns="https://theseed.dev/schema/entity"
        xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
        xsi:schemaLocation="https://theseed.dev/schema/entity ../../schemas/entity.xsd"
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
<!-- entities/Avatar/properties.xml — 属性定义 -->
<xi:include xmlns:xi="http://www.w3.org/2001/XInclude">

    <!-- 基础属性：直接列映射（默认策略） -->
    <property name="playerName" type="STRING" size="64"
              flags="BASE_AND_CLIENT" persistent="true"/>

    <property name="level" type="UINT32" flags="ALL_CLIENTS" persistent="true">
        <Default>1</Default>
    </property>

    <property name="hp" type="FLOAT32" flags="OWN_CLIENT" persistent="true">
        <Default>100.0</Default>
    </property>

    <!-- JSON 存储：利用数据库原生 JSON 能力 -->
    <property name="equipment" type="FIXED_DICT" flags="BASE" persistent="true">
        <Fields>
            <field name="weapon" type="UINT32"><Default>0</Default></field>
            <field name="armor" type="UINT32"><Default>0</Default></field>
            <field name="helmet" type="UINT32"><Default>0</Default></field>
            <field name="boots" type="UINT32"><Default>0</Default></field>
            <field name="accessories" type="ARRAY" max="4">
                <Of>UINT32</Of>
            </field>
        </Fields>
    </property>

    <!-- 嵌套 JSON：任意结构 -->
    <property name="stats" type="FIXED_DICT" flags="BASE" persistent="true">
        <Fields>
            <field name="kill_count" type="UINT32"><Default>0</Default></field>
            <field name="death_count" type="UINT32"><Default>0</Default></field>
            <field name="play_time" type="UINT64"><Default>0</Default></field>
            <field name="achievements" type="ARRAY" max="100">
                <Of>STRING</Of>
            </field>
        </Fields>
    </property>

    <!-- 数组存为子表（BigWorld SequenceMapping 模式） -->
    <property name="inventory" type="ARRAY" max="200"
              flags="BASE" persistent="true">
        <Of>FIXED_DICT</Of>
        <Fields>
            <field name="item_id" type="UINT32"/>
            <field name="count" type="UINT32"/>
            <field name="expire_at" type="UINT64"><Default>0</Default></field>
        </Fields>
    </property>

    <!-- 大文本 -->
    <property name="biography" type="STRING" size="10000"
              flags="BASE" persistent="true"/>

    <!-- 纯二进制 -->
    <property name="custom_data" type="BLOB" max="65536"
              flags="BASE" persistent="true"/>

</xi:include>
```

```xml
<!-- entities/Avatar/db_mapping.xml — 数据库映射 -->
<xi:include xmlns:xi="http://www.w3.org/2001/XInclude">

    <!-- playerName: VARCHAR + 唯一索引 -->
    <property name="playerName">
        <Storage columnType="VARCHAR" length="64" index="unique"/>
    </property>

    <!-- level: 普通索引（用于范围查询） -->
    <property name="level">
        <Storage index="btree"/>
    </property>

    <!-- equipment: 存为 JSON 列，不是 BLOB -->
    <property name="equipment">
        <Storage columnType="JSON">
            <jsonIndex path="$.weapon" name="idx_weapon"/>
        </Storage>
    </property>

    <!-- stats: JSON + 多值索引（MySQL 8.0+） -->
    <property name="stats">
        <Storage columnType="JSON">
            <jsonIndex path="$.achievements[*]" name="idx_achievements" type="multivalue"/>
        </Storage>
    </property>

    <!-- inventory: 存为关联子表 -->
    <property name="inventory">
        <Storage columnType="SUBTABLE" table="player_inventory" foreignKey="player_dbid">
            <jsonIndex path="item_id" name="idx_item"/>
            <jsonIndex path="expire_at" name="idx_expire"/>
        </Storage>
    </property>

    <!-- biography: TEXT -->
    <property name="biography">
        <Storage columnType="TEXT"/>
    </property>

    <!-- custom_data: BLOB（只有真正需要二进制时才用） -->
    <property name="custom_data">
        <Storage columnType="BLOB"/>
    </property>

</xi:include>
```

```xml
<!-- entities/Avatar/client_sync.xml — 客户端同步配置 -->
<xi:include xmlns:xi="http://www.w3.org/2001/XInclude">

    <!-- hp: 高精度同步，客户端插值 -->
    <property name="hp" detailLevel="0" interpolate="true" sendLatestOnly="true"/>

    <!-- level: 精确同步 -->
    <property name="level" detailLevel="0"/>

    <!-- 没有列出的属性使用默认同步策略 -->

</xi:include>
```

```xml
<!-- entities/Avatar/cell_methods.xml — Cell 方法 -->
<xi:include xmlns:xi="http://www.w3.org/2001/XInclude">

    <Method name="attack" exposed="true" rateLimit="5/1s">
        <Arg name="targetId" type="ENTITY_ID"/>
        <Arg name="skillId" type="UINT32"/>
    </Method>

    <Method name="move" exposed="true" rateLimit="20/1s">
        <Arg name="position" type="VECTOR3"/>
        <Arg name="rotation" type="VECTOR3"/>
    </Method>

</xi:include>
```

```xml
<!-- entities/Avatar/base_methods.xml — Base 方法 -->
<xi:include xmlns:xi="http://www.w3.org/2001/XInclude">

    <Method name="useItem" exposed="true" rateLimit="10/1s">
        <Arg name="itemId" type="UINT32"/>
        <Arg name="count" type="UINT32"/>
    </Method>

    <Method name="onLevelUp">
        <Arg name="newLevel" type="UINT32"/>
    </Method>

</xi:include>
```

```xml
<!-- entities/Avatar/queries.xml — 命名查询 -->
<xi:include xmlns:xi="http://www.w3.org/2001/XInclude">

    <Query name="findOnlineByLevel" cache="30s">
        <Where>level >= :minLv AND online = true</Where>
        <Order>level DESC</Order>
        <Limit>100</Limit>
    </Query>

    <Query name="searchByName" cache="10s">
        <Where>playerName LIKE :pattern</Where>
        <Limit>50</Limit>
    </Query>

    <Query name="killRanking" cache="60s">
        <Sql>SELECT id, playerName, JSON_EXTRACT(stats, '$.kill_count') as kills
             FROM tbl_Avatar ORDER BY kills DESC LIMIT :limit</Sql>
        <Limit>100</Limit>
    </Query>

</xi:include>
```

### 4.2 共享属性片段（XInclude 复用）

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
<!-- entities/Monster/Monster.def — 怪物实体，复用战斗属性 -->
<Entity name="Monster" sides="cell"
        xmlns="https://theseed.dev/schema/entity"
        xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
        xsi:schemaLocation="https://theseed.dev/schema/entity ../../schemas/entity.xsd"
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

### 4.3 合并规则

```
XInclude 合并后，同名 property = 合并属性，不是覆盖：

  properties.xml 定义了:  playerName { type=STRING, size=64, flags=BASE_AND_CLIENT }
  db_mapping.xml 补充了:  playerName { Storage { columnType=VARCHAR, index=unique } }
  client_sync.xml 未提及: playerName

  → 合并结果: playerName { type=STRING, size=64, flags=BASE_AND_CLIENT,
                           Storage { columnType=VARCHAR, length=64, index=unique } }

冲突检测:
  - 两个文件对同一属性定义了不同的 type → 编译报错
  - 两个文件对同一属性定义了不同的 flags → 编译报错
  - Storage 补充与属性类型不兼容（如 STRING 配 columnType=BLOB）→ 警告
```

### 4.4 存储策略映射表

```
theseed 支持的 column_type：

column_type    │ MySQL         │ PostgreSQL   │ MongoDB    │ 说明
───────────────┼───────────────┼──────────────┼────────────┼────────────
VARCHAR        │ VARCHAR(N)    │ VARCHAR(N)   │ String     │ 定长字符串
TEXT           │ TEXT          │ TEXT         │ String     │ 不限长文本
BLOB           │ BLOB          │ BYTEA        │ BinData    │ 二进制
JSON           │ JSON          │ JSONB        │ Object     │ 结构化 JSON
DOCUMENT       │ JSON          │ JSONB        │ 嵌套文档    │ MongoDB 原生
SUBTABLE       │ 关联表         │ 关联表        │ 嵌套数组    │ ARRAY 的关系映射
COMMA_SEP      │ VARCHAR       │ VARCHAR      │ Array      │ 逗号分隔（简单枚举）
BITMAP         │ BLOB          │ BYTEA        │ BinData    │ 位图（标签/权限）
```

### 4.5 与 KBEngine/BigWorld 的对比

```
KBEngine 映射（5 种 TABLE_ITEM_TYPE）：
  STRING      → VARCHAR/TEXT
  DIGIT       → INT/BIGINT
  VECTOR3     → 3×FLOAT
  BLOB        → BLOB
  ARRAY/DICT  → BLOB（全部序列化成二进制）
  PYTHON      → BLOB（pickle）

BigWorld 映射（15+ 种 PropertyMapping）：
  每个属性可以有独立的映射策略
  支持子表映射（SequenceMapping）
  支持 Blob/Composite/ClassMapping
  不支持 JSON（当时还没有）

theseed 映射（8 种 column_type + JSON 原生 + XInclude 拆分）：
  继承 BigWorld 的属性级映射思想
  新增 JSON/DOCUMENT 原生支持
  新增 SUBTABLE 替代 BLOB 存数组
  新增 json_path 索引（MySQL 8.0+ / PostgreSQL）
  新增 XML + XSD 校验 + XInclude 文件拆分
```

---

## 5. JSON 原生支持

### 5.1 为什么不用 BLOB 存 ARRAY/FIXED_DICT

```
BLOB 的问题：
  1. 无法在 SQL 中查询（"找装备了武器 ID=500 的玩家" → 全表扫描+反序列化）
  2. 无法建索引
  3. 无法做数据库层面的统计/聚合
  4. 调试困难（SELECT * 看到的是乱码）
  5. 跨语言访问困难（运维脚本、数据分析平台）

JSON 的优势：
  1. 可查询：SELECT * FROM Player WHERE JSON_EXTRACT(equipment, '$.weapon') = 500
  2. 可索引：MySQL 8.0 多值索引、PostgreSQL GIN 索引
  3. 可读：SELECT equipment FROM Player → {"weapon":500,"armor":300}
  4. 可统计：SELECT COUNT(*) FROM Player WHERE JSON_EXTRACT(stats,'$.level') > 50
  5. 工具友好：任何 JSON 工具都能读取
```

### 5.2 JSON 查询接口

```python
# 脚本层 JSON 查询 API

import theseed

# 方法 1：结构化查询（推荐，引擎优化）
results = theseed.db.query("Player") \
    .filter(theseed.db.json_path("equipment.weapon") == 500) \
    .filter(theseed.db.field("level") >= 50) \
    .limit(100) \
    .execute()

# 方法 2：自定义查询（类似 MyBatis，但更安全）
results = theseed.db.execute("""
    SELECT id, name, JSON_EXTRACT(equipment, '$.weapon') as weapon_id
    FROM tbl_Player
    WHERE level > :min_level
      AND JSON_EXTRACT(stats, '$.kill_count') > :min_kills
    ORDER BY level DESC
    LIMIT :limit
""", min_level=50, min_kills=1000, limit=100)

# 方法 3：聚合查询
count = theseed.db.query("Player") \
    .filter(theseed.db.json_path("stats.guild_id") == guild_id) \
    .count()

avg_level = theseed.db.query("Player") \
    .filter(theseed.db.field("level") > 0) \
    .avg("level")
```

### 5.3 自定义查询的安全机制

```cpp
// storage/CustomQuery.h

class CustomQuery {
public:
    // 参数化查询（防 SQL 注入）
    QueryResult execute(const std::string& entity,
                        const std::string& sql,
                        const QueryParams& params);

    // 查询白名单（限制可执行的查询模板）
    void registerNamedQuery(const std::string& name,
                            const std::string& sqlTemplate);

    // 执行命名查询
    QueryResult executeNamed(const std::string& name,
                             const QueryParams& params);
};
```

---

## 6. 合服工具链

### 6.1 合服的冲突场景

```
服 A 和服 B 合并到服 C：

冲突 1：EntityID 重叠
  服 A: Player id=10042 (Alice)
  服 B: Player id=10042 (Bob)
  → 不能直接合并，ID 冲突

冲突 2：唯一索引冲突
  服 A: Player name="Alice"
  服 B: Player name="Alice"（不同的人）
  → name 有 unique 索引，不能有两个 "Alice"

冲突 3：关联数据断裂
  服 A: Mail id=5001, sender=10042, receiver=10089
  合服后 Player 10042 的 id 变成了 20042
  → Mail.sender 还指向旧的 10042（断裂）

冲突 4：自增 ID 重叠
  服 A 和服 B 的 auto_increment 都从 1 开始
  → 合并后新插入的 ID 会冲突

冲突 5：EntityCall 引用失效
  服 A 的 Player 持有服 B 玩家的 EntityCall（跨服好友？）
  → 合服后 EntityCall 中的 ID 已经变了
```

### 6.2 合服流程设计

```
theseed-merge 工具：

Phase 1: 分析
  ├─ 读取两个服的 def 文件（确保 schema 兼容）
  ├─ 扫描两个服的数据库
  ├─ 检测冲突：
  │   ├─ ID 范围重叠
  │   ├─ 唯一索引冲突
  │   └─ 关联关系图
  └─ 生成冲突报告

Phase 2: ID 重映射
  ├─ 策略 A：偏移映射
  │   服 A 的 ID 不变，服 B 的 ID + offset
  │   offset = max(服 A 的最大 ID) + 1
  │
  ├─ 策略 B：命名空间映射
  │   服 A: id → realm_a:{id}
  │   服 B: id → realm_b:{id}
  │
  └─ 策略 C：完全重分配
      所有实体重新分配 ID

Phase 3: 唯一索引冲突解决
  ├─ 策略：可配置的冲突解决规则
  │   name 冲突 → 自动加后缀（"Alice" → "Alice_B"）
  │   account 冲突 → 合并账号（需要人工确认）
  │   自定义字段 → 脚本回调处理
  │
  └─ 冲突解决规则配置

Phase 4: 关联数据修复
  ├─ 根据 ID 重映射表更新所有关联
  │   Mail.sender → 新 ID
  │   Mail.receiver → 新 ID
  │   Guild.leader → 新 ID
  │   Guild.members → 批量替换
  │   Friend.list → 批量替换
  │
  ├─ EntityCall 引用更新
  │   扫描所有 BLOB/JSON 中的 EntityID
  │   根据映射表替换
  │
  └─ SUBTABLE 外键更新

Phase 5: 数据合并
  ├─ 按依赖顺序合并（先主表后子表）
  ├─ 验证外键完整性
  └─ 重建索引

Phase 6: 验证
  ├─ 数据完整性检查
  ├─ 唯一索引验证
  ├─ 外键一致性验证
  └─ 生成合并报告
```

### 6.3 合服配置

```xml
<!-- config/merge.xml -->
<merge xmlns="https://theseed.dev/schema/merge">

    <sourceRealms>
        <realm name="realm_a">
            <database host="mysql-a.internal" port="3306" name="theseed_game"/>
        </realm>
        <realm name="realm_b">
            <database host="mysql-b.internal" port="3306" name="theseed_game"/>
        </realm>
    </sourceRealms>

    <targetRealm name="realm_merged">
        <database host="mysql-merged.internal" port="3306" name="theseed_game"/>
    </targetRealm>

    <!-- ID 重映射策略 -->
    <idRemap strategy="offset">
        <offset realm="realm_a" value="0"/>
        <offset realm="realm_b" value="auto"/>  <!-- 自动计算 -->
    </idRemap>

    <!-- 唯一索引冲突解决 -->
    <conflictResolution default="suffix">
        <field name="playerName" strategy="suffix" suffixTemplate="_{realm}"/>
        <field name="account" strategy="script" script="merge_account_conflict"/>
    </conflictResolution>

    <!-- 关联数据修复 -->
    <relationFix autoScanJson="true" autoScanBlob="true">
        <explicitRelation table="tbl_Mail" fields="sender_id,receiver_id"/>
        <explicitRelation table="tbl_Guild" fields="leader_id"/>
        <explicitRelation table="tbl_Guild_Members" fields="player_id"/>
    </relationFix>

    <!-- 验证 -->
    <validation checkUniqueIndexes="true"
                checkForeignKeys="true"
                checkEntityCount="true"
                abortOnError="true"/>

</merge>
```

### 6.4 合服工具 CLI

```bash
# 分析冲突
theseed-merge analyze --config config/merge.xml --output report.json

# 预览合并（dry run，不实际写入）
theseed-merge preview --config config/merge.xml

# 执行合并
theseed-merge execute --config config/merge.xml --output merge_result.json

# 回滚（如果合并出问题）
theseed-merge rollback --config config/merge.xml --from merge_result.json
```

---

## 7. 开发时校验（XSD + defcheck 双层）

### 7.1 第一层：XSD 结构校验

```
XSD 自动校验（编辑器保存时）：

  ✅ 元素结构：Properties 必须包含 property 子元素
  ✅ 类型枚举：type 只能是 UINT8/UINT32/STRING/FIXED_DICT 等
  ✅ 必填检查：name 和 type 是 required 属性
  ✅ 值域约束：size 必须是正整数，persistent 是布尔值
  ✅ Flags 枚举：只能是 ALL_CLIENTS | BASE | OWN_CLIENT 等
  ✅ column_type 枚举：只能是 VARCHAR | JSON | SUBTABLE 等
  ✅ 结构嵌套：Storage 只能出现在 property 内部

开发者写错格式，编辑器直接标红，不需要等到 defcheck。
```

### 7.2 第二层：defcheck 语义校验

```xml
<!-- config/defcheck.xml -->
<DefcheckRules xmlns="https://theseed.dev/schema/defcheck"
               xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
               xsi:schemaLocation="https://theseed.dev/schema/defcheck ../schemas/defcheck-rules.xsd">

    <!-- 字段大小校验 -->
    <Rule name="string_without_size" severity="error">
        <Check>STRING 类型属性必须声明 size 或 Storage.length</Check>
        <Message>{entity}.{prop}: STRING 没有 size 限制，可能导致数据库存储溢出</Message>
    </Rule>

    <Rule name="string_size_exceeds_column" severity="error">
        <Check>STRING.size > Storage.length</Check>
        <Message>{entity}.{prop}: STRING size={size} 超过列长度 {column_length}，数据会被截断</Message>
    </Rule>

    <Rule name="blob_without_max" severity="warning">
        <Check>BLOB 类型没有声明 max 大小</Check>
        <Message>{entity}.{prop}: BLOB 没有 max 限制，可能存储超大对象</Message>
    </Rule>

    <Rule name="array_too_large_for_blob" severity="warning">
        <Check>ARRAY 存储为 BLOB 且 max*element_size > 65535</Check>
        <Message>{entity}.{prop}: ARRAY 最大 {max} 个元素，估计 {estimated} 字节，可能超过 BLOB 限制</Message>
    </Rule>

    <!-- 索引校验 -->
    <Rule name="persistent_without_index_hint" severity="info">
        <Check>频繁查询的 persistent 属性没有声明 index</Check>
        <Message>{entity}.{prop}: 考虑为经常查询的属性添加索引</Message>
    </Rule>

    <Rule name="json_without_path_index" severity="warning">
        <Check>JSON 列中有查询需求但没有声明 json_path 索引</Check>
        <Message>{entity}.{prop}: JSON 列建议添加 jsonIndex 以提升查询性能</Message>
    </Rule>

    <!-- 类型兼容性 -->
    <Rule name="type_change_breaks_storage" severity="error">
        <Check>已存在的属性类型变更后与数据库列不兼容</Check>
        <Message>{entity}.{prop}: 类型从 {old} 改为 {new}，需要数据迁移</Message>
    </Rule>

    <Rule name="float_to_int_truncation" severity="warning">
        <Check>FLOAT → INT 类型变更会截断小数部分</Check>
        <Message>{entity}.{prop}: 从 FLOAT 改为 INT 会截断小数部分</Message>
    </Rule>

    <!-- 唯一约束 -->
    <Rule name="unique_field_without_default" severity="info">
        <Check>有 unique 索引的字段没有 default 值</Check>
        <Message>{entity}.{prop}: 唯一索引字段建议提供默认值或确保创建时一定赋值</Message>
    </Rule>

    <!-- JSON schema -->
    <Rule name="json_field_schema_mismatch" severity="error">
        <Check>FIXED_DICT 的 Fields 与实际 JSON 结构不匹配</Check>
        <Message>{entity}.{prop}: FIXED_DICT 定义的字段与 JSON schema 不一致</Message>
    </Rule>

</DefcheckRules>
```

### 7.3 校验流程

```
def 变更时自动执行：

1. XSD 校验（编辑器/保存时自动）
   → 结构错误立即标红

2. defcheck --rules config/defcheck.xml --defs defs/
   → 输出语义校验报告（error/warning/info）

3. XInclude 合并
   → 解析所有 xi:include 引用
   → 同名属性合并 + 冲突检测

4. 生成 IDE 提示（.pyi / .d.ts / LuaLS 注解）
   → 编辑器中实时提示字段约束

5. Schema diff（对比上一次提交的 def）
   → 生成迁移计划
   → 标记破坏性变更（删属性、改类型）

6. CI 集成
   → XSD 校验 + defcheck 作为 CI 步骤
   → error 级别阻断提交
   → warning 级别生成报告
```

### 7.4 IDE 集成示例

```python
# 自动生成的 Avatar.pyi（mypy 类型存根）
# 包含字段约束信息，IDE 可以实时提示

class Avatar(BaseEntity):
    # playerName: STRING, max 64 chars, unique index
    # 超过 64 字符会被数据库截断
    playerName: str  # max_length=64, unique=True

    # level: UINT32, default=1, btree index
    level: int  # min=0, max=4294967295

    # equipment: FIXED_DICT, stored as JSON
    # 可通过 db.query().filter(json_path("equipment.weapon") == value) 查询
    equipment: EquipmentData

    # inventory: ARRAY<FIXED_DICT>, max 200 items
    # stored in subtable "player_inventory"
    inventory: list[InventoryItem]  # max_length=200
```

---

## 8. 存储后端抽象

### 8.1 IStorageBackend 增强

```cpp
// storage/IStorageBackend.h — 增加自定义查询和 JSON 支持

class IStorageBackend {
public:
    // === 基本 CRUD ===
    virtual Future<EntityData> load(EntityId id, const EntityDef& def) = 0;
    virtual Future<void> save(EntityId id, const EntityData& data, const EntityDef& def) = 0;
    virtual Future<void> remove(EntityId id) = 0;

    // === 查询 ===
    virtual Future<std::vector<EntityId>> query(const StorageQuery& q) = 0;

    // === 自定义查询 ===
    virtual Future<QueryResult> executeRaw(const std::string& sql,
                                           const QueryParams& params) = 0;

    virtual Future<QueryResult> executeNamed(const std::string& queryName,
                                             const QueryParams& params) = 0;

    // === JSON 查询 ===
    virtual Future<std::vector<EntityId>> queryJsonPath(
        const std::string& entityType,
        const std::string& jsonPath,
        const std::string& op,
        const QueryParam& value) = 0;

    // === 聚合 ===
    virtual Future<AggregateResult> aggregate(
        const std::string& entityType,
        const std::string& field,
        AggregateOp op,
        const StorageQuery& filter) = 0;

    // === Schema 管理 ===
    virtual Future<void> createTable(const EntityDef& def) = 0;
    virtual Future<MigrationPlan> planMigration(const EntityDef& oldDef,
                                                 const EntityDef& newDef) = 0;
    virtual Future<void> executeMigration(const MigrationPlan& plan) = 0;

    // === 合服 ===
    virtual Future<MergeReport> mergeFrom(const IStorageBackend& source,
                                          const MergeConfig& config) = 0;

    // === 后端能力 ===
    virtual std::string backendName() const = 0;
    virtual std::vector<std::string> capabilities() const = 0;
    // capabilities 可能返回: ["json", "json_path_index", "subtable",
    //                          "transaction", "multi_value_index"]
};

enum class AggregateOp { COUNT, SUM, AVG, MIN, MAX };
```

### 8.2 MySQL 后端的 JSON 支持

```cpp
// storage/MySQLBackend.cpp

Future<std::vector<EntityId>>
MySQLBackend::queryJsonPath(const std::string& entityType,
                             const std::string& jsonPath,
                             const std::string& op,
                             const QueryParam& value) {
    // MySQL 5.7+: JSON_EXTRACT
    // MySQL 8.0+: -> 操作符 + 多值索引
    std::string sql = fmt::format(
        "SELECT id FROM tbl_{} WHERE JSON_EXTRACT({}, '{}') {} {}",
        entityType, jsonPath, op, value.toSql()
    );
}
```

### 8.3 PostgreSQL 后端的 JSONB 支持

```cpp
// storage/PostgreSQLBackend.cpp
// JSONB 更强大：@> 包含、? key存在、->/->> 路径、GIN 索引

Future<std::vector<EntityId>>
PostgreSQLBackend::queryJsonPath(const std::string& entityType,
                                  const std::string& jsonPath,
                                  const std::string& op,
                                  const QueryParam& value) {
    std::string sql = fmt::format(
        "SELECT id FROM tbl_{} WHERE {} @> '{}'",
        entityType, jsonPath, value.toJson()
    );
}
```

### 8.4 MongoDB 后端（文档模型）

```cpp
// storage/MongoDBBackend.cpp
// 天然文档模型：FIXED_DICT → 嵌套文档，ARRAY → 数组

Future<std::vector<EntityId>>
MongoDBBackend::queryJsonPath(const std::string& entityType,
                               const std::string& jsonPath,
                               const std::string& op,
                               const QueryParam& value) {
    // MongoDB: db.tbl_Player.find({"equipment.weapon": 500})
    // 不需要 JSON 函数，直接用点路径
}
```

---

## 9. 与 KBEngine/BigWorld 的完整对比

| 能力 | KBEngine | BigWorld | theseed |
|------|---------|---------|---------|
| **定义格式** | XML (.def) 无 XSD | XML (.def) 无 XSD | XML (.def) + XSD 校验 + XInclude 拆分 |
| **映射粒度** | 表级（5 种类型） | 属性级（15+ 种） | 属性级（8 种 column_type） |
| **ARRAY 存储** | BLOB | 子表（SequenceMapping） | JSON / SUBTABLE（可选） |
| **FIXED_DICT 存储** | BLOB | 多列映射（CompositeMapping） | JSON / 多列（可选） |
| **JSON 支持** | 无 | 无 | 原生（MySQL JSON / PG JSONB / Mongo） |
| **JSON 索引** | 无 | 无 | json_path 索引 / 多值索引 |
| **自定义查询** | 无 | 无 | 参数化 SQL + 命名查询 + 缓存 |
| **聚合查询** | 无 | 无 | COUNT/SUM/AVG/MIN/MAX |
| **合服工具** | 无 | consolidate_dbs/transfer_db/sync_db | theseed-merge（ID重映射+冲突解决） |
| **结构校验** | 无（运行时报错） | 无（运行时报错） | XSD 编辑器实时校验 |
| **语义校验** | 无 | 无 | defcheck（15+ 规则） |
| **IDE 集成** | 无 | 无 | .pyi 类型存根 + 字段约束提示 |
| **文件拆分** | 无 | 无 | XInclude 按关注点拆分 |
| **存储后端** | MySQL + Redis | MySQL + XML | MySQL/PostgreSQL/MongoDB/Redis/Memory |
| **Schema 迁移** | syncToDB（有限） | 手动 | 自动 diff + 迁移计划 |
| **字段大小校验** | 无（运行时才报错） | 无 | XSD 约束 + defcheck 语义检查 |

---

## 10. 目录结构

```
theseed/
├── schemas/
│   ├── entity.xsd                    # 实体定义 Schema
│   ├── defcheck-rules.xsd            # 校验规则 Schema
│   └── merge.xsd                     # 合服配置 Schema
│
├── entities/
│   ├── Avatar/
│   │   ├── Avatar.def                # 主文件（组合入口）
│   │   ├── properties.xml            # 属性定义
│   │   ├── db_mapping.xml            # 数据库映射
│   │   ├── client_sync.xml           # 客户端同步配置
│   │   ├── cell_methods.xml          # Cell 方法
│   │   ├── base_methods.xml          # Base 方法
│   │   ├── client_methods.xml        # 客户端方法
│   │   └── queries.xml               # 命名查询
│   ├── Monster/
│   │   ├── Monster.def
│   │   └── ...
│   └── shared/
│       ├── combat_props.xml           # 共享战斗属性
│       └── movement_props.xml         # 共享移动属性
│
├── src/
│   ├── storage/
│   │   ├── IStorageBackend.h           # 抽象接口
│   │   ├── StorageQuery.h              # 查询 DSL
│   │   ├── CustomQuery.h/cpp           # 自定义查询
│   │   ├── PropertyMapping.h           # 属性级映射策略
│   │   ├── SchemaManager.h/cpp         # Schema 管理 + 自动迁移
│   │   ├── MergeManager.h/cpp          # 合服工具核心
│   │   ├── MySQLBackend.h/cpp          # MySQL（JSON 支持）
│   │   ├── PostgreSQLBackend.h/cpp     # PostgreSQL（JSONB 支持）
│   │   ├── MongoDBBackend.h/cpp        # MongoDB（文档模型）
│   │   ├── RedisBackend.h/cpp          # Redis（缓存）
│   │   └── MemoryBackend.h/cpp         # 纯内存（测试）
│   │
│   └── tools/
│       └── defcheck/                   # def 校验器
│           ├── main.go
│           ├── rules/                  # 校验规则
│           └── stubs/                  # IDE 类型存根生成
│
├── tools/
│   └── theseed-merge/                 # 合服工具
│       ├── main.go
│       ├── analyzer.go                # 冲突分析
│       ├── remapper.go                # ID 重映射
│       ├── resolver.go                # 冲突解决
│       └── validator.go              # 数据验证
│
├── config/
│   ├── defcheck.xml                   # 校验规则配置
│   └── merge.xml                      # 合服配置
│
└── docs/
    └── design/
        └── 06-data-definition-persistence.md  # 本文档
```
