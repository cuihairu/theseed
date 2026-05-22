# Audit Scope & Layering — 审计口径与分层原则

> 本轮文档重组按 BigWorld 14.4.1 的服务端实现与服务端工具链对标，
> 同时参考 KBEngine 的运行时主干。
> 不把客户端渲染、资源编辑器 UI、WorldEditor 交互细节纳入当前必做项。

---

## 0.5 引擎实现对照与取舍

### BigWorld 是怎么实现的

```
BigWorld 的服务端实现面很厚：
  - runtime 主干
  - HA
  - 数据运维
  - 登录与控制面
  - 世界资产与工具链
```

### KBEngine 是怎么实现的

```
KBEngine 更偏向运行时主干：
  - tick / entity / AOI / witness / migration
  - 系统层和工具链相对更薄
```

### 优缺点

```
BigWorld 的优点：
  - 系统面完整
  - 能直接支撑更重的运维和运营能力

KBEngine 的优点：
  - 轻
  - 更适合作为 runtime core 起点

共同缺点：
  - 如果不先立审计口径，很容易把“长期边界”和“当前实现”混在一起
```

### theseed 的取舍

```
theseed 先按 BigWorld 的系统分层重排文档，
再用 KBEngine 的可落地 runtime 主干做 MVP 起点。
这样文档不会低估 BigWorld 差异，
也不会一次把所有复杂度塞进实现计划。
```

---

## 0. 审计口径

当前文档的审计范围固定为：

```
纳入范围：
  - BaseApp / CellApp / BaseAppMgr / CellAppMgr 一类运行时
  - LoginApp / Gateway / Challenge / 受控停服
  - AOI / Witness / Ghost / EntityCall / Migration
  - BackupHash / SecondaryDB / 数据运维工具链
  - Watcher / Profiler / 运维控制面
  - Chunk / Compiled Space 的服务端资产边界

不纳入当前必做项：
  - 客户端渲染
  - WorldEditor 的交互 UI 细节
  - 美术生产工具的完整实现
```

因此这套文档要回答的不是：

```
如何做一个完整 MMO 编辑器生态
```

而是：

```
theseed 作为分布式游戏服务端引擎，
在 BigWorld / KBEngine 的能力版图里应该覆盖哪些系统面，
哪些只是 Phase 2 / Phase 3 的边界占位。
```

---

## 1. 为什么要按系统面重组

旧目录的主要问题不是“文件名不好看”，而是抽象层次混杂：

```
旧的 runtime/core 目录
  同时混了 runtime model、replication、topology、HA

旧的 infrastructure 目录
  同时混了 access plane、cluster lifecycle、runtime feedback

旧的 gameplay 目录
  实际包含大量 world / space / chunk / server framework 内容

旧的 observability 目录
  旧目录名，已拆分为 telemetry、ops control plane、script debug 三个边界
```

这样会导致两个后果：

```
1. BigWorld 的系统级实现面被拆散，看不出完整边界
2. KBEngine 风格的运行时主干和 theseed 自己加的现代基础设施缠在一起
```

所以本轮重组采用新的判断标准：

```
不是按“功能散点”分类，
而是按“引擎系统面”分类。
```

---

## 2. 新的分层原则

### 2.1 0-foundation

负责：

```
  - MVP 基线
  - 审计范围
  - 文档分层原则
```

### 2.2 1-runtime-model

负责：

```
  - tick 模型
  - entity 基本对象模型
  - timer / object pool / runtime memory
```

这一层只回答：

```
引擎在单个 tick 线程内如何正确运行。
```

### 2.3 2-replication-and-space

负责：

```
  - AOI / Space / Topology
  - Ghost / Witness
  - Runtime Data Plane
  - Property Replication
  - Entity Migration
  - AoI Update / Load Bounds
  - BSP Rebalance / Offload
```

这一层对应 BigWorld / KBEngine 最核心的运行时分布式能力。

### 2.4 3-cluster-and-availability

负责：

```
  - fault tolerance
  - BackupHash / HA
  - service fragment / retire / controlled shutdown
  - profiler → 调度反馈闭环
```

这一层主要对标 BigWorld 真正拉开差距的系统层。

### 2.5 4-data-and-ops

负责：

```
  - data definition
  - persistence
  - local archive / secondary db
  - snapshot / transfer / consolidate / sync / repair
  - server merge
```

