# TODO

## 主机级非受控进程的策略化治理（Linux 起步，2026-09-26）

对齐 06-machine-agent-and-host-ops §7 MVP "process list / state / pid" 的
控制面治理切片。选型理由：枚举面已存在（supervisor 的 /proc 全主机枚举
+ managed 标记，machine.snapshot 已带全表），真实缺口是**处置侧**——
supervisor stop/restart 只对受管子进程生效，主机上其他进程无策略化出口；
且治理与 execute 的授权语义必须分离（execute 是受管进程编排，治理是
对主机其他进程的越权面，风险等级与审计语义不同，不得共享白名单）。

1. **能力面**：`IProcessSupervisor::terminateUnmanaged(pid)`（Linux
   SIGTERM；受管进程一律拒绝——唯一停止入口是 stop/restart，绕开会脱离
   reap/记账；非 Linux 平台暂不开放处置，枚举照常）；`currentProcessId()`
   跨平台 pid 助手。`IMachineAgent` 增 `enumerateHostProcesses()`（全主机
   表，不采资源摘要）与 `terminateHostProcess(pid)`（纯能力转发，策略
   判定在 daemon 侧）。
2. **策略面**（与 ExecPolicy 分离的 `ProcessGovernPolicy`）：
   trustedComponents（枚举/处置共用来源白名单）+ killableNames（处置目标
   comm 名精确匹配白名单）；双空集 = 全拒（安全缺省）。
3. **RPC 面**：machine.processes → machine.processes.ok（非受控进程 JSON
   数组，不含受管进程、不带 managed 标记——受管编排走 execute，治理视图
   不重复暴露）；machine.terminate → machine.terminate.ok（pid 十进制
   串载荷；守卫链：载荷解析 → 来源白名单 → pid 存在 → 非自身 → 非受管 →
   名单匹配；守卫与 supervisor 登记簿互为纵深）。
4. **遥测/审计面**：枚举与处置各一对接受/拒绝计数（snake_case 同族）；
   处置全程 `SpanScope("machine.terminate")`（拒绝也入 span：accepted/
   reason/pid 属性），枚举只读不进 trace；全部尝试（含拒绝）照记审计
   （command = process.list / process.kill）并推中心审计环形。
5. **测试**（MachineDaemonTest 21→24、MachineAgentTest 8→10）：三块 E2E
   ——枚举策略门（受信含自身 pid、受管被过滤、非受信拒绝、审计切片）、
   处置守卫七连（空载荷/非数字/非受信/未知 pid/自身/受管/名单外）、
   接受处置（未登记 fork sleep 经 SIGTERM 终结，waitpid 断言信号死因，
   span accepted/ok、审计带 pid）；agent 转发与 terminateUnmanaged 三态
   （未登记成功/受管拒绝/pid_max 上界外恒 ESRCH）。

验证口径：gcc-coverage 113/113 全绿、gcovr 100%（10281/10281）；clang 21
树零警告、113/113 全绿。

## Telemetry 导出面：控制面遥测经 /metrics Prometheus 端点导出（2026-09-26）

对齐 05-telemetry-and-debug 的导出层（OTel SDK/OTLP 判定过重：vcpkg 工具
链未安装，引入需全量重建 protobuf/abseil 与 preset 改造——按既定降级路径
先落只读导出面，OTel 迁移留待依赖就绪）：

1. **导出面盘点**：OpsServer 的 GET /metrics 早已接线
   `OpsInspector::renderMetrics()` → 全局 `MetricsRegistry::renderText()`
   （Prometheus 文本格式，Content-Type `text/plain; version=0.0.4`），
   HTTP 解析/内容类型/端点矩阵在 OpsServerTest 全有覆盖——生产代码无需
   改动，缺口是**控制面切片零证明**：machine_*/ops_* 指标从未被断言流经
   导出面（Inspector 测试只覆盖自定义 counter，直方图渲染无 HTTP 侧证据）。
2. **端到端测试**（MachineDaemonTest 20→21）：部署形态同进程复刻——
   OpsControlCenter（report+audit 双 sink）+ MachineDaemon + OpsServer
   同宿主，reset 注册表后从零计数：一笔 accepted（失败 pid 不留子进程）+
   一笔 rejected 驱动真实 execute 路径，HTTP GET /metrics 按
   Content-Length 读全响应，断言 `machine_execute_accepted_count 1`、
   `machine_execute_rejected_count 1`、直方图
   `_bucket{le=` / `_count 1` / `_sum`、中心水位 `ops_nodes_registered 1`
   与 `ops_audit_entries 2` 均以精确值出现在导出文本中。测试侧新增
   theseed_ops 链接（无循环依赖：ops 不依赖 control）。
