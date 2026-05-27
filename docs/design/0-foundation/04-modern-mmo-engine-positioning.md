# Modern MMO Engine Positioning — 现代 MMO 引擎定位

> 本篇用于统一 theseed 的顶层取舍：BigWorld 是源头设计，KBEngine 是参考实现之一，theseed 的目标是整合两者优点并现代化落地。

---

## 1. 总判断

theseed 应被定位为：

```
以 BigWorld 的系统级 MMO 服务端架构为源头，
以 KBEngine 的轻量运行时落地为参考，
用现代 C++、现代运维、现代数据与工具链重新组织的分布式游戏服务器引擎。
```

这意味着：

```
BigWorld 决定系统边界的上限。
KBEngine 帮助验证最小运行时闭环。
theseed 负责现代化、工程化和阶段化落地。
```

---

## 2. 三个参考层次

### 2.1 BigWorld：源头设计

BigWorld 的价值不只是 Base / Cell / AOI 这些名词，而是完整系统面：

```
  - Runtime Data Plane: Mailbox / Mercury / Channel / Bundle
  - Replication & Space: Space / Cell / Witness / Ghost / EntityCache
  - Cluster & Availability: BackupHash / retire / controlled shutdown
  - Data & Ops: SecondaryDB / transfer / consolidate / sync / repair
  - Control & Diagnostics: Watcher / profiler / status / load feedback
  - World Framework: chunk / geometry mapping / compiled space
```

这些能力共同构成“大型 MMO 服务端引擎”的系统骨架。

### 2.2 KBEngine：参考实现之一

KBEngine 的价值在于：

```
  - 更容易阅读的运行时主干
  - 更轻的 Base / Cell / EntityCall / Witness / Ghost 路径
  - 可作为 MVP 运行链路的现实参照
```

但 KBEngine 不应成为 theseed 的能力上限：

```
  - 系统级 HA 较弱
  - 数据运维工具链较薄
  - load feedback 与调度闭环不足
  - 运维控制面不如 BigWorld 成体系
```

### 2.3 theseed：现代化整合

theseed 需要把旧引擎中正确的系统语义重新组织为现代工程结构：

```
  - C++23 / CMake / vcpkg 的现代构建基线
  - Runtime Data Plane / Control Plane / Cross-Realm Async Plane 的明确分离
  - 结构化日志、metrics、trace 与 Ops Control Plane 分层
  - 属性级存储映射 + JSON / 多后端 / schema diff 的现代数据能力
  - 代码生成优先的客户端 SDK 与协议约束
  - 受限热更、滚动发布、审计、回滚的工程化发布边界
```

---

## 3. 取舍原则

### 3.1 保留 BigWorld 的系统状态对象

现代化不等于抹掉 BigWorld 的核心状态对象。theseed 应优先保留这些语义：

| BigWorld 概念 | theseed 需要保留的语义 |
|------|------|
| Channel | 连接状态、背压、inactivity、可靠性上下文 |
| Bundle | tick 内消息聚合、可靠性标记、flush 边界 |
| RealEntity / Haunt | real 权威、ghost 目标、控制权、迁移/offload 状态 |
| Witness / EntityCache | 每个观察关系的可见性、detail level、priority、bandwidth budget |
| EntityGhostMaintainer | ghost 集合维护与跨 Cell 边界一致性 |
| BackupHash / BackupHashChain | 备份责任路由、priming、ack、promote、恢复权威 |
| EntityProfiler / EntityTypeProfiler | 负载信号进入调度闭环，而不是只做展示指标 |

这些对象不必按旧代码形态复制，但它们承载的运行时语义不能丢。

### 3.2 采用 KBEngine 的最小闭环路径

MVP 应继续保持轻量：

```
  - 单 Realm
  - 单线程 tick
  - SingleCell Space
  - Base / Cell 双体
  - EntityCall 主路径
  - Witness 同步
  - Ghost / migration 的最小路由窗口
```

但这个轻量路径只是起点，不是最终边界。

### 3.3 用现代化拆分旧引擎的混层

旧引擎里很多能力是正确的，但实现上常混在一起。theseed 应显式拆开：

| 不应混合的内容 | theseed 分层 |
|------|------|
| 实体主路径消息与运维命令 | Runtime Data Plane / Ops Control Plane |
| metrics 展示与控制命令 | Telemetry / Ops Control Plane |
| profiler 采样与调度决策 | Diagnostics / Load Feedback |
| 可见性、更新优先级、payload 编码 | AOI / Priority / Property Replication |
| 存储主路径与离线数据运维 | Persistence / Data Ops Toolchain |

---

## 4. 现代 MMO 引擎的核心链路

theseed 的长期架构应围绕以下链路组织：

```
Login / Gateway
  → Base Entity
  → Cell Entity
  → Space / AOI
  → Witness / Ghost
  → Runtime Transport
  → Client SDK
```

并且同步建设系统链路：

```
Runtime Profiler
  → Load Feedback
  → Topology / Offload / Throttle
  → Ops Control Plane
```

以及可用性链路：

```
Primary Entity
  → Backup Route
  → Backup Snapshot
  → HA Coordinator
  → Restore / Promote
```

这三条链路分别对应：

```
玩家在线主路径
系统调度反馈路径
高可用恢复路径
```

任何文档或实现如果只覆盖第一条链，都还不能称为完整 MMO 引擎。

---

## 5. 分阶段目标

### Phase 1：现代化运行时内核

```
  - Base / Cell / Tick / EntityDef
  - Runtime Data Plane
  - SingleCell Space
  - AOI / Witness
  - Property Replication
  - Gateway MVP
  - MySQL MVP
  - Unity SDK MVP
```

目标是得到一个能稳定运行的单 Realm 分布式游戏服务器内核。

### Phase 2：系统面补齐

```
  - Static Partition
  - 更完整的 Channel / Bundle 状态机
  - EntityCache / witness priority
  - Entity / EntityType load profiler
  - Ops Control Plane
  - BackupTopologyCoordinator 原型
  - Data Ops Toolchain 原型
```

目标是从“能跑”进入“可运维、可扩展、可调度”。

### Phase 3：BigWorld 级系统能力现代化

```
  - BSP grow / shrink
  - Offload / retire / controlled shutdown
  - BackupHash / restore authority 闭环
  - load bounds / chunk-aware balance
  - SecondaryDB / local archive / consolidate
  - 多 Realm / Cross-Realm Async Plane
```

目标是形成真正面向大型 MMO 的系统级引擎能力。

---

## 6. 一句话判断

```
theseed 不是 KBEngine 的重写版，
也不是 BigWorld 的逐项搬运。

theseed 应是以 BigWorld 为源头、以 KBEngine 为参考之一、
用现代工程方式重新设计和落地的 MMO 服务端引擎。
```
