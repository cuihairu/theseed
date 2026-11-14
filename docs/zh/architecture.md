# theseed 架构谱系：从 BigWorld 与 KBEngine 到现代实现

> English | **中文**

> 本文回答三个问题：theseed 从 BigWorld 与 KBEngine 分别借鉴了什么、改了什么、为什么改。
> 逐系统面的详细论证见 `docs/design/` 九层设计文档（0-foundation ~ 8-reference），
> 来源逐项追溯见 `docs/design/8-reference/source-attribution.md`。

## 1. 定位

theseed 是面向现代 MMO 的分布式游戏服务器引擎：

- **BigWorld（14.4.1）决定系统边界的上限**——服务端系统面（Space/AOI/Witness/Ghost/
  HA/数据运维/登录/控制面）以它为源头边界审计；
- **KBEngine 帮助验证最小运行时闭环**——tick/Entity/AOI/EntityCall 的轻量落地方式
  是重要参考，但不把它的简化能力当成上限；
- **theseed 负责现代化与工程化落地**——C++23、monorepo + vcpkg、三平面分离、
  测试与覆盖率红线（见 `docs/design/0-foundation/04-modern-mmo-engine-positioning.md`）。

theseed 不是 KBEngine 的重写版，也不是 BigWorld 的逐项搬运：旧引擎"能力正确但混层"，
theseed 显式拆开实体主路径与运维命令、metrics 展示与控制命令、profiler 采样与调度决策、
存储主路径与离线数据运维，各归各层。

## 2. 谱系总表

| 层 | 继承自 BigWorld | 参考自 KBEngine | theseed 的改动 |
| --- | --- | --- | --- |
| 运行时模型（tick/Entity） | Base/Cell 双体、单线程 tick 主循环 | `EventDispatcher::processUntilBreak` 式分段 tick、轻量实体落地 | PropertyBlock 连续内存替代 Python dict 属性；TimerWheel 选型；C++23 全量 |
| 复制与空间 | AOI 十字链表/RangeTrigger、Witness/RealEntity/EntityCache/Ghost/Haunt、Mailbox(EntityCall)、BSP 负载均衡 | EntityCall 单向化简、迁移路由窗口（GhostManager） | Runtime Data Plane 不走消息总线（自有判断）；迁移路由窗口语义收紧 |
| 集群与可用性 | BackupHash/BackupHashChain/BackupSender 热备责任链 | （相对弱，仅参考） | BackupTopologyCoordinator 原型 + LoadProfiler 负载反馈 |
| 数据与运维 | 属性级存储映射（15+ 种 PropertyMapping）、Archiver/SecondaryDB/consolidate_dbs 工具链、Server Merge | MySQL 主路径、5 种 TABLE_ITEM 简化映射 | file/MySQL/PostgreSQL 三后端同接口（`IEntityStore`/`IAccountStore`）；BigWorld 只支持 MySQL+XML |
| 接入与控制面 | LoginApp 登录面（Challenge/statusCheck/ban 清理）、bwmachined、Watcher/message_logger/profiler | 轻量接入路径、machine 进程管理 | Redis 会话/限流栈、只读 Ops HTTP（/metrics /health /inspect）、OTel 式 Tracing——均属新增 |
| 世界与游戏框架 | Physics2、Recast/Detour 导航、30+ 种 Controller、Compiled Space/世界流送 | 4 种 Controller 的简洁接口 | 取 BigWorld 能力边界 + KBEngine 落地方式；当前实现 MoveToPoint/MoveToEntity |
| 脚本与客户端 | L1-L4 热更分级思想、SDK digest 校验 | 混合式客户端 SDK 生成链（参考但解耦生成链与运行时绑定） | 受限 L1/L2 热更原型；现代 codegen（C# Emitter）；Merkle diff 替代单 digest |

## 3. 关键改动与理由

### 3.1 属性布局：PropertyBlock 连续内存（替代 KBEngine 的 Python dict）