3. **语义澄清**：导出面是读路径——注册表快照即时渲染，无导出侧状态；
   machine/ops 指标经既有 Registry 单例自然汇流，无需控制面新增导出
   代码（「遥测接导出面」的实质是证明汇流通路，而非再建一个端点）。

验证口径：gcc-coverage 113/113 全绿、gcovr 100%（10131/10131）；clang 21
树零警告、113/113 全绿。

## Telemetry 联动：控制面三路遥测（2026-09-26）

对齐 05-telemetry-and-debug MVP（结构化 logs + 基础 metrics + 关键 traces）
的控制面切片，与 OpsControlPlane 注册/审计闭环衔接：

1. **MachineDaemon 侧**：五计数器（snapshot/audit/execute_accepted/
   execute_rejected/unknown_method，进程级 MetricsRegistry，snake_case 同族
   口径）+ `machine_execute_duration_ms` 直方图（分桶沿用 DBApp 先例）；
   execute 全臂包 `SpanScope("machine.execute")`——拒绝也入 span
   （accepted=false + reason 属性）；接受/拒绝/未知方法/生命周期四类结构化
   日志，日志在 span 上下文内自动带 traceId/spanId（Logger 既有机制）。
2. **OpsControlCenter 侧**：`ops_nodes_registered` 水位 gauge（注册/首报/
   注销/摘除/TTL 全变异点同步）、`ops_audit_entries` 水位、
   `ops_nodes_pruned_count` 与 `ops_audit_dropped_count` 累计——审计有损
   与节点抖动成为 ops 决策信号；注册/注销/摘除/丢审计结构化日志。
3. **实现纪律**：多行调用表达式会把 gcc 覆盖计数错归因到续行（与旧
   `<<` 链问题同族）——遥测属性一律命名变量单行化。
4. **测试**（MachineDaemonTest 17→18、OpsControlCenterTest 23→24）：真实
   TCP 双请求（接受但失败 + 策略拒绝）断言计数增量、直方图采样数、span
   逐请求且属性可辨、日志与对应 span 的 traceId 关联、中心 gauge/计数增量
   与摘除/丢审计联动；指标为进程单例，全部按增量断言。

验证口径：gcc-coverage 113/113 全绿、gcovr 100%（10131/10131）；clang 21
树零警告、113/113 全绿。

## 审计对接：execute 审计汇入 OpsControlCenter 可查询审计环形（2026-09-26）

对齐 todo 遗留「与 Ops Control Plane 的注册、上报和审计对接」之审计侧
（04-ops-control-plane §8 MVP「操作审计」+ §6.2 审计要求的 MVP 映射）：

1. **AuditEntry.h 上提**（与 NodeReport.h 同法）：AuditEntry 自
   MachineDaemon.h 独立成共享头——daemon 生产与中心聚合共用同一数据形状；
   §6.2 映射口径写在头注释（operatorId ≙ source 组件 id、target ≙ 聚合侧
   nodeId、result ≙ accepted+ok；requestId 待协议层支持后补）。
2. **INodeAuditSink + NodeAuditEntry**：审计上报出口接缝（daemon 只依赖
   接口）；NodeAuditEntry = nodeId 归属 + 条目；审计是历史事实——节点
   注销/掉线摘除不清审计，存储上界由中心环形容量约束。
3. **OpsControlCenter 审计面**：实现 INodeAuditSink；Config.maxAuditEntries
   （默认 1024，0 = 关闭审计聚合）；publish 环形追加（满后丢最旧，无 nodeId
   同身份纪律丢弃）；auditTrail() / auditTrail(nodeId) 时间升序可查询。
4. **MachineDaemon 转发**：Config 增 auditSink；appendAudit 逐条推中心
   （含全部拒绝路径——拒绝不产生审计盲区）；本地环形容量与中心聚合正交
   （auditCapacity=0 只关本地视图）；auditSink-only 部署采身份汇审计流但
   不上注册簿（注册与审计互不牵动）。
5. **测试**（MachineDaemonTest 16→17、OpsControlCenterTest 19→23）：真实
   TCP execute→中心全链路（8 条含拒绝逐条汇入、nodeId 归属 = 快照
   hostname、时间升序、audit-only 不注册）、本地容量 0 不拦中心转发、
   聚合排序/逐节点过滤/环形逐出/容量 0/空身份、审计在注销与 prune 后留存。

