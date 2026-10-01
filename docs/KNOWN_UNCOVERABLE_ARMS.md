# KNOWN_UNCOVERABLE_ARMS——分支覆盖不可达/噪声臂登记

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
组补指令级背书（gcov -b -c 逐臂 taken 计数）；Entity.h 12 条模板
内联副本边补登记 inline-noise。总缺口 192→204。

类别：`variant-noise`（LogAttribute::Value 四路 std::variant 转换构造
被归因到调用行的 0-对，gcc 库内联）；`inline-noise`（std 库内联机械
弧：hash 桶/字符串 SSO 副本/三目汇合副本）；`structural`（结构性不
可达：守卫臂被上游不变量封死、平台窗口、枚举全覆盖的隐式出口）。

## control

### MachineDaemon.cpp
- 171-173/191-193/373-374/396/473/478/731/733/819/949/1010/1089/1167/
  1241/1290-1292/1371-1372/1442-1443/1561-1562/1675-1676/1711-1712/
  1743/1799/1842 [variant-noise / inline-noise]：log attr 行与三目/
  字符串拼接汇合弧。【2026-09-30 重编】源文件在异步批量接线后行号
  整体下移（171 起同值、362-363→373-374 与 385→396 为 +11、457 及
  之后 +16），定性不变。【2026-10-01 复核】余 35 行全部满足噪声签名
  （行已执行 + 存在 0 弧）；396 系 logInfo("machine.daemon.stopped")
  行，其 0 弧对为 attr variant 未用替代的构造弧（字符串实参恒走
  string 替代），stop() 守卫本身在 395 且 sink×身份四象限全测。
   【2026-10-01 续轮指令级背书】gcov-14 -b -c 逐臂 taken 口径全 35 行
   （74 弧）逐项甄别：全部存活非 throw 臂 taken 全 >0（无一条 taken 0
   的语义真臂——无翻案候选，不硬造用例），全部零弧皆 never executed
   死块。按死块来源拆分定类：variant-noise 60 弧——LogAttribute 单值
   构造行（171/172/191/192/373/473/731/1290/1291/1371/1442/1561/1675：
   branches 6/7 + call 8 死块，即未用 variant 替代构造）与 log 调用
   行（173/193/374/396/478/733/819/1010/1089/1292/1372/1443/1562/
   1676/1743/1799/1842：calls 14/15 + branches 16/17 + call 18 死块，
   1712 为三目续行形态略异但同属死块）；inline-noise 14 弧——三目/
   字符串拼接行（949/1167 各 2 弧为 to_string 侧 SSO/堆副本死块，
   三目真/假语义臂 22/8 与 12/8 全走；1241 四弧为 key+"="+value 拼接
   双死块，三目 20/2 全走；1711 四弧 + 1712 两弧为 draining 嵌套三目，
   parsed 8/4 与 draining 6/2 全走）。行执行计数最低 2 次（1290/
   1291），余皆 ≥6。
【2026-10-01 翻案】原三条结构臂补测收口移出：25 toBytes 空串防御臂
（空载荷剖面下载——EmptyPayloadProfiler 真 + 空串驱动 memcpy 跳过，
响应合法零字节）、545 auditSink publish 空身份跳过臂（空 hostname 假
agent + 双 sink，发布跳过且中心环形保持空）、1011 `ok ? 0x01 : 0x00`
假臂（假 agent 处置恒失败——ESRCH 竞态语义的确定性替身，0x00 响应
可及；"恒竞态不可稳定注入"论断不成立）。

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
  显示对应行执行计数 >0 但分支/调用计数全 0。

---

维护纪律：新收割轮次在本文件补登记（文件:行 + 类别 + 一句背书），
并在 todo.md 对应批次交叉引用；摘除某条（臂变可测并补测）时同步删除。