KBEngine 属性存 Python dict，每次读写都经过 Python API，性能与缓存局部性差
（`docs/design/1-runtime-model/02-entity-system.md`）。theseed 按实体定义把属性
排进连续 `PropertyBlock`，配合 DirtyMask 做脏标记、按 tick 批量复制。
这是"KBEngine 验证了闭环、theseed 换掉落地形态"的典型改动。

### 3.2 可靠性语义显式化

BigWorld 的 Channel/Bundle 有 Ordered Reliable 与 Unordered Lossy 两类语义但与
传输实现耦合。theseed 在 `IRuntimeTransport`/`NetworkTransport` 上显式建模：
EntityCall/控制消息走 Ordered Reliable，volatile/AOI 广播走 Unordered Lossy，
并有 `TransportStatsCollector` 统计 Channel/Bundle 状态。

### 3.3 三平面分离（theseed 自有判断）

- **Runtime Data Plane**：实体 tick、EntityCall、属性复制——单线程 tick，不走消息总线；
- **Control Plane**：进程编排（MachineAgent/ProcessSupervisor）、Ops 只读面——HTTP/TCP 独立端口；
- **Cross-Realm Async Plane**：跨服异步（消息总线、Redis）——与运行时数据面隔离。

KBEngine 把这些混在一个进程模型里；BigWorld 能力齐全但观测/控制/调度相互渗透。
分层的动机是故障域隔离与可测试性。

### 3.4 存储抽象与多后端

`IEntityStore`/`IAccountStore` 接口 + FileEntityStore/MySQLEntityStore/
PostgreSQLEntityStore 三实现，构建期缺客户端库自动回退 file 后端
（`THESEED_HAS_MYSQL`/`THESEED_HAS_POSTGRESQL` 条件编译）。
BigWorld 只支持 MySQL(+XML)；KBEngine 以 MySQL 为主路径。存储模型统一为
"每类型一张 `tbl_<Type>` 表 + 标量原生列 + VECTOR3 展开 + 变长 JSON/BYTEA"。

### 3.5 工程基线

monorepo + vcpkg manifest + CMake presets（gcc/clang/msvc/coverage/sanitize 五树）；
C++23；`-Wall -Wextra -Wpedantic -Werror`（`THESEED_WARNINGS_AS_ERRORS=ON` 默认开）；
每个模块配单元/集成测试，行/函数覆盖率 100% 硬门、分支覆盖率信息性跟踪（不可达臂登记于
`docs/KNOWN_UNCOVERABLE_ARMS.md`；豁免需实验论证，见 `docs/design/8-reference/coverage-report.md`）。

## 4. 对照 BigWorld 历史遗留问题的规避

| BigWorld 的坑 | theseed 的对策 | 落点 |
| --- | --- | --- |
| 核心与脚本/平台耦合重 | 纯 C++23 核心库分层（foundation→runtime→core/db/login/...），脚本层只经受限接口 | `src/` 分层与依赖方向 |
| 平台绑定（Windows 服务器工具链、专用构建） | CMake ≥3.28 + vcpkg manifest，gcc/clang/msvc 三编译器矩阵 + coverage/sanitize 树 | `CMakePresets.json` |
| 测试缺失、回归靠人工 | 128 个测试全绿为提交门槛；行/函数覆盖率 100% 硬门、分支信息性跟踪；写而未注册的测试会被 `-Wunused-function` 拦下 | `tests/`、CI、`-Werror` |
| 观测/控制/数据面互相渗透 | 三平面分离；Ops 只读面独立 HTTP 端口；控制命令与 metrics 展示分开 | `src/ops/`、架构基线文档 |
| 数据库层只支持 MySQL | `IEntityStore` 抽象 + 文件 / MySQL / PostgreSQL / 远程 DB 后端（测试用内存实现），SQL 集成段环境变量门控可重复验证 | `src/db/` |

## 5. 当前实现状态（截至 2026-11）

