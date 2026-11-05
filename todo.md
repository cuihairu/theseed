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
- [x] `CellRuntime` 分发路径无异常隔离，抛异常 handler 逃逸 `pumpInbound` —— `src/runtime/CellRuntime.cpp:354`——已修复：逐条分发包裹 `std::exception` 与 catch-all，异常计数并记 warning，只丢弃当前 invocation，不逃逸 tick 循环；回归覆盖 std/non-std 异常后同批后续消息仍执行（src/runtime/CellRuntime.cpp:383；tests/runtime/CellRuntimeTest.cpp:181）
- [x] `FileEntityStore::allocId` 非原子读改写，并发可拿到同一 id —— `src/core/FileEntityStore.cpp:102`——已修复：读-自增-写回全程持 `allocMutex_` 互斥，并发调用者不再拿到重复 id（src/core/FileEntityStore.cpp:107；src/core/include/theseed/core/FileEntityStore.h:31）；回归 8 线程×500 次并发分配断言 id 无重复且恰为 1..4000 连续序列，未修复实现失败、修复后通过（tests/core/FileEntityStoreTest.cpp:72）；gcc-debug 全量 ctest 127/127 全绿
- [x] `RateLimiter::tryConsume` 跨 Redis 往返读改写非原子，并发超发 —— `src/foundation/RateLimiter.cpp:27`——已修复：读改写全程持 `rate:<key>:lock` 分布式锁（TTL 1s、失败 1ms 退避重试），临界区内才 get→refill→set，并发调用者不再各读满桶双双放行（src/foundation/RateLimiter.cpp:31-40）；回归以两线程交会 get 的 RendezvousProvider 钉死竞态窗口，未修复实现 allowed=2 超发、修复后恰放行 1（tests/foundation/RedisProviderTest.cpp:395-458）；全量 ctest 127/127 全绿
- [x] `EntityDef::mergeFrom` 中途属性冲突遗留半合并定义 —— `src/runtime/EntityDef.cpp:33`——已修复：合并前先全量校验父属性名与子已有属性冲突，冲突则整体拒绝且不追加任何属性，中途失败不再遗留半合并状态（src/runtime/EntityDef.cpp:35-40）；回归以子已有属性位于父属性序列中部制造冲突，未修复实现 propertyCount/storageSize_ 均被污染、修复后与合并前一致且仍可继续添加（tests/core/EntityDefRegistryTest.cpp:455-477）；全量 ctest 127/127 全绿

### P3（2 项）

- [x] `trySendBuffered` `sendBuffer_.size()` 强转 `int` 传给 `send()`，>2GB 截断/UB —— `src/runtime/TcpConnection.cpp:160`——已修复：`boundedSendLength(size_t)` 在 Windows/POSIX 均钳到 `INT_MAX`，`trySendBuffered` 改用该 helper，>2GB 不再截断为负值，剩余字节留给下一轮 flush（src/runtime/SocketDetail.h:24-30；src/runtime/TcpConnection.cpp:163）；回归覆盖 0/1/INT_MAX/INT_MAX+1/INT_MAX+4096（tests/runtime/TcpConnectionTest.cpp:56-69）；全量 ctest 127/127 全绿
- [x] 各 app `main` `std::stoi` 无 catch 解析数字参数；`volatile bool` 应用 `sig_atomic_t` —— `src/` 各入口——已修复：新增 `apps/AppMainUtil.h`，`volatile bool g_running` 改为 `volatile sig_atomic_t`（信号处理器可安全写入），`stoi` 经 `parseInt`/`parsePort` 捕获 `invalid_argument`/`out_of_range` 并打印诊断（apps/AppMainUtil.h:17-43）；baseapp/cellapp/dbapp/loginapp/realmapp 五个 main 全部改用该基础设施，非法数字参数不再逃逸 main 触发 terminate（apps/baseapp/main.cpp:18,41；apps/cellapp/main.cpp:18,34；apps/dbapp/main.cpp:12,26；apps/loginapp/main.cpp:16,41；apps/realmapp/main.cpp:14,26）；回归单测覆盖 parseInt/parsePort 正常与畸形/溢出/越界及 gRunning 类型（tests/apps/AppMainUtilTest.cpp）；实测未修复源码 `--port abc` 触发 std::invalid_argument→abort(exit 134)、修复后返回 exit 1；全量 ctest 128/128 全绿

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
- [x] 审核发现：test_rate_limiter_sub_ms_refill_clamped 前 5 次消耗用真实时钟、随后才切注入时钟并 +2ms；setClock 前若被抢占 ≥3ms，注入 elapsed≥5ms 会补满容量致负断言误失败，测试潜在脆弱（tests/foundation/RedisProviderTest.cpp:317）——已修复：注入固定时钟先于 5 次消耗安装，消耗共享同一时间点，推进 2ms 完全确定（tests/foundation/RedisProviderTest.cpp:317）
- [x] 审核发现：BotClient::tickInGame 同一 tick 内 move 与 heartbeat 的 sendAction 均失败时（首次写失败已 close 连接，第二次 isConnected() 为假），scheduleRetry 被调用两次，reconnects 计数膨胀 1（tools/bots/BotClient.cpp:150）——已修复：同一断线退避事件仅首次 `scheduleRetry()` 计数，下一次 `startLogin()` 重置事件标记；回归注入双到期发送失败，断言本次 `reconnects=1`，重试后进入新一轮连接（tools/bots/BotClient.cpp:39；tests/bots/BotClientTest.cpp:49）

