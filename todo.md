# TODO

## 会话运维命令面：list-sessions + kick-sessions（04 §8 Phase 2 切片，2026-09-26）

Phase 2「更完整的登录与会话运维命令」里前置已真实存在的一块（SessionStore
+ 上一批接好的 kick），两条新命令 + 枚举能力：

1. **SessionStore 枚举能力（最小扩展，不造假）**：
   - `IRedisProvider` 补 `zrem`（核心 redis 原语，此前缺；InMemory
     实现三态用例：命中/未命中/集合不存在）；
   - 二级索引：会话键 `session:<token>` 之外，zset `sessions:index`
     （member=token，score=保存时刻 epoch 毫秒）——provider 原语无
     键空间扫描，枚举走索引；同一 provider 即同一视图（跨进程一致，
     LoginApp/daemon 共享）。save 双写（set+zadd，索引写失败如实返
     false——会话在而索引缺是对枚举面的不一致，让调用方重试）；revoke
     命中才摘索引（未命中不误删他人视图，跨进程竞争安全）；
     `listSessions()` 惰性清账：过期键 / 损坏 blob 顺手 zrem——索引与
     会话键两次写不原子，枚举是收敛时机，吐出的行必对应活会话。
   - `SessionView{token/accountId/realmId/userId}`：token 原文仅供本
     进程续作（如批量吊销），协议出口一律掩码——纪律写在结构注释里。
2. **machine.list-sessions（≥ReadOnly，inspect 面）**：双门沿用
   （NodeOpsPolicy 来源白名单 → 角色门）；响应 = 行 JSON 数组
   `[{"session":"session(len=N)","account":…,"realm":…,"user_id":N}]`，
   account/realm 过 escapeJsonString，metadata（客户端态）不进运维面，
   令牌原文绝不回显。**读面成对计数**（machine_list_sessions_{accepted,
   rejected}_count）且接受臂照记审计（args=count=N）——超越 snapshot/
   audit 只记拒绝的旧读面口径：会话存在性本身敏感，与剖面清单面同款。
3. **machine.kick-sessions（≥Operator，operate 面，≙ 批量 kick）**：
   载荷 = 作用域选择器 `all | account=<id> | realm=<id>`（空值后缀
   不成立，三畸形臂具名拒绝）——指纹不可逆，批量按运维可见属性圈选，
   令牌列表不做入口。枚举 → 过滤 → 逐个吊销，成功/失败分列回
   `{"requested":N,"revoked":[指纹…],"missing":[指纹…]}`；部分失败
   （枚举后吊销前被并发摘除）仍算动作接受，missing 即对账凭证。审计
   args = `scope=<sel> revoked=N missing=M`（key=value；指纹序列只进
   应答体不进审计环）+ span + machine_kick_sessions_{accepted,rejected}
   _count 成对。
4. **入口关闭口径沿用**：sessionStore==nullptr 时 list/kick-sessions 与
   kick-session 同样视同未知方法（主 daemon 测试臂覆盖）。
5. **测试**（MachineDaemonTest 30→31、RedisProviderTest 28→33）：
   foundation 侧 zrem 三态、枚举/吊销退出/过期清账（advanceClock +
   zcard 断言）/损坏 blob 清账；daemon 侧三角色 × 两命令正反矩阵 +
   未绑定/策略外/三畸形选择器拒绝臂、realm/account/all 三作用域真实
   吊销（load 断言）、**部分失败臂**（FlakyDelProvider：del 按后缀
   失败一次的包装提供者，确定性复现并发摘除窗口；两个失败目标 →
   missing 分列 ≥2）、幸存者单 kick 收尾、计数配对增量（枚举 2/2、
   批量 3/6）、15 条审计全留痕 + args 逐条扫描零令牌原文。

**边界与遗留（如实记录）**：
- clear temporary bans 仍不做（沿既有记录：仓库无封禁存储前置）。
- 枚举顺序由提供者定（内存实现按 token 字典序），不承诺保存时序
  （score 只跨进程可辨先后）。
- 长度指纹有同长歧义：同账号多会话且令牌等长时指纹不可区分——运维
  以账号/领域行为粒度核对；引入哈希指纹属扩面，未做。