- **Phase A（完成）**：TickScheduler、Entity/Base-Cell 双体、PropertyBlock、
  Base↔Cell EntityCall、SingleCell Space、Witness 同步、FileEntityStore、极简登录。
- **Phase B（进行中）**：十字链表 AOI ✅、Controller（MoveToPoint/MoveToEntity）✅、
  MySQL/PostgreSQL 持久化后端 ✅（真实实例验证）、Redis 会话/限流 ✅、
  C# 代码生成 ✅、Metrics/Logger/Tracing ✅、只读 Ops inspect ✅、
  Channel/Bundle 状态统计 ✅。
- **Phase C（完成）**：受限热更（L1/L2）✅、迁移路由窗口 ✅、
  BackupTopologyCoordinator ✅、LoadProfiler ✅。
- 进程目标：loginapp / realmapp / baseapp / cellapp / dbapp / machine（`apps/`）。
- 代码地图：`src/foundation`（基础设施）/ `src/runtime`（运行时核心）/
  `src/core`（应用骨架）/ `src/db`（实体存储与 EntityDef）/ `src/login` / `src/realm` /
  `src/ops` / `src/control` / `src/scripting`。

## 6. 质量红线（本仓执行标准）

1. 现代 C++：RAII、智能指针，禁裸 `new`/`delete`，const 正确；
2. 统一错误处理与日志（边界内返回 bool/optional，日志走 `foundation/Logger`）；
3. 接口有注释与清晰边界（纯接口类接受 trivial 虚析构的覆盖率豁免）；
4. 无魔数与重复代码；
5. 编译零警告：`-Wall -Wextra -Wpedantic` + `-Werror`（可经
   `THESEED_WARNINGS_AS_ERRORS=OFF` 临时关闭，默认开）；
6. 每个模块有单元测试且持续跑通；提交前全量 ctest 全绿是硬门槛；
   覆盖率 miss 要么补测试、要么带实验论证的豁免标记。

## 7. 源码抽象层

下列抽象层是读源码后归纳出的职责边界；具体目录只作为证据出现。

| 层 | 职责 | 代表文件 |
| --- | --- | --- |
| LAY-001 进程启动/装配 | 解析进程输入，组装 transport/store/app，并驱动 init → tick → stop。 | `apps/baseapp/main.cpp:117` |
| LAY-002 横向基础设施 | 持有序列化、路由、定时器、日志、指标、追踪、Redis 形状态与限流。 | `src/foundation/include/theseed/foundation/ChannelRouter.h:13` |
| LAY-003 运行时传输与调度 | 帧化并路由 `RuntimeInvocation`，管理 TCP/内存 peer，执行六个 tick 阶段。 | `src/runtime/include/theseed/runtime/TickScheduler.h:16` |
| LAY-004 实体与空间运行时 | 持有实体属性/AOI/Witness/Ghost/迁移状态，以及负载与备份拓扑。 | `src/runtime/include/theseed/runtime/CellRuntime.h:26` |
| LAY-005 应用服务 | 把协议、会话、定义、实体运行时、存储与可观测性组合成 Base/Cell/Login/Realm/DB 用例。 | `src/core/include/theseed/core/BaseApp.h:26` |
| LAY-006 持久化 | 用 `IEntityStore`/`IAccountStore` 统一 file、MySQL、PostgreSQL 和远程 DB。 | `src/db/include/theseed/db/RemoteEntityStore.h:17` |
| LAY-007 运维观测与控制 | 把只读 HTTP 检查、主机 RPC、聚合、策略/审计和 OTLP 导出从游戏 tick 中分离。 | `src/ops/include/theseed/ops/OpsServer.h:24` |
| LAY-008 工具与验证面 | 外部工具消费 XML 定义和客户端协议，完成代码生成与协议压测。 | `tools/codegen/main.cpp:3` |

### 7.1 LAY-001：进程启动/装配

