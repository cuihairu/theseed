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
- [x] 更完整的单元测试（跨平台 CI 矩阵已落地）——bots 配置/回环 E2E、Login 协议客户端编解码 round-trip/truncation、CellRuntime 异常隔离（std/non-std）、Ops dashboard/RSS、控制面 64KB+ 快照帧回归与 RateLimiter 确定时钟；全量 ctest 126/126 全绿，gcc-coverage 行/函数 100%（11708/11708、1690/1690）

## 代码深审待办（2026-11-01）

> 来源：全仓深审 `docs/review/2026-11-01-code-review.md`。P1（12 项）已当场修复并各自成 commit；
> 此处仅列 P2/P3。每项修复须带回归测试且测试在未修复源码上必失败。

### P2（12 项）

- [x] `TcpConnection::write` 无 sendBuffer 上限（对端滞止 → 无界增长）—— `src/runtime/TcpConnection.cpp:76`
- [x] `createAccount` 两阶段写非原子（主表行先落库，索引行失败/崩溃遗留孤儿 Account）—— `src/db/MySQLEntityStore.cpp:324`、`src/db/PostgreSQLEntityStore.cpp:329`
- [x] `MySQLConnection` 双重 `mysql_init` 未先 close 泄漏首句柄 —— `src/db/MySQLConnection.cpp:141`、`:164`
- [x] `handleListIds` 对畸形 payload 静默返回不发错误响应（调用方永久等待）—— `src/db/DBApp.cpp:305`
- [x] `RemoteEntityStore` 按 method 字符串而非 requestId 关联响应，乱序可错配 —— `src/db/RemoteEntityStore.cpp:35`——已修复：request 铸造 requestId（mintRequestId）并在响应上按 requestId+method 双键匹配，超时后迟到的同 method 旧响应不再被新请求误认领（src/db/RemoteEntityStore.cpp:19-48）；DBApp::sendResponse 原样回传 inv.requestId（src/db/DBApp.cpp:490-505）；回归注入同 method 异 requestId 陈旧响应，未修复实现误认领失败、修复后丢弃（tests/db/RemoteEntityStoreTest.cpp:294-320）
- [x] ~~`MachineAgent::execute` 负数 pid 经 `from_chars` 回绕成巨大值 —— `src/control/machine/MachineAgent.cpp:74`~~ ——不属实：`std::from_chars` 对无符号整数类型拒绝前导负号（实测 "-1" 返回 `invalid_argument`、pid 保持 0、零字节消费），`execute("stop"/"restart", "-1")` 直接返回 false，无回绕；既有回归 MachineAgentTest:123/:134 断言负 pid 失败且全绿
- [x] `TimerWheel::cancel` 只置 cancelled 从不删除，已取消定时器累积 —— `src/foundation/TimerWheel.cpp:86`——已修复：cancel 记录槽位（slotLevel/slotIndex）并从所在槽摘除、立即回池复用，取消条目不再永久滞留 entries_（src/foundation/TimerWheel.cpp:24-42,86-98）；回归反复 add/cancel 100 轮，未修复实现 entries_ 增长 200、修复后封顶（tests/foundation/TimerWheelTest.cpp:106-128）
- [x] `RateLimiter::tryConsume` refillInterval<1ms 除零（限流器静默失效）—— `src/foundation/RateLimiter.cpp:41`——已修复：refill 分母钳到 1ms 下限，sub-ms 配置不再产生 inf refillRate、桶不会瞬间补满（src/foundation/RateLimiter.cpp:38-44）；回归容量 5 耗尽后等待 2ms 再试成本 5，未修复实现瞬间回满放行、修复后只补 1 token 被拒（tests/foundation/RedisProviderTest.cpp:305-328）
- [ ] `CellRuntime` 分发路径无异常隔离，抛异常 handler 逃逸 `pumpInbound` —— `src/runtime/CellRuntime.cpp:354`
- [ ] `FileEntityStore::allocId` 非原子读改写，并发可拿到同一 id —— `src/core/FileEntityStore.cpp:102`
- [ ] `RateLimiter::tryConsume` 跨 Redis 往返读改写非原子，并发超发 —— `src/foundation/RateLimiter.cpp:27`
- [ ] `EntityDef::mergeFrom` 中途属性冲突遗留半合并定义 —— `src/runtime/EntityDef.cpp:33`

### P3（2 项）

- [ ] `trySendBuffered` `sendBuffer_.size()` 强转 `int` 传给 `send()`，>2GB 截断/UB —— `src/runtime/TcpConnection.cpp:160`
- [ ] 各 app `main` `std::stoi` 无 catch 解析数字参数；`volatile bool` 应用 `sig_atomic_t` —— `src/` 各入口

## 审核发现（5123ad4058ff..HEAD，只审不改）

