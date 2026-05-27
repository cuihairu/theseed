# Space Topology & AOI — 空间拓扑、AOI 索引与 Space 生命周期

> 来源源头：BigWorld `CoordinateSystem / RangeTrigger / BSP topology / Space`。
> KBEngine 是轻量参考实现，继承 AOI 主干但把 Space 收缩成更接近单 Cell 的路径。
> theseed 在文档层显式拆开运行时空间、拓扑阶段与世界资产边界。

---

## 0.5 引擎实现对照与取舍

### BigWorld 是怎么实现的

```
BigWorld 把 Space / Topology / AOI 视为同一条运行时链：
  - Space 是运行时容器
  - Topology 决定 Cell 分布
  - AOI 决定可见性候选集
  - 动态多 Cell 进一步接到 BSP / rebalance / chunk awareness
```

### KBEngine 是怎么实现的

```
KBEngine 继承了 AOI 主干和 Space 语义，
但整体更收缩到单 Space / 单 Cell 路线，
没有 BigWorld 那么完整的动态多 Cell 系统层。
```

### 优缺点

```
BigWorld 的优点：
  - 空间、AOI、拓扑边界完整
  - 适合大世界动态调度

KBEngine 的优点：
  - 简洁
  - 更容易先把运行时主路径做出来

共同缺点：
  - 这三层一旦拆散写，文档就很容易自相矛盾
```

### theseed 的取舍

```
theseed 先把 Space / Topology / AOI 放回同一层，
MVP 选单 Cell + 十字链表，
把 static partition 和 BSP 明确收进 Phase 2 / 3。
```

---

## 0. 设计边界

本篇负责：

```
  - Space 作为运行时空间容器的职责
  - AOI 索引选择
  - ISpaceTopology 的阶段模型
  - Space 生命周期与配置边界
```

本篇不负责：

```
  - Witness payload 预算
  - Property Replication 细节
  - AoI Update priority curve
  - Load Bounds / BSP rebalance 具体策略
  - Chunk / Geometry Mapping / Compiled Space 的离线资产边界
```

相关主题见：

```
02-ghost-and-witness
03-runtime-communication-and-transport
04-property-replication
06-aoi-update-and-load-bounds
07-bsp-rebalance-and-offload
../6-world-and-game-framework/04-world-streaming-and-compiled-space
```

---

## 1. 为什么把 AOI 和 Space 放在一起

在 BigWorld / KBEngine 这类引擎里，下面三件事其实是一条链：

```
1. Space
   - 决定实体属于哪个空间上下文

2. Topology
   - 决定这个空间如何分布到一个或多个 Cell

3. AOI
   - 决定同一空间内哪些实体彼此可见
```

如果把它们拆成互不相关的文档，很容易出现两种错误：

```
1. 把 topology 误写成当前能力
2. 把 AOI 误写成和 Space 无关的独立插件
```

因此这轮重组把它们统一放回“复制与空间”层。

---

## 2. 概念对比

| 维度 | BigWorld | KBEngine | theseed |
|------|------|------|------|
| Space 本质 | 独立管理单元 | 逻辑上更贴近单 Cell Space | 独立管理单元 |
| Cell 拓扑 | BSP 动态拓扑 | 单 Space 单 Cell | Phase 1 单 Cell / Phase 2 静态分区 / Phase 3 BSP |
| AOI 基础结构 | 十字链表 + RangeTrigger | 十字链表 + RangeTrigger | 十字链表 + RangeTrigger |
| Chunk / Compiled Space | 有 | 无完整等价层 | 单独放到世界系统层 |

结论不是“theseed 复制 BigWorld 全部世界系统”，而是：

```
先把运行时 Space / Topology / AOI 的边界摆正，
再决定哪些 BigWorld 世界资产能力进入后续阶段。
```

---

## 3. AOI 索引选择

theseed 继续采用十字链表作为 MVP AOI 索引。
这里不是因为它“最现代”，而是因为它在 BigWorld / KBEngine 的目标场景里足够稳，且能直接承接 RangeTrigger 语义。

### 3.1 候选方案对比