验证口径：gcc-coverage 113/113 全绿、gcovr 100%（10045/10045）；clang 21
树零警告、113/113 全绿。

## 控制面中心注册：MachineDaemon 生命周期注册/注销 + OpsControlCenter（2026-09-26）

对齐 todo 遗留「与 Ops Control Plane 的注册、上报和审计对接」之注册侧：

1. **INodeReportSink 升级为三段生命周期接缝**：`registerNode`（上线占位：
   早于首份快照中心即可见；已知节点只续 lastSeen、不覆盖快照——注册不是
   快照）+ `publish`（摘要 upsert，原语义）+ `deregister`（优雅下线立即
   摘除，返回是否确有该节点）。
2. **OpsControlCenter**：注册占位行（summary 空 = 已注册未报快照）、注销
   摘行并清接入序残留（与 pruneStale 同一清理纪律）；注册与首报共用同一
   接入序，容量逐出口径不变（最早接入优先）；注册但静默的节点同样受
   pruneStale TTL 兜底——注册/注销/掉线摘除三者语义自洽。
3. **MachineDaemon 生命周期**：start() 成功监听后采快照 hostname（nodeId
   既有口径）注册；stop() 先注销再关听（二次 stop/未 start 幂等跳过）；
   空 hostname（异常探针）不入中心。注册与上报节奏解耦（reportInterval=0
   仍注册、只是不推快照）。
4. **测试**（OpsControlCenterTest 11→19、MachineDaemonTest 15→16）：占位
   可见性、注册退化心跳不覆快照、空身份丢弃、注销返回值与残留、注销后
   容量逐出仍正确、静默注册 TTL 摘除、daemon start 注册/stop 注销 E2E
   （真实 TCP 上报把占位行升级为快照）、空 hostname 不注册。

验证口径：gcc-coverage 113/113 全绿、src/control gcovr 100%；clang 21 树
零警告、113/113 全绿。

## 权限边界落地：MachineDaemon execute 策略门（2026-09-26）

对齐 04-ops-control-plane MVP「少量**受控**命令」的受控语义与 todo 遗留
「非受控全局进程的策略化管理与权限边界」的控制面切面：

1. **ExecPolicy**：`trustedComponents`（来源组件白名单）+ `allowedCommands`
   （命令名白名单）双门；**缺省全拒**（安全缺省：未显式授权即不可执行，
   与 DBApp「显式选择后端」同哲学）。snapshot/audit 只读不设限。
2. **执行序**：载荷解析（malformed/空命令臂在前）→ 来源白名单 → 命令
   白名单 → agent 分发；两类策略拒绝均回 `machine.error`（原因串区分
   not trusted / not allowed）并照记审计（accepted=false，来源组件可见）
   ——拒绝路径不产生审计盲区。
3. **测试**（MachineDaemonTest 15 项，增 2）：第二组件身份直连验非受信
   拒绝与审计归因、非白名单命令拒绝、缺省策略对合法客户端也拒；
   既有 execute 场景全部显式授权（安全缺省的正交陈述）。
4. **非目标**：主机级非受控进程（非 agent 管辖的系统进程）的策略化
   治理仍留 todo——本切片只封闭控制面命令入口的权限边界。

验证口径：gcc-coverage 113/113 全绿、gcovr 100%（9997/9997）；clang 21 树
零警告、113/113 全绿。

## 节点摘要上报落地：IMachineAgent::report() + OpsControlCenter 聚合器（2026-09-26）

对齐设计文档 06-machine-agent-and-host-ops §2.4/§4.3 的 `report()` 与
04-ops-control-plane MVP 的聚合基座：

1. **NodeReport/INodeReportSink**（control/machine/NodeReport.h）：
   NodeSummary 上提到独立头（快照 RPC 与上报共用数据形状，消除 MachineAgent.h
   的循环包含）；上报帧 = nodeId（取快照 hostname）+ 时间戳 + 快照；
   agent 只依赖 sink 接口，不认识具体中心。
2. **MachineAgent::report()**：采一次快照推给出口；无出口空操作（上报是
   能力而非义务）；`setReportSink` 运行期换绑。
