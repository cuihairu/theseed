# Source Glossary

> **English** | [简体中文](/zh/design/8-reference/glossary)

This glossary records the abstract layers found by reading the current sources, not the directory names that happen to contain them. Each entry cites the defining contract or first implementation boundary.

## Glossary

| Term | English | Definition | Evidence |
| --- | --- | --- | --- |
| 进程装配 | Process Assembly | A main-level boundary that parses process input, wires transport/store/app dependencies, and drives init → tick → stop without owning business protocols. | apps/baseapp/main.cpp:117 |
| 基础设施 | Foundation | Shared primitives for streams, routing, timers, logging, metrics, tracing, Redis-shaped state, and rate limiting; owns no game or process policy. | src/foundation/include/theseed/foundation/ChannelRouter.h:13 |
| 运行时传输 | Runtime Transport | The component-to-component invocation channel: a frame, delivery class, queue/backpressure, peer registration, TCP/in-memory pipes, and periodic pump/flush. | src/runtime/include/theseed/runtime/RuntimeTransport.h:15 |
| 调度器 | Tick Scheduler | The ordered six-phase game loop that snapshots tickables, executes queued tasks, reports duration, and exposes stop control. | src/runtime/include/theseed/runtime/TickScheduler.h:16 |
| 实体 | Entity | The runtime game object with identity/type/side, state, properties, timers, controllers, events, and base/cell relations. | src/runtime/include/theseed/runtime/Entity.h:34 |
| 实体定义 | EntityDef | The parsed XML contract for an entity: properties, exposed methods, events, inheritance, and defaults. | src/runtime/include/theseed/runtime/EntityDef.h:19 |
| 实体定义注册表 | EntityDef Registry | The load/inheritance/factory boundary that turns XML files into executable entity factories. | src/db/include/theseed/db/EntityDefRegistry.h:15 |
| 属性块 | PropertyBlock | Contiguous per-entity storage laid out from an EntityDef, with typed accessors, defaults, and serialization. | src/runtime/include/theseed/runtime/PropertyBlock.h:21 |
| AOI | Area of Interest | Cross-linked X/Z coordinate indexes and range triggers that derive entity visibility and enter/leave events. | src/runtime/include/theseed/runtime/AOI.h:71 |
| 观察者视图 | Witness | Per-observer distance bands and staged property/position deltas that turn AOI state into client updates. | src/runtime/include/theseed/runtime/Witness.h:41 |
| 幽灵 | Ghost | The real/ghost authority relation and bounded forwarding route used during entity migration. | src/runtime/include/theseed/runtime/GhostManager.h:17 |
| 实体迁移 | Entity Migration | The epoch/version-safe route table and snapshot protocol that move an entity while callers preserve old IDs. | src/runtime/include/theseed/runtime/EntityMigration.h:32 |
| 空间 | Space | The authoritative spatial topology, entity membership, and range queries; SpaceRuntime adds observers, replication, and timers. | src/runtime/include/theseed/runtime/Space.h:19 |
| Base/Cell 双体 | Base/Cell Split | The ownership split between persistent session-side Base entities and spatial Cell entities, connected by typed EntityCalls. | src/core/include/theseed/core/BaseApp.h:26 |
| 应用服务 | Application Service | A process-level use case that combines protocols, entity runtime, sessions, storage, and observability (Base, Cell, Login, Realm, or DB). | src/core/include/theseed/core/BaseApp.h:26 |
| 客户端会话 | Client Session | The framed client connection state and parser/sender; it does not interpret business results. | src/login/include/theseed/login/ClientSession.h:13 |
| 登录协议 | Login Protocol | The compact length/type/payload protocol and encoders for login, realm selection, and errors. | src/login/include/theseed/login/LoginProtocol.h:36 |
| 会话令牌 | Session Token | The stateless token format that carries and validates account/realm identity across access hops. | src/login/include/theseed/login/SessionToken.h:7 |
| 实体存储 | Entity Store | The persistence abstraction for load/save/remove/id allocation/listing, implemented by memory, file, MySQL, PostgreSQL, and a remote DB adapter. | src/db/include/theseed/db/IEntityStore.h:16 |
| 文件存储 | File Store | The filesystem-backed entity format under a root directory, with mutex-guarded ID allocation. | src/db/include/theseed/db/FileEntityStore.h:12 |
| SQL 存储后端 | SQL Store Backend | The MySQL/PostgreSQL store family sharing one binary entity payload plus SQL-specific ID/account indexes and schema creation. | src/db/include/theseed/db/MySQLEntityStore.h:28 |
| 远程存储适配器 | Remote Store Adapter | The BaseEntity-side client that hides DBApp request/response calls behind IEntityStore with timeout handling. | src/db/include/theseed/db/RemoteEntityStore.h:17 |
| Redis 形状态 | Redis-Shaped State | The provider seam for strings, sorted sets, locks, and atomic owned writes; current production wiring uses the in-memory implementation. | src/foundation/include/theseed/foundation/RedisProvider.h:28 |
| 会话登记 | Session Registry | The cross-process login-session index backed by Redis-shaped state, including list/renew/revoke views. | src/foundation/include/theseed/foundation/SessionStore.h:20 |
| 运维检查器 | Ops Inspector | The process-local read-only snapshot and renderer for health, runtime, entities, Prometheus metrics, and dashboard HTML. | src/ops/include/theseed/ops/OpsInspector.h:32 |
| 运维服务器 | Ops Server | The non-blocking HTTP/1.0 surface exposing `/metrics`, `/health`, `/inspect`, `/entities`, and dashboard assets. | src/ops/include/theseed/ops/OpsServer.h:24 |
| 主机代理 | Machine Agent | The local probe/supervisor facade that snapshots host state and executes allowed commands. | src/control/include/theseed/control/machine/MachineAgent.h:43 |
| 守护代理 | Machine Daemon | The agent's TCP RPC lifecycle: policy/role gates, audit/report/artifact sinks, session operations, OTLP export, and graceful shutdown. | src/control/include/theseed/control/machine/MachineDaemon.h:168 |
| 运维控制中心 | Ops Control Center | The in-memory node/audit/profile aggregation state machine with bounded rings and stale pruning. | src/control/include/theseed/control/ops/OpsControlCenter.h:45 |
| OTLP 导出器 | OTLP Exporter | The opt-in trace/metrics HTTP/JSON exporters installed and uninstalled with daemon lifecycle. | src/control/include/theseed/control/machine/OtlpTraceExporter.h:38 |
| 诊断策略 | Diagnostics Policy | The source allowlist and read/trigger permission model for profiler metadata and artifacts. | src/control/include/theseed/control/machine/ProfileRelay.h:9 |
| 访问角色 | Access Role | The monotonic None/ReadOnly/Operator/Admin gate applied above method-specific policy allowlists. | src/control/include/theseed/control/machine/AccessControl.h:14 |
| 审计条目 | Audit Entry | The request-scoped operation record retained as a bounded local/central ring. | src/control/include/theseed/control/machine/AuditEntry.h:18 |
| 负载剖面 | Load Profiler | Cell-side per-entity dispatch/load samples used for scheduling and operational feedback. | src/runtime/include/theseed/runtime/LoadProfiler.h:31 |
| 备份拓扑协调器 | Backup Topology Coordinator | The staged process membership/backup-route state machine with version/epoch acknowledgements. | src/runtime/include/theseed/runtime/BackupTopologyCoordinator.h:48 |
| 热更新 | Hot Update | The L1/L2 classification, validation, snapshot apply, and rollback model for config/script changes; L3/L4 are blocked. | src/scripting/include/theseed/scripting/HotUpdate.h:25 |
| 协议机器人 | Protocol Bot | The external login/base protocol state machine and ramp manager used to exercise and load the access path. | tools/bots/BotClient.h:18 |
| 代码生成器 | Code Generator | The offline EntityDef-to-C# emitter and CLI that write generated client assets. | tools/codegen/main.cpp:1 |

