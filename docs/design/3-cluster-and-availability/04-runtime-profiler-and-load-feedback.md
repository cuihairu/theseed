# Runtime Profiler & Load Feedback — 负载剖面、过载判定与调度反馈链

> 来源：BigWorld `EntityProfiler / EntityTypeProfiler / artificialMinLoad / CellAppMgr::loadBalance / overloadCheck / shouldOffload`。
> 这篇讲的是“负载信号如何回到调度与限流”，不是通用 Telemetry，也不是 Ops 查询入口。

---

## 0.5 引擎实现对照与取舍

### BigWorld 是怎么实现的

```
BigWorld 的 profiler 不只是观察工具：
  - EntityProfiler / EntityTypeProfiler 产生运行时负载信号
  - 这些信号会反馈到 loadBalance / shouldOffload / 登录限流
```

### KBEngine 是怎么实现的

```
KBEngine 也会做负载判断，
但缺少 BigWorld 那种 profiler → 调度反馈 的完整链路。
```

### 优缺点

```
BigWorld 的优点：
  - 负载信号能真正参与调度
  - 系统反馈闭环更完整

KBEngine 的优点：
  - 简单
  - 不需要先建设完整剖面体系

共同缺点：
  - 这层一旦和 Telemetry 或 Ops 混写，边界就会非常模糊
```

### theseed 的取舍

```
theseed 把 profiler feedback 单独成篇，
因为它既不是“多打一组指标”，
也不是“多一个在线查询入口”，
而是系统调度输入层。
```

### 为什么不能并回 Telemetry 或 Ops

```
如果并回 Telemetry，
会误以为 profiler 的任务只是“采样后展示”。

如果并回 Ops，
又会误以为 profiler 的任务只是“给人看状态”。

但 BigWorld 这层的本质是：
  profiler 输出要继续进入调度器，
  最终影响 offload、rebalance、login throttling。
```

---

## 0. 设计边界

```
本篇负责：
  - runtime profiler 采样对象与层级
  - 负载平滑、人工最小负载、过载判定
  - profiler 信号如何进入负载均衡、offload、登录限流

本篇不负责：
  - traces / metrics / logs 后端
  - 在线命令和 watcher 查询入口
  - 具体 Cell 划分算法的实现细节
```

和其他文档的关系：

```
05-telemetry-and-debug
  - 讲 Telemetry / Debug / 基础 Profiling

04-ops-control-plane
  - 讲怎样 inspect / command

本篇
  - 讲 profiler 信号如何成为系统调度输入
```

---

## 1. 为什么这层不能继续放在 OTel 里

BigWorld 的 `EntityProfiler` 不是“多一组 metrics”。

它更接近：

```
运行时负载定价器
```

因为它的输出会影响：

```
1. entity / entity type load 统计
2. Cell / Space 的负载判断
3. loadBalance / updateRanges
4. shouldOffload
5. overloadCheck 触发登录限制
```

如果还把这层写成：

```
profiling = 火焰图 + 慢 tick 告警
```

就会继续低估 BigWorld 的系统差异。

更准确的边界应该是：

```
火焰图、trace、slow tick 告警
  属于诊断输出

entity load、space load、overloadCheck 输入
  属于系统调度输入
```

---

## 2. BigWorld 的信号链

BigWorld 可以抽象成下面这条反馈链：

```
Entity / Base call time
  → EntityProfiler
  → EntityTypeProfiler
  → Cell / Space smoothed load
  → CellAppMgr balancing / overloadCheck
  → offload / retire / login throttling
```

这里最重要的是：

```
Profiler 不是只向外暴露，
还向内喂给调度器。
```

---

## 3. 三层 profiler 语义

### 3.1 EntityProfiler

职责：

```
记录单个实体一个 tick 内花掉的执行时间
输出 raw / smoothed / adjusted load
```

### 3.2 EntityTypeProfiler

职责：

```
把同类实体的负载聚合成类型级画像
```

### 3.3 Space / Cell / App load

职责：

```
把实体级负载进一步聚合成 CellApp / Space 的调度输入
```

这三层不能再只当成 “metrics label aggregation”。

---

## 4. 关键概念

### 4.1 Raw Load

```
当前 tick 原始执行成本
```

### 4.2 Smoothed Load

```
经过指数平滑后的负载
避免单 tick 抖动直接触发调度
```

### 4.3 Adjusted Load

```
在 smoothed load 之上应用人工修正后的负载
```

### 4.4 artificialMinLoad

BigWorld 的一个关键系统面就是：

```
人工最小负载可以覆盖统计负载下限
```

它的意义不是调试方便，而是：

```
把“世界或实体即使当前 CPU 不高，也应视作更重”
这种运营/调度意图注入系统。
```

---

## 5. artificialMinLoad 到底解决什么

BigWorld 同时在实体和空间层使用 `artificialMinLoad`，说明它解决的是两类问题：

### 5.1 统计负载不足以表达真实成本

例如：

```
1. 某空间仍在加载 chunk
2. 某实体后续会触发大批广播或脚本逻辑
3. 某区域虽然当前安静，但不应再继续收新负载
```

### 5.2 调度器需要人为偏置

例如：

```
1. 暂时不希望某空间被继续压入更多 Cell 工作
2. 某类实体要被“看起来更重”，以便优先迁走
```