3. **OpsControlCenter**（control/ops/，INodeReportSink 实现）：按 nodeId
   upsert 每节点最新快照；容量上界 maxNodes（逐出最早接入节点——活跃节点
   不因持续上报改变首报序）；`pruneStale` 按 TTL 摘除掉线节点并清理首报序
   残留（否则容量逐出目标错乱）；`snapshotNodes` 按 nodeId 升序稳定输出。
   跨机网络转发属跨 realm 平面，后续接入。
4. **MachineDaemon 周期上报**：Config 增 reportInterval/reportSink；
   到期即调 agent.report()，首个 tick 立即上报（中心侧新鲜度）；0 或无
   sink 关闭。tick 单线程上下文，无锁。
5. **测试**（OpsControlCenterTest，11 项）：upsert 语义、无身份丢弃、
   miss、排序快照、容量逐出（活跃刷新不改变逐出序）、TTL 摘除、
   prune 后容量逐出仍正确（含首报序残留场景）、agent report 推送与
   无 sink 空操作、daemon 周期上报真链路、双开关关闭路径。
   ——初版断言模型错了一处（prune 后容量 2 再进两节点必然挤掉最早幸存者，
   产品逻辑正确），已修正。

验证口径：gcc-coverage 113/113 全绿、gcovr 100%（9975/9975）；clang 21 树
零警告、113/113 全绿。

## Ops Control Plane MVP 切片：受控命令审计（2026-09-26）

对齐 docs/design/5-access-and-control-plane/04-ops-control-plane.md §8 MVP
的“操作审计”项：

1. **审计日志**：`MachineDaemon` 为每条 execute 尝试（成败）与协议层拒绝
   （畸形载荷/空命令/未知方法）记 `AuditEntry`（时间/来源组件/命令/参数/
   accepted/ok）；snapshot 等只读操作不记（避免噪声）。环形容量
   `auditCapacity`（默认 128，满后丢最旧，0 = 关闭）。单线程 tick 上下文
   写入，无锁（与 DBApp 同假设）。
2. **machine.audit**：查询最近审计的 JSON 数组（时间升序），与本地
   `auditLog()` 视图同源；command/args 经 `escapeJsonString` 转义——
   该转义从 MachineSnapshotCodec 内部提升为公共工具（快照与审计共用，
   不复制转义逻辑）。
3. **测试**（MachineDaemonTest 增 3 项，共 15 项）：审计轨迹完整性与
   时间序、容量 2 的环形逐出、容量 0 关闭。
4. 途中修复 gcc 对多行 `<<` 链的覆盖归因错误（拆命名字段串）。

验证口径：gcc-coverage 112/112 全绿、gcovr 100%（9900/9900）；clang 21 树
零警告构建、112/112 全绿。

## Machine 遗留事项落地：MachineAgent RPC 输出（控制面端点）（2026-09-26）

1. **MachineDaemon**：把 `IMachineAgent` 的 snapshot/execute 暴露为控制面
   TCP 端点，协议复用组件间 RuntimeInvocation 帧（与 DBApp 同族，不另起协议）：
   - `machine.snapshot` → `machine.snapshot.ok`，payload = 快照 JSON；
   - `machine.execute`（payload = command NUL args）→ `machine.execute.ok`，
     payload = 1 字节成败位；
   - 未知方法/畸形载荷/空命令 → 统一 `machine.error` + 可读原因串
     （统一错误处理红线：不静默丢包）。
   服务端连接由对端首条请求 sourceComponent 自报注册（attachServerTransport，
   与 DBApp 同机制）；tick 全非阻塞，调用方泵入自身循环。
2. **分层**：theseed_control 增 PUBLIC 依赖 theseed_runtime（控制面用运行面
   传输，无环）。
3. **测试**（`MachineDaemonTest`，12 项，真 TCP 回环 + 真实
   LocalHostProbe/LocalProcessSupervisor/LocalProcessSupervisor 链）：
   幂等 start、snapshot JSON 往返、execute start/stop 受管子进程闭环
   （从快照 JSON 提取 pid 再 stop）、未知 pid 失败位与协议错误区分、
   畸形载荷/空命令/未知方法错误响应、端口冲突、stop 后 tick 空转。

验证口径：gcc-coverage 112/112 全绿、gcovr 100%（9849/9849）；clang 21 树
零警告构建、112/112 全绿。

## Machine 遗留事项落地：端口占用扫描 + 二进制版本探测（2026-09-26）

