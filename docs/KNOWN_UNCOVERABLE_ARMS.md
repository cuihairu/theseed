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

类别：`variant-noise`（LogAttribute::Value 四路 std::variant 转换构造
被归因到调用行的 0-对，gcc 库内联）；`inline-noise`（std 库内联机械
弧：hash 桶/字符串 SSO 副本/三目汇合副本）；`structural`（结构性不
可达：守卫臂被上游不变量封死、平台窗口、枚举全覆盖的隐式出口）。

## control

### MachineDaemon.cpp
- 25 toBytes 空串防御臂 [structural]：全部调用点传非空串。
- 171-173/191-193/373-374/396/473/478/731/733/819/949/1010/1089/1167/
  1241/1290-1292/1371-1372/1442-1443/1561-1562/1675-1676/1711-1712/
  1743/1799/1842 [variant-noise / inline-noise]：log attr 行与三目/
  字符串拼接汇合弧。【2026-09-30 重编】源文件在异步批量接线后行号
  整体下移（171 起同值、362-363→373-374 与 385→396 为 +11、457 及
  之后 +16），定性不变。
- 545 `!nodeId_.empty()` 假臂 [structural]：auditSink publish 跳过臂，
  nodeId 取自真机 hostname 恒非空（空身份臂已由 f3d7a47 在
  relayArtifacts 守卫面清掉，publish 面的对称臂由同一探针事实封死）。
- 1011 `ok ? 0x01 : 0x00` 假臂 [structural]：terminate ok=false = 枚举
  ↔处置固有竞态，码注释自认 MVP SIGTERM 语义。

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
- 311 entityId 命中臂 [structural]：ProfileMeta 的 entity/entityType
  维度无生产者（码注释自认「如实落空」），中心侧查询恒空。

### ProcessPortScanner.cpp
- 96 迭代中途 error 臂 [structural]：进程消失/权限竞态不可稳定注入
  （LCOV_EXCL 区）。
- 111 `native.back() != ']'` 真臂 [structural]：/proc fd 链接恒规范
  （socket:[inode]），畸形形态防御。
- 134/138/142/172/174/189/191/200/202/214/221/223/231/232/234/235/240/
  242/255 [structural]：资源异常与平台窗口臂（/proc 恒在、合法 fd 的
  fcntl/getsockopt/setsockopt 不失败、Linux 非阻塞 TCP connect 恒
  EINPROGRESS、回环已连接 send 失败需即刻 RST、/health 响应恒小于
  2KB）。【2026-09-30 勘误】行内豁免（LCOV_EXCL_* / LCOV_EXCL_BR_*）
  对 gcovr 8.6 同样生效（A/B 实证：HostProbe:318 挂/撤 BR_LINE 对
  同一 gcda，gcovr JSON 臂 gcovr/excluded 随之翻转；本条 200 的 0T
  弧现由 BR_LINE 排除出分母，SocketDetail.h 同值链 22 弧同证）——
  旧记「不识别、仅 lcov 生效」系观察混淆，详见 todo.md 2026-09-30
  批。登记仍保留：定性/背书台账与维护纪律载体，非 gcovr 排除的
  唯一依据（200 于当轮从 LINE 对齐为 BR_LINE，行口径与分支口径的
  豁免各归其位）。
- 199 connect 同步成功臂（0F）[structural]：Linux 非阻塞 connect 完成
  握手才返回 0，回环握手异步，恒 EINPROGRESS。

### HostProbe.cpp
- 318 冒号前全空白行防御臂 [structural]：sumNetworkBytes 匿名命名空间
  解析器只吃真实 /proc/net/dev 流（istream 无注入缝），数据行恒有接口
  名（码注释自认）。行豁免在 319 continue（LCOV_EXCL_LINE），分支归因
  本行——BR_LINE 已照 ProcessPortScanner 200 先例对齐。

## login

### LoginApp.cpp
- 243/354 [structural]：LCOV_EXCL_BR_LINE 已标——Linux 非阻塞 connect
  恒 EINPROGRESS，factory/真实 connect 失败臂不可达。
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