- requestId 仍缺（沿既有记录）；kick 入口是进程内 SessionStore 指针
  而非独立 LoginApp 组件（沿上一批记录）。

## §6.1 受控命令落地：kick session / set draining / controlled shutdown（2026-09-26）

把上一批（权限分级 + 配置热改）如实记录的遗留①——「角色档位已预留、
命令本身仓库未建」的三条受控命令建起来（clear temporary bans 见边界）：

1. **三条命令、三个真实面**（MachineDaemon 新增 RPC，双门模式沿用
   execute：先 NodeOpsPolicy 来源白名单（空集全拒）再 AccessRole 角色门；
   全部尝试含拒绝入审计环形 + 中心聚合，成对计数，args=key=value）：
   - `machine.kick-session`（Operator，`session.kick` 审计）：载荷 =
     会话令牌原文，吊销 foundation/SessionStore（redis 后备，跨进程
     可见——LoginApp 写入、本 daemon 吊销即全集群生效，真实可测的
     kick 语义）；未知令牌具名拒绝；**令牌原文不进审计环**——args 只记
     `session(len=N)` 长度指纹（会话令牌是登录凭证）。sessionStore
     未配置 = kick 入口关闭（视同未知方法，与采样入口同口径）。
   - `machine.set-draining`（Operator，`node.drain` 审计）：载荷
     1 字节 0x01/0x00（开/关排水），落
     `IMachineAgent::setDraining`——agent 是节点状态持有方，排水位经
     snapshot() 进快照 RPC（MachineSnapshotCodec 既有 `"draining"`
     字段透出）与 report() 通道，中心 latest 即见（快照读侧与上报
     聚合同源，不另设状态存储）。
   - `machine.shutdown`（Admin，`node.shutdown` 审计，≙ §6.1
     controlled shutdown）：**应答先出站**，置位后本 tick 收尾
     （processMessages 之后、周期上报之前）优雅停机——注销中心 +
     关听；延迟到 tick 末是不悬空消息分发上下文（hub 不得在分发中途
     销毁）；停机后的最后动作是干净下线而非再报一次状态；不触碰受管
     子进程与主机非受控进程（§6.1 controlled shutdown 是 daemon
     生命周期，编排归 execute 族）。
2. **策略族单列**：NodeOpsPolicy 与 ExecPolicy（受管进程编排）、
   ProcessGovernPolicy（主机非受控进程）刻意分离——授权口径与风险
   等级互不牵动，不得共享白名单（与既有两族同纪律）。沿用 execute 的
   双门「模式」而非把命令塞进 machine.execute：shutdown 是 daemon
   生命周期动作，不属 agent 可编排命令；独立方法名也让拒绝原因串与
   审计 command 各自可辨。
3. **§6.2 口径沿用上一批**：operatorId ≙ AuditEntry.source 组件 id、
   result=rejected ≙ accepted=false、args=key=value、三对新增成对计数
   （machine_{kick,drain,shutdown}_{accepted,rejected}_count）；
   requestId 仍缺（沿既有记录，不编造）。
4. **测试**（MachineDaemonTest 29→30）：三角色 × 三命令正反矩阵，
   拒绝臂全覆盖（角色位/策略位/载荷畸形/未知令牌），真实效果断言
   （kick 后 SessionStore 里令牌不复可查、排水位中心 latest 同步翻转、
   shutdown 后 isListening=false + 中心 nodeCount==0、停机后 tick
   空转无副作用）、计数增量配对断言、15 次尝试全留痕 + 令牌指纹
   抽查、kick 入口关闭臂（主 daemon 未配 sessionStore → unknown
   method）。

**边界与遗留（如实记录，未编造）**：
- **clear temporary bans 未建**：仓库没有任何临时封禁存储（全库检索
  仅 Witness.cpp 的无关同名误配），前置缺失不造假实现；§6.1 档位
  留档，待登录/网关侧封禁面建立后再按 Operator 档落角色门。