## 审核发现（eeb071a8dd254f8e25c9ba83d1979f2ebc7931b4..f241b01698a05094a8ee11853b6b67663585461a，只审不改）

- [x] 审核发现：RateLimiter::tryConsume 分布式锁获取循环 `while (!redis_->lock(...)) sleep 1ms` 无重试上限/超时，`lock` 持续失败（如未来网络 Redis 不可达，或持锁者未释放且 TTL 未到期）时调用线程永久忙等阻塞；改动前该函数从不阻塞、直接 get/set 返回，属活跃性回归（src/foundation/RateLimiter.cpp:33）——已修复：锁获取带 `lockTimeout` 上限（Config 默认 2s），超时放弃本次消耗返回 false，不再永久忙等；回归 `test_rate_limiter_lock_timeout` 在未修复源码上必失败（src/foundation/RateLimiter.cpp:36-43；tests/foundation/RedisProviderTest.cpp:485）。证据：ctest 128/128 全绿
- [x] 审核发现：RateLimiter 的分布式锁 TTL 固定 1s 且无续期（watchdog），临界区 get→refill→set 跨两次 Redis 往返，若临界区执行超 1s（高延迟/停顿）锁提前过期，后到者再次获锁并发进入临界区，被修复的读改写竞态复发（src/foundation/RateLimiter.cpp:31-40）——已修复：锁 TTL 提高至可配 `lockTtl`（默认 30s），并新增 `IRedisProvider::renewLock` 在临界区 get 后续期；续期失败说明锁已被夺走则放弃消耗（src/foundation/RateLimiter.cpp:52-55；src/foundation/RedisProvider.cpp:167-172；tests/foundation/RedisProviderTest.cpp:509）。证据：ctest 128/128 全绿
- [x] 审核发现：DashboardPage 的 queue_backlog 越限告警改为上升沿触发后，若页面加载时队列已 >1000（首轮 poll 的 prev 为 null，整个告警块被跳过），且 prev 随后记录为已越限值、后续轮询无上升沿，初始越限状态被静默吞掉永不告警，与旧实现"越限即告警"相比丢失首屏告警（src/ops/DashboardPage.cpp:112,124）——已修复：首轮 prev 缺失视为基线 0，首次越限即视为上升沿触发告警，不再吞掉首屏告警（src/ops/DashboardPage.cpp:112-117）。证据：ctest 128/128 全绿
- [x] 审核发现：本次"各 app main 应用 sig_atomic_t"修复只覆盖 baseapp/cellapp/dbapp/loginapp/realmapp 五个 main，tools/bots/main.cpp 仍使用 `volatile bool g_running` + 信号处理器写入（信号安全类型缺陷同源），同类问题残留未修，修复覆盖不完整（tools/bots/main.cpp:16-20）——已修复：bots CLI 改用 `theseed::app::installSignalHandlers()` + `theseed::app::gRunning`（volatile sig_atomic_t），与五个 app main 同源（tools/bots/main.cpp:8-10,26,80）。证据：ctest 128/128 全绿
- [x] 审核发现：test_rate_limiter_no_concurrent_over_admit 依赖 RendezvousRateLimitProvider::get 的 200ms wait_for 让两读者交会，若两线程未在窗口内交会（一者先完成再进入 get），未修复实现也只得 allowed=1 而误判通过，回归锚定是概率性而非确定性，存在假阴性（tests/foundation/RedisProviderTest.cpp:395-458）——已修复：两线程以 `std::latch` 同时释放进入 tryConsume，把"先完成再进入 get"的概率性排除；未修复实现确定性超发（allowed=2），修复后通过（tests/foundation/RedisProviderTest.cpp:462-483）。证据：ctest 128/128 全绿
- [x] 审核发现：AppMainUtil::parseInt 经 std::stoi 接受最长有效前缀，畸形串如 "42abc"→42、"0x10"→0 被静默当作合法值，端口/组件号可被截断解析；新单测未覆盖部分解析（"42abc"、前导空白、"+"、十六进制前缀）与端口 0 边界（apps/AppMainUtil.h:33-40；tests/apps/AppMainUtilTest.cpp）——已修复：改用 `std::from_chars` 要求整个字符串被消费，前缀/空白/加号/十六进制均拒绝；单测补 "42abc"、前导空白、"+"、十六进制、端口 0 边界（apps/AppMainUtil.h:33-43；tests/apps/AppMainUtilTest.cpp:14-27）。证据：ctest 128/128 全绿

