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
  步维护，nodes_ 超容量时 order 必非空；空表防御臂无路径。
【2026-10-01 翻案】原 311 entityId 命中臂 [structural] 补测收口移出：
中心过滤与数据来源解耦——假生产者填 entityId 后实体维命中即出行、
未命中仍过滤（生产侧恒空仍是事实，但查询臂可及，非结构不可达）。

### ProcessPortScanner.cpp
- 96 迭代中途 error 臂 [structural]：进程消失/权限竞态不可稳定注入
  （LCOV_EXCL 区）。
- 111 `native.back() != ']'` 真臂 [structural]：/proc fd 链接恒规范
  （socket:[inode]），畸形形态防御。
- 134/138/142/187/189/198/200/212/219/221/229/230/232/233/238/240
  [structural]：资源异常与平台窗口臂（/proc 恒在、合法 fd 的
  fcntl/getsockopt/setsockopt 不失败、Linux 非阻塞 TCP connect 恒
  EINPROGRESS、回环已连接 send 失败需即刻 RST）。【2026-10-01 翻案
  轮重编】原 172 fd<0 臂与 255 >64KB 截断臂补测收口（setrlimit 压
  RLIMIT_NOFILE 注入 fd 耗尽；70KB 回环应答注入读循环越界截断——
  总量守卫的语义本就是为异常大响应兜底），源内两处 LCOV_EXCL 区
  摘除、两行转入实测，其后行号 -2 位移（本条行号已按新源校准）。
  行内豁免对 gcovr 8.6 生效的勘误（2026-09-30 批，详见 todo.md）
  不变：登记仍是定性/背书台账，非 gcovr 排除的唯一依据。
- 197 connect 同步成功臂（0F）[structural]：Linux 非阻塞 connect 完成
  握手才返回 0，回环握手异步，恒 EINPROGRESS（2026-10-01 本机实证：
  非阻塞 connect 对 127.0.0.1:9/:1 与 240.0.0.1 恒 errno 115）。

### HostProbe.cpp
- 318 冒号前全空白行防御臂 [structural]：sumNetworkBytes 匿名命名空间
  解析器只吃真实 /proc/net/dev 流（istream 无注入缝），数据行恒有接口
  名（码注释自认）。行豁免在 319 continue（LCOV_EXCL_LINE），分支归因
  本行——BR_LINE 已照 ProcessPortScanner 200 先例对齐。

## login

### LoginApp.cpp
- 243/354 [structural]：LCOV_EXCL_BR_LINE 已标——Linux 非阻塞 connect
  恒 EINPROGRESS，factory/真实 connect 失败臂不可达（2026-10-01 本机
  实证背书：非阻塞 connect 对 127.0.0.1:9/:1 与 240.0.0.1 恒
  errno 115，与 ProcessPortScanner:197 同证）。
- 306-307/413-414/474-475/478/493-494/499-500 [variant-noise]。
- 318/425 `!hub_` 臂 [structural]：tick() 的 `if (hub_)` 包住全部
  supervise 调用，stop() 置 Backoff 后 tick 不再进监督。
- 320/427 switch 隐式出口弧 [structural]：LinkState 三枚举全覆盖且无
  default。
- 322/429/328/435 null-transport 臂 [structural]：PendingAck/Up 态
  transport 必非空（attempt 成功才置态，失败/markDown 即 reset+Backoff）。

## foundation

### SessionStore.cpp
- 44 尾部 4 零弧 [inline-noise]：`kSessionKeyPrefix + token` 字符串
  拼接 SSO/堆副本库弧；双写契约语义臂已全测（SessionStoreTest）。

## runtime

### TickDiagnostics.cpp
- 39 [variant-noise]：logWarn 三属性行。

### TcpConnection.cpp
- 116 pending 空转臂 [structural]：SO_ERROR ∈ EINPROGRESS 系只在握手
  跨 tick 未落定的真实网络出现，内核回环握手同步完成；语义集由
  TcpConnectionTest 直测 connectStillPending 锚定，码注释背书。
- 165 [inline-noise]：gcc `||` 链汇合副本弧（EPIPE 断连行为已由 RST
  send 臂断言钉死）。

---

维护纪律：新收割轮次在本文件补登记（文件:行 + 类别 + 一句背书），
并在 todo.md 对应批次交叉引用；摘除某条（臂变可测并补测）时同步删除。