- kick 的入口面是 daemon 配置直连 SessionStore（进程内指针），不是
  独立 LoginApp 组件——仓库无 LoginApp，跨进程语义由 redis 共享存储
  承载（同一 IRedisProvider 即同两会话视图），完整登录运维归 §8
  Phase 2（「更完整的登录与会话运维命令」）。
- requestId 仍缺（沿既有记录）；Gateway 限流边界沿用上一批记录。

## 权限分级 + 配置热改限制：04 §6 安全模型落地（2026-09-26）

把前几批一直记录为「沿用边界、未顺手扩面」的 §6 落地成码：

1. **§6.1 角色模型**（machine/AccessControl.h，agent 与中心共用）：
   `AccessRole{None/ReadOnly/Operator/Admin}` 单调档位 + `RoleBinding`
   （来源组件 → 角色）+ `roleMeets` 分级判定。分级口径（§6.1 的动作族 →
   本仓库实际动作的映射，缺命令的留档位不造命令）：
   - inspect（≥ReadOnly）：snapshot / audit 查询 / 进程枚举 / 剖面清单
     与下载（agent 侧 + 中心侧查询下载）；
   - operate（≥Operator）：execute 受管编排（start/stop/restart）、
     诊断采样触发（§6.1 的 kick session / set draining / clear
     temporary bans 同级——三命令仓库未建，见边界）；
   - administer（=Admin）：machine.terminate（≙ §6.1 的 retire
     process）、machine.config.apply（≙ apply runtime config）。
2. **叠位语义（先策略后角色）**：全部入口先过既有策略白名单
   （ExecPolicy / ProcessGovernPolicy / DiagnosticsPolicy 的
   canTrigger/canAccess），再过角色门——既有拒绝原因串不失真（策略拒绝
   照旧），角色位是第二道独立关口（canAccess 放行 ≠ 可读）。未绑定角色
   = None = 全拒（含只读面；绑定表缺省空 = 安全缺省）。角色拒绝照记
   审计 + 各动作族既有拒绝计数同款口径；只读面（snapshot/audit 查询）
   原本接受不留痕，拒绝补齐留痕并配对补计数
   （machine_snapshot_rejected_count / machine_audit_rejected_count）。
3. **§6.2 审计映射（如实对齐，无新口径）**：operatorId ≙ 既有
   AuditEntry.source 组件 id（绑定表就是按组件 id 查角色，不另设操作者
   命名空间）；result=rejected ≙ accepted=false；command/args/timestamp
   同既有条目形状。requestId 字段仍缺（沿用既有边界记录）。
4. **§6.3 配置热改**（machine.config.apply，Admin 级，新方法）：载荷
   key NUL value（与 execute 同 NUL 约定）。**禁改四类先行指认**——键
   前缀命中协议定义（protocol.*）/ 持久化 schema（persistence.*）/
   entity property flags（entity.property*）/ 迁移语义（migration.*）
   即拒且指认类别；**白名单是唯一通道**——白名单外任意键一律拒（扩面
   须改码评审，配置自身开不了门）；白名单内目前唯一可调项
   `ops.report_interval_ms`（毫秒，0 = 关闭上报），就地生效、效果可观测
   （上报从关闭到打开，下一轮 tick 中心即见快照）。全程 span + 审计
   （args 记 key=value）+ 一对计数
   （machine_config_apply_accepted/rejected_count）。
5. **Gateway 限流/超时：如实边界**。仓库无 Gateway 组件，不造；检查过
   foundation 既有 RateLimiter——Redis 后备（IRedisProvider），接到
   进程内 ops 读入口需引入 redis 依赖，不存在「直接、明显」的节流点，
   不接、不扩面（限流语义属于跨进程控制面入口，归 Gateway 层）。
6. **测试**（MachineDaemonTest 27→29、OpsControlCenterTest 27→28）：
   三级角色 × 三档动作正反矩阵（ReadOnly 可读不可写、Operator 可操作
   不可处置、Admin 全通、策略白名单内未绑定者全拒、策略外者既有消息
   不变）、只读面角色拒绝照记审计与配对计数、中心侧 canAccess+角色
   叠位（canAccess 四者皆含、角色表缺一者被拒）、配置热改（畸形载荷/
   角色不足/禁改四类各指认/白名单外/非法值 ×2/白名单内生效可观测/
   全部尝试留痕 10 条 + span 10 个）。