所以 theseed 不应把它误建模成一个“调试变量”。

---

## 6. 负载信号如何进入调度

### 6.1 平衡输入

调度器至少需要下面几类输入：

```
  - entity adjusted load
  - entity-type aggregated load
  - cell / space smoothed load
  - chunk / bounds / loading state
  - artificialMinLoad overrides
```

### 6.2 调度输出

在 BigWorld 里，这些信号会影响：

```
  - loadBalance
  - metaLoadBalance
  - updateRanges
  - shouldOffload
  - retire underloaded cell
  - login overload status
```

这表明 profiler 和 lifecycle / login / topology 是连通的，不是孤立模块。

---

## 7. overloadCheck 不只是告警

BigWorld 的 `overloadCheck` 还有一个设计重点：

```
过载要满足持续时间阈值
然后才升级为系统级状态
```

也就是：

```
instant overload
  != admitted overload
```

这条边界很重要，因为它避免了：

```
单个尖峰 tick
  → 立刻触发限流 / 扩容 / 退役
```

theseed 应显式写入：

```
系统调度信号必须带持续时间判定
```

---

## 8. theseed 的建议建模

### 8.1 运行时 profiler

```cpp
// runtime/IEntityLoadProfiler.h

struct EntityLoadSnapshot {
    EntityId entityId;
    EntityTypeId entityTypeId;
    float rawLoad = 0.0f;
    float smoothedLoad = 0.0f;
    float adjustedLoad = 0.0f;
    float artificialMinLoad = 0.0f;
};

class IEntityLoadProfiler {
public:
    virtual ~IEntityLoadProfiler() = default;

    virtual void startScope(EntityId entityId) = 0;
    virtual void stopScope(EntityId entityId) = 0;
    virtual EntityLoadSnapshot tick(EntityId entityId,
                                    Duration tickDt) = 0;
};
```

### 8.2 类型级聚合

```cpp
// runtime/IEntityTypeLoadAggregator.h

struct EntityTypeLoadSnapshot {
    EntityTypeId entityTypeId;
    uint32_t entityCount = 0;
    float rawLoad = 0.0f;
    float smoothedLoad = 0.0f;
    float maxRawLoad = 0.0f;
};

class IEntityTypeLoadAggregator {
public:
    virtual ~IEntityTypeLoadAggregator() = default;

    virtual void record(const EntityLoadSnapshot& snapshot) = 0;
    virtual EntityTypeLoadSnapshot snapshot(EntityTypeId typeId) const = 0;
};
```

### 8.3 调度反馈接口

```cpp
// runtime/ILoadFeedbackController.h

struct RuntimeLoadStatus {
    float minLoad = 0.0f;
    float avgLoad = 0.0f;
    float maxLoad = 0.0f;
    bool anyOverloaded = false;
    bool avgOverloaded = false;
};

class ILoadFeedbackController {
public:
    virtual ~ILoadFeedbackController() = default;

    virtual void onLoadSample(const RuntimeLoadStatus& status) = 0;
    virtual void setArtificialMinLoad(SpaceId spaceId, float load) = 0;
    virtual void setShouldOffload(SpaceId spaceId, bool value) = 0;
};
```

---

## 9. 必须拆开的三层

这条链里必须区分三层：

| 层 | 问题 | 典型输出 |
|------|------|----------|
| Telemetry | 让人看到发生了什么 | metrics / trace / logs |
| Diagnostics | 让人定位谁重 | entity / type / space profiler |
| Control Feedback | 让系统据此调整 | offload / balance / throttle |

如果混成一层，会出现：

```
1. OTel 文档看起来像已覆盖了 BigWorld profiler
2. 实际却没有定义 profiler 如何影响调度
3. 运维看到指标，但系统本身没有反馈回路
```

---

## 10. 与 Login / Lifecycle / Topology 的关系

### 10.1 与 Login Service

```
overload admitted
  → 更新 login gate 状态
  → 触发限流或暂停新登录
```

### 10.2 与 Cluster Lifecycle

```
underloaded group
  → 候选 retire

overloaded group
  → 候选 add cell / rebalance
```

### 10.3 与 Topology

```
space / cell load
  → updateRanges / loadBalance
```

也就是说，这篇其实补的是 BigWorld 的“系统内反馈闭环”。

---

## 11. 分阶段边界

```
MVP：
  - 只保留基础 tick / process profiler
  - 不承诺 entity / type / space 的系统性反馈闭环

Phase 2：
  - 引入 entity / entity-type load profiler
  - 引入 overload duration 判定
  - 与 login overload gate、基础 offload 联动

Phase 3：
  - 引入 space artificialMinLoad
  - 与 loadBalance / updateRanges / retire / rollout 联动
  - 形成完整的 runtime load feedback loop
```

---

## 12. 与 BigWorld / KBEngine / theseed 的对比

| 维度 | BigWorld | KBEngine | theseed |
|------|---------|---------|---------|
| EntityProfiler | 有 | 弱 | Phase 2 目标 |
| EntityTypeProfiler | 有 | 弱 | Phase 2 目标 |
| artificialMinLoad | 有 | 基本无 | 明确建模 |
| overload duration gate | 有 | 弱 | 应显式建模 |
| profiler → balance feedback | 有 | 弱 | 当前缺失，本篇补齐 |