- **实现位置：** `apps/baseapp/main.cpp`、`apps/cellapp/main.cpp`、`apps/dbapp/main.cpp`、`apps/loginapp/main.cpp`、`apps/machine/main.cpp`、`apps/realmapp/main.cpp`；共享信号/数字解析在 `apps/AppMainUtil.h:23`。
- **配置 schema：** main 内局部结构如 `PeerConfig`、`DbConfig`（`apps/baseapp/main.cpp:18`）填充各服务 config 结构；这些进程没有外部配置文件 schema。
- **专属协议文件：** 无；装配层只调用服务 API。
- **文案挂载：** 各 `main.cpp` 的启动/stderr 文案；共享解析诊断在 `apps/AppMainUtil.h:42`。
- **生命周期：** 安装信号 → 构造配置/依赖 → `init()`/`attach()` → 信号驱动 `tick()` 循环 → `detach()`/`stop()`（代表：`apps/baseapp/main.cpp:117`、`apps/baseapp/main.cpp:142`）。

### 7.2 LAY-002：横向基础设施

- **实现位置：** `src/foundation/*.cpp`，包括 `ChannelRouter`、`RedisProvider`、`SessionStore`、`RateLimiter`、`TimerWheel`、`MetricsRegistry`、`Logger`、`Tracing`。
- **配置 schema：** `Channel::Watermark`（`src/foundation/include/theseed/foundation/Channel.h:20`）、`RateLimiter::Config`（`src/foundation/include/theseed/foundation/RateLimiter.h:23`），以及 `RedisProvider.h` 中的类型级状态模型契约。
- **专属协议文件：** `MessageHeader` 和 `Bundle` 定义可复用消息帧原语；`src/foundation/include/theseed/foundation/Tracing.h:73` 定义 W3C 风格 carrier 注入，不是独立网络协议。
- **文案挂载：** `Logger` 产生的事件名/属性；`MetricsRegistry` 保存的指标描述。
- **生命周期：** 对象由宿主持有；`TimerWheel` 随宿主 tick 推进；`MetricsRegistry` 是进程级单例（`src/foundation/include/theseed/foundation/Metrics.h:75`）。

### 7.3 LAY-003：运行时传输与调度

- **实现位置：** `src/runtime/NetworkTransport.cpp`、`TcpConnection.cpp`、`TcpListener.cpp`、`TransportHub.cpp`、`InvocationCodec.cpp`、`RuntimeLoop.cpp`、`TickScheduler.cpp`。
- **配置 schema：** `NetworkTransport::Config`（`src/runtime/include/theseed/runtime/NetworkTransport.h:31`）、`NetworkNode::Config`、`RuntimeLoop` 调度设置，以及 `TickScheduler` 构造间隔（`src/runtime/include/theseed/runtime/TickScheduler.h:52`）。
- **专属协议文件：** `RuntimeInvocation` 与 `InvocationCodec` 是专属线协议（`src/runtime/include/theseed/runtime/InvocationCodec.h:11`）；客户端帧仍留在接入层。
- **文案挂载：** transport 统计名和调度诊断；没有直接 UI 文案。
- **生命周期：** 调用方构造 transport、连接/挂载 peer，并调用 `tick()`/`flush()`；`TickScheduler::run()`/`runOnce()` 持有阶段顺序和停止（`src/runtime/include/theseed/runtime/TickScheduler.h:77`）。

### 7.4 LAY-004：实体与空间运行时

