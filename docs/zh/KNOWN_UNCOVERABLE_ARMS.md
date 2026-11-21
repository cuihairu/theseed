# KNOWN_UNCOVERABLE_ARMS——分支覆盖不可达/噪声臂登记

> English | **中文**

行/函数门禁恒 100% 后的分支层（信息性口径）余量台账。三轮分支收割
（control / login / runtime+foundation+db，见 todo.md 各批次）逐行
arc 级甄别后的排除清单：**下列臂不为补测而硬凑**——测试不可达或纯
库机械弧。每条带定性类别与背书来源；产品码在场的豁免均带行内
`LCOV_EXCL_BR_LINE` / 码注释。

测量口径【2026-09-30 增补】：本台账行号以「清扫 gcda 后连跑两轮全量
ctest 的并集」为准——单轮扫描会因 e2e 进程内 tick 循环的时序臂整域
缺席而虚报缺口（首轮 DBApp 曾伪报 88 条，二轮并集归零），勿据单轮
数据补登记或硬凑测试。

【2026-10-01 重审翻案轮】重点大户逐条重审：MachineDaemon 77→74 弧
（25/545/1011 三条结构臂补测收口移出）、OpsControlCenter 34→33（311
entityId 命中臂补测收口移出）、ProcessPortScanner 13→11（172 fd 耗尽
臂、255 >64KB 截断臂补测收口移出，源内两处 LCOV_EXCL 区随之摘除、
其后行号 -2 重编）、LoginApp 32 不变（论证补强留档）。34 条 OTel
noise 不动。总缺口 198→192，行/函数门禁双 100% 不回退。

【2026-10-01 噪声台账复核续轮】未裁定结构臂/行批次：HostProbe 318
翻案收口移出（sumNetworkBytes 移出匿名命名空间作 probe_detail 测试
缝 + 合成流直测四防御臂，源内三组 LCOV_EXCL 区摘除转实测——总行数
12212→12215、分支分母 9008→9018，新增弧全数覆盖、零弧计数不变）；
SessionStore 44 照实重记（原描述误指向 41 行拼接，44 行实为 zadd
索引键固定字面量构造）；TcpConnection 165 由 inline-noise 重归类为
structural 平台窗口（ENOTCONN 真臂，Linux 本机实证不可达）；Ops 150、
TickDiagnostics 39、TcpConnection 116、LoginApp 结构组、Scanner 平台
组补指令级背书（gcov -b -c 逐臂 taken 计数）。【2026-10-01 照实
重记】"Entity.h 12 条模板内联副本边、总缺口 192→204" 系非门禁口径
单次扫描——现行两轮并集门禁实测该 12 行弧对全非零（231 taken
[2,26]、326-331 [18]），Entity.h 真零弧 21 条（188/252/287/295/
305/323，非 throw）全数被 --exclude-unreachable-branches 排除、不入
缺口；总缺口维持 192（门禁 branches 8826/9018、行 12215/函数 1689
双 100%、GATE_EXIT=0）。

【2026-11-15 G2-B 重基】本轮（2026-11-09 daemon 拆分 + 本轮
ProcessPortScanner 105/106 行内豁免）后的门禁分母：**行 12501/12501、
函数 1721/1721 双 100%（GATE_EXIT=0）**；分支信息性口径 9119/9347（本地两轮并集；
gcc-coverage 树按 CI parity 配置：gcc-14 +
CMAKE_DISABLE_FIND_PACKAGE_PostgreSQL=ON）。CI run 43671280096 复现同值
（12501/12501、1721/1721、分支 9116/9343）。本地 9347 与 CI 9343 的 4 弧分母差系
既有偏差（baee29a 已见），本轮未追——分支为信息性口径。上方数字的口径范围：本轮
仅重测 MachineDaemon 族六个 TU，其余条目仍为 2026-10-01 坐标与弧数、未重测；
gcda 快照未留存，故上文「总缺口 192」仍是 2026-10-01 基线、**非今日计数**——勿当现值读。

类别：`variant-noise`（LogAttribute::Value 四路 std::variant 转换构造
被归因到调用行的 0-对，gcc 库内联）；`inline-noise`（std 库内联机械
弧：hash 桶/字符串 SSO 副本/三目汇合副本）；`structural`（结构性不
可达：守卫臂被上游不变量封死、平台窗口、枚举全覆盖的隐式出口、
异常展开落地 pad——仅语句内抛出 bad_alloc 级异常才可达的清理分派，
测试不注入异常不可测）。

## control

### MachineDaemon 族（MachineDaemon.cpp + Process/Session/Diagnostic/Config/Detail）