**边界与缺字段（如实记录，未编造）**：
- ~~§6.1 的 kick session / set draining / clear temporary bans /
  controlled shutdown 四命令仓库未建（无会话面/排水命令/临时禁名单/
  受控停机命令），角色档位已预留（operate/operate/operate/administer），
  建命令时按档位落角色门即可。~~（2026-09-26 完成三条：kick session /
  set draining / controlled shutdown 见顶部批；clear temporary bans
  因无封禁存储仍留边界）
- metrics summary（/metrics HTTP 导出）与中心聚合器基础查询
  （latest/snapshotNodes/auditTrail）不在角色门内：前者是无鉴权 HTTP
  导出面（05 遥测口径），后者是 ops 宿主自身装配面——§6.1 的管控落点
  是控制面 RPC 与中心剖面读入口，宿主内嵌调用属信任边界内侧。
- RateLimiter 未接（理由见上 5）；Gateway 层（正式鉴权/限流/超时）
  整体沿用既有边界记录。
- requestId 字段仍缺（§6.2 七字段之六已对齐，沿既有记录）。
- 白名单可调项仅 ops.report_interval_ms 一项：其余运行时可调项（如
  诊断阈值 slow_threshold）目前只进产物统计（05 边界），需要热改时
  逐项评审入白名单。

## 诊断产物跨机回传 + 中心侧剖面查询：04 §7 中心半边（2026-09-26）

补齐上一批明确留下的边界（「产物仅存 agent 本地，跨机器回传中心的通道
未做」），落地 04 §7 的中心侧两件事：「按 entity / entity type / process
查询当前剖面」+ 中心持有产物副本后中心侧下载不再依赖直连 agent：

1. **回传选型：agent 推送（非中心拉取）**。理由：当前拓扑只有
   agent→center 单向通道（daemon 是 TCP 服务端，中心不持有 agent 连接），
   拉取需要新增中心→agent 反向传输腿，远超本批只读优先的范围；推送与
   审计/上报同向，复用既有接缝族（INodeArtifactSink 与 INodeAuditSink
   同构）。daemon 在 tick 里轮询 `listArtifacts()`，发现新固化句柄即推
   （元数据 + 只读字节），已回传账本只留仍在产物环形里的句柄（句柄不复
   用，被逐出者不会复现，账本上界 = 产物环形容量）。
2. **元数据通道：复用 report() 上报**。`NodeReport` 增 profiles 字段
   （快照语义：后到覆盖中心侧该节点剖面索引；无剖面来源的报告照常清空
   索引——诚实反映 agent 侧已无剖面）；`IMachineAgent::setProfileMeta
   Source` 能力接缝（nullptr = 无剖面，报告照发），daemon 是自然实现方。
   维度纪律（不编造数据）：机器 agent 的 TickProfiler 是进程级 tick 粒度
   ——entityId/entityType 无生产者恒空上报，中心按这两维查询如实落空；
   process 维 ≙ nodeId（06 的 hostname 口径），不冗余进 ProfileMeta。
3. **中心侧（OpsControlCenter）**：实现 INodeArtifactSink，产物副本进
   跨节点全局环形（容量 maxProfileArtifacts=32，满后按到达序丢最旧，
   0 = 关闭副本存储——索引照常、下载如实报无副本；副本是历史事实，不随
   节点注销/pruneStale/容量逐出而清，与审计同纪律；剖面索引是节点状态，
   随节点摘除而清，与快照同纪律）。`queryProfiles` / `downloadProfile
   Artifact` 读入口：沿用 `DiagnosticsPolicy` 读位 canAccess（类型从
   MachineDaemon 嵌套上提为命名空间级，授权口径不复制不走样；canTrigger
   写位只在 agent 侧生效，中心无写命令）；查询输出按 (nodeId, handle)
   稳定排序；产物帧并额外并入查询索引（报告通道未及的窗口也能查到）。