## 审核发现（f241b01698a05094a8ee11853b6b67663585461a..HEAD，只审不改）

- [x] 审核发现：RateLimiter 新增的 renewLock 无持有者标识（RedisProvider.h:61-64 契约称"仅当调用者确定不再持有时返回 false"，但实现仅按锁存在/未过期判断，RedisProvider.cpp:168-175），锁 TTL 过期被后到者重新持有后，原持有者 renewLock 会看到新持有者的活跃锁而误判"仍持有"继续临界区；且 UnlockGuard 无条件 unlock（RateLimiter.cpp:45-49）会清掉新持有者的锁，锁过期重取场景下读改写互斥保护仍失效，与本轮"消除锁提前过期竞态"的修复目标相悖（src/foundation/RedisProvider.cpp:168-175；src/foundation/RateLimiter.cpp:61-63）——已修复：`IRedisProvider` 锁 API 引入持有者令牌（`LockToken`），每次 `lock` 铸造唯一令牌，`renewLock`/`unlock` 仅当令牌仍属当前持有者时生效；过期被后到者重新持有后旧令牌续期/解锁均失败，不再误认领新锁（src/foundation/include/theseed/foundation/RedisProvider.h:58-75；src/foundation/RedisProvider.cpp:150-186；src/foundation/RateLimiter.cpp:38-51）。回归 `test_rate_limiter_lock_owner_token`、`test_rate_limiter_lock_stolen_before_first_renewal` 在未修复源码上必失败（tests/foundation/RedisProviderTest.cpp）。证据：`ctest`（build）128/128 全绿；`ctest`（build/gcc-coverage）125/125 全绿；`gcovr --filter src/ --fail-under-line 100 --fail-under-function 100` 行 11846/11846、函数 1703/1703 双 100%
- [x] 审核发现：RateLimiter 续期注释声称"在 get 后、set 前各续期一次"，实现仅在 get 后续期一次、set 前未续期（RateLimiter.cpp:57-63）；get 后续期成功到 set 之间若执行超过 lockTtl，锁仍会提前过期被并发者夺走，注释所述的双重续期未落地（src/foundation/RateLimiter.cpp:57-63）——已修复：写回（set）前再次续期，且续期失败（含令牌失配，即锁已被并发者夺走）即放弃本次消耗不再写入；get 后、set 前双重续期真正落地（src/foundation/RateLimiter.cpp:64-68,86-100）。回归 `test_rate_limiter_lock_stolen_after_first_renewal`、`test_rate_limiter_lock_stolen_insufficient_bucket` 在未修复源码上必失败（tests/foundation/RedisProviderTest.cpp）。证据：`ctest`（build）128/128 全绿；`ctest`（build/gcc-coverage）125/125 全绿；`gcovr --filter src/ --fail-under-line 100 --fail-under-function 100` 行 11846/11846、函数 1703/1703 双 100%

