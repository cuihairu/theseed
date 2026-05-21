# AoI Update Schemes & Load Bounds — 更新优先级曲线与空间负载边界

> 来源：BigWorld `cellapp/aoi_update_schemes`、`Witness / EntityCache priority`、`SPACE_DATA_SERVER_LOAD_BOUNDS`、`cellappmgr::Space::updateRanges`。
> 这篇补的是 BigWorld 在 AOI 与空间负载之间的系统化边界，不是再讲一遍十字链表。

---

## 0. 设计边界

```
本篇负责：
  - AoI update scheme 的真实职责
  - 每个可见实体的更新优先级曲线
  - load bounds / range update 与空间负载边界
  - AoI 更新频率与空间分区负载的接口关系

本篇不负责：
  - 十字链表索引本身
  - 属性同步协议编码
  - 完整 BSP 拓扑实现细节
```

和其他文档的关系：

```
03-aoi-and-space
  - 讲 AOI 索引和 topology 主体

06-property-sync
  - 讲属性怎么发

本篇
  - 讲“什么时候给谁发得更快/更慢”
  - 讲空间负载边界如何反馈给 Cell 划分
```

---

## 1. 先纠正一个常见误读

BigWorld 的 `aoi_update_schemes` 不是“不同游戏类型的整套 AOI 插件框架”。

它更精确的职责是：

```
给 Witness 视野中的某个实体，
定义一条基于距离的更新优先级变化曲线。
```

也就是说，它控制的是：

```
EntityCache priority delta
```

而不是：

```
CoordinateSystem 是否存在
AOI 是否改成房间广播
整套可见性模型如何替换
```

这也是 theseed 之前文档里说得过宽的地方。

---

## 2. BigWorld 到底在做什么

从 `AoIUpdateScheme::init/apply`、`Witness::update` 可以看出，BigWorld 的思路是：

```
1. 每个可见实体都有一个 EntityCache
2. 每个 EntityCache 有一个 updateSchemeID
3. scheme 根据距离给 priority 施加 delta
4. Witness 每 tick 只发送优先级窗口内的一部分实体
```

这意味着 BigWorld 在 AOI 上有两层：

```
层 A：谁在视野里
  - AOI enter / leave

层 B：视野里的人，谁先发、多久发一次
  - update scheme + priority queue + bandwidth budget
```

当前 theseed 已覆盖层 A 主体，但层 B 还没单独立边界。

---

## 3. AoI Update Scheme 的原义

### 3.1 职责

AoI Update Scheme 描述的是：

```
distance
  → priority delta
```

BigWorld 的配置本质上是两个控制点：

```
minDelta
maxDelta
```

然后结合 `maxAoIRadius` 推导一条线性曲线。

### 3.2 特殊语义

BigWorld 里还有一个重要特例：

```
minDelta = 0
maxDelta = 0
```

这意味着：

```
把目标视作 coincident
即“按最高细节、最高优先级处理”
```

这不是 another scheme name，而是一种明确的优先级语义。

---

## 4. 它解决的不是“可见性”，而是“预算分配”

Witness 的关键问题不是只知道谁可见，而是：

```
在一个 tick / packet budget 内，
先更新哪些实体。
```

BigWorld 的做法可以抽象成：

```
visible entities
  → priority queue
  → update scheme changes priority delta over distance
  → cap max priority delta each tick
  → fit into desired packet size
```

所以 theseed 要补的不是“又一个 AOI 策略接口”，而是：

```
AoI update priority policy
```

---

## 5. theseed 应如何建模

### 5.1 建模目标

theseed 需要同时满足：

```
1. 不丢 BigWorld 的 per-entity priority update 语义
2. 不把整套 AOI 系统误抽象成“任意插件”
3. 给未来 MMO / 高频战斗场景留下差异化配置入口
```

### 5.2 推荐抽象

```cpp
// aoi/IAoIUpdatePolicy.h

using AoIUpdatePolicyId = uint16_t;

struct AoIUpdatePolicyConfig {
    std::string name;
    float minPriorityDelta = 1.0f;
    float maxPriorityDelta = 5.0f;
    bool treatAsCoincident = false;
};

class IAoIUpdatePolicy {
public:
    virtual ~IAoIUpdatePolicy() = default;

    virtual std::string name() const = 0;
    virtual double priorityDelta(float distanceMeters) const = 0;
    virtual bool isCoincident() const = 0;
};
```

### 5.3 Witness 侧使用点

```cpp
// aoi/WitnessPriorityScheduler.h

class IWitnessPriorityScheduler {
public:
    virtual ~IWitnessPriorityScheduler() = default;

    virtual void setPolicy(EntityId entityId, AoIUpdatePolicyId policyId) = 0;
    virtual void onDistanceChanged(EntityId entityId, float distanceMeters) = 0;
    virtual std::vector<EntityId> selectForUpdate(
        size_t packetBudgetBytes,
        float maxPriorityDeltaPerTick) = 0;
};
```