4. **审计与指标**：中心侧查询/下载及全部拒绝逐条入审计（command 前缀
   center.profiler.* 与 agent 侧 profiler.* 区分归属；nodeId 为空 =
   中心本地动作——与「daemon 无身份上报丢弃」纪律区分：后者管的是无法
   归属的节点上报，中心本地动作的身份就是中心）+ 指标四对计数
   （center_profile_query/download_accepted/rejected_count）+ 副本逐出
   计数；读动作不进 trace（与 agent 侧清单/下载同口径）。
5. **测试**（OpsControlCenterTest 24→27、MachineDaemonTest 25→27）：
   中心聚合（多 agent 上报→维度查询，entity 维诚实落空、未知节点落空、
   稳定序）、副本存储（回传→中心下载同源字节→未知句柄/未知节点/未授权
   三拒绝→环形逐出）、索引生命周期（副本关闭仍可查询、快照覆盖清索引、
   注销/prune/容量逐出清索引、仅产物帧节点可查、空帧身份纪律丢弃）、
   E2E（真实 agent 推送→中心查询/下载与 agent 侧字节同源→report 通道
   携带元数据→agent 侧环形逐出后中心副本仍可下载→不可读产物帧安全
   跳过不落库）。

**边界与缺字段（如实记录，未编造）**：
- 中心侧查询/下载是进程内 API（ops 宿主进程直调 OpsControlCenter），
  跨机网络转发属跨 realm 异步平面，后续接入；调用方身份为进程内自报，
  正式鉴权归 Gateway 层（沿用既有边界记录）。
- entity / entityType 维度当前无生产者（不做 EntityProfiler→负载反馈
  链，归 03 谱系），查询如实落空；过滤逻辑本身已按维度实现并有测试。
- 中心副本的保留时长只有容量上界（无 TTL/落盘）——中心进程重启即失，
  跨机持久化留待跨 realm 平面。
- 触发入口仍在 agent 侧（machine.profile.trigger），本批无中心侧写命令
  （只读优先）；§6.1 角色模型、Gateway 限流/超时、配置在线热改边界
  沿用上一批记录，未顺手扩面。
- daemon 私有继承 IProfileMetaSource 使析构隐式虚化（D0/D1/D2 多符号
  变体），签名行覆盖按既有 ABI 结构性口径排除。

验证口径：gcc-coverage 115/115 全绿、gcovr 100%（10702/10702 行 +
1589/1589 函数）；clang 树全新重建零警告、115/115 全绿。

## 诊断采样触发/下载入口：04 §7 控制面 MVP 切片（2026-09-26）

对齐 04-ops-control-plane §7 的职责拆分（采样/导出语义归 05，谁可以
触发采样、谁可以下载结果归 04）——上一批 TickDiagnostics 把采样触发权
显式留空，本批补上「按需触发一次采样 + 诊断产物查询/下载」：

1. **采样能力面（runtime 侧）**：`ITickProfiler` 能力接缝（daemon 只依赖
   接口）+ `TickProfiler` 实现——作为 `ITickObserver` 挂到 TickScheduler，
   `trigger()` 开固定 tick 数窗口（句柄触发时即占号，自 1 单调递增；
   0 = 拒绝），窗口内逐 tick 记实测耗时，收满固化产物（JSON 快照：
   samples + min/max/avg + 超阈值样本数 + 窗口墙钟跨度）。窗口进行中
   重复触发被拒（限流口径：同一时刻只允许一个窗口）；产物进环形存储
   （满后丢最旧，与审计环形同一容量纪律）。不做 flamegraph 全量采样、
   不做 EntityProfiler→负载反馈链（归 03 谱系）。
2. **RPC 面（control 侧）**：machine.profile.trigger → .ok（句柄十进制
   串）；machine.profiles → .ok（产物元数据清单 JSON 数组）；machine.profile
   → .ok（按句柄下载产物字节）。`DiagnosticsPolicy` 独立策略（与
   ExecPolicy/ProcessGovernPolicy 分离，授权口径互不牵动）：canTrigger
   （写侧，消耗主机性能预算）/ canAccess（读侧，产物明细外泄面）；
   缺省全拒。守卫顺序：载荷解析 → 鉴权 → 句柄存在性（鉴权先于存在性，
   下载不向未授权方泄漏句柄空间信息）。`tickProfiler = nullptr` = 入口
   关闭：machine.profile.* 视同未知方法，落统一 unknown-method 错误臂
   照记审计（语义决策：入口未开无"诊断动作"发生，不计入诊断拒绝指标）。