| 方案 | 更新成本 | 查询成本 | 适配不均匀密度 | 触发语义 | 结论 |
|------|------|------|------|------|------|
| 十字链表 | 稳定 | 中等 | 较好 | 原生支持 | 适合本阶段 |
| Uniform Grid | 简单 | 依赖格子尺寸 | 一般 | 需额外封装 | 可做基础方案，但不优先 |
| Spatial Hash | 简单到中等 | 依赖哈希质量 | 一般 | 需额外封装 | 更像工程技巧，不是语义层方案 |
| QuadTree | 较高 | 视分布而定 | 较好 | 需额外封装 | 适合静态或弱动态场景 |
| ECS Chunk + Grid | 较高 | 较好 | 较好 | 与 ECS 强耦合 | 适合 ECS 世界，不是当前主线 |

### 3.2 BigWorld 是怎么实现的

```
BigWorld 的 AOI 不是“简单半径查询”。
它核心上是：
  - RangeList / RangeTrigger 维护坐标轴上的有序边界
  - Witness 订阅实体进入/离开候选集
  - AOI 半径变化、移动、跨 Cell 触发都走同一套触发链

这套设计的价值是：
  - 更新和查询共用一条语义链
  - 触发器直接服务于 Witness / Ghost
  - 对 MMO 的中等半径可见性足够高效
```

### 3.3 KBEngine 是怎么实现的

```
KBEngine 也继承了十字链表式 AOI 主干。
它的重点更偏向：
  - 单 Space / 单 Cell 的简化模型
  - 直接承接 EntityCall 和 Witness 逻辑
  - 保留范围变化触发，但没有 BigWorld 那么完整的系统级周边
```

### 3.4 优缺点

```
十字链表的优点：
  1. 实体移动时更新成本稳定
  2. RangeTrigger 可以直接挂在坐标轴边界上
  3. 适合 MMO 常见的中等半径 AOI 查询
  4. 与 Witness / Ghost 的触发语义天然匹配
  5. 复杂度低于树结构的动态平衡

十字链表的缺点：
  1. 比纯网格更依赖实现细节
  2. 密度极端不均匀时不是最优解
  3. 不是现代 ECS 世界里最自然的索引结构
```

### 3.5 theseed 的取舍

```
theseed 仍然选择十字链表，原因不是保守，而是边界清楚：
  - 本阶段目标是保留 BigWorld 的 Space / AOI 语义，并借鉴 KBEngine 的轻量主路径
  - AOI 要优先服务 Witness / Ghost / Migration，而不是追求最“新”的索引名词
  - Uniform Grid / Spatial Hash / QuadTree / ECS Chunk + Grid 都可以作为后续候选，但不应抢占当前主线

因此结论是：
  1. MVP 用十字链表
  2. 未来如果引入 ECS 世界或超大稀疏场景，再评估替代索引
  3. 文档上必须明确：当前没有把这些替代方案定义成实现面
```

### 3.6 核心组件

```cpp
class CoordinateSystem {
public:
    void insert(CoordinateNode* node);
    void remove(CoordinateNode* node);
    void update(CoordinateNode* node, const Vector3& newPos);

    void entitiesInRange(std::vector<Entity*>& out,
                         const Vector3& origin,
                         float radius,
                         uint16_t entityTypeFilter = 0) const;
};

class RangeTrigger {
public:
    RangeTrigger(Entity* owner, float range);
    virtual ~RangeTrigger() = default;

    void install(CoordinateSystem* coordSys);
    void uninstall();
    void updateRange(float newRange);

    virtual void onEnter(CoordinateNode* node) = 0;
    virtual void onLeave(CoordinateNode* node) = 0;
};
```

这里先只定义“空间索引与触发器”。

Witness、detail level、payload 构建不在本篇展开。

---

## 4. Space 作为运行时容器

Space 在 theseed 里应被定义成：

```
运行时空间上下文
  - 承载 entity membership
  - 绑定 topology
  - 绑定 navigation / physics
  - 持有 space-level data
```

而不是：

```
一个纯场景资源对象
```

### 4.1 核心接口

```cpp
class Space {
public:
    Space(SpaceId id, const std::string& name);

    void initialize(const SpaceConfig& config);
    void shutdown();

    void addEntity(Entity& entity);
    void removeEntity(EntityId id);
    Entity* findEntity(EntityId id) const;

    std::vector<Entity*> queryRange(const Vector3& center, float radius) const;
    uint32_t entityCount() const;

    INavigationSystem& navigation();
    IPhysicsQuery& physics();

    ISpaceTopology& topology();

    void setData(const std::string& key, const std::string& value);
    std::optional<std::string> getData(const std::string& key) const;
};
```

