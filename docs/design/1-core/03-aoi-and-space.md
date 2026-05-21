# AOI & Space — 空间感知与动态拓扑

> AOI（Area of Interest）是 MMO 服务器的核心子系统，决定哪些实体互相可见。
> Space Topology 决定空间如何跨进程分布。
>
> 来源：BigWorld 十字链表 + BSP 拓扑，KBEngine 完整继承 AOI 但不做动态拓扑。

---

## 1. AOI 设计选择：十字链表

KBEngine 和 BigWorld 都使用**十字链表（Coordinate System / Range List）**作为 AOI 索引。

为什么不用网格或四叉树/八叉树：

```
十字链表优势：
  1. 实体移动时只需 O(1) 调整链表位置
  2. 范围查询：在每条轴上找范围 → 取交集
  3. 触发器可以"挂"在链表节点上，节点经过时自动触发
  4. 不需要预分配网格，实体密度不均匀时也高效

劣势：
  1. 范围查询不是 O(1)，是 O(视野内实体数)
  2. 不适合超大范围查询（如全地图广播）

结论：MMO 中 AOI 通常在 50-500 米范围，十字链表是最优选择。
```

---

## 2. 核心组件

```
AOI 系统组件关系（来自 KBEngine 源码分析）：

CoordinateSystem
  ├── CoordinateNode（每个实体一个）
  │     ├── x_prev / x_next  ← X 轴双向链表
  │     ├── y_prev / y_next  ← Y 轴双向链表（可选）
  │     └── z_prev / z_next  ← Z 轴双向链表
  │
  ├── RangeTrigger（每个有视野的实体一个或多个）
  │     ├── 在每条轴上安装边界节点
  │     ├── 节点穿越边界时触发回调
  │     └── X/Y/Z 三轴取交集确定进入/离开
  │
  └── Witness（每个有客户端的实体一个）
        ├── ViewTrigger：主视野半径
        ├── HysteresisTrigger：滞后防抖区域
        ├── viewEntities_：当前可见实体集合
        ├── changeDefDataLogs[]：按 detailLevel 分组的脏属性
        └── update()：tick 末执行，处理进出+同步
```

---

## 3. 核心接口

```cpp
// runtime/CoordinateSystem.h

class CoordinateSystem {
public:
    // 插入实体节点
    void insert(CoordinateNode* node);

    // 移除实体节点
    void remove(CoordinateNode* node);

    // 移动节点（实体位置变化时调用）
    void update(CoordinateNode* node, const Vector3& newPos);

    // 范围查询
    void entitiesInRange(std::vector<Entity*>& out,
                         const Vector3& origin,
                         float radius,
                         uint16_t entityTypeFilter = 0) const;
};

class CoordinateNode {
public:
    Entity* entity() const;
    const Vector3& position() const;

    // 链表操作（内部使用）
    void insertX(CoordinateNode* pos);
    void insertY(CoordinateNode* pos);
    void insertZ(CoordinateNode* pos);
    void removeX();
    void removeY();
    void removeZ();

private:
    Entity* entity_;
    Vector3 pos_;

    // 三轴双向链表指针
    CoordinateNode* xPrev_; CoordinateNode* xNext_;
    CoordinateNode* yPrev_; CoordinateNode* yNext_;
    CoordinateNode* zPrev_; CoordinateNode* zNext_;
};

// runtime/RangeTrigger.h

class RangeTrigger {
public:
    RangeTrigger(Entity* owner, float range);
    ~RangeTrigger();

    // 在坐标系统中安装/卸载
    void install(CoordinateSystem* coordSys);
    void uninstall();

    // 更新范围
    void updateRange(float newRange);

    // 节点穿越边界时的回调
    virtual void onEnter(CoordinateNode* node) = 0;
    virtual void onLeave(CoordinateNode* node) = 0;

private:
    Entity* owner_;
    float range_;
    CoordinateSystem* coordSys_;

    // 边界节点（每条轴的正负方向各一个）
    CoordinateNode* xNegBound_; CoordinateNode* xPosBound_;
    CoordinateNode* yNegBound_; CoordinateNode* yPosBound_;
    CoordinateNode* zNegBound_; CoordinateNode* zPosBound_;
};
```

---

## 4. 可插拔 AOI 策略

> 来源：BigWorld `aoi_update_schemes`。
> KBEngine 把 AOI 更新逻辑硬编码在 Witness/Entity 中。
> theseed 取 BigWorld 的可扩展设计。

### 4.1 为什么需要可插拔