3. **遥测/审计面**：触发/访问各一对接受/拒绝计数（machine_profile_*，
   snake_case 同族）；触发全程 `SpanScope("machine.profile.trigger")`
   （拒绝也入 span：accepted/reason/handle；查询/下载只读不进 trace）；
   触发、查询、下载及全部拒绝逐条入审计（command = profiler.trigger /
   profiler.list / profiler.download）并推中心审计环形。
4. **测试**（TickProfilerTest 3 用例新增 + MachineDaemonTest 24→25）：
   采样全生命周期（触发占号/空闲 tick 忽略/统计逐字段/恰等于阈值不算
   慢）、限流与无效配置、环形逐出与句柄增长；E2E 走真实 TCP + 真实
   TickScheduler 驱窗——入口关闭视同未知方法、stranger 三连拒（鉴权先
   于存在性）、授权触发→限流重触发→双窗口产物→清单/下载→未知句柄/
   畸形/空载荷拒绝、指标四路增量（2/2/4/5）、3 个触发 span 属性、审计
   13 条按发生序落账（中心 + 本地环形镜像）。

**边界与缺字段（如实记录，未编造）**：
- 04 把限流与超时归 Gateway 层——本切片的"限流"只有 agent 侧口径
  （单窗口进行中 + 产物环形容量）；Gateway 限流/超时未做。
- §6.1 权限分级（ReadOnly/Operator/Admin）简化为 canTrigger/canAccess
  两个独立授权位，角色模型未引入（触发⊇访问的组合角色用并集表达）。
- §6.2 审计字段的 requestId（请求关联 id）协议层仍缺，审计条目暂无
  请求级唯一标识。
- 阈值/窗口为构造期配置；04「采样、阈值、限流」的在线配置热改未做。
- 产物仅存 agent 本地，跨机器回传中心的通道未做（当前下载即控制面
  RPC 直读 agent）。
- slow_threshold 只进产物统计字段（slow_samples），全局慢告警仍归
  TickDiagnostics，两处口径刻意分开。

验证口径：gcc-coverage 115/115 全绿、gcovr 100%（10534/10534 行 +
1576/1576 函数）；clang 树零警告、115/115 全绿。

## ~~慢 tick 诊断：只读诊断采样切片（2026-09-26）~~ ✅ 已完成（2026-09-26）

~~对齐 05-telemetry §6 Diagnostics Profiling 的 MVP 最小切片（"慢 tick 诊断"
三主题之一；flamegraph 采样与分阶段归因属 Phase 2 更强采样策略，留待
后续）。上一批指令的既定退路切片，本轮正式落地：

1. **边界盘点**：TickScheduler::runOnce 已自带整 tick span（tick_duration_ms
   属性）与同名直方图观测——遥测联动已覆盖耗时分布；真正缺口是**阈值
   分类与告警**。实体级负载信号归 EntityLoadProfiler（另一篇谱系），本
   切片只做 tick 粒度"慢"判定，职责无重叠。
2. **ITickObserver 观察者接缝**：调度器只广播事实（tick 序号 + 实测
   耗时），策略不进调度器；观察者不持有、tick 线程内联回调（实现不得
   阻塞），建议调度线程启动前挂接，nullptr = 关闭广播。
3. **TickDiagnostics 只读诊断组件**：Config.slowThreshold（默认 200ms
   ≈ 2 × 默认 tick 间隔 100ms，超预算两倍即信号；恰好等于不算——严格
   大于）；slow_tick_count 计数器（snake_case 同族口径）+ 结构化警告
   runtime.slow_tick（tick_index / duration_ms / threshold_ms 属性，tick
   线程通常无 span 上下文、日志自动关联语义不变）；slowTickCount() /
   lastSlowTickIndex() 只读视图（后者仅在 slowTickCount()>0 时有意义）。
   只读留痕不触发控制动作——采样触发与结果下载权归 04-ops-control-plane，
   后续接入。
