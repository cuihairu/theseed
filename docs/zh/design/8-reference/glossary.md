# 源码术语表

> [English](/design/8-reference/glossary) | **简体中文**

本术语表记录“读源码后归纳出的抽象层”，不是目录名复读。每条引用定义性契约或首个实现边界。

## 术语表

| 术语 | 英文 | 定义 | file:line |
| --- | --- | --- | --- |
| 进程装配 | Process Assembly | main 层边界：解析进程输入、组装 transport/store/app，并驱动 init → tick → stop，不承载业务协议。 | apps/baseapp/main.cpp:117 |
| 基础设施 | Foundation | 流、路由、定时器、日志、指标、追踪、Redis 形状态与限流的横向原语；不含游戏或进程策略。 | src/foundation/include/theseed/foundation/ChannelRouter.h:13 |
| 运行时传输 | Runtime Transport | 组件间 Invocation 通道：帧、投递语义、队列/背压、peer 注册、TCP/内存管道与周期 pump/flush。 | src/runtime/include/theseed/runtime/RuntimeTransport.h:15 |
| 调度器 | Tick Scheduler | 按六阶段排序的游戏循环：快照 tickable、执行任务、上报耗时并暴露停止控制。 | src/runtime/include/theseed/runtime/TickScheduler.h:16 |
| 实体 | Entity | 拥有 id/type/side、状态、属性、定时器、控制器、事件与 Base/Cell 关系的运行时游戏对象。 | src/runtime/include/theseed/runtime/Entity.h:34 |
| 实体定义 | EntityDef | 解析后的 XML 实体契约：属性、暴露方法、事件、继承与默认值。 | src/runtime/include/theseed/runtime/EntityDef.h:19 |
| 实体定义注册表 | EntityDef Registry | 将 XML 文件解析、继承合并并转成实体工厂的加载边界。 | src/db/include/theseed/db/EntityDefRegistry.h:15 |
| 属性块 | PropertyBlock | 按 EntityDef 布局的连续实体属性存储，含类型化访问、默认值与序列化。 | src/runtime/include/theseed/runtime/PropertyBlock.h:21 |
| AOI | Area of Interest | X/Z 十字链表坐标索引与范围触发器，用于推导实体可见性与进出事件。 | src/runtime/include/theseed/runtime/AOI.h:71 |
| 观察者视图 | Witness | 每观察者的距离分层与暂存属性/位置增量，把 AOI 状态转成客户端更新。 | src/runtime/include/theseed/runtime/Witness.h:41 |
| 幽灵 | Ghost | 实体迁移期间的真实/幽灵权威关系，以及带 TTL 的转发路由。 | src/runtime/include/theseed/runtime/GhostManager.h:17 |
| 实体迁移 | Entity Migration | 以 epoch/version 保护的迁移路由表与快照协议，使调用方在旧 ID 上继续工作。 | src/runtime/include/theseed/runtime/EntityMigration.h:32 |
| 空间 | Space | 权威空间拓扑、实体成员与范围查询；SpaceRuntime 在其上补观察者、复制与定时器。 | src/runtime/include/theseed/runtime/Space.h:19 |
| Base/Cell 双体 | Base/Cell Split | 持久会话侧 Base 实体与空间侧 Cell 实体的所有权拆分，由类型化 EntityCall 连接。 | src/core/include/theseed/core/BaseApp.h:26 |
| 应用服务 | Application Service | 把协议、实体运行时、会话、存储和可观测性组合成进程用例的服务（Base、Cell、Login、Realm、DB）。 | src/core/include/theseed/core/BaseApp.h:26 |
| 客户端会话 | Client Session | 客户端连接的帧状态与解析/发送器；不解释业务结果。 | src/login/include/theseed/login/ClientSession.h:13 |
| 登录协议 | Login Protocol | 紧凑 length/type/payload 协议，以及登录、选区、错误消息的编解码器。 | src/login/include/theseed/login/LoginProtocol.h:36 |
| 会话令牌 | Session Token | 跨接入跳携带并校验 account/realm 身份的无状态令牌格式。 | src/login/include/theseed/login/SessionToken.h:7 |
| 实体存储 | Entity Store | load/save/remove/ID 分配/列表的持久化抽象，由 memory、file、MySQL、PostgreSQL 与远程 DB 适配实现。 | src/db/include/theseed/db/IEntityStore.h:16 |
| 文件存储 | File Store | 以根目录组织的文件实体格式，ID 分配由互斥锁保护。 | src/db/include/theseed/db/FileEntityStore.h:12 |
| SQL 存储后端 | SQL Store Backend | 共享同一二进制实体载荷的 MySQL/PostgreSQL 后端族，另有 SQL 方言差异的 ID/账号索引与建表逻辑。 | src/db/include/theseed/db/MySQLEntityStore.h:28 |
| 远程存储适配器 | Remote Store Adapter | BaseEntity 侧客户端，把 DBApp 请求/应答隐藏在 IEntityStore 后，并处理超时。 | src/db/include/theseed/db/RemoteEntityStore.h:17 |
| Redis 形状态 | Redis-Shaped State | 字符串、有序集合、锁和所有权原子写的 provider 接缝；当前生产装配使用内存实现。 | src/foundation/include/theseed/foundation/RedisProvider.h:28 |
| 会话登记 | Session Registry | 由 Redis 形状态支撑的跨进程登录会话索引，含列表/续期/吊销视图。 | src/foundation/include/theseed/foundation/SessionStore.h:20 |
| 运维检查器 | Ops Inspector | 进程本地的只读快照与渲染器，覆盖 health、runtime、entities、Prometheus 与 dashboard HTML。 | src/ops/include/theseed/ops/OpsInspector.h:32 |
| 运维服务器 | Ops Server | 非阻塞 HTTP/1.0 面，暴露 `/metrics`、`/health`、`/inspect`、`/entities` 与 dashboard。 | src/ops/include/theseed/ops/OpsServer.h:24 |
| 主机代理 | Machine Agent | 本地探测/进程监督门面，输出主机快照并执行允许的命令。 | src/control/include/theseed/control/machine/MachineAgent.h:43 |
| 守护代理 | Machine Daemon | agent 的 TCP RPC 生命周期：策略/角色门、审计/上报/产物 sink、会话操作、OTLP 导出与优雅停机。 | src/control/include/theseed/control/machine/MachineDaemon.h:168 |
| 运维控制中心 | Ops Control Center | 有界环形和过期剪枝的内存节点/审计/剖面聚合状态机。 | src/control/include/theseed/control/ops/OpsControlCenter.h:45 |
| OTLP 导出器 | OTLP Exporter | 默认关闭、随 daemon 生命周期安装/卸载的 trace/metrics HTTP JSON 导出器。 | src/control/include/theseed/control/machine/OtlpTraceExporter.h:38 |
| 诊断策略 | Diagnostics Policy | 剖面元数据/产物的来源白名单与读/触发权限模型。 | src/control/include/theseed/control/machine/ProfileRelay.h:9 |
| 访问角色 | Access Role | 叠加在方法策略白名单之上的 None/ReadOnly/Operator/Admin 单调角色门。 | src/control/include/theseed/control/machine/AccessControl.h:14 |
| 审计条目 | Audit Entry | 请求关联的操作记录，以本地/中心有界环形留存。 | src/control/include/theseed/control/machine/AuditEntry.h:18 |
| 负载剖面 | Load Profiler | Cell 侧按实体采集的调度/负载样本，供调度和运维反馈使用。 | src/runtime/include/theseed/runtime/LoadProfiler.h:31 |
| 备份拓扑协调器 | Backup Topology Coordinator | 带版本/epoch 确认的进程成员与备份路由暂存状态机。 | src/runtime/include/theseed/runtime/BackupTopologyCoordinator.h:48 |
| 热更新 | Hot Update | 配置/脚本变更的 L1/L2 分类、校验、快照应用与回滚模型；L3/L4 阻断。 | src/scripting/include/theseed/scripting/HotUpdate.h:25 |
| 协议机器人 | Protocol Bot | 以外部工具身份执行登录/Base 协议状态机和斜坡加压的客户端。 | tools/bots/BotClient.h:18 |
| 代码生成器 | Code Generator | 离线把 EntityDef 生成为 C# 并落盘的 emitter 与 CLI。 | tools/codegen/main.cpp:1 |