## Layer Map

| Layer | Responsibility | Representative modules |
| --- | --- | --- |
| LAY-001 Process Assembly | Turn CLI defaults/overrides into a wired process and run its lifecycle. | apps/baseapp/main.cpp:117 |
| LAY-002 Foundation | Provide cross-cutting state, transport queues, time, logging, metrics, and tracing primitives. | src/foundation/include/theseed/foundation/ChannelRouter.h:13 |
| LAY-003 Runtime Transport & Scheduling | Move framed invocations and drive deterministic phased ticks. | src/runtime/include/theseed/runtime/TickScheduler.h:16 |
| LAY-004 Entity & Space Runtime | Own entities, properties, spatial interest, replication, migration, and topology/load state. | src/runtime/include/theseed/runtime/CellRuntime.h:26 |
| LAY-005 Application Services | Implement process use cases above the runtime, storage, and observability seams. | src/core/include/theseed/core/BaseApp.h:26 |
| LAY-006 Persistence | Normalize file/SQL/local remote persistence behind store interfaces. | src/db/include/theseed/db/RemoteEntityStore.h:17 |
| LAY-007 Observability & Control | Separate read-only inspection, host control, aggregation, and telemetry from the game tick. | src/ops/include/theseed/ops/OpsServer.h:24 |
| LAY-008 Tooling & Verification | Consume public definitions/protocols from external codegen and bot tools. | tools/codegen/main.cpp:3 |
