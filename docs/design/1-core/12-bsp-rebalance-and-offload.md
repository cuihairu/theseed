# BSP Rebalance & Offload — grow/shrink、chunk-aware balance 与退役迁出

> 来源：BigWorld `InternalNode::doBalance / balanceChildren / balanceOnUnloadedChunks / chunkLimitInDirection / CellData::balance / retireCell / shouldOffload`。
> 这篇补的是 BigWorld 动态多 Cell 真正复杂的那一层，不是再重复“Phase 3 支持 BSP”。

---

## 0. 设计边界

```
本篇负责：
  - BSP 分区点如何 grow / shrink
  - rebalance 时如何结合实体负载与 chunk 边界
  - offload / retireCell 与 rebalance 的关系
  - updateRanges 和 loadBalance 的区别

本篇不负责：
  - 十字链表 AOI
  - witness 优先级调度
  - 迁移快照序列化协议
```

和其他文档的关系：

```
03-aoi-and-space
  - 讲 topology 大框架

07-entity-migration
  - 讲实体怎么迁

11-aoi-update-schemes-and-load-bounds
  - 讲 load bounds / range update 输入

本篇
  - 讲 BSP 怎么据此移动边界、触发 offload
```

---

## 1. 为什么这篇必须单独立

BigWorld 的 `BSP` 复杂度不在“树形分区”这四个字。

真正的复杂度在于它同时处理：

```
1. 实体 CPU / 逻辑负载
2. chunk 加载边界
3. ghost distance 邻接成本
4. 新 Cell 尚未 ready 时的保护
5. retire / offload / underload shrink
```

如果这些都继续只被概括成：

```
Phase 3: BSP / auto rebalance
```

文档仍然没有覆盖 BigWorld 最关键的系统面。

---

## 2. BigWorld 到底在平衡什么

从 `InternalNode::doBalance` 可以提炼出它不是单一目标优化，而是同时看四类信号：

```
1. 左右子树平均负载差
2. maxLoad 是否触碰 safety bound
3. 是否还有未加载 chunk
4. 子 Cell 是否已经创建完成
```

所以 `rebalance` 不是：

```
哪里实体多就把边界往那边推
```

而更接近：

```
在可移动前提下，
在实体边界、chunk 边界、ghost 邻接成本和 loadSafetyBound 之间取最近可行点
```

---

## 3. grow / shrink 的真实语义

### 3.1 Grow

```
某侧边界外扩
意味着该侧将承接更多空间区域与潜在实体
```

### 3.2 Shrink

```
某侧边界内收
意味着该侧区域被削减，并可能触发实体迁出或 Cell 退役
```

### 3.3 不对称约束

BigWorld 的 `balanceChildren()` 已经体现出一个重要点：

```
一边 grow
另一边 shrink
并且 shrink 方会被显式标记
```

这意味着 theseed 的 BSP rebalance 不应只是“更新一个 split 值”，而必须携带：

```
which side grows
which side shrinks
whether shrink may force offload
```

---

## 4. 先决条件比算法本身更重要

BigWorld 在 rebalance 前有几个非常关键的保护条件：

### 4.1 新 Cell 必须已创建

```
childrenCreated = left.hasBeenCreated && right.hasBeenCreated
```

设计含义：

```
不能在目标 Cell 还没 ready 时就把边界推过去，
否则其他 Cell 可能先开始 offload，导致目标不存在。
```

### 4.2 retire 状态改变目标函数

如果某侧正在 retire：

```
不是继续做平滑均衡，
而是优先把区域往非 retiring 一侧推。
```

### 4.3 loadSafetyBound 是硬保护

BigWorld 明确阻止：

```
为了平衡 chunk 或 split，
把已经接近过载的一侧再继续长大。
```

这也是 theseed 以前文档里还没单独写清的边界。

---

## 5. 不是只看实体，还要看 chunk

`balanceOnUnloadedChunks()` 和 `chunkLimitInDirection()` 说明 BigWorld 的平衡不是纯逻辑负载。

它还在优化：

```
哪些区域还没把所需 chunk 加载完
```

这意味着 BSP rebalance 至少有两个目标函数：

| 模式 | 主要目标 |
|------|----------|
| CPU / entity balance | 平衡平均逻辑负载 |
| unloading / loading aware balance | 减少未加载 chunk 带来的不完整区域 |

这也是为什么 `loadBalance` 和 `updateRanges` 要分开。

---

## 6. entity limit 与 chunk limit 是两个不同边界

BigWorld 的 `closestLimit(entityLimit, chunkLimit, direction)` 非常关键。

它说明分区点移动时至少受两个限制：

### 6.1 entityLimit

```
基于实体分布和 loadDiff 推导的最远可移动点
```

### 6.2 chunkLimit