1. **端口占用扫描**：新增 `ProcessPortScanner`——`collectListenInodes` 解析
   /proc/net/tcp[tcp6]（LISTEN 套接字 inode → 端口，解析器独立暴露供合成流
   测试），`scanListeningPorts(pids)` 经 /proc/<pid>/fd 反查拥有者；只反查
   调用方给出的 pid 集合（不做全机 fd 扫描），无权限目录静默跳过
   （快照是观察而非审计）。`LocalProcessSupervisor::listProcesses` 为全部
   枚举进程（含受管子进程）补 `port` 字段。
2. **二进制版本探测**：`probeProcessVersion(port)` 对回环端口发 GET /health、
   从响应 JSON 取 "version"（theseed 各进程 OpsServer 均暴露该端点），
   SO_RCVTIMEO 短超时、RAII fd 收口。权限边界：listProcesses 只对**受管**
   进程探测，不主动连接非受管进程。
3. **测试**（`ProcessPortScannerTest`，10 项）：合成流驱动解析器畸形分支；
   真实 /proc 反查本进程 TcpListener 端口；探测活体 OpsServer 版本往返、
   拒连/静默超时/无版本/截断值四类失败；端到端——子进程模式自 exec 挂真实
   OpsServer，listProcesses 补出受管子进程 port+version 后 stop。
4. 途中修复 `lookupPidPort` 的 inode 提取 off-by-one（"socket:[" 是 8 字符，
   原取 7——测试先行暴露，真实 /proc 反查失败）。

验证口径：gcc-coverage 111/111 全绿、gcovr 100%（9760/9760）；clang 21 树
零警告构建、111/111 全绿。

## Machine 遗留事项 1+2 落地：CPU 采样稳定化 + 网络流量统计（2026-09-26）

1. **CPU 采样稳定化（遗留事项 1）**：`LocalHostProbe` 新增 `Config`（首采自举窗口
   200ms / 轮询粒度 10ms）与可注入查询点（`CpuTickQuery` / `NetworkBytesQuery`，
   生产实现读 /proc/stat 与 /proc/net/dev，测试注入脚本化序列）。首采自举
   `primeCpuSample`：首次 `sample()` 无基线读数，窗口内轮询等 tick 推进后以两端
   差值出读数——CLI 单次调用也能拿到真实使用率；零差值/查询失败走粘滞
   `lastCpuUsage_` 不闪回 0；读数 clamp 到 [0,100]。
2. **网络流量统计（遗留事项 2，Linux）**：`sumNetworkBytes` 解析 /proc/net/dev，
   聚合除回环外全部网卡（rx=第 1 列、tx=第 9 列），`HostSummary` 增
   `networkRxBytes`/`networkTxBytes`，text/JSON 快照格式同步输出。
   Windows/macOS 暂返 0（留在遗留清单）。
3. **测试**：新增 `HostProbeTest`（7 场景：首采自举 50%、窗口耗尽保持、零差值粘滞、
   查询失败保持、clamp 边界、零窗口不自举、真实 /proc 单调性）；DBAppE2E 补
   短载荷 remove 解码失败、mysql/postgresql 后端构建不可用回退 file 两场景；
   NetworkNodeTest 调度循环停止测试改为原子计数 tickable 等满两拍再停（重负载
   下覆盖不靠时序运气）。
4. **DBApp.cpp 收尾**：`accountStore_` 快路径（query/create）在本构建（无
   libmysql/libpq）恒不可达——`accountStore_` 仅由 SQL 后端置位。按既有惯例加
   带理由的 LCOV 豁免（真臂 + 函数体），SQL 树由 THESEED_MYSQL_HOST/
   THESEED_PG_HOST 门控 E2E 覆盖。全仓 gcovr 100%（9644/9644）。

验证口径：gcc-coverage 树 110/110 全绿（新增 theseed_host_probe_test）；
gcc 15 `-Werror` 零警告构建。遗留清单同步划掉已完成两项。

## 质量红线落地：-Werror 固化 + 零警告清零 + 架构谱系文档（2026-09-25）

1. **`-Werror` 固化**：根 CMakeLists 新增 `THESEED_WARNINGS_AS_ERRORS`（默认 ON）。
   测试侧聚合初始化豁免经 `theseed_test_options` 接口库（`-Wno-missing-field-initializers`
   族，含 clang 18+ 的 `-Wmissing-designated-field-initializers`），测试目标按序链接
   `theseed_project_options theseed_test_options`——顺序保证豁免在 -Wextra 之后生效
   （CMake 接口库选项是直接依赖自身在前，串接式写法会把豁免排到 -Wextra 前面失效）。
   gcc 15 / clang 21 双树 `-Wall -Wextra -Wpedantic -Werror` 零警告。