> [2026-11-15 G2-B 重采] 2026-11-09 的拆分（commit 62fedfa）把原单文件 1861 行
> MachineDaemon.cpp 逐字搬移（零逻辑改动）为 `src/control/machine/` 下六个 TU。
> 本轮按台账基线重采 74 弧：清扫 gcda 后连续两轮全量 ctest（125/125 全绿，
> PID namespace），每轮后各留一份 gcovr 快照——两轮零弧集合完全一致（本轮无
> 首轮伪缺口）。守恒精确：拆分前后均为 74 弧 / 35 行。本轮后门基准：
> 12501 行 / 1721 函数双 100%（GATE_EXIT=0）、分支 9119/9347 信息性。
> 并集集签名抽查与旧形态一致：全部缺口行已执行（最低 1 次，位于
> machine.config.applied 行）、
> 零弧全为成对 never 死块（attr+call 行 2/2 形、三目/拼接行 4/4 形）、无
> throw 旗标零弧、语义臂两轮全走（Process:127 三目非零弧 11/4/4/4/11；
> Session:442 嵌套三目 4/2/3/1/4/4/2/4/2；Config:53 拼接 10/1/10/10/1/1/
> 10/10/1；Diagnostic:171 三目 6/4/6/4/4/6）。2026-10-01 的定性（结构性异常
> 展开落地 pad；拆分前 171/373 死块的 objdump 背书——endbr64 + 构造完成
> 标志 test/je + 条件 ~basic_string）原样沿用：同一批行、同一生成机制，
> 只是坐标迁移。

拆分后坐标，按 TU [structural]：

- `MachineDaemon.cpp` 43/44/45（logExecuteRejected attr + "machine.execute.rejected"
  warn）、90/91（start "machine.daemon.started"）、113（stop
  "machine.daemon.stopped"）、190/195（relayArtifacts "machine.profile.relayed"）、
  456/458（execute 审计 "machine.execute"）、544（"machine.unknown_method"
  warn）——22 弧 / 11 行。
- `MachineDaemonProcess.cpp` 127（terminate args 三目
  `parsed.ok ? std::to_string(...) : ""`）、188（"machine.terminate" 调用）
  ——4 弧 / 2 行。
- `MachineDaemonSession.cpp` 102/103（kick-session）、173/174（list-sessions）、
  292/293（kick-sessions）、406/407（extend-sessions）attr+call 对、442/443
  （draining 嵌套三目）、474（"machine.set-draining" 调用）、530
  （"machine.shutdown" 调用）、573（"machine.session.notify.dropped" warn）
  ——28 弧 / 13 行。
- `MachineDaemonDiagnostic.cpp` 93（"machine.profile.trigger" 调用）、171
  （profile-download args 三目）——4 弧 / 2 行。
- `MachineDaemonConfig.cpp` 53（config.apply args 拼接 `parsed.key + "=" +
  parsed.value`）、102/103/104（"machine.config.applied" attr + 调用）——
  10 弧 / 4 行。
- `MachineDaemonDetail.cpp` 67/68/69（rejectControlAction attr + 调用）——
  6 弧 / 3 行。

沿革：[2026-09-30 收割] 登记 77 弧；[2026-10-01 翻案轮] 三条结构臂补测收口
移出（toBytes 空串防御臂；auditSink publish 空身份跳过臂；`ok ? 0x01 : 0x00`
假臂——ESRCH 竞态语义的确定性替身），77→74；同日机制归因从 variant-noise/
inline-noise 翻案为 structural EH 落地 pad（指令级实证），本轮在新坐标复核
同一结论。

### OtlpTraceExporter.cpp
【2026-09-30 第五批分支收割】21 条登记缺口中 3 条真臂已补测收口：
315 from_chars 解析成功但尾部残留臂（`http://…:6553x` 拒绝表新增）、
375 状态码后紧随 >'9' 字符的 break 臂（`HTTP/1.1 200x` 纯函数断言
新增）、409 真实传输收到 1xx 状态的 ≥200 假臂（回环收集端 Reply100
直测 postViaTcp 新增）——余量重扫后登记如下：
- 222-223/250-252/256/503/506 [variant-noise]：log attr 行（invalid
  endpoint warn、enabled 六属性、failed warn）。
- 337 单 span 重载委托行首尾 2 弧 [inline-noise]：std::vector 列表
  初始化机械弧，行已执行；语义臂由批量重载共用实现覆盖。

### OtlpMetricsExporter.cpp
- 161-162/170-175/250/253 [variant-noise]：log attr 行（invalid
  endpoint warn、enabled 四属性、failed warn）。【2026-09-30 第五批
  分收割登记】

### OpsControlCenter.cpp
- 62-64/103/121/210/230/259/286/297/337/358/380 [variant-noise]。
- 98 try_emplace 六 0 弧 [inline-noise]：std 库内联（hash 冲突/重哈希），
  inserted 真/假两语义臂均已测（12 真/1 假）。
