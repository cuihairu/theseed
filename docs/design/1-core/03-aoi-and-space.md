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

## 4. AOI 更新优先级策略

> 来源：BigWorld `aoi_update_schemes`。
> 这里要特别注意：BigWorld 的原义不是“整套 AOI 插件系统”，而是“每个可见实体的更新优先级曲线配置”。
> 详细边界见 [11-aoi-update-schemes-and-load-bounds](11-aoi-update-schemes-and-load-bounds.md)。

### 4.1 为什么需要可插拔

```
核心问题不是“是否可见”，而是：
  - 视野内哪些实体本 tick 先更新
  - 远近实体的 priority delta 如何变化
  - 如何在 packet budget 内保持近处更细、远处更疏
```

### 4.2 更新优先级策略接口

```cpp
// runtime/aoi/IAoIUpdatePolicy.h

class IAoIUpdatePolicy {
public:
    virtual ~IAoIUpdatePolicy() = default;

    virtual const char* name() const = 0;
    virtual double priorityDelta(float distanceMeters) const = 0;
    virtual bool treatAsCoincident() const = 0;
};
```

### 4.3 关键约束

```
1. policy 控制 priority，不直接控制 visibility
2. policy 粒度是“某个 witness 视角下某个目标实体的更新节奏”
3. coincident 是特殊语义，不是普通 detailLevel
4. detailLevel / payload 选择仍应和 policy 分层
```

### 4.4 配置占位

```yaml
# config/aoi.yaml

aoi:
  update_policies:
    - name: default
      min_priority_delta: 1.0
      max_priority_delta: 5.0

    - name: coincident
      treat_as_coincident: true
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

BigWorld 的 `load bounds / range update` 系统边界见：

`./11-aoi-update-schemes-and-load-bounds.md`