- [x] ~~审核发现：MySQLConnection::ping() 可在未初始化/已关闭 MYSQL 句柄上调用 mysql_ping，属 API 契约外行为（越界/UB）——构造函数已移除急切 mysql_init（:146 起）、disconnect() 不再重初始化句柄（:211），但 ping() 未检查 initialized（:535）；测试 MySQLConnectionTest.cpp:83/:108 即触发此路径（src/db/MySQLConnection.cpp:535）~~ ——不属实：ping() 首行即 `if (!impl_->connected) return false`（:536），而 `connected` 与 `initialized` 恒同生命周期（connect 成功 :205-206 同置真，disconnect/失败 :199-201/211-215 同置假），故 connected=true 必 initialized=true；`mysql_ping` 只在已初始化句柄上被调用。MySQLConnectionTest.cpp:83/:108 在未连接/已断开时返回 false，测试全绿

- [x] 审核发现：MySQL createAccount 的 P2-2 原子性未真正达成——createSchema 预建 tbl_Account 走直接 execute（:121）未填充 knownTables_ 缓存，事务内 save()→ensureTable("Account") 首次调用（缓存未命中 :140）仍发 CREATE TABLE IF NOT EXISTS DDL（:143）；按代码自身注释（MySQL DDL 隐式提交当前事务 :118-120），该 DDL 会终止事务，回滚无法撤销已提交的主表行，首个账号创建仍可能遗留孤儿（src/db/MySQLEntityStore.cpp:121）——已修复：createSchema 预建后即填 knownTables_。行为级回归（MySqlBranchTest P2-2）需带 THESEED_MYSQL_* 凭据的实例；本机 MariaDB 8 实例以 theseed 凭据现场验证：事务内 CREATE TABLE IF NOT EXISTS 隐式提交（主表行回滚后仍存活，探针 count=1），修复后 createAccount 不再补发 DDL、索引写失败后无孤儿，MySqlBranchTest 全绿
- [x] 审核发现：TcpConnection 新增 sendBuffer 上限（:81）后，ClientSession::send 忽略 pipe_->write() 返回值（:30）——对端滞止超水位（默认 8MB）时消息被静默丢弃，而非退避/断开，引入静默丢包（src/login/ClientSession.cpp:30）
- [x] 审核发现：PhaseCIntegrationTest 负载剖面断言 vacuous——仅检查 rawLoad/smoothedLoad >= 0（:119-120），负载不可能为负，故定时器恒为 0 或 Scope 未累计真实耗时也会通过，未证明 dispatch 路径确实测量了非零负载（tests/core/PhaseCIntegrationTest.cpp:119）——已修复：handler 注入确定性忙等保证 Scope 记录非零 wall-clock；断言改为严格 > 0（tests/core/PhaseCIntegrationTest.cpp:87-99,118-123）

## 审核发现（6e25c6b1..HEAD，只审不改）