- 150 `!insertionOrder_.empty()` 假臂 [structural]：order 与 nodes_ 同
  步维护（插入同步成对增长、逐出成对弹出），逐出路径只在 nodes_ 超
  容量增长后可达，届时 order 必非空——空表防御臂无路径。【2026-10-01
  续轮指令级背书】16 次逐出评估全走真臂，假臂 taken 0。

### ProcessPortScanner.cpp
- 96 迭代中途 error 臂 [structural]：进程消失/权限竞态不可稳定注入
  （LCOV_EXCL 区）。【2026-10-01 续轮指令级背书】3,768,066 次迭代
  评估 error 臂 taken 0。
- 105/106 fd 关闭竞态臂（`if (linkError)` 真臂及其 `continue`）
  【2026-11-15 G2-B 轮补登，structural 平台窗口】：仅当被扫进程在扫描中途
  关闭 fd 时 read_symlink 才失败——不可稳定注入；PID namespace 沙箱
  /proc 安静，本地三轮全量真臂 taken 恒 0（每轮 4,151-8,836 次迭代），
  CI 真机每轮至少命中一次（行门 12502/12502，run 43626120187）。本轮起
  `continue;` 行加行内 LCOV_EXCL_LINE（与 96 中途出错臂同待遇，仅豁免
  行门；取行内形态以固定 105/106 坐标）；105 真臂保持为在册活性分支
  缺口。本轮之前本地行门读 12501/12502=99.9% 即本臂所致，非回归——
  勿补测试。
  【2026-11-15 背书边界】上行行内豁免的背书**仅对 gcovr 8.6 实证成立**
  （本地两轮并集门）。CI 以 `pipx install gcovr` 无版本钉安装
  （build.yml），产出 12501/12501 基线时（run 43671280096）为 **8.8**；
  该标记在 CI 上并不独立受验——真机上此臂被走到，第 106 行无论有无
  豁免都被覆盖。若 CI 的 gcovr 将来不认行内 LCOV_EXCL_LINE，本地 8.6 门
  仍绿、只有 CI 行计数会动——假定等价前先钉版本。
- 111 `native.back() != ']'` 真臂 [structural]：/proc fd 链接恒规范
  （socket:[inode]），畸形形态防御。【2026-10-01 续轮指令级背书】
  3,487,977 次读取评估真臂 2 弧 taken 0。
- 134/138/142/187/189/198/200/212/219/221/229/230/232/233/238/240
  [structural]：资源异常与平台窗口臂（/proc 恒在、合法 fd 的
  fcntl/getsockopt/setsockopt 不失败、Linux 非阻塞 TCP connect 恒
  EINPROGRESS、回环已连接 send 失败需即刻 RST）。【2026-10-01 翻案
  轮重编】原 172 fd<0 臂与 255 >64KB 截断臂补测收口（setrlimit 压
  RLIMIT_NOFILE 注入 fd 耗尽；70KB 回环应答注入读循环越界截断——
  总量守卫的语义本就是为异常大响应兜底），源内两处 LCOV_EXCL 区
  摘除、两行转入实测，其后行号 -2 位移（本条行号已按新源校准）。
  行内豁免对 gcovr 8.6 生效的勘误（2026-09-30 批，详见 todo.md）
  不变：登记仍是定性/背书台账，非 gcovr 排除的唯一依据。【2026-10-01
  续轮指令级背书】187 行 26 次评估 3 弧、142/212/219/238 各 1 弧
  均 taken 0。
- 197 connect 同步成功臂（0F）[structural]：Linux 非阻塞 connect 完成
  握手才返回 0，回环握手异步，恒 EINPROGRESS（2026-10-01 本机实证：
  非阻塞 connect 对 127.0.0.1:9/:1 与 240.0.0.1 恒 errno 115）。
  【2026-10-01 续轮指令级背书】26 次 connect 评估同步成功臂 taken 0。

### HostProbe.cpp
- 318 冒号前全空白行防御臂【2026-10-01 翻案收口移出】：sumNetworkBytes
  系纯字符串计算面（istream 可注入），匿名命名空间私属才是补测唯一
  障碍——移出为 `probe_detail::sumNetworkBytes`（声明入 HostProbe.h，
  全平台编译）后合成流直测四防御臂（表头无冒号/冒号前全空白/rx 列
  非数值/tx 列缺失截断，HostProbeTest::testSumNetworkBytesSyntheticStream）。
  源内三组 LCOV_EXCL 区（BR_LINE/EXCL_LINE/EXCL_START/STOP）随之摘除
  转实测：门禁行数 12212→12215、分支分母 9008→9018，新增 10 弧全数
  覆盖、零弧计数 192 不变，行/函数双 100% 不回退。原论断"匿名空间
  解析器无注入缝、数据行恒有接口名"随测试缝成立而失效。