```
基于 chunkBounds + ghostDistance 推导的最远可移动点
```

设计含义：

```
边界不是想推多远就推多远，
而是只能推到“负载约束”和“世界资产约束”共同允许的最近点。
```

这点是 BigWorld 多 Cell 世界和 KBEngine 单 Cell 世界最本质的差异之一。

---

## 7. ghostDistance 不只是 AOI 参数

在 `chunkLimitInDirection()` 里，BigWorld 把 `ghostDistance` 直接纳入了分区边界限制。

这说明：

```
ghostDistance
  不只是同步半径参数
  还是 topology 约束参数
```

因为边界变动后，相邻 Cell 仍然需要在 ghost 邻接区内保持正确加载和查询能力。

theseed 应把这一点单独写死，避免将来把 `ghostDistance` 只留在同步配置里。

---

## 8. updateRanges 和 loadBalance 不是一回事

BigWorld 把两者分成两条路径：

### 8.1 updateRanges

```
重算当前 BSP 节点与叶子的空间范围
不必做完整负载决策
```

### 8.2 loadBalance

```
基于当前负载和 chunk 状态决定边界是否移动
必要时触发 grow / shrink / offload / delete cell
```

theseed 之前已经把 `load bounds / range update` 提出来了，现在要再补一句：

```
range update 是几何传播
rebalance 是调度决策
```

---

## 9. offload 与 retireCell 的关系

BigWorld 这里至少有三种相近但不同的动作：

### 9.1 shouldOffload

```
允许 / 禁止某个 Cell 继续主动迁出实体
```

### 9.2 offload

```
把某些实体或区域工作转移给另一侧
```

### 9.3 retireCell

```
让一个 Cell 进入退役路径，最终退出分区树
```

它们的关系不是：

```
rebalance = retire
```

而是：

```
rebalance
  可能只移动边界
  可能触发 offload
  在 shrink 到无面积且无实体时，才可能进入 delete / retire
```

---

## 10. theseed 的建议建模

### 10.1 BSP rebalance 输入

```cpp
// topology/BspRebalanceInput.h

struct BspRebalanceInput {
    SpaceId spaceId;
    Rect2f fullRange;
    float loadSafetyBound = 0.0f;
    bool shouldLimitToChunks = true;
    bool shouldBalanceUnloadedChunks = false;
    float ghostDistanceMeters = 0.0f;
};
```

### 10.2 节点决策结果

```cpp
// topology/BspRebalanceDecision.h

enum class BspBalanceDirection {
    None,
    Left,
    Right
};

struct BspRebalanceDecision {
    BspBalanceDirection direction = BspBalanceDirection::None;
    float oldSplit = 0.0f;
    float newSplit = 0.0f;
    bool causedShrink = false;
    bool chunkConstrained = false;
    bool loadConstrained = false;
};
```

### 10.3 控制接口

```cpp
// topology/IBspRebalanceController.h

class IBspRebalanceController {
public:
    virtual ~IBspRebalanceController() = default;

    virtual BspRebalanceDecision evaluate(
        const BspRebalanceInput& input) = 0;

    virtual void apply(const BspRebalanceDecision& decision) = 0;
    virtual void updateRanges(SpaceId spaceId) = 0;
};
```

设计要求：

```
1. evaluate 和 apply 分离
2. 结果必须显式说明受 chunk 约束还是 load 约束
3. shrink 是否会触发 offload / retire 必须可见
```

---

## 11. 与迁移文档的接口

本篇只定义：

```
什么时候需要迁
哪一侧 grow/shrink
哪些 Cell 进入 offload / retire
```

真正的迁移执行仍归 [07-entity-migration](07-entity-migration.md)。

因此二者关系应写成：

```
BSP rebalance
  → produces migration / offload intent

Entity migration
  → executes data-plane movement safely
```

---

## 12. 分阶段边界

```
MVP：
  - 不承诺 BSP rebalance
  - 只支持 single_cell

Phase 2：
  - static partition
  - 不做动态 grow / shrink
  - 允许人工控制分区与 offload 策略

Phase 3：
  - 动态 split position 调整
  - load-aware + chunk-aware rebalance
  - grow / shrink / offload / retireCell 联动
```

---

## 13. 与 BigWorld / KBEngine / theseed 的对比

| 维度 | BigWorld | KBEngine | theseed |
|------|---------|---------|---------|
| BSP split move | 有 | 无 | Phase 3 目标 |
| chunk-aware balance | 有 | 无 | Phase 3 目标 |
| loadSafetyBound | 有 | 弱 | 明确建模 |
| shouldOffload / retireCell 联动 | 有 | 弱 | 需联动 lifecycle/migration |
| updateRanges 与 loadBalance 分离 | 有 | 无 | 现在单列边界 |