### 4.2 生命周期

```
create
  → load runtime bindings
  → activate
  → running
  → draining
  → shutdown
```

其中：

```
运行时 topology 初始化
  属于 Space 生命周期

chunk / compiled space 离线资源准备
  不属于本篇主线
```

---

## 5. Topology 的阶段模型

### 5.1 Phase 1：SingleCellTopology

```
  - 一个 Space = 一个 Cell
  - 参考 KBEngine 的轻量单 Cell 路径
  - 适合房间制、副本制、中小地图
```

### 5.2 Phase 2：StaticPartitionTopology

```
  - 一个 Space = 多个固定 Cell
  - 分区边界静态配置
  - 实体跨分区走 Ghost + Migration
```

### 5.3 Phase 3：BSPTopology

```
  - 对标 BigWorld 的动态 grow / shrink
  - 支持自动 rebalance / offload
  - 与 load bounds、profiler feedback、chunk awareness 联动
```

### 5.4 接口表达

```cpp
class ISpaceTopology {
public:
    virtual ~ISpaceTopology() = default;

    virtual CellId locateCell(const Vector3& position) const = 0;
    virtual std::vector<CellId> getAdjacentCells(const Vector3& pos,
                                                 float radius) const = 0;
    virtual void onTopologyChanged(std::function<void()> callback) = 0;
    virtual void reportLoad(CellId cell, float load) = 0;
    virtual void rebalance() = 0;
};

class SingleCellTopology : public ISpaceTopology {};
class StaticPartitionTopology : public ISpaceTopology {};
class BSPTopology : public ISpaceTopology {};
```

文档上必须明确：

```
`topology="bsp"` 不是当前可兑现能力，
只允许作为 Phase 3 占位。
```

---

## 6. 配置边界

当前文档只把 `single_cell` 作为 MVP 配置。

```xml
<spaces>
    <space name="main_world" topology="single_cell">
        <navmesh layer="ground" path="navmesh/main_ground.nav"/>
        <collision path="collision/main.col"/>
        <bounds minX="-5000" maxX="5000" minZ="-5000" maxZ="5000"/>
    </space>
</spaces>
```

未来占位可以保留，但必须单独标注阶段：

```xml
<!-- Phase 2 -->
<space name="main_world" topology="static_partition">
    <partition rows="2" cols="2"/>
</space>

<!-- Phase 3 -->
<space name="continent_01" topology="bsp">
    <rebalance policy="auto"/>
</space>
```

这样写的目的是：

```
保留设计抽象，
但不把远期能力伪装成当前实现面。
```

---

## 7. 与其他系统面的接口

### 7.1 与 Ghost / Witness

```
AOI 决定“谁会进入视野候选集”
Witness 决定“这些候选集如何变成客户端可见状态”
```

### 7.2 与 Runtime Transport

```
Space / Topology 决定实体路由上下文
Runtime Transport 负责把跨 Cell 的运行时消息送出去
```

### 7.3 与 Load Bounds / BSP Rebalance

```
Space topology 是载体
load bounds / rebalance 是调度策略
```

### 7.4 与 World Streaming / Compiled Space

```
Space runtime
  != chunk asset pipeline
```

BigWorld 这两层是相关的，但不是一个抽象。

---

## 8. 分阶段边界

```
MVP：
  - single_cell
  - 十字链表 AOI
  - 基础 Space 生命周期

Phase 2：
  - static_partition
  - 固定边界多 Cell
  - 与 ghost / migration 主路径联动

Phase 3：
  - BSP topology
  - grow / shrink
  - 与 load bounds、rebalance、chunk awareness 联动
```

---

## 9. 一句话判断

这篇文档要表达的核心不是：

```
theseed 现在已经有 BigWorld 式动态世界
```

而是：

```
theseed 已经把 Space / AOI / Topology 的抽象放回同一层，
并明确了它与 BigWorld 世界系统、KBEngine 单 Cell 路径之间的边界差异。
```