## login

### LoginApp.cpp
- 243/354 [structural]：LCOV_EXCL_BR_LINE 已标——Linux 非阻塞 connect
  恒 EINPROGRESS，factory/真实 connect 失败臂不可达（2026-10-01 本机
  实证背书：非阻塞 connect 对 127.0.0.1:9/:1 与 240.0.0.1 恒
  errno 115，与 ProcessPortScanner:197 同证）。
- 306-307/413-414/474-475/478/493-494/499-500 [variant-noise]。
- 318/425 `!hub_` 臂 [structural]：tick() 的 `if (hub_)` 包住全部
  supervise 调用，stop() 置 Backoff 后 tick 不再进监督。【2026-10-01
  续轮指令级背书】318 执行 1039 次：machineHost 空臂 390/非空 649
  全走，`!hub_` 真臂 taken 0。
- 320/427 switch 隐式出口弧 [structural]：LinkState 三枚举全覆盖且无
  default。【2026-10-01 续轮指令级背书】320 三 case 计数 52/504/93、
  隐式出口 taken 0。
- 322/429/328/435 null-transport 臂 [structural]：PendingAck/Up 态
  transport 必非空（attempt 成功才置态，失败/markDown 即 reset+Backoff）。
  【2026-10-01 续轮指令级背书】322 于 52 次 PendingAck、328 于 504 次
  Up 评估中 null-transport 臂均 taken 0；isConnected 真/假两臂覆盖
  （50/2 与 494/10）。

## foundation

### SessionStore.cpp
- 44 尾部 4 零弧 [inline-noise]【2026-10-01 续轮照实重记】：
  `redis_->zadd(std::string(kIndexKey), token, indexScore())` 的索引键
  固定字面量构造弧——kIndexKey 为 14 字符 constexpr（恒走 SSO，堆
  分配/拷贝替代块为死路；token 以 const 引用传入，本行不构造）。
  gcov 指令级：行执行 302 次，&& 两侧与各语义臂对全非零，branches
  19/20/22/23 + call 21 never executed。原描述"`kSessionKeyPrefix +
  token` 拼接 SSO/堆副本库弧"误指 41 行拼接语句（该行无零弧），兹
  更正；双写契约语义臂已全测（SessionStoreTest）。

## runtime

### TickDiagnostics.cpp
- 39 [variant-noise]：logWarn 三属性行。【2026-10-01 续轮指令级背书】
  行执行 42 次；calls 14/15 + branches 16/17 + call 18 never executed
  = std::variant 构造的未用替代路径（三属性源码定型 int64，其余
  替代的构造块恒不执行）。

### TcpConnection.cpp
- 116 pending 空转臂 [structural]：SO_ERROR ∈ EINPROGRESS 系只在握手
  跨 tick 未落定的真实网络出现，内核回环握手同步完成；语义集由
  TcpConnectionTest 直测 connectStillPending 锚定，码注释背书。
  【2026-10-01 续轮指令级背书】4 次 SO_ERROR≠0 评估全走断连臂，
  EINPROGRESS 复检假臂 taken 0。
- 165【2026-10-01 续轮重归类：inline-noise → structural 平台窗口】零
  弧为 `||` 链尾 notConnectedYet() 真臂（ENOTCONN 兜底 return false）：
  wouldBlock 假侧 20 次全落假臂（EPIPE 真错误断连由 RST send 臂断言
  钉死），真臂 taken 0——Linux 握手窗口内非阻塞 send 恒 EAGAIN
  （2026-10-01 本机实证 errno 115，与 LoginApp:243/Scanner:197 同证），
  ENOTCONN 命中面在 macOS（码注释自认"命中率高，Linux 窗口极窄"），
  系平台窗口结构臂而非库机械弧。

### Entity.h
- 231-232/326-331（成对）[inline-noise]：模板 `onPropertyChanged` 与
  `bindTypedMethodHandler` 多实例化内联展开的副本边——源码定型参数
  仅触发已测实例化，其余替代路径的构造/分支块为死码。gcov 指令级
  显示对应行执行计数 >0 但分支/调用计数全 0。【2026-10-01 照实
  重记】本条 12 弧在现行两轮并集门禁口径不可复现——231/232/326-331
  弧对全非零（231 taken [2,26]、326-331 [18]），行执行 >0 而弧亦
  >0；Entity.h 现行真零弧 21 条在 188/252/287/295/305/323（非
  throw），全数被 --exclude-unreachable-branches 排除、不入门禁
  缺口。"总缺口 192→204" 据此照实更正：门禁口径维持 192（实测
  branches 8826/9018）。

---

维护纪律：新收割轮次在本文件补登记（文件:行 + 类别 + 一句背书），
并在 todo.md 对应批次交叉引用；摘除某条（臂变可测并补测）时同步删除。