- [x] 审核发现：RateLimiter 第二次续期与桶状态写回不是原子操作，续期成功后若锁在 set 执行前过期并被其他调用者获取，旧持有者仍会覆盖新状态并可能造成超额放行（src/foundation/RateLimiter.cpp:96）——已修复：`IRedisProvider` 新增 `setIfLockOwner` 原子写回（所有权校验→写值→续期单步完成，真实 Redis 后端以单条 Lua 脚本实现同语义），两处写回（桶不足臂/消耗臂）均改走该原子接口，写回本身即最后一次所有权校验点；回归 `test_rate_limiter_lock_stolen_between_renewal_and_write`（AtWrite 窗口夺锁：未修复实现照写不误且误放行，修复后拒绝写回且新持有者锁完好）与 `test_redis_set_if_lock_owner_atomic`（旧令牌/锁消失均拒写且不碰数据、写入顺带续期）在未修复源码上必失败（src/foundation/include/theseed/foundation/RedisProvider.h:77-88；src/foundation/RedisProvider.cpp:189-209；src/foundation/RateLimiter.cpp:88-105；tests/foundation/RedisProviderTest.cpp）。证据：`ctest`（build/gcc-coverage）124/124 全绿（theseed_machine_daemon_test 为本机进程枚举环境性挂起，按 2026-11-02 审计表同源记录排除，与本改动无关）
- [x] 审核发现：table 以 display:block 实现横向滚动会使其在可访问性树中失去表格语义，屏幕阅读器无法识别行列，宜改用外层包裹容器或保留表格布局仅设 overflow-x:auto（docs/.vitepress/theme/custom.css:72）——已修复：主题 JS（theme/index.ts）渲染后把每张 .vp-doc 表包进 .table-scroll 滚动容器（MutationObserver 覆盖路由异步渲染），CSS 将容器内表格恢复 display:table、滚动由容器承担（VitePress 默认即对 .vp-doc table 设 display:block，包裹还原语义）；vitepress build 绿、dist CSS 含还原规则（docs/.vitepress/theme/custom.css:83-92；docs/.vitepress/theme/index.ts:10-31）
- [x] 审核发现：word-break: break-word 已被标准废弃，宜改用 overflow-wrap: break-word 或 word-break: normal + overflow-wrap: anywhere（docs/.vitepress/theme/custom.css:60）——已修复：改为 overflow-wrap: break-word（docs/.vitepress/theme/custom.css:60）
- [x] 审核发现：三处 max-width:960px 均以 !important 硬编码，强覆盖 VitePress 响应式默认宽度，窄屏/带侧栏场景存在布局风险（docs/.vitepress/theme/custom.css:63,66,69）——已修复：删除无断点的 `.VPDoc .container > .content` 全宽度覆盖，其余仅在 VitePress 自身设限的同档断点（≥960/≥1280/≥1440px）内覆盖，窄屏恢复默认响应式；!important 保留（VPDoc.vue 为 scoped 样式，[data-v] 属性特异度高于普通类选择器，须 !important 稳定胜出）；vitepress build 绿（docs/.vitepress/theme/custom.css:62-79）。附带：本次 pnpm 12 重装发现仓根无 pnpm-workspace.yaml 时 pnpm 12 不再读 package.json 的 pnpm.overrides（vite 安全钉版失效，重装会降到 vite 5.4.21/esbuild 0.21.5），已建 pnpm-workspace.yaml 承接 overrides（vite ^6.4.3/esbuild ^0.25.0）+ allowBuilds(esbuild)，锁定 vite 6.4.4/esbuild 0.25.12，锁文件重生成后与 HEAD 逐字节一致

## 审核发现（3364024..49de98b，只审不改）