- **实现位置：** `Entity.cpp`、`PropertyBlock.cpp`、`PropertyReplication.cpp`、`AOI.cpp`、`Witness.cpp`、`GhostManager.cpp`、`EntityMigration.cpp`、`Space.cpp`、`SpaceRuntime.cpp`、`CellRuntime.cpp`、`LoadProfiler.cpp`、`BackupTopologyCoordinator.cpp`。
- **配置 schema：** `SpaceConfig`（`src/runtime/include/theseed/runtime/Space.h:50`）、`LoadProfiler::Config`（`src/runtime/include/theseed/runtime/LoadProfiler.h:45`），以及迁移 TTL 等 `CellRuntime` 构造参数（`src/runtime/include/theseed/runtime/CellRuntime.h:30`）。
- **专属协议文件：** `EntityCall`、`InvocationCodec`、属性复制编码提供运行时方法/状态载荷。
- **文案挂载：** AOI/entity 事件携带语义事件名；本层没有本地化/UI 文案。
- **生命周期：** CellRuntime attach/detach 分阶段 tickable；Space 初始化/关闭；实体经 runtime 调用激活/销毁（`src/runtime/include/theseed/runtime/CellRuntime.h:41`）。

### 7.5 LAY-005：应用服务

- **实现位置：** `src/core/BaseApp.cpp`、`src/core/CellApp.cpp`，`src/login/LoginApp.cpp`，`src/realm/RealmApp.cpp`，`src/db/DBApp.cpp`，以及共享客户端/会话与协议模块。
- **配置 schema：** `BaseApp::Config`（`src/core/include/theseed/core/BaseApp.h:35`）、`CellApp::Config`（`src/core/include/theseed/core/CellApp.h:31`）、`LoginAppConfig`（`src/login/include/theseed/login/LoginApp.h:36`）、`RealmAppConfig`（`src/realm/include/theseed/realm/RealmApp.h:29`）、`DBApp::Config`（`src/db/include/theseed/db/DBApp.h:29`）。
- **专属协议文件：** 客户端侧 `ClientProtocol`/`LoginProtocol`；DB RPC 用 `DBProtocol`；控制通知腿复用 machine 常量。
- **文案挂载：** 协议字段/文本编码器，以及各服务选择的 dashboard/ops 文案；启动横幅仍在装配层。
- **生命周期：** 每个服务实现 `init`/`tick`/`stop`；Base/Cell 还 attach/detach tickable 并绑定/解绑回调（`src/core/BaseApp.cpp:126`、`src/login/LoginApp.cpp:95`、`src/db/DBApp.cpp:197`）。

### 7.6 LAY-006：持久化

- **实现位置：** `FileEntityStore.cpp`，`MySQLConnection.cpp`/`MySQLEntityStore.cpp`，`PostgreSQLConnection.cpp`/`PostgreSQLEntityStore.cpp`，`RemoteEntityStore.cpp`。
- **配置 schema：** `DBApp::Config` 选择后端和共享 DB 设置（`src/db/include/theseed/db/DBApp.h:29`）；后端细节是 `MySQLConnectionConfig`、`PostgreSQLConnectionConfig` 及其 store config。
- **专属协议文件：** `DBProtocol` 定义远程持久化 RPC；SQL DDL 在 store 实现内，没有独立 schema 文件。
- **文案挂载：** DB 错误串和 DBApp 启动横幅；store 不挂 dashboard 文案。
- **生命周期：** DBApp `init` 构造/初始化所选 store 并拒绝未知后端；`tick` 处理 RPC；`stop` 关闭 listener/hub（`src/db/DBApp.cpp:123`、`src/db/DBApp.cpp:172`、`src/db/DBApp.cpp:197`）。

### 7.7 LAY-007：运维观测与控制