## 分层映射

| 层 | 职责 | 代表模块 |
| --- | --- | --- |
| LAY-001 进程启动/装配 | 把 CLI 默认值/覆盖项装配成进程并驱动生命周期。 | apps/baseapp/main.cpp:117 |
| LAY-002 横向基础设施 | 提供状态、传输队列、时间、日志、指标和追踪等共用原语。 | src/foundation/include/theseed/foundation/ChannelRouter.h:13 |
| LAY-003 运行时传输与调度 | 移动帧化 Invocation 并驱动确定性分阶段 tick。 | src/runtime/include/theseed/runtime/TickScheduler.h:16 |
| LAY-004 实体与空间运行时 | 持有实体、属性、空间兴趣、复制、迁移与拓扑/负载状态。 | src/runtime/include/theseed/runtime/CellRuntime.h:26 |
| LAY-005 应用服务 | 在运行时、存储和可观测性接缝之上实现进程用例。 | src/core/include/theseed/core/BaseApp.h:26 |
| LAY-006 持久化 | 以存储接口统一 file/SQL/本地远程持久化。 | src/db/include/theseed/db/RemoteEntityStore.h:17 |
| LAY-007 运维观测与控制 | 把只读检查、主机控制、聚合和遥测从游戏 tick 中分离。 | src/ops/include/theseed/ops/OpsServer.h:24 |
| LAY-008 工具与验证面 | 由外部 codegen 和 bot 工具消费公共定义/协议。 | tools/codegen/main.cpp:3 |