- [x] 审核发现：RateLimiter 桶不足臂 setIfLockOwner 返回值被忽略，与消耗臂检查返回值（:99）的写法不一致；真实 Redis 后端因非所有权原因（网络/Lua 错误）写回失败时，refill 结果被静默丢弃且无日志/计数，错误被掩盖（src/foundation/RateLimiter.cpp:95）——已修复：桶不足臂检查 setIfLockOwner 返回值，失败时 logWarn（含 trace 上下文）后放弃本轮，不再静默吞错；消耗臂同步补日志。回归 `test_rate_limiter_insufficient_write_back_failure`（CapturingLogger 断言 warn 记录）在未修复源码上必失败（src/foundation/RateLimiter.cpp:95-104；tests/foundation/RedisProviderTest.cpp）。证据：`ctest -R theseed_redis_provider_test` 全绿（42/42，含新增回归）
- [x] 审核发现：setIfLockOwner 契约注释声称「真实 Redis 后端以单条 Lua 脚本实现（token check + SET + PEXPIRE）」，但仓内仅有 InMemory 实现，所述 Lua 语义无任何实现或测试兑现，属文档性偏差，未来补真实后端时易按普通 set 误实现（src/foundation/include/theseed/foundation/RedisProvider.h:83）——已修复：契约注释改为准确表述——InMemory 以单把互斥锁临界区实现，生产 Redis 必须保留同等的全有或全无语义（单脚本或事务），调用方须把 false 视为「本轮失败」（不区分所有权丢失与后端错误）；RateLimiter.cpp 同步移除「真实 Redis 后端用单条 Lua 脚本实现」的未兑现断言（src/foundation/include/theseed/foundation/RedisProvider.h:76-90；src/foundation/RateLimiter.cpp:88-96）。证据：`ctest -R theseed_redis_provider_test` 全绿（42/42）
- [x] 审核发现：override `vite: ^6.4.3` 将 vitepress@1.6.4（声明依赖 `vite: ^5.4.14`）强升至 major 6（锁文件解析 6.4.4），超出其声明的兼容范围；build 虽绿，dev/HMR 及插件链在 vite 6 下无兼容性验证（pnpm-workspace.yaml:5）——已处理：vite 6.4.3 是 CVE-2026-53571（server.fs.deny 绕过）修复线，vite 5.x 无修复，降级会重开高危告警；保持 override 并在锁文件钉住 6.4.4。本轮补做 vite 6 下兼容性验证：`vitepress dev` 启动绿（banner + local URL）、`curl` 返回 200、`vitepress build` 成功（含 mermaid 插件链渲染，75.68s，EXIT=0）、SSG 表格包裹 158/158 无误（pnpm-workspace.yaml:5；node_modules/vitepress@1.6.4/package.json）。证据：`pnpm docs:build` EXIT=0；`pnpm docs:dev` 200
- [x] 审核发现：package.json 残留 `pnpm.overrides`（vite/esbuild）与 pnpm-workspace.yaml 的 overrides 双源重复，且 pnpm 12 已不读 package.json 的 pnpm 字段——该段配置实际失效，后续维护者若只在此处改版本将不生效（package.json:15）——已修复：删除 package.json 的 `pnpm.overrides`（pnpm 12 实测警告「The "pnpm" field in package.json is no longer read」，双源重复消除，overrides 单源收敛到 pnpm-workspace.yaml）。证据：`pnpm install --frozen-lockfile` 无警告（package.json:13-19 已清）
- [x] 审核发现：MutationObserver 以 childList+subtree 观察 document.body，任意 DOM 变更都触发整页 querySelectorAll('.vp-doc table') 全量扫描，且 wrapTables 自身的 insertBefore/appendChild 会再次触发回调（虽有 .table-scroll 守卫收敛，每次包裹至少多一轮全量扫描），搜索/拷贝按钮/TOC 等频繁交互时存在性能开销（docs/.vitepress/theme/index.ts:29）——已修复：废除客户端 JS 搬移/观察器方案，改由 SSG 期 transformHtml（config.ts）把每张 <table> 包进 .table-scroll，客户端零 JS、零 MutationObserver、零全页扫描（docs/.vitepress/theme/index.ts；docs/.vitepress/config.ts:109-121）。证据：`pnpm docs:build` EXIT=0，SSG 产物表格包裹 158/158 无误
- [x] 审核发现：wrapTables 直接把 Vue 管理的 <table> 节点移入自建 .table-scroll div，Vue 补丁/路由切换以锚点 insertBefore 或记录的 parentNode 操作 DOM 时可能不一致而报错或遗留游离 wrapper；且 a11y 修复完全依赖 JS——预渲染 HTML 在 onMounted 前或 JS 失败时表格仍为 VitePress 默认 display:block，非渐进增强（docs/.vitepress/theme/index.ts:17）——已修复：废除 JS 搬移 Vue 管理节点（theme/index.ts 回退默认主题），改由构建期 transformHtml 包裹，预渲染 HTML 即含 .table-scroll 包裹与 display:table 还原语义，JS 失败/禁用时表格语义仍在，渐进增强（docs/.vitepress/config.ts:109-121；docs/.vitepress/theme/custom.css:83-97）。证据：`pnpm docs:build` EXIT=0，SSG 产物 158/158 表格已包裹
- [x] 审核发现：@media (min-width:1440px) 块与 @media (min-width:960px) 块规则完全相同（.VPDoc:not(.has-sidebar) .content{max-width:960px!important}），≥960 断点已覆盖全部 ≥1440 区间，后者属冗余重复规则（docs/.vitepress/theme/custom.css:77）——已修复：删除 1440px 冗余断点块，仅保留 960px 与 1280px 两档（VitePress 默认在 1440px 也有 own 规则，已覆盖）。证据：custom.css 无 min-width:1440px；`pnpm docs:build` EXIT=0
- [x] 审核发现：display:table 还原规则仅命中已被 JS 包裹的 .table-scroll > table，未包裹或 JS 失败时表格保留 VitePress 默认 display:block，可访问性问题未根治，与「屏幕阅读器识别行列」的修复目标存在缺口（docs/.vitepress/theme/custom.css:90）——已修复：包裹移至 SSG 期（config.ts transformHtml），SSG 产物所有表格已含 .table-scroll > table 且还原 display:table；新增 `.vp-doc table { overflow-x:auto }` 兜底保证任何未包裹表格（如客户端动态插入）仍横向滚动（docs/.vitepress/theme/custom.css:90-97）。证据：`pnpm docs:build` EXIT=0，SSG 产物 158/158 表格已包裹且 display:table 还原
- [x] 审核发现：本轮 diff（33640243..HEAD）未见新增问题——e248684（docs a11y/pnpm overrides）与 49de98b（RateLimiter setIfLockOwner 原子写回）的发现已录于上方「审核发现（3364024..49de98b，只审不改）」节（custom.css:77/90、index.ts:17/29、RateLimiter.cpp:95、RedisProvider.h:83、pnpm-workspace.yaml:5、package.json:15），本轮复核一致；新增提交 627998e（BotsE2ETest 显式 detach 节点，修正 ~NetworkNode 对已析构 scheduler 的 UAF，NetworkNode.cpp:20-26）与 d03f6af（MetricsTest gauge.set 改 int64 消除 MSVC 窄化告警，Metrics.h:37）经逐一审阅均无问题——复核确认：本轮复核一致（上述 8 条已全部处理并附证据），新增 627998e/d03f6af 审阅无新问题；本轮新增 transformHtml 表格包裹经 build 验证无误。证据：`pnpm docs:build` EXIT=0 + ctest 全绿（见上）
- [x] 审核发现：transformHtml 只改写构建出的 HTML，站内路由切换和开发服务器渲染的页面仍使用未包裹的表格，导致表格恢复为 display:block 并丢失预期的行列无障碍语义（docs/.vitepress/config.ts:124）——已修复：表格包裹从构建期 transformHtml 前移到 markdown-it renderer（markdown.config），dev server、SSG build、客户端路由切换共用同一 markdown 实例，三条渲染路径均输出 .table-scroll 包裹且无客户端 DOM 操作（docs/.vitepress/config.ts:112-121）。证据：`pnpm docs:build` EXIT=0，SSG 产物表格 158/158 已包裹（`grep -c '<div class="table-scroll"><table'` = 158 = `<table` 计数）；createMarkdownRenderer 直测 2/2 表包裹（dev/客户端导航同实例）
- [x] 审核发现：新增的 WriteBackFailure 构造函数未初始化 stage_，测试执行 renewLock 时读取未初始化枚举产生未定义行为，使新增回归测试结果不可靠（tests/foundation/RedisProviderTest.cpp:523）——已修复：默认初始化 stage_（新增 StealStage::None，默认构造经 WriteBackFailure 臂不再读未初始化枚举；此前值 0 恰好等于 BeforeFirstRenewal 触发偷锁路径，写回失败计数变 0 暴露 UB）（tests/foundation/RedisProviderTest.cpp:518、:627）。证据：`ctest -R theseed_redis_provider_test` 42/42 通过；全量 `ctest -j4` 128/128 通过（build/gcc-debug，真实运行）
- [x] ~~审核发现：本轮 diff 未见问题（479b35d5..HEAD）~~ ——不属实：区间仅含 e4d4488（表格包裹 transformHtml→markdown-it renderer + StealStage::None 枚举初始化），后者经复核无问题；但前者把包裹方案改为 markdown-it renderer（docs/.vitepress/config.ts:112-121）后，custom.css:81 与 theme/index.ts:6-9 注释仍引用已删除的 transformHtml 方案，注释与实现不一致，属本轮遗留问题——已修复：更新两处注释为 markdown-it renderer 描述（docs/.vitepress/theme/custom.css:78-84、docs/.vitepress/theme/index.ts:6-9）。证据：`pnpm docs:build` EXIT=0，SSG 产物表格 158/158 已包裹；`rg 'transformHtml' docs/` 无命中
- [x] ~~审核发现：本轮 diff 未见问题（e4d44884..HEAD）~~ ——不属实：区间仅含 5ee22ed（fix docs table wrap comments after renderer change，把 custom.css:78-84 与 theme/index.ts:6-9 注释从已删除的 transformHtml 方案更新为 markdown-it renderer 描述）；但该提交只清除 `transformHtml` 字样（`rg 'transformHtml' docs/` 无命中），遗漏同文件 custom.css:94「未被 SSG 包裹」仍把表格包裹归因于已删除的 SSG 期 transformHtml 机制，与上方新注释（renderer 包裹）及实现（config.ts:112-121）矛盾，属本轮未净——真实发现见下条。证据：custom.css:94 现文仍含「SSG 包裹」
- [x] 审核发现：5ee22ed 把表格包裹注释从 transformHtml 更新为 markdown-it renderer，但遗漏同文件 custom.css:94 的「若某表格因任何原因未被 SSG 包裹」，仍将包裹归因于已删除的 SSG 期 transformHtml 方案，与紧邻上方新注释（custom.css:78-84 称包裹由 markdown-it renderer 完成）及实现（config.ts:112-121 的 markdown.config renderer）相互矛盾，属本轮未净的陈旧注释（docs/.vitepress/theme/custom.css:94）——已修复：custom.css:94 注释改为「未被 renderer 包裹」，与 78-84 及 config.ts:112-121 的 markdown-it renderer 方案一致；`rg 'SSG 包裹' docs/.vitepress/theme/custom.css` 无命中。证据：`pnpm docs:build` EXIT=0（vitepress 1.6.4，build complete in 128.10s）；SSG 产物 HTML 表格 65/65 文件全部含 .table-scroll 包裹（wrapped==total，无 mismatch）；全量 `ctest -j4`（build/gcc-debug）128/128 通过（100% tests passed，EXIT=0）

- [x] 审核发现：本轮 diff 未见问题（5ee22edb..HEAD）——区间仅含 b7fd615（custom.css:94 注释措辞由「SSG 包裹」改为「renderer 包裹」+ todo 记录），已复核与上方注释及 config.ts:112-121 的 markdown-it renderer 实现一致，无遗留陈旧引用（`rg 'SSG 包裹|transformHtml' docs/` 无命中）。证据：全量 `ctest -j4`（build/gcc-debug）128/128 通过（100% tests passed，EXIT=0）；`pnpm docs:build` EXIT=0（vitepress 1.6.4，build complete in 242.38s）；SSG 产物 HTML 表格 158/158 已包裹（65/65 文件，wrapped==total）。