2. **-Werror 拦下的真实问题（产品码 5 处）**：
   - `EntityDefLoader` 闭合标签校验从未实现——注释声称 "Verify it matches parent tag"
     但只算了不用（`trimmed` 死变量、`parentTag` 死参数）。已实现：错配闭合标签上卷到
     祖先层而非静默吞掉，`trimWs` 双向去空白；补 2 个解析器测试（错配上卷契约、
     `</Tag >` 带空白收尾）
   - `NetworkNode` 未用变量 `rawPtr`、`SessionStore` 死常量 `kLineSeparator`、
     `PipedTransport`/`TransportHub` 只写不读的 `localComponent_` 字段（clang
     `-Wunused-private-field` 独有警告，gcc 不报）——删除，公共构造签名保留
   - `BaseApp::onEntityDestroyed`/`RealmApp::onClientMessage` 未用回调参数——
     `static_cast<void>` + 注释（与 NetworkNode 既有惯例一致）
3. **测试侧清理（-Werror 拦下的 19 处）**：死测试 `test_apply_reports_warning_count`
   写了从未注册进 main（这正是 BigWorld 式"测试缺失"坑——测试存在≠测试运行）；
   2 个死辅助函数（DBAppTest `writeFile`、LoginAppTest `encodeRealmId`）；
   未用变量/lambda 参数/恒真比较（`pendingCount() >= 0` 对 unsigned 恒真、
   `&ref != nullptr` 引用取址恒真）逐处改为有意义的断言或删除。
4. **架构谱系文档**：新增 `docs/architecture.md`（BigWorld/KBEngine 借鉴矩阵、
   theseed 改动与理由、对照 BigWorld 历史坑的规避表、当前实现状态、质量红线），
   VitePress 导航挂"架构谱系"入口。
5. **裸 new/delete 审计**：src/ 全量 grep 零命中（RAII/智能指针红线现状达标）。

验证口径：gcc-coverage 树 109/109 全绿（本环境无 libmysql/libpq/podman，SQL 段
按环境变量门控跳过，与 clang18/gcc13 树口径一致）；clang 21 树构建零警告。

## 测试覆盖率专项（2026-09-22，行 78.5% [86.9%，gcovr 实测]

基线（gcc-coverage preset + gcovr）：行 78.5%、函数 85.3%、分支 46.0%。
本轮以端到端 TCP 回环测试补齐 0% 文件，过程中发现并修复 5 个真实缺陷：

1. `IRuntimeTransport` 缺 `tick()` 接口——`TransportHub::tick` 只 flush 不 pump，
   TCP 服务端永远读不到入站字节（接口加默认实现，hub tick 调 tick()+flush()）
2. `decodeEntityData` 解码边界异常穿透——畸形网络载荷（截断/垃圾长度）抛
   `runtime_error` 直达服务进程 tick 循环并 terminate（改为边界内返回 false +
   属性数上限 1<<20 防超大分配；DBProtocol 与 FileEntityStore 共用此边界）
3. `OpsServer` 未知路径回 200——监控把打错的路径当成功（改 404，respond 支持
   自定义 statusLine）
4. `DBApp` 用函数级 `static` 分配 peerId——跨实例残留，同进程第二个 DBApp 把
   客户端编成 2 号，回复按 sourceComponent 路由时静默丢弃
5. **DBApp 回复路由与客户端自报身份不一致（协议级）**：服务端按本地分配的
   peerId 注册连接，而客户端回复 target 取请求 sourceComponent——两者只在
   客户端猜中 id=1 时巧合一致，LoginApp(localComponentId=20)→DBApp 的
   生产链路实际全断。修法：`TransportHub::attachServerTransport`，服务端
   连接由首条入站消息的 sourceComponent 自学习注册（`awaitingIdentity_`）

新增测试（全部真 TCP 回环，非 mock）：

- `DBAppE2ETest`（21 项）：file 双库门控场景，覆盖 init/tick/accept/全部
  db.* 分发/账号线性扫描回退/畸形载荷错误分支/OpsServer 四端点/端口冲突；
  mysql、postgresql 后端经环境变量门控在真库上验证
- `RealmAppE2ETest`（7 项）：QueryRealms 往返/未知消息存活/ops 端点/断开清理
- `LoginAppE2ETest`（16 项）：null+限流+SessionStore、password 回退、db 鉴权
  三条路径（auto-register/正确/错误密码，后台线程驱动 DBApp）