- **实现位置：** `src/ops/OpsServer.cpp`/`OpsInspector.cpp`/`DashboardPage.cpp`，`src/control/machine/*`，`src/control/ops/OpsControlCenter.cpp`，以及 OTLP exporter。
- **配置 schema：** `OpsServer::Config`（`src/ops/include/theseed/ops/OpsServer.h:26`）、`MachineDaemon::Config` 与策略结构（`src/control/include/theseed/control/machine/MachineDaemon.h:211`）、`OpsControlCenter::Config`（`src/control/include/theseed/control/ops/OpsControlCenter.h:49`），以及 OTLP exporter 配置。
- **专属协议文件：** 复用 `RuntimeInvocation` 加 machine 方法常量（`src/control/include/theseed/control/machine/MachineDaemon.h:127`）；dashboard 标记内嵌在 `DashboardPage.cpp`。
- **文案挂载：** health/inspect JSON、dashboard HTML、machine JSON 快照、审计字段和 OTLP payload 命名。
- **生命周期：** OpsServer 随应用 start/tick/stop；MachineDaemon 在 start 注册、tick 上报、优雅 stop 注销并卸载 exporter（`src/control/machine/MachineDaemon.cpp:65`、`src/control/machine/MachineDaemon.cpp:109`）；中心通过 TTL 剪枝（`src/control/ops/OpsControlCenter.cpp:186`）。

### 7.8 LAY-008：工具与验证面

- **实现位置：** `tools/codegen/main.cpp`/`CSharpEmitter.cpp`，`tools/bots/main.cpp`/`BotsConfig.cpp`/`BotClient.cpp`/`BotManager.cpp`。
- **配置 schema：** CLI `--defs`、`--output-unity`、`--namespace`（`tools/codegen/main.cpp:35`）；`BotsConfig` 定义 key/value bot 配置契约（`tools/bots/BotsConfig.h:13`）。
- **专属协议文件：** C# emitter 消费 EntityDef；bots 复用 Login/Client 协议编码器。
- **文案挂载：** CLI usage/error、生成的 C# 头文本、bot 统计输出。
- **生命周期：** 短命 CLI 解析配置，加载定义/连接 bot，按 tick 运行到时限，最后落盘输出并 stop（`tools/codegen/main.cpp:80`、`tools/bots/main.cpp:57`）。

### 7.9 拆分理由

1. **装配层保持一次性。** main 只解析输入并接线依赖，随后调用服务生命周期 API，进程策略变化不会牵动协议（`apps/baseapp/main.cpp:117`）。
2. **先泛化传输，再叠加游戏语义。** `RuntimeInvocation` 是组件帧信封，DB 和 machine 流量共用一个 hub/投递模型（`src/runtime/include/theseed/runtime/RuntimeTransport.h:15`）。
3. **调度与工作分离。** tick 阶段名和调度契约不依赖实体；工作由宿主注册，保持 network/timer/entity/sync/flush 的确定性顺序（`src/runtime/include/theseed/runtime/TickScheduler.h:16`）。
4. **隔离可变世界状态。** Entity/PropertyBlock/AOI/Witness 状态独立于 invocation 传输和服务编排，避免跨层持有实体（`src/runtime/include/theseed/runtime/PropertyBlock.h:21`）。
5. **进程用例可扩展而传输不变。** BaseApp/CellApp/LoginApp/RealmApp/DBApp 各自组合 runtime 与协议/配置；装配层始终只是消费者（`src/core/include/theseed/core/BaseApp.h:35`）。
6. **按接口存储，延后选择后端。** `IEntityStore` 统一持久化，DBApp 在 init 时选择 file/MySQL/PostgreSQL，缺客户端库时可回退（`src/db/DBApp.cpp:123`）。
7. **观测不得阻塞 tick。** Ops HTTP 使用独立 TCP 面和非阻塞 tick；metrics 按需渲染（`src/ops/include/theseed/ops/OpsServer.h:14`）。
8. **控制授权显式且缺省关闭。** MachineDaemon 同时使用方法白名单和单调角色；未绑定调用者拒绝（`src/control/include/theseed/control/machine/AccessControl.h:14`）。
9. **遥测保持可选择的旁路。** OTLP 默认关闭并随 daemon 生命周期安装/卸载，不会悄悄成为启动依赖（`src/control/include/theseed/control/machine/OtlpTraceExporter.h:40`）。
10. **定义与协议是外部工具输入。** codegen 和 bots 消费同一 XML/客户端契约，在不链接游戏服务内部的情况下完成验证（`tools/codegen/main.cpp:68`）。