4. **测试**（RuntimeFrameworkTest +观察者接缝块、新增 TickDiagnosticsTest
   4 用例）：预算内/恰好等于阈值静默（计数不动）、慢 tick 计数 +2（回
   落不累加）、警告逐属性断言、默认阈值 200ms 两臂、调度器接缝大阈值
   零误报（无时钟竞态）与解绑停止广播、getter/序号递增。

验证口径：gcc-coverage 114/114 全绿、gcovr 100%（10308/10308）；clang 21
树零警告、114/114 全绿。~~

## ~~主机级非受控进程的策略化治理（Linux 起步，2026-09-26）
~~
~~对齐 06-machine-agent-and-host-ops §7 MVP "process list / state / pid" 的
~~控制面治理切片。选型理由：枚举面已存在（supervisor 的 /proc 全主机枚举
~~+ managed 标记，machine.snapshot 已带全表），真实缺口是**处置侧**——
~~supervisor stop/restart 只对受管子进程生效，主机上其他进程无策略化出口；
~~且治理与 execute 的授权语义必须分离（execute 是受管进程编排，治理是
~~对主机其他进程的越权面，风险等级与审计语义不同，不得共享白名单）。
~~
~~1. **能力面**：`IProcessSupervisor::terminateUnmanaged(pid)`（Linux
~~   SIGTERM；受管进程一律拒绝——唯一停止入口是 stop/restart，绕开会脱离
~~   reap/记账；非 Linux 平台暂不开放处置，枚举照常）；`currentProcessId()`
~~   跨平台 pid 助手。`IMachineAgent` 增 `enumerateHostProcesses()`（全主机
~~   表，不采资源摘要）与 `terminateHostProcess(pid)`（纯能力转发，策略
~~   判定在 daemon 侧）。
~~2. **策略面**（与 ExecPolicy 分离的 `ProcessGovernPolicy`）：
~~   trustedComponents（枚举/处置共用来源白名单）+ killableNames（处置目标
~~   comm 名精确匹配白名单）；双空集 = 全拒（安全缺省）。
~~3. **RPC 面**：machine.processes → machine.processes.ok（非受控进程 JSON
~~   数组，不含受管进程、不带 managed 标记——受管编排走 execute，治理视图
~~   不重复暴露）；machine.terminate → machine.terminate.ok（pid 十进制
~~   串载荷；守卫链：载荷解析 → 来源白名单 → pid 存在 → 非自身 → 非受管 →
~~   名单匹配；守卫与 supervisor 登记簿互为纵深）。
~~4. **遥测/审计面**：枚举与处置各一对接受/拒绝计数（snake_case 同族）；
~~   处置全程 `SpanScope("machine.terminate")`（拒绝也入 span：accepted/
~~   reason/pid 属性），枚举只读不进 trace；全部尝试（含拒绝）照记审计
~~   （command = process.list / process.kill）并推中心审计环形。
~~5. **测试**（MachineDaemonTest 21→24、MachineAgentTest 8→10）：三块 E2E
~~   ——枚举策略门（受信含自身 pid、受管被过滤、非受信拒绝、审计切片）、
~~   处置守卫七连（空载荷/非数字/非受信/未知 pid/自身/受管/名单外）、
~~   接受处置（未登记 fork sleep 经 SIGTERM 终结，waitpid 断言信号死因，
~~   span accepted/ok、审计带 pid）；agent 转发与 terminateUnmanaged 三态
~~   （未登记成功/受管拒绝/pid_max 上界外恒 ESRCH）。
~~
~~验证口径：gcc-coverage 113/113 全绿、gcovr 100%（10281/10281）；clang 21
~~树零警告、113/113 全绿。

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
- ~~非受控全局进程的策略化管理与权限边界~~（2026-09-26 完成：控制面
  execute 双白名单 + 主机级非 agent 进程的策略化治理——machine.processes
  / machine.terminate RPC、ProcessGovernPolicy 独立策略、全动作审计入
  中心环形）
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