- `MachineSnapshotCodecTest`：text/JSON 格式化、全部转义分支、空进程列表
- `BackupTopologyCoordinatorTest` 增补 processes() 排序与 reset()
- `PostgreSQLEntityStoreTest` 补自隔离清理（表名含大写需引号），消除对
  新鲜库的隐式依赖

当前覆盖（gcovr，含双库门控测试）：行 **89%**（2026-09-22 第二轮）。
**已收官：2026-09-23 达成行级 100.0%**（10441/10441，行级口径 + 131 行带理由
LCOV_EXCL 豁免；口径与豁免定性见 docs/design/8-reference/coverage-report.md）。
第二轮补齐两个点名缺口：

- `ProcessSupervisor` 75%→92%：`ProcessSupervisorTest` fork 真子进程
  （/bin/sleep、/bin/true）覆盖 start/stop 往返、restart 换 pid（旧 pid 用
  waitpid ECHILD 验证已收尸）、reap 已退出子进程、析构兜底终止；
  splitCommandLine/basenameOf 提为 public 直接测引号/空串分支。
- `EntityQuery` 62%→95%：逐数值类型（含 8/16 位窄类型，按 DataType 宽度
  手工编码——`QueryFilter::of` 窄整型会提升到 Int32 重载，须绕开）、
  compareProperty 三态/unordered、LoadFailingStore 验证 query 跳过 load
  失败、of() 全重载冒烟（int64/double 补齐）。剩余 miss 为 NRVO 下
  `return f;` 归因伪影与 switch 后 unreachable 行，测试不可达。

下一批缺口：`CellRuntime`（85%）/`BaseRuntime`（89%）分支覆盖。
**（已完成**：CellRuntime/BaseRuntime 分支覆盖已补齐，覆盖率专项整体收官。**）**
顺带发现：`CellRuntime.cpp` 匿名 ns 的 `encodeCellCreation`/`decodeCellReady`
是死代码（gcc -Wunused-function 警告，Linux 构建从未调用）——**已删除**（b45bc44）。

## 遗留事项

当前只优先实现 `MachineAgent -> HostProbe -> ProcessSupervisor -> snapshot` 核心链路。
以下事项暂不进入当前实现：

- ~~`LocalHostProbe` 的高精度 CPU 采样稳定化，当前 CLI 两次短窗口采样仍可能得到 `0.00`~~（2026-09-26 完成）
- ~~网络流量统计与多网卡聚合~~（2026-09-26 完成，Linux）
- 非受控全局进程的策略化管理与权限边界（控制面 execute 双白名单已落地
  2026-09-26；主机级非 agent 进程的策略化治理仍开放）
- ~~端口占用扫描与二进制版本探测~~（2026-09-26 完成，Linux；Windows/macOS 留空）
- Linux / macOS 的等价主机探针完整实现与验证（网络部分 Linux 已完成，缺 macOS）
- ~~`MachineAgent` 的 RPC 输出与控制面注册~~（2026-09-26 完成：MachineDaemon
  TCP 端点；控制面中心注册待 Ops Control Plane 对接时一并做）
- ~~与 `Ops Control Plane` 的注册、上报和审计对接~~（2026-09-26 完成：
  生命周期注册/注销 + pruneStale 掉线摘除自洽；execute 审计（含拒绝）
  汇入中心可查询审计环形）
- ~~与 `Telemetry` 的指标、日志、trace 联动~~（2026-09-26 完成控制面切片：
  machine/ops 双侧计数与水位仪表、结构化审计与生命周期日志、
  execute SpanScope + 日志自动 trace 关联；OTel 导出器仍开放）
- 更完整的单元测试与跨平台 CI 构建矩阵

## MySQL 持久化后端（Phase B，已在真实环境验证通过 2026-09-22）

`MySQLEntityStore` 已通过真实 MySQL 8.0.46 实例的完整验证：
`MySqlEntityStoreTest` 22/22 通过，`theseed_dbapp --backend mysql` 启动冒烟通过。
真实编译暴露并修复了三个客户端层 bug（prepared 查询悬垂指针、my_bool 移除、
整数参数按 BLOB 绑定被严格模式拒绝），见提交 a27ef6b。

真实构建前置依赖（vcpkg 编译 mysql-server 8.0.46 源码，~15-30 分钟）：