- [x] ~~审核发现：ClientSession::send 改为返回 bool，但全部 4 个生产调用点均忽略返回值（LoginApp.cpp:601/709/714/760），"传播失败给上层"契约无消费方，仅测试使用；失败语义实际靠 close()+下一 tick cleanupDisconnected 收敛，未提供退避/重试选择（src/login/LoginApp.cpp:601）~~ ——不属实：send() 内部写失败已 `close()`（:33-35），tick 每轮 `cleanupDisconnected()`（:178）摘除 isConnected()=false 的会话与绑定（:763-777），失败收敛是设计内闭环；bool 返回值是给需要退避/重试的上层的可选契约，当前生产调用方无需该能力，非缺陷
- [x] 审核发现：ClientSession::send 写失败即调用 close()（ClientSession.cpp:33-35），该路径位于 TcpConnection::pump 的 recv 回调栈内（pump→onReceived_→onRawReceived→tryParseMessages→onMessage_→handleLogin→send）；close() 将 socket_ 置 0 后，pumpWithResult 的 while 循环下一轮仍以 toSocket(0)=fd0（stdin）调用 ::recv（TcpConnection.cpp:137,150-154），属错误 fd 越界读；fd0 恰为 socket（systemd/inetd 激活）时会把外部数据投喂给已关闭连接的回调（src/login/ClientSession.cpp:33；src/runtime/TcpConnection.cpp:137）——已修复：recv 循环改以 connected_ 为条件，回调返回后 close() 的副作用（socket_ 清零、connected_=false）对循环可见即停（src/runtime/TcpConnection.cpp:136）；回归测试以 socketpair 哨兵替换 fd0，未修复实现会读到哨兵并二次回调，修复后停止（tests/runtime/TcpConnectionTest.cpp:123-158）
- [x] 审核发现：knownTables_ 播种仅在 autoCreateSchema=true 时执行（MySQLEntityStore.cpp:86,135）；autoCreateSchema=false（DBApp.h:45 可配）时 createAccount 事务内 save()→ensureTable("Account") 仍发 CREATE TABLE IF NOT EXISTS DDL，依 MySQL DDL 隐式提交语义仍可能破坏两阶段写原子性，P2-2 修复未覆盖该配置（src/db/MySQLEntityStore.cpp:86,143-161）——已修复：createAccount 在 beginTransaction 前先 ensureTable("Account")，DDL 落在事务外不再隐式提交（src/db/MySQLEntityStore.cpp:350-355）；回归 MySqlBranchTest autoCreateSchema=false 用例在未修复源码上必失败（索引写失败后主表行遗留孤儿），修复后通过（tests/db/MySqlBranchTest.cpp:320-345）
- [x] 审核发现：MySqlBranchTest P2-2 回归依赖服务器严格 sql_mode（130 字符 username 超 VARCHAR(128)）且无凭据时整体跳过；非严格模式下索引 INSERT 静默截断成功，CHECK(!createAccount) 反而失败，该用例在非严格/无凭据环境不可靠，行为级验证未在 CI 落地（tests/db/MySqlBranchTest.cpp:303-317）——已修复：回归在 P2-2 与 autoCreateSchema=false 两处显式 SET SESSION sql_mode 严格模式，不再依赖服务器全局配置（tests/db/MySqlBranchTest.cpp:66-77,308,329）；非严格全局下旧用例 4 项失败、修复后全绿
- [x] 审核发现：RealmAppTest 新 send 写失败用例只覆盖 write()=false 分支（:92-107）；null/未连接 pipe（:79/:83）与成功返回 true 均未断言新返回值，send 返回值契约覆盖不全（tests/realm/RealmAppTest.cpp:75-107）——已修复：null pipe、未连接 pipe 两分支改为断言返回 false；新增已连接 InMemoryBytePipe 成功路径断言返回 true（tests/realm/RealmAppTest.cpp:73-105）
- [x] 审核发现：MySqlBranchTest:320-321 注释"新实例（knownTables_ 为空…）"已过时——init()→createSchema 现已播种 knownTables_（:135），幸福路径用例的已知表状态描述与实现不符（tests/db/MySqlBranchTest.cpp:320）——已修复：注释改为反映 createSchema 预建并播种 knownTables_、事务内 ensureTable 已短路的现状（tests/db/MySqlBranchTest.cpp:365-368）

## 审核发现（e7323b97..HEAD，只审不改）

- [x] 审核发现：TimerWheel::cancel 将条目索引回池（freeList_）后，若同一 advance 批次内另一定时器回调先 cancel 掉本批次待处理条目、再 addTimer 复用该索引，advance 循环随后再次处理被复用的索引致重复插入轮盘，周期定时器会每周期多次触发（src/foundation/TimerWheel.cpp:89）——已修复：advance 快照携带 `(index,id)`，触发前双重校验，批内复用槽不再误触发新条目；回归覆盖取消者回池并立即复用索引，验证新定时器不在同批早触发且下一批仅一次（src/foundation/TimerWheel.cpp:115；tests/foundation/TimerWheelTest.cpp:400）
- [x] 审核发现：OpsInspector::readProcessRssBytes 在非 Linux/读失败时返回 0，注释称"dashboard 显示为不可用而非伪值"，但 DashboardPage 对 0 显示 "0 B" 而非 n/a，伪值展示与注释意图不符（src/ops/OpsInspector.cpp:24）——已修复：dashboard 对 `process_rss_bytes` 缺失或 0 哨兵均显示 `n/a`，不再渲染伪值 `0 B`（src/ops/DashboardPage.cpp:106）
- [x] 审核发现：DashboardPage 每 2s 轮询在 queue_backlog>1000 期间重复 pushAlarm 同一告警，告警列表被相同消息刷屏（src/ops/DashboardPage.cpp:122）——已修复：阈值告警仅在从未越限到越限的上升沿触发，持续越限轮询不重复入列（src/ops/DashboardPage.cpp:124）
- [ ] 审核发现：test_rate_limiter_sub_ms_refill_clamped 前 5 次消耗用真实时钟、随后才切注入时钟并 +2ms；setClock 前若被抢占 ≥3ms，注入 elapsed≥5ms 会补满容量致负断言误失败，测试潜在脆弱（tests/foundation/RedisProviderTest.cpp:317）
- [ ] 审核发现：BotClient::tickInGame 同一 tick 内 move 与 heartbeat 的 sendAction 均失败时（首次写失败已 close 连接，第二次 isConnected() 为假），scheduleRetry 被调用两次，reconnects 计数膨胀 1（tools/bots/BotClient.cpp:150）