```
不同游戏类型对 AOI 的需求不同：

MMO（千人同屏）：
  - 需要严格的 detailLevel 分级
  - 需要带宽预算管理
  - 远处实体降频同步

MOBA/射击（低延迟）：
  - 需要高频率位置同步
  - 不需要 detailLevel（视野范围固定）
  - 可靠性要求更高

房间制（小规模）：
  - 不需要 AOI（所有人互相可见）
  - 全量广播即可
```

### 4.2 AOI 策略接口

```cpp
// runtime/aoi/IAOIScheme.h

class IAOIScheme {
public:
    virtual ~IAOIScheme() = default;

    virtual const char* name() const = 0;

    virtual void onEntityEnterView(Witness* witness,
                                    Entity* entity,
                                    float distance) = 0;
    virtual void onEntityLeaveView(Witness* witness,
                                    Entity* entity) = 0;

    virtual void updateView(Witness* witness,
                            Duration tickBudget,
                            Bundle& outBundle) = 0;

    virtual float calculatePriority(const Witness* witness,
                                     const Entity* entity) const = 0;

    virtual int calculateDetailLevel(float distance) const = 0;

    virtual size_t estimateBundleSize(const Witness* witness) const = 0;
};
```

### 4.3 内建策略

```cpp
// runtime/aoi/schemes/MMOScheme.h
// 默认策略：来自 BigWorld + KBEngine 的融合
class MMOScheme : public IAOIScheme {
    // detailLevel 分级（来自 KBEngine 3 级 + BigWorld 4 级）
    // 带宽预算管理（来自 BigWorld EntityCache 优先级队列）
    // Hysteresis 防抖（来自 BigWorld DataLoDLevels）
    // volatile threshold（来自两边的 VolatileInfo）
};

// runtime/aoi/schemes/ActionScheme.h
// 动作游戏策略：高频同步，无 detailLevel
class ActionScheme : public IAOIScheme {
    // 固定视野范围
    // 所有可见实体全量同步
    // 高频位置更新（每 tick）
    // 无带宽预算（视野内实体数有限）
};

// runtime/aoi/schemes/RoomScheme.h
// 房间制策略：无 AOI，全量广播
class RoomScheme : public IAOIScheme {
    // 所有实体互相可见
    // 所有变更广播给所有人
    // 不需要 CoordinateSystem
};
```

### 4.4 配置

```yaml
# config/aoi.yaml

aoi:
  default_scheme: mmo

  space_overrides:
    - space_type: "BattleRoyale"
      scheme: action
    - space_type: "ChatRoom"
      scheme: room

  mmo:
    detail_levels:
      - level: 0
        distance: 30
        sync_props: all
      - level: 1
        distance: 80
        sync_props: [position, rotation, name, equipment_visual]
      - level: 2
        distance: 200
        sync_props: [position, rotation]
    bandwidth_budget: 8192
    hysteresis: 5.0
    volatile_position_threshold: 0.5
    volatile_rotation_threshold: 0.1

  action:
    view_radius: 100
    sync_frequency: every_tick
    sync_props: all
```

---

## 5. 动态空间拓扑

> 来源：BigWorld BSP 树 + grow/shrink + Offload。
> KBEngine 不做动态拓扑，每个 Space 只有一个 Cell。
> theseed 分阶段演进。

### 5.1 方案对比

```
BigWorld BSP 树：
  优势：单 Space 大世界无缝地图，Cell 边界自动调整
  代价：BSP 树维护复杂，Cell 边界同步，实体迁移频繁

KBEngine 简化：
  优势：简单，一个 Space = 一个 Cell
  代价：单 CellApp 处理不了大世界，无法水平扩展

theseed 的选择（分阶段）：

Phase 1：KBEngine 模式
  - 一个 Space = 一个 Cell
  - 够用 90% 的场景（房间制/副本制/中型地图）

Phase 2：静态多 Cell
  - 手动配置 Space 的 Cell 划分
  - 每个 Cell 可以放在不同 CellApp
  - Cell 边界固定
  - 实体跨 Cell 走 Ghost + 迁移

Phase 3：动态拓扑（BigWorld 级）
  - BSP 树动态分割
  - Cell 边界自动 grow/shrink
  - 基于负载的自动均衡
```

### 5.2 接口预留

```cpp
// runtime/ISpaceTopology.h

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

// Phase 1
class SingleCellTopology : public ISpaceTopology {
    // 一个 Space 只有一个 Cell
};

// Phase 2
class StaticPartitionTopology : public ISpaceTopology {
    // 手动配置的固定分区
};

// Phase 3（远期）
class BSPTopology : public ISpaceTopology {
    // BigWorld 风格的 BSP 树，动态 grow/shrink
};
```