设计要求：

```
1. policy 控制 priority，不直接控制可见性
2. policy 粒度是“目标实体在某个 witness 里的更新策略”
3. scheduler 负责预算窗口，不让 policy 直接操作 bundle
```

---

## 6. 不要再把三件事混在一起

AOI 设计里必须持续拆开三件事：

| 层 | 问题 | BigWorld 代表机制 |
|------|------|------------------|
| Visibility | 谁进入/离开视野 | CoordinateSystem / RangeTrigger |
| Priority | 视野里谁先发 | AoIUpdateScheme / EntityCache priority |
| Payload | 发哪些字段 | detail level / property sync |

如果把三者混成一个 `IAOIScheme`，会导致：

```
1. 对 BigWorld 原义失真
2. 同步策略、优先级策略、可见性策略耦死
3. Runtime Core 很快长出一个“能力桶接口”
```

---

## 7. Load Bounds 是另一条系统线

BigWorld 的 `SPACE_DATA_SERVER_LOAD_BOUNDS` 和 `updateRanges()` 说明：

```
Space 不只是有实体负载，
还存在“服务端应关注的空间负载边界”。
```

这层解决的问题是：

```
在多 Cell / BSP 场景里，
哪些区域的负载、边界、chunk 覆盖应参与分区更新。
```

因此 load bounds 不应被写成：

```
一个普通的 SpaceData key
```

而应被看作：

```
Topology / balancing 的输入元数据
```

---

## 8. BigWorld 的空间负载边界在做什么

从 `cellappmgr::Space::loadBalance / updateRanges`、`CellData::balanceChunkBounds` 可以提炼出几点：

```
1. 空间边界更新不是只看当前 Cell 的实体数
2. 还要考虑 chunk 覆盖、未加载区域、ghost distance 邻接成本
3. loadSafetyBound 会限制平衡过程中不要把过载区域进一步放大
4. range update 和 load balance 虽相关，但不是一回事
```

这部分恰好是当前 theseed 只写 phase 边界、没写系统语义的地方。

---

## 9. theseed 的建议建模

### 9.1 负载边界描述

```cpp
// topology/SpaceLoadBounds.h

struct SpaceLoadBounds {
    Rect2f logicalBounds;
    Rect2f loadedChunkBounds;
    Rect2f serverInterestBounds;
    float ghostMarginMeters = 0.0f;
};
```

### 9.2 分区更新接口

```cpp
// topology/ISpaceRangeUpdater.h

struct SpaceRangeUpdateInput {
    SpaceId spaceId;
    SpaceLoadBounds bounds;
    float avgSmoothedLoad = 0.0f;
    float maxSmoothedLoad = 0.0f;
    float loadSafetyBound = 0.0f;
};

class ISpaceRangeUpdater {
public:
    virtual ~ISpaceRangeUpdater() = default;

    virtual void updateRanges(const SpaceRangeUpdateInput& input) = 0;
};
```

### 9.3 设计要求

```
1. load bounds 是 topology 输入，不是 gameplay 配置糖
2. updateRanges 可以独立于完整 rebalance 执行
3. chunk / load bounds / ghost margin 必须可观测
```

---

## 10. 和世界流送的关系

这篇与 [07-world-streaming-and-compiled-space](../4-gameplay/07-world-streaming-and-compiled-space.md) 直接相关，但边界不同：

```
world streaming
  - 关注静态世界资产如何进入 Space

load bounds
  - 关注这些资产/边界如何影响分区与均衡
```

这两篇合起来，才比较接近 BigWorld 的世界侧系统语义。

---

## 11. 分阶段边界

```
MVP：
  - 只保留默认 AoI update priority policy
  - 不承诺 per-entity 自定义策略
  - 不承诺 load bounds 驱动的 range update

Phase 2：
  - 支持按实体或实体类型覆盖 update policy
  - 引入 SpaceLoadBounds 抽象
  - 静态分区下支持基础 updateRanges

Phase 3：
  - 支持更接近 BigWorld 的 per-entity priority curve
  - 支持 load bounds / chunk bounds / ghost margin 驱动的动态范围更新
  - 与 BSP rebalance 联动
```

---

## 12. 与 BigWorld / KBEngine / theseed 的对比

| 维度 | BigWorld | KBEngine | theseed |
|------|---------|---------|---------|
| visible set | 有 | 有 | 已覆盖 |
| per-entity update scheme | 有 | 基本无 | Phase 2/3 目标 |
| coincident 高优先级语义 | 有 | 弱 | 应显式建模 |
| load bounds / range update | 有 | 弱 | 需单列边界 |
| AOI / priority / payload 分层 | 明确但分散 | 较混 | 现在开始显式拆分 |
