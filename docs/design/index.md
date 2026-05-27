# theseed Engine — 设计文档总览

> theseed 的设计源头是 BigWorld 14.4.1 的服务端系统面与工具链思想。
> KBEngine 是重要参考实现之一，主要用于校验运行时闭环如何被轻量落地。
> 当前目标是整合 BigWorld 源头设计与 KBEngine 参考实现，形成面向现代 MMO 的可演进服务器引擎。
> 当前口径不把客户端渲染和 WorldEditor UI 细节计入必做项。

---

## 分层原则

这轮重组不再按旧的功能散列目录来读，而是按引擎系统面来读：

```
0-foundation
  引擎定位、基线、审计口径、分层原则

1-runtime-model
  tick、entity、timer、runtime memory

2-replication-and-space
  AOI、Space、Ghost、Witness、Runtime Data Plane、迁移、拓扑

3-cluster-and-availability
  fault tolerance、BackupHash、lifecycle、load feedback

4-data-and-ops
  数据定义、主持久化、本地归档、数据运维工具链、合服

5-access-and-control-plane
  gateway、login、message bus、ops control plane、telemetry/debug

6-world-and-game-framework
  physics、navigation、controllers、world streaming、lifecycle

7-scripting-and-client
  脚本安全、热更新、脚本调试、客户端 SDK

8-reference
  来源追溯与覆盖判断
```

审计口径与重组理由见：

- [04-modern-mmo-engine-positioning](0-foundation/04-modern-mmo-engine-positioning.md)
- [02-audit-scope-and-layering](0-foundation/02-audit-scope-and-layering.md)

---

## 文档索引

### 0-foundation

| 文档 | 内容 |
|------|------|
| [01-mvp-architecture-baseline](0-foundation/01-mvp-architecture-baseline.md) | MVP 范围、运行时边界、同步时序、迁移与热更约束 |
| [02-audit-scope-and-layering](0-foundation/02-audit-scope-and-layering.md) | 审计口径、BigWorld/KBEngine 对照与新分层原则 |
| [03-repository-and-build-layout](0-foundation/03-repository-and-build-layout.md) | monorepo 目录、vcpkg、C++23 工具链边界 |
| [04-modern-mmo-engine-positioning](0-foundation/04-modern-mmo-engine-positioning.md) | BigWorld 源头、KBEngine 参考、theseed 现代化整合口径 |

### 1-runtime-model

| 文档 | 内容 |
|------|------|
| [01-tick-model](1-runtime-model/01-tick-model.md) | 单线程 tick、阶段时序、主循环约束 |
| [02-entity-system](1-runtime-model/02-entity-system.md) | Base/Cell 双体模型、PropertyBlock、生命周期、对象池 |
| [03-timer-and-memory](1-runtime-model/03-timer-and-memory.md) | TimerWheel、对象池、运行时内存管理 |
| [04-io-runtime-and-backend-abstraction](1-runtime-model/04-io-runtime-and-backend-abstraction.md) | IORuntime、Reactor/Completion 后端、跨平台 I/O 抽象 |

### 2-replication-and-space

| 文档 | 内容 |
|------|------|
| [01-space-topology-and-aoi](2-replication-and-space/01-space-topology-and-aoi.md) | Space、AOI、拓扑阶段模型与配置边界 |
| [02-ghost-and-witness](2-replication-and-space/02-ghost-and-witness.md) | Witness 视野同步、Ghost real/ghost 权威模型 |
| [03-runtime-communication-and-transport](2-replication-and-space/03-runtime-communication-and-transport.md) | EntityCall、Runtime Data Plane、传输语义与可靠性边界 |
| [04-property-replication](2-replication-and-space/04-property-replication.md) | 属性复制路径、DirtyMask、同步构建与 flush |
| [05-entity-migration](2-replication-and-space/05-entity-migration.md) | 迁移场景、窗口期消息处理、状态约束 |
| [06-aoi-update-and-load-bounds](2-replication-and-space/06-aoi-update-and-load-bounds.md) | AoI 更新优先级曲线、Load Bounds、Range Update |
| [07-bsp-rebalance-and-offload](2-replication-and-space/07-bsp-rebalance-and-offload.md) | BSP grow/shrink、chunk-aware balance、offload |

### 3-cluster-and-availability

| 文档 | 内容 |
|------|------|
| [01-fault-tolerance](3-cluster-and-availability/01-fault-tolerance.md) | 三级容错、恢复分层、MVP 边界 |
| [02-backup-hash-and-ha](3-cluster-and-availability/02-backup-hash-and-ha.md) | BackupHash、恢复链、热备切换权威 |
| [03-cluster-lifecycle](3-cluster-and-availability/03-cluster-lifecycle.md) | Service Fragment、Drain、Retire、Controlled Shutdown |
| [04-runtime-profiler-and-load-feedback](3-cluster-and-availability/04-runtime-profiler-and-load-feedback.md) | EntityProfiler、负载反馈、过载判定、调度联动 |

### 4-data-and-ops

