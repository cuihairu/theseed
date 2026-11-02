# TODO

> 需求台账：每条需求一行；已完成项保留一行验收证据（提交 + 测试数）。
> 设计基线见 `docs/design/0-foundation/01-mvp-architecture-baseline.md`；实现状态速览见 `README.md`。
> 本文件是给人类看的需求清单，不是工作流水账。

## 阶段

- [x] Phase A — 现代化运行时最小闭环：TickScheduler / Entity 双体 / PropertyBlock / Base↔Cell EntityCall / SingleCell Space / Witness 同步 / FileEntityStore / 极简登录
- [x] Phase B — 在线玩法与基础运维：十字链表 AOI / Controller（MoveToPoint·MoveToEntity）/ MySQL+PostgreSQL 持久化 / Redis 会话限流 / C# codegen / Metrics·Logger·Tracing / 只读 Ops inspect / Channel·Bundle 统计
- [x] Phase C — BigWorld 级系统面原型，端到端集成：受限热更（L1/L2）/ 迁移路由窗口 / BackupTopologyCoordinator / LoadProfiler（`PhaseCIntegrationTest`，121/121 全绿，含两个既有环境性失败见下）

## 数据与运维

- [x] MySQL 持久化后端（`db/MySQLEntityStore`）——真实 MySQL 8.0.46 实例验证通过（2026-09-22）
- [x] PostgreSQL 持久化后端（`db/PostgreSQLEntityStore`）——真实实例验证通过（2026-09-22）
- [x] 存储抽象：`IEntityStore` / `IAccountStore` 接口 + File / MySQL / PostgreSQL 三后端，缺库自动回退（`THESEED_HAS_MYSQL` / `THESEED_HAS_POSTGRESQL` 条件编译）

## 控制面与遥测

- [x] Machine 核心链路：MachineAgent → HostProbe → ProcessSupervisor → snapshot（CPU/内存/网络采样 + 全主机进程枚举 + 端口占用扫描 + 二进制版本探测）
- [x] 控制面中心注册：MachineDaemon 生命周期注册/注销 + OpsControlCenter 聚合器（pruneStale 掉线摘除自洽）
- [x] 权限边界：MachineDaemon execute 双白名单策略门 + 权限分级 + 配置热改限制（04 §6 安全模型）
- [x] 受控命令面：kick session / set draining / controlled shutdown + 会话运维命令（list-sessions / kick-sessions）
- [x] 运行期韧性：登录/通知/db 三腿断链退避重连 + 订阅/查询自愈
- [x] Telemetry 三路联动：machine/ops 双侧计数与水位仪表 + 结构化审计与生命周期日志 + execute SpanScope trace 关联
- [x] /metrics Prometheus 端点导出（控制面遥测）
- [x] OTel trace 导出器：OTLP/HTTP JSON 外发（零依赖、默认关）
- [x] OTel metrics 导出器：控制面注册表快照的 OTLP/HTTP JSON 外发（零依赖、默认关）
- [x] 诊断采样：慢 tick 只读诊断切片 + 跨机回传 + 中心侧剖面查询（04 §7）
- [x] 跨平台主机探针：macOS 等价实现 + Windows 网络探针（GetIfTable2 64 位计数，`85c6a58..cbacb3d`）+ CI 矩阵补 macOS（linux×3 + coverage 门 + windows + macos）

## 质量与覆盖

- [x] 质量红线：`-Werror` 固化 + 零警告清零（`-Wall -Wextra -Wpedantic`，`THESEED_WARNINGS_AS_ERRORS=ON` 默认）+ 架构谱系文档
- [x] 覆盖率三口径门禁：行/函数 100% 硬门 + 分支信息性；分支缺口逐轮收割，不可达臂登记 `docs/KNOWN_UNCOVERABLE_ARMS.md`
- [x] 测试基线：全量 ctest 120/120 两轮并集全绿 + gcc14-gate 门禁（行 12215/12215、函数 1689/1689 双 100%）+ clang-debug 零警告（2026-10-01）

## 打开项

- [x] Phase C 端到端集成（受限热更 / 迁移路由窗口 / BackupTopologyCoordinator / LoadProfiler）
- [ ] 更完整的单元测试（跨平台 CI 矩阵已落地；测试覆盖仍开放）

## 代码深审待办（2026-11-01）

> 来源：全仓深审 `docs/review/2026-11-01-code-review.md`。P1（12 项）已当场修复并各自成 commit；
> 此处仅列 P2/P3。每项修复须带回归测试且测试在未修复源码上必失败。

### P2（12 项）

- [x] `TcpConnection::write` 无 sendBuffer 上限（对端滞止 → 无界增长）—— `src/runtime/TcpConnection.cpp:76`
- [x] `createAccount` 两阶段写非原子（主表行先落库，索引行失败/崩溃遗留孤儿 Account）—— `src/db/MySQLEntityStore.cpp:324`、`src/db/PostgreSQLEntityStore.cpp:329`
- [ ] `MySQLConnection` 双重 `mysql_init` 未先 close 泄漏首句柄 —— `src/db/MySQLConnection.cpp:141`、`:164`
- [ ] `handleListIds` 对畸形 payload 静默返回不发错误响应（调用方永久等待）—— `src/db/DBApp.cpp:305`
- [ ] `RemoteEntityStore` 按 method 字符串而非 requestId 关联响应，乱序可错配 —— `src/db/RemoteEntityStore.cpp:35`
- [ ] `MachineAgent::execute` 负数 pid 经 `from_chars` 回绕成巨大值 —— `src/control/machine/MachineAgent.cpp:74`
- [ ] `TimerWheel::cancel` 只置 cancelled 从不删除，已取消定时器累积 —— `src/foundation/TimerWheel.cpp:86`
- [ ] `RateLimiter::tryConsume` refillInterval<1ms 除零（限流器静默失效）—— `src/foundation/RateLimiter.cpp:41`
- [ ] `CellRuntime` 分发路径无异常隔离，抛异常 handler 逃逸 `pumpInbound` —— `src/runtime/CellRuntime.cpp:354`
- [ ] `FileEntityStore::allocId` 非原子读改写，并发可拿到同一 id —— `src/core/FileEntityStore.cpp:102`
- [ ] `RateLimiter::tryConsume` 跨 Redis 往返读改写非原子，并发超发 —— `src/foundation/RateLimiter.cpp:27`
- [ ] `EntityDef::mergeFrom` 中途属性冲突遗留半合并定义 —— `src/runtime/EntityDef.cpp:33`

### P3（2 项）

- [ ] `trySendBuffered` `sendBuffer_.size()` 强转 `int` 传给 `send()`，>2GB 截断/UB —— `src/runtime/TcpConnection.cpp:160`
- [ ] 各 app `main` `std::stoi` 无 catch 解析数字参数；`volatile bool` 应用 `sig_atomic_t` —— `src/` 各入口
