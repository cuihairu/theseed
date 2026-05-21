# theseed Engine — 设计文档总览

> 继承 BigWorld / KBEngine 架构思想，面向千台集群的现代游戏服务器引擎。

---

## 文档分层

```
┌─────────────────────────────────────────────────────────────────────┐
│  8-reference   参考溯源 — 设计决策来源与对比                         │
├─────────────────────────────────────────────────────────────────────┤
│  7-observability 可观测性 — OTel 全链路追踪、指标、日志、告警         │
├─────────────────────────────────────────────────────────────────────┤
│  6-client       客户端 SDK — Unity / UE5 / Cocos 代码生成与同步     │
├─────────────────────────────────────────────────────────────────────┤
│  5-scripting    脚本层 — 安全防护、热更新                           │
├─────────────────────────────────────────────────────────────────────┤
│  4-gameplay     玩法支撑 — 物理、导航、控制器、场景、内置实体        │
│                  生命周期钩子、脚本绑定                              │
├─────────────────────────────────────────────────────────────────────┤
│  3-infrastructure 基础设施 — 网关、消息总线、Redis、异步框架、配置   │
├─────────────────────────────────────────────────────────────────────┤
│  2-data         数据层 — 定义规范、持久化、合服                     │
├─────────────────────────────────────────────────────────────────────┤
│  1-core         核心运行时 — Tick、Entity、AOI、Ghost、通信、       │
│                  同步、迁移、定时器、容错                            │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 文档索引

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
| [09-fault-tolerance](1-core/09-fault-tolerance.md) | 3 级容错 (Supervisor/Backup/Archiver) |

### 2-data 数据层

| 文档 | 内容 |
|------|------|
| [01-data-definition](2-data/01-data-definition.md) | XML + XSD 定义规范、存储策略、列类型映射 |
| [02-persistence](2-data/02-persistence.md) | JSON 原生支持、IStorageBackend 抽象、多后端 |
| [03-server-merge](2-data/03-server-merge.md) | 5 种冲突场景、6 阶段合服流、CLI 工具 |

### 3-infrastructure 基础设施

| 文档 | 内容 |
|------|------|
| [01-gateway](3-infrastructure/01-gateway.md) | Gateway 架构、路由、登录安全 (Challenge PoW) |
| [02-message-bus](3-infrastructure/02-message-bus.md) | Aeron 双角色、IMessageBus、跨服桥接 |
| [03-redis-async-config](3-infrastructure/03-redis-async-config.md) | Redis 用例、异步框架、统一配置 |

### 4-gameplay 玩法支撑

| 文档 | 内容 |
|------|------|
| [01-physics](4-gameplay/01-physics.md) | 服务端物理查询、IPhysicsQuery 接口 |
| [02-navigation](4-gameplay/02-navigation.md) | INavigationSystem、NavMesh-Space 绑定 |
| [03-controllers](4-gameplay/03-controllers.md) | 移动/导航/接近/面朝控制器、ControllerManager |
| [04-space-scene](4-gameplay/04-space-scene.md) | Space 生命周期、配置、ISpaceTopology |
| [05-built-in-entities](4-gameplay/05-built-in-entities.md) | 掉落物/投射物/怪物/NPC/触发器/载具等内置实体 |
| [06-lifecycle](4-gameplay/06-lifecycle.md) | 生命周期钩子全表、脚本绑定架构、def 驱动自动绑定 |

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
| [01-otel-integration](7-observability/01-otel-integration.md) | OTel 全链路追踪、Metrics、Debug (DAP)、Profiling、告警 |

### 8-reference 参考

| 文档 | 内容 |
|------|------|
| [source-attribution](8-reference/source-attribution.md) | 所有设计决策来源 (BigWorld / KBEngine / theseed 原创) |

---

## 阅读建议

**入门路径** (理解引擎全貌):
1-core/01-tick-model → 02-entity-system → 03-aoi-and-space → 04-ghost-and-witness → 05-communication

**业务开发路径** (写游戏逻辑):
4-gameplay/06-lifecycle → 4-gameplay/03-controllers → 2-data/01-data-definition → 5-scripting/01-security

**运维路径** (部署与监控):
7-observability/01-otel-integration → 5-scripting/02-hot-update → 3-infrastructure/01-gateway