| 文档 | 内容 |
|------|------|
| [01-data-definition](4-data-and-ops/01-data-definition.md) | XML + XSD 定义规范、存储策略、列类型映射 |
| [02-persistence](4-data-and-ops/02-persistence.md) | 主持久化、查询接口拆分、在线存储主路径 |
| [03-local-archive-and-secondary-db](4-data-and-ops/03-local-archive-and-secondary-db.md) | LocalArchiveStore、SecondaryDB、本地归档暂存层 |
| [04-data-ops-toolchain](4-data-and-ops/04-data-ops-toolchain.md) | Snapshot、Transfer、Consolidate、Sync、Repair |
| [05-server-merge](4-data-and-ops/05-server-merge.md) | 合服范围、冲突修复、CLI 工具与边界 |

### 5-access-and-control-plane

| 文档 | 内容 |
|------|------|
| [01-gateway-and-login](5-access-and-control-plane/01-gateway-and-login.md) | 接入层、登录服务、Challenge 与登录运维边界 |
| [02-message-bus-and-cross-realm](5-access-and-control-plane/02-message-bus-and-cross-realm.md) | 控制面消息、跨 Realm 异步与桥接 |
| [03-redis-async-and-config](5-access-and-control-plane/03-redis-async-and-config.md) | Redis、异步任务框架、统一配置 |
| [04-ops-control-plane](5-access-and-control-plane/04-ops-control-plane.md) | 分布式观察、在线命令、配置热改、审计 |
| [05-telemetry-and-debug](5-access-and-control-plane/05-telemetry-and-debug.md) | Telemetry、Debug、Diagnostics 与 OTel 边界 |
| [06-machine-agent-and-host-ops](5-access-and-control-plane/06-machine-agent-and-host-ops.md) | machine / host ops / 节点摘要 / 资源上报 |

### 6-world-and-game-framework

| 文档 | 内容 |
|------|------|
| [01-physics](6-world-and-game-framework/01-physics.md) | 服务端物理查询、IPhysicsQuery 接口 |
| [02-navigation](6-world-and-game-framework/02-navigation.md) | INavigationSystem、NavMesh 与 Space 绑定 |
| [03-controllers](6-world-and-game-framework/03-controllers.md) | 行为控制器、ControllerManager、脚本 API |
| [04-world-streaming-and-compiled-space](6-world-and-game-framework/04-world-streaming-and-compiled-space.md) | Chunk、Geometry Mapping、Compiled Space 边界 |
| [05-built-in-entities](6-world-and-game-framework/05-built-in-entities.md) | 内建游戏对象与框架边界 |
| [06-lifecycle-and-script-binding](6-world-and-game-framework/06-lifecycle-and-script-binding.md) | 生命周期钩子、脚本绑定架构、def 驱动自动绑定 |

### 7-scripting-and-client

| 文档 | 内容 |
|------|------|
| [01-script-security](7-scripting-and-client/01-script-security.md) | 脚本层安全、源码保护、运行时韧性 |
| [02-hot-update](7-scripting-and-client/02-hot-update.md) | L1-L4 分级热更、验证流程、滚动更新 |
| [03-script-debug](7-scripting-and-client/03-script-debug.md) | 脚本断点、执行栈、Entity 上下文、yield/resume、版本一致性 |
| [04-client-sdk](7-scripting-and-client/04-client-sdk.md) | 代码生成、Unity/UE5/Cocos SDK、属性插值 |

### 8-reference

| 文档 | 内容 |
|------|------|
| [source-attribution](8-reference/source-attribution.md) | 来源追溯、覆盖判断与 BigWorld/KBEngine 对照 |

---

## 阅读建议

**运行时闭环**

`0-foundation/04-modern-mmo-engine-positioning`
→ `0-foundation/01-mvp-architecture-baseline`
→ `1-runtime-model/01-tick-model`
→ `1-runtime-model/02-entity-system`
→ `2-replication-and-space/01-space-topology-and-aoi`
→ `2-replication-and-space/03-runtime-communication-and-transport`

**BigWorld 源头系统面**

`0-foundation/04-modern-mmo-engine-positioning`
→ `3-cluster-and-availability/02-backup-hash-and-ha`
→ `4-data-and-ops/03-local-archive-and-secondary-db`
→ `4-data-and-ops/04-data-ops-toolchain`
→ `5-access-and-control-plane/01-gateway-and-login`
→ `5-access-and-control-plane/04-ops-control-plane`

**业务框架**

`6-world-and-game-framework/06-lifecycle-and-script-binding`
→ `6-world-and-game-framework/03-controllers`
→ `4-data-and-ops/01-data-definition`
→ `7-scripting-and-client/01-script-security`
→ `7-scripting-and-client/03-script-debug`

**诊断与运维**

`5-access-and-control-plane/05-telemetry-and-debug`
→ `3-cluster-and-availability/04-runtime-profiler-and-load-feedback`
→ `7-scripting-and-client/02-hot-update`
→ `7-scripting-and-client/03-script-debug`
→ `3-cluster-and-availability/03-cluster-lifecycle`

**仓库与工具链**

`0-foundation/03-repository-and-build-layout`
→ `0-foundation/01-mvp-architecture-baseline`
→ `1-runtime-model/04-io-runtime-and-backend-abstraction`

**主机运维**

`5-access-and-control-plane/06-machine-agent-and-host-ops`
→ `5-access-and-control-plane/04-ops-control-plane`
→ `5-access-and-control-plane/05-telemetry-and-debug`