这一层把“在线存储主路径”和“离线数据运维工具链”明确拆开。

### 2.6 5-access-and-control-plane

负责：

```
  - gateway / login service
  - message bus / cross-realm async plane
  - redis / async / config
  - ops control plane
  - telemetry / debug / diagnostics
```

这里的重点是：

```
接入面、控制面、观测面属于系统外缘，
但它们在 BigWorld 里同样是服务端能力边界的一部分。
```

### 2.7 6-world-and-game-framework

负责：

```
  - physics / navigation
  - controller framework
  - world streaming / compiled space
  - built-in entities
  - lifecycle / script binding
```

这一层不再叫 gameplay，是因为其中有大量“世界系统”和“服务端游戏框架”。

### 2.8 7-scripting-and-client

负责：

```
  - script security
  - hot update
  - script debug
  - client sdk / codegen
```

### 2.9 8-reference

负责：

```
  - 来源追溯
  - 覆盖判断
  - BigWorld / KBEngine / theseed 的差异说明
```

---

## 3. 与 BigWorld / KBEngine 的映射

| 系统面 | BigWorld 主要来源 | KBEngine 主要来源 | theseed 文档层 |
|------|------|------|------|
| Tick / Entity / Runtime Memory | BaseApp / CellApp runtime | EventDispatcher / Entity | 1-runtime-model |
| AOI / Witness / Ghost / Migration | CellApp AOI / Ghost / Mailbox | AOI / Witness / EntityCall | 2-replication-and-space |
| BSP / Load Bounds / Offload | CellAppMgr / BSP / load balancing | 基本无完整系统 | 2-replication-and-space |
| HA / BackupHash / Retire | BaseAppMgr / BackupSender / Service Fragment | 相对弱 | 3-cluster-and-availability |
| SecondaryDB / DB Tools | DBMgr / SecondaryDB / transfer_db | 持久化主路径较简化 | 4-data-and-ops |
| Login / Watcher / Status Check | LoginApp / Watcher / message_logger | LoginApp / Watcher 简化版 | 5-access-and-control-plane |
| Space Asset / Chunk / Compiled Space | Geometry Mapping / Chunk / Compiled Space | 基本无完整等价层 | 6-world-and-game-framework |
| Script / Client | Python integration / client protocol | Python + client SDK | 7-scripting-and-client |

---

## 4. 当前覆盖判断

按 KBEngine 为标尺：

```
运行时主干已经基本进入“可设计、可落地”的状态。
```

按 BigWorld 为标尺：

```
这些文档现在不再只覆盖 runtime core，
而是把 HA、数据运维、登录运维、控制面、load feedback 这些系统边界也立住了。
```

但仍要明确分阶段：

| 能力域 | 当前状态 |
|------|------|
| Base / Cell / Tick / AOI / Ghost / Witness | 已覆盖 |
| SingleCell / Static Partition | 已覆盖 |
| BigWorld 级 BSP grow-shrink / auto rebalance | 已定义边界，非 MVP |
| 主持久化与数据定义 | 已覆盖 |
| LocalArchiveStore / SecondaryDB | 已定义边界，非 MVP |
| BackupHash / 热备恢复链 | 已定义边界，非 MVP |
| Login 运维面 / Challenge lifecycle | 已覆盖设计边界 |
| Watcher 式控制面 | 已覆盖设计边界 |
| Runtime profiler → 调度反馈闭环 | 已覆盖设计边界 |

---

## 5. 阅读顺序建议

### 5.1 先看运行时主干

```
01-mvp-architecture-baseline
  → 1-runtime-model
  → 2-replication-and-space
```

### 5.2 再看 BigWorld 系统级差异

```
3-cluster-and-availability
  → 4-data-and-ops
  → 5-access-and-control-plane
```

### 5.3 最后看游戏框架与客户端边界

```
6-world-and-game-framework
  → 7-scripting-and-client
```

---

## 6. 当前定位

这轮整理后的 theseed 文档应当被理解为：

```
以 KBEngine 风格运行时主干为可落地起点，
按 BigWorld 14.4.1 的服务端系统面补齐长期边界，
同时把 theseed 自己新增的现代化基础设施明确放回合适层级。
```

这比“Runtime Core + 一堆扩展”更接近真正可审计、可演进的引擎文档结构。
