# theseed Engine — 设计文档总览

> 继承 BigWorld / KBEngine 架构思想，面向千台集群的现代游戏服务器引擎。

---

## 文档分层

```
┌─────────────────────────────────────────────────────────────────────┐
│  0-foundation  设计基线 — MVP 范围、边界约束、实现优先级             │
├─────────────────────────────────────────────────────────────────────┤
│  8-reference   参考溯源 — 设计决策来源与对比                         │
├─────────────────────────────────────────────────────────────────────┤
│  7-observability 可观测性 — OTel 遥测、调试、运维控制面                │
├─────────────────────────────────────────────────────────────────────┤
│  6-client       客户端 SDK — Unity / UE5 / Cocos 代码生成与同步     │
├─────────────────────────────────────────────────────────────────────┤
│  5-scripting    脚本层 — 安全防护、热更新                           │
├─────────────────────────────────────────────────────────────────────┤
│  4-gameplay     玩法支撑 — 物理、导航、控制器、场景、内置实体        │
│                  生命周期钩子、脚本绑定                              │
├─────────────────────────────────────────────────────────────────────┤
│  3-infrastructure 基础设施 — 网关、登录服务、消息总线、生命周期等     │
├─────────────────────────────────────────────────────────────────────┤
│  2-data         数据层 — 定义规范、持久化、合服                     │
├─────────────────────────────────────────────────────────────────────┤
│  1-core         核心运行时 — Tick、Entity、AOI、Ghost、通信、       │
│                  同步、迁移、定时器、容错                            │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 文档索引

### 0-foundation 设计基线

| 文档 | 内容 |
|------|------|
| [01-mvp-architecture-baseline](0-foundation/01-mvp-architecture-baseline.md) | MVP 范围、运行时边界、同步时序、迁移与热更约束 |

### 1-core 核心运行时

| 文档 | 内容 |
|------|------|
| [01-tick-model](1-core/01-tick-model.md) | 单线程 Tick 模型、5 阶段生命周期、TickScheduler |
| [02-entity-system](1-core/02-entity-system.md) | Base/Cell 双体模型、PropertyBlock 连续内存、状态机、对象池 |
| [03-aoi-and-space](1-core/03-aoi-and-space.md) | 十字链表 AOI、可插拔方案 (MMO/Action/Room)、动态 Space 拓扑 |
| [04-ghost-and-witness](1-core/04-ghost-and-witness.md) | Witness 视野同步、Ghost Real/Ghost 权威模型 |
| [05-communication](1-core/05-communication.md) | EntityCall 跨进程通信、Aeron 传输层、消息可靠性分级 |
| [06-property-sync](1-core/06-property-sync.md) | 4 条同步路径、DirtyMask 位图、Bundle 带宽优化 |
| [07-entity-migration](1-core/07-entity-migration.md) | 4 种迁移场景、5 阶段迁移流、零消息丢失路由窗口 |
| [08-timer-and-pool](1-core/08-timer-and-pool.md) | 4 层哈希时间轮、O(1) 增删 |
| [09-fault-tolerance](1-core/09-fault-tolerance.md) | 3 级容错、SecondaryDB 边界、恢复分层 |
| [10-backup-hash-and-ha](1-core/10-backup-hash-and-ha.md) | BackupHash、恢复链、热备切换权威 |
| [11-aoi-update-schemes-and-load-bounds](1-core/11-aoi-update-schemes-and-load-bounds.md) | AoI 更新优先级曲线、Load Bounds、Range Update |
| [12-bsp-rebalance-and-offload](1-core/12-bsp-rebalance-and-offload.md) | BSP grow/shrink、chunk-aware balance、offload |

### 2-data 数据层

| 文档 | 内容 |
|------|------|
| [01-data-definition](2-data/01-data-definition.md) | XML + XSD 定义规范、存储策略、列类型映射 |
| [02-persistence](2-data/02-persistence.md) | 主持久化、查询接口拆分、本地归档边界 |
| [03-server-merge](2-data/03-server-merge.md) | 合服范围、冲突修复、CLI 工具 |
| [04-secondary-db](2-data/04-secondary-db.md) | SecondaryDB、本地归档暂存、数据运维边界 |
| [05-data-ops-toolchain](2-data/05-data-ops-toolchain.md) | Snapshot、Transfer、Consolidate、Sync、Repair |

### 3-infrastructure 基础设施

| 文档 | 内容 |
|------|------|
| [01-gateway](3-infrastructure/01-gateway.md) | Gateway 架构、路由、登录安全边界 |
| [02-message-bus](3-infrastructure/02-message-bus.md) | 控制面消息、IMessageBus、跨服桥接 |
| [03-redis-async-config](3-infrastructure/03-redis-async-config.md) | Redis 用例、异步框架、统一配置 |
| [04-login-service](3-infrastructure/04-login-service.md) | 登录服务、Challenge、风控、登录运维 |
| [05-cluster-lifecycle](3-infrastructure/05-cluster-lifecycle.md) | Service Fragment、Drain、Retire、Controlled Shutdown |
| [06-runtime-profiler-and-load-feedback](3-infrastructure/06-runtime-profiler-and-load-feedback.md) | EntityProfiler、负载反馈、过载判定、调度联动 |
| [07-runtime-transport-reliability](3-infrastructure/07-runtime-transport-reliability.md) | 可靠性层级、piggyback、overflow、inactivity |

### 4-gameplay 玩法支撑

| 文档 | 内容 |
|------|------|
| [01-physics](4-gameplay/01-physics.md) | 服务端物理查询、IPhysicsQuery 接口 |
| [02-navigation](4-gameplay/02-navigation.md) | INavigationSystem、NavMesh-Space 绑定 |
| [03-controllers](4-gameplay/03-controllers.md) | 移动/导航/接近/面朝控制器、ControllerManager |
| [04-space-scene](4-gameplay/04-space-scene.md) | Space 生命周期、Phase 边界、ISpaceTopology |
| [05-built-in-entities](4-gameplay/05-built-in-entities.md) | 掉落物/投射物/怪物/NPC/触发器/载具等内置实体 |
| [06-lifecycle](4-gameplay/06-lifecycle.md) | 生命周期钩子全表、脚本绑定架构、def 驱动自动绑定 |
| [07-world-streaming-and-compiled-space](4-gameplay/07-world-streaming-and-compiled-space.md) | Chunk、Geometry Mapping、Compiled Space 边界 |

### 5-scripting 脚本层

| 文档 | 内容 |
|------|------|
| [01-security](5-scripting/01-security.md) | 源码保护、客户端三防线、运行时韧性 |
| [02-hot-update](5-scripting/02-hot-update.md) | L1-L4 分级热更、验证流程、滚动更新 |

### 6-client 客户端 SDK

| 文档 | 内容 |
|------|------|
| [01-sdk-architecture](6-client/01-sdk-architecture.md) | 代码生成、Unity/UE5/Cocos SDK、属性插值 |

### 7-observability 可观测性

| 文档 | 内容 |
|------|------|
| [01-otel-integration](7-observability/01-otel-integration.md) | OTel 遥测、Debug (DAP)、Profiling、告警 |
| [02-ops-control-plane](7-observability/02-ops-control-plane.md) | 分布式观察、在线命令、配置热改、审计 |

### 8-reference 参考

| 文档 | 内容 |
|------|------|
| [source-attribution](8-reference/source-attribution.md) | 所有设计决策来源 (BigWorld / KBEngine / theseed 原创) |

---

## 阅读建议

**实现路径** (先定边界再做模块):
0-foundation/01-mvp-architecture-baseline → 1-core/01-tick-model → 02-entity-system → 05-communication → 06-property-sync

**入门路径** (理解引擎全貌):
1-core/01-tick-model → 02-entity-system → 03-aoi-and-space → 04-ghost-and-witness → 05-communication

**业务开发路径** (写游戏逻辑):
4-gameplay/06-lifecycle → 4-gameplay/03-controllers → 2-data/01-data-definition → 5-scripting/01-security

**运维路径** (部署与监控):
7-observability/01-otel-integration → 02-ops-control-plane → 5-scripting/02-hot-update → 3-infrastructure/01-gateway
