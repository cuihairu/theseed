# theseed 架构谱系：从 BigWorld 与 KBEngine 到现代实现

> 本文回答三个问题：theseed 从 BigWorld 与 KBEngine 分别借鉴了什么、改了什么、为什么改。
> 逐系统面的详细论证见 `docs/design/` 八层设计文档（0-foundation ~ 8-reference），
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
每个模块配单元/集成测试，行/分支/函数三口径覆盖率 100%（豁免需实验论证，见
`docs/design/8-reference/coverage-report.md`）。

## 4. 对照 BigWorld 历史遗留问题的规避

| BigWorld 的坑 | theseed 的对策 | 落点 |
| --- | --- | --- |
| 核心与脚本/平台耦合重 | 纯 C++23 核心库分层（foundation→runtime→core/db/login/...），脚本层只经受限接口 | `src/` 分层与依赖方向 |
| 平台绑定（Windows 服务器工具链、专用构建） | CMake ≥3.28 + vcpkg manifest，gcc/clang/msvc 三编译器矩阵 + coverage/sanitize 树 | `CMakePresets.json` |
| 测试缺失、回归靠人工 | 115 个测试全绿为提交门槛；覆盖率三口径 100% 红线；写而未注册的测试会被 `-Wunused-function` 拦下 | `tests/`、CI、`-Werror` |
| 观测/控制/数据面互相渗透 | 三平面分离；Ops 只读面独立 HTTP 端口；控制命令与 metrics 展示分开 | `src/ops/`、架构基线文档 |
| 数据库层只支持 MySQL | `IEntityStore` 抽象 + 三后端，SQL 集成段环境变量门控可重复验证 | `src/db/` |

## 5. 当前实现状态（截至 2026-09）

- **Phase A（完成）**：TickScheduler、Entity/Base-Cell 双体、PropertyBlock、
  Base↔Cell EntityCall、SingleCell Space、Witness 同步、FileEntityStore、极简登录。
- **Phase B（进行中）**：十字链表 AOI ✅、Controller（MoveToPoint/MoveToEntity）✅、
  MySQL/PostgreSQL 持久化后端 ✅（真实实例验证）、Redis 会话/限流 ✅、
  C# 代码生成 ✅、Metrics/Logger/Tracing ✅、只读 Ops inspect ✅、
  Channel/Bundle 状态统计 ✅。
- **Phase C（已启动，待端到端集成）**：受限热更（L1/L2）、迁移路由窗口、
  BackupTopologyCoordinator、LoadProfiler。
- 进程目标：loginapp / realmapp / baseapp / cellapp / dbapp / machine（`apps/`）。
- 代码地图：`src/foundation`（基础设施）/ `src/runtime`（运行时核心）/
  `src/core`（应用骨架与 EntityDef）/ `src/db` / `src/login` / `src/realm` /
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