- `libtirpc-dev`：提供 `rpc/rpc.h`（glibc 2.28+ 已移除 sunrpc 头）
- `libncurses-dev`：提供系统 `term.h`。MySQL 的 CMake 在配置期检测
  `CHECK_INCLUDE_FILES(term.h)` 时不会带 vcpkg 的 include 路径，缺失时
  内嵌 libedit 编译失败（GCC 15 默认 C23 把隐式声明当硬错误）
- 配置命令：`cmake --preset gcc-debug -D CMAKE_TOOLCHAIN_FILE=~/vcpkg/scripts/buildsystems/vcpkg.cmake -D VCPKG_MANIFEST_INSTALL=ON`
  （`CMAKE_TOOLCHAIN_FILE` 只在全新缓存的首配生效）

本地真实验证环境（podman，rootless）：

```
podman run -d --name theseed-mysql -e MYSQL_ROOT_PASSWORD=theseed_test_pw \
  -e MYSQL_DATABASE=theseed_test -p 127.0.0.1:13306:3306 docker.io/library/mysql:8.0

THESEED_MYSQL_HOST=127.0.0.1 THESEED_MYSQL_PORT=13306 THESEED_MYSQL_USER=root \
THESEED_MYSQL_PASSWORD=theseed_test_pw THESEED_MYSQL_DATABASE=theseed_test \
./build/gcc-debug/tests/db/theseed_mysql_store_test
```

- 无 libmysql 时自动回退 FileEntityStore（条件编译保护），集成测试自动跳过

## PostgreSQL 持久化后端（Phase B，已在真实环境验证通过 2026-09-22）

`PostgreSQLEntityStore` 已通过真实 PostgreSQL 16 实例的完整验证：
`PostgreSQLEntityStoreTest` 24/24 通过，全量 ctest 96/96，
`theseed_dbapp --backend postgresql` 启动冒烟通过（`pg_stat_activity` 确认真实 libpq 连接），
`--backend mysql` 回归冒烟通过（两个后端共用 `db*` 连接配置）。

设计要点：

- 与 `MySQLEntityStore` 实现同一 `IEntityStore`/`IAccountStore` 接口，存储模型
  （每类型一张 `tbl_<Type>` 表 + BYTEA 序列化 blob）与 MySQL 版完全一致
- `allocId` 用 PG 方言单语句原子完成：
  `INSERT ... ON CONFLICT ... DO UPDATE ... RETURNING next_id`（无需 MySQL 的
  LAST_INSERT_ID(expr) 两步），首 id 为 1
- `DBApp` 经 `--backend file|mysql|postgresql` 选择后端；CLI 同时提供 `--mysql-*`
  与 `--pg-*` 参数别名（写同一组 `db*` 配置字段，`--backend postgresql` 未指定
  端口时默认 5432）；构建期缺 libpq 时回退 FileEntityStore
- 共享参数模型 `SqlParam`（`u64()` 整数 / `str()` 文本 / 字节串 / `null()`）：
  PG text 协议下字节串按 `\x...` 十六进制 + `::bytea` cast，文本参数必须原样
  传（按 bytea 编码绑进 VARCHAR 列会存成 `"\\x..."` 字面文本）
- PG 18 客户端头不再导出 `BYTEAOID`（移入 server 头 `catalog/pg_type_d.h`），
  代码本地定义 `kByteaOid = 17`（目录表冻结常量）

真实构建前置依赖（vcpkg 编译 PostgreSQL 18.4 客户端源码，meson + ninja）：

- `bison` + `flex`：PG 源码需要生成 parser/lexer。无 sudo 时可用
  `apt-get download bison flex && dpkg -x <deb> ~/.local/tools/` 解包，
  配 `PATH` 与 `BISON_PKGDATADIR=~/.local/tools/usr/share/bison`
  （bison 的数据文件编译期固定在 /usr/share/bison，缺失时报
  `m4sugar.m4: cannot open`）
- 配置命令同 MySQL 一节

本地真实验证环境（podman，rootless）：

```
podman run -d --name theseed-postgres -e POSTGRES_PASSWORD=theseed_test_pw \
  -e POSTGRES_DB=theseed_test -p 127.0.0.1:13307:5432 docker.io/library/postgres:16

THESEED_PG_HOST=127.0.0.1 THESEED_PG_PORT=13307 THESEED_PG_USER=postgres \
THESEED_PG_PASSWORD=theseed_test_pw THESEED_PG_DATABASE=theseed_test \
./build/gcc-debug/tests/db/theseed_pg_store_test
```

- 无 libpq 时自动回退 FileEntityStore（条件编译保护），集成测试自动跳过

