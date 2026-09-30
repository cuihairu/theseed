# TODO

## OTel 导出器：控制面 trace 的 OTLP/HTTP JSON 外发（2026-09-30）✓

遗留项「OTel 导出器仍开放」的 trace 半落地（05-telemetry §2.1 同步成文；
metrics 导出仍开放登记）。零第三方依赖与数据外发默认关闭为两条硬约束，
设计假设自行拍板如下：

1. **零依赖口径**：不引 OTel SDK / protobuf / abseil（vcpkg 工具链未装，
   引入需全量重建——沿 2026-09-26 导出面板的既定降级路径）；OTLP/HTTP
   JSON 报文手写编码，HTTP 传输复用仓内 `runtime::TcpConnection` 一次性
   阻塞 POST（`OtlpTraceExporter::HttpPost` 接缝供测试注入假传输）。
2. **数据外发默认关闭（硬要求）**：`Config.enabled` 缺省 false，不显式
   开启不安装不外发、零网络副作用；开启且 endpoint 合法即打启动日志
   `otlp.traces.export.enabled`，`data_scope` 属性明示导出范围（已完结
   span 的 name/traceId/spanId/parentSpanId/起止纳秒/attributes 分型值/
   resource service.name/scope 名；不导出结构化日志、metrics、请求与
   审计载荷），与 05 遥测 §2.1 文档同源。
3. **endpoint 仅 `http://<IPv4字面量>[:port][/path]`**（端口缺省 4318
   OTLP/HTTP 约定、path 缺省 /）：仓内 transport 无 DNS 与 TLS，需域名
   或加密在 endpoint 前置本地代理；IPv4 校验与 TcpConnection 的
   inet_pton 同口径（前导零、越界段、4+ 位段全拒）。
4. **每 span 一 POST、同步挂发射钩子**：timeout（缺省 500ms）即发射点
   阻塞上界，控制面每 RPC 一笔的低频可接受；失败（建连拒/对端先关/
   超时/非 2xx）丢弃 + `++exportFailed` + warn 带 http_status（0=无响
   应），2xx 计 exportedOk 且成功路径零日志；批量化/重试/异步队列后置
   登记。
5. **开启即全量导出无采样**；安装链式保留宿主既有 SpanEmitter（导出后
   照常转发），uninstall 与析构还原槽位——析构兜底卸载保证全局槽位绝
   不悬挂已亡实例的回调（测试进程实证过无兜底的悬挂 segfault）。
6. **配置面 = `MachineDaemon::Config::otlpTrace`**：daemon 是控制面 span
   属主；当前无生产宿主装配 daemon（仅测试实例化），配置面先行、装配随
   宿主落地。scope 名 theseed.control.tracing、kind=INTERNAL、status 不
   写（UNSET）；service.name 缺省 theseed。
7. **测试**：新 theseed_otlp_trace_exporter_test 19 例（resolveEndpoint
   正反 23+2 形态、状态行解码、默认关/非法端点、回环 e2e 200/500/拒绝
   连接/静默超时/关连接/拆包状态行/不可路由建连即败、JSON 转义全族、
   链式与还原、双 install 幂等、析构兜底、注入传输接缝、非有限 double
   文本化；MockOtlpCollector 多连接回环收集器，Linux 门控同 TCP 测试惯
   例）+ MachineDaemonTest 装配线用例（默认关不碰槽位、开启装导出器且
   data_scope 落日志、stop 还原）。

验证口径：gcc14-gate 全量重链 119/119 全绿、gcovr 行 100%（11970/11970）
函数 100%（1667/1667）双过门（分支 97.9% 8635/8818 信息口径）；clang-
debug 全量重链零警告、119/119 全绿。

## 上轮遗留收尾：LoadProfiler 时序 flake 修复 + gcovr BR 识别结论勘误（2026-09-30）✓

前轮收尾两个尾巴一次清：①macOS CI 腿偶发 theseed_load_profiler_test
失败（test_scope_with_empty_type_keeps_first_type 的 rawLoad 断言）——
RAII Scope 构造/析构各取一次 steady_clock::now()，块内无 sleepMs 的空
块两次 now() 间时钟未走则 elapsed=0（2026-09-30 macOS runner 实证；
当轮 rerun --failed 偶绿但根因未除）；②勘误「gcovr 8.6 不识别
LCOV_EXCL_BR_LINE」的第三轮结论——A/B 实测推翻，见下。

1. **BR 识别 A/B 实证**：HostProbe.cpp:318 对同一 gcda 挂/撤
   LCOV_EXCL_BR_LINE，gcovr 8.6 JSON 臂的 gcovr/excluded 随标记翻转
   （挂→True 出分母，撤→回缺口）。旁证三件：ProcessPortScanner L200
   的 0T 弧（旧结论引用的反例行）现由 BR_LINE 排除出分母、
   SocketDetail.h 同值链 22 弧全排除、行门 100% 本身依赖 START/STOP
   标记的未执行行被行排除（HostProbe 140/254/297 均 count=0 且
   excluded=True）。结论：LCOV_EXCL 家族（LINE/BR_LINE/START-STOP）
   对 gcovr 8.6 全部生效。
2. **历史结论更正**：本文件 2026-09-29 第三轮门禁纪录与
   docs/KNOWN_UNCOVERABLE_ARMS.md ProcessPortScanner 条的「不识别/
   仅 lcov 生效」表述就地勘误；runtime/foundation/db 轮「豁免 +8 弧」
   的原归因恢复成立（SocketDetail BR 豁免确在 gcovr 口径削减分母），
   第三轮对其的「gcda 副产物」再归因作废。登记台账保留——定性/背书
   与维护纪律载体，非 gcovr 排除的唯一依据。
3. **测试修复**：LoadProfilerTest.test_scope_with_empty_type_keeps_first_type
   三个空 scope 块补 sleepMs(1)（同文件其余用例既有惯例；该测试失败
   与当笔 HostProbe 提交零文件交集）。产品码零改动。

--- 2026-09-30 门禁纪录 ---
- 双树 ctest 118/118 全绿（gcc14-gate / clang-debug 均全量重链后复跑）。
- gcovr 行 11699/11699、函数 1644/1644 = 100% 硬门；分支 8427/8591
  （信息性；本笔测试改动不动分母）。
- mingw 口径不适用：产品码零改动，测试为平台中立 sleep 调整。

## 分支缺口第三轮复核：control 余量定性收口 + 排除清单固化（2026-09-29）✓

派发口径：剩余缺口 top 模块补单测，遵守排除口径勿硬凑不可达臂。基线
（f3d7a47）：分支缺失 166 = control 126（MachineDaemon 77 / OpsControl
Center 34 / ProcessPortScanner 13 / HostProbe 2）＞ login 32。

1. **复核范围与结论**：MachineDaemon 77 为上轮刚复核完的已定性余量
   （variant 噪声 30 行 + 结构性 8 行），不重复挖；主攻同模块内未深挖
   的 OpsControlCenter 34 与 ProcessPortScanner 13，login 32 抽查维持
   上轮定性（码内证据链完整）。
2. **OpsControlCenter 34 全定性**：29 缺失为 variant 噪声模板；L98
   try_emplace 六 0 弧为 std 库内联机械弧（inserted 语义两臂已测，
   12 真/1 假）；L150 order 空表防御臂（order 与 nodes_ 同步维护，
   超容量时 order 必非空）；L311 entityId 命中臂（无生产者，码注释
   自认如实落空）。真臂 0。
3. **ProcessPortScanner 13 全定性**：11 缺失已有 LCOV 豁免/EXCL 背书；
   新发现两处——L111 fd 链接畸形防御臂（/proc 恒规范）与 L199 connect
   同步成功臂（非阻塞握手异步恒 EINPROGRESS）补登记；**L200 漏标
   BR_LINE**（原 LINE 豁免不清分支弧），照同文件 5 处既有 BR_LINE
   惯例对齐——本轮唯一产品码改动（豁免注释，无行为变更）。
4. **真臂净增量 = 0**：三轮收割（control/login/runtime+foundation+db）
   后真臂已尽，本轮无新测试——符合「勿为凑数硬凑不可达分支」口径。
5. **排除口径固化**：新建 docs/KNOWN_UNCOVERABLE_ARMS.md——三轮留档
   臂全部归档（文件:行 + 类别 variant-noise/inline-noise/structural +
   一句背书 + 维护纪律），作为后续收割轮的既定排除清单。

--- 2026-09-29 门禁纪录 ---
- gcovr 行/函数门禁：行 11699/11699 = 100%（+1 = L200 从 LINE 对齐为
  BR_LINE 后该行判断语句回到行分母，仍全执行）、函数 1644/1644 =
  100%；分支 8427/8593、缺失 166 维持。
  【2026-09-30 勘误】本段原记「实证 gcovr 8.6 不识别
  LCOV_EXCL_BR_LINE，200 的 0T 弧仍在缺失内」系观察混淆——A/B
  实证（HostProbe:318 挂/撤 BR_LINE 对同一 gcda，gcovr JSON 臂
  gcovr/excluded 随之翻转）gcovr 8.6 实际识别 LCOV_EXCL 家族
  （LINE/BR_LINE/START-STOP）；本文件 200 的 0T 弧现由 BR_LINE 排除
  出分母（gcovr/excluded=True 可复核），SocketDetail.h 同值链 22 弧
  同证。由此再勘误：runtime/foundation/db 轮「豁免 +8 弧」的原归因
  恢复成立（BR 豁免确在 gcovr 口径削减分母），本段当时的「gcda
  副产物」勘误作废。详见 2026-09-30 收尾批。
- 双树 ctest 118/118 全绿（src 注释级改动，全量重链后复跑）。
- mingw 口径不适用：本轮产品码改动为注释级，测试零新增。

## runtime/foundation/db 分支补测：尾量三模块一次收割（2026-09-29）✓

口径同 control/login 批次：分支层收割（信息性，门禁不卡），arc 级甄别后
真臂全清、余量逐行留档。control 128 / login 32 均为前两批已定性余量，本
轮按缺失排名收剩余三模块（runtime 9 > foundation 8 > db 1，合计 18），
收尾 8。

1. **arc 级甄别**（JSON 弧 + 行级证据对照源码）：
   - TickDiagnostics.cpp:39（2 弧）：logWarn 三属性行的 LogAttribute::Value
     variant 转换构造库噪声，同 control/login 轮定性——留档；
   - SocketDetail.h:144 connectStillPending（4 分支）：与相邻 connectInProgress
     同构的 `EAGAIN || EWOULDBLOCK` 同值短路链——EWOULDBLOCK 真臂结构性
     不可达，**补上漏掉的同款 LCOV_EXCL_BR_LINE 豁免**（相邻 wouldBlock/
     connectInProgress 均已有）；EINPROGRESS/EINTR/EAGAIN 三真臂为运行时
     pending 窗口语义（SO_ERROR ∈ pending 系只在握手跨 tick 未落定的真实
     网络出现，内核回环握手同步完成）——纯函数直测锚定；
   - TcpConnection.cpp:113（现 116）：pending 空转臂，同上窗口论证，码
     注释补背书留档；
   - TcpConnection.cpp:129：recv 真错误（非 EAGAIN）臂——SO_LINGER{1,0}
     裸 socket 注入 RST 可达，真臂；
   - TcpConnection.cpp:162（现 165）：send 真错误（非 EAGAIN 且非
     ENOTCONN）臂——RST 后 write 走 EPIPE 断连可达，真臂；
   - SessionStore.cpp:44：save 的 `stored && zadd` 双写契约——set 失败
     短路（索引不写）与 zadd 失败上抛（会话键保留供重试）两真臂；尾部
     4 零弧为 `kSessionKeyPrefix + token` 字符串拼接 SSO/堆副本库噪声；
   - DBApp.cpp:124：非 file 臂——本构建无 SQL 时已知后端名被 #else 转回
     file，原假臂只剩垃圾后端名可达，且 store_ 保持 null 进运行期会在首个
     请求解引用崩溃（真实配置校验缺口）。
2. **产品码**（防御/豁免，无行为变更 1 + 缺口修复 1）：
   - SocketDetail.h connectStillPending 补同值豁免（照抄相邻格式与理由）；
   - DBApp::init 补 `else if (!store_)` 守卫：后端名拼错 init 即拒（统一
     错误处理），SQL 可用构建的成功路径不受影响（假臂 LCOV 豁免注明）；
   - TcpConnection.cpp 裁决点注释补 pending 窗口不可达论证。
3. **测试**：
   - TcpConnectionTest：connectStillPending 六值直测（pending 系/最终错误/
     成功）+ RST 双场景（裸 socket SO_LINGER{1,0} 触发——TcpConnection 的
     close 走 FIN 无法产生 RST）：A 先 pump 走 recv ECONNRESET 断连、B 先
     write 走 send 真错误断连；私有胶合头经测试目标私有 include 直测
     （不为测试提升头可见性）；
   - tests/foundation/SessionStoreTest.cpp（新，4 用例）：可注入失败的
     FakeRedisProvider——双写全成功往返+索引可见 / set 失败短路不碰索引 /
     zadd 失败如实上抛且会话键保留 / 空 token 拒绝；
   - DBAppTest 补 unknown storeBackend init 拒绝。
4. **余量留档（8）**：TickDiagnostics 39×2（variant 噪声）、TcpConnection
   116×1（pending 窗口，码注释背书）、165×1（gcc || 汇合副本弧，EPIPE
   断连行为已由断言钉死）、SessionStore 44×4（SSO 副本噪声）。

--- 2026-09-29 门禁纪录 ---
- gcc14-gate ctest：118/118 全绿（+SessionStoreTest）；clang-debug 全量
  重建后 118/118 全绿。
- gcovr 行/函数门禁：行 11698/11698 = 100%、函数 1644/1644 = 100%；
  分支 8423→8425（缺失 178→168；runtime 9→4、foundation 8→4、db 1→0），
  豁免 +8 弧（connectStillPending 同值链）。
- 中途坑：只构建三个测试 target 时其余链接 theseed_runtime 的二进制未
  重链，全量 ctest 新旧二进制混写 gcda → gcov stamp mismatch、四个重编
  TU 整体退出聚合（分支分母 8601→8352 假象）。清 gcda + ninja 全量重链
  + 重跑 ctest 后分母恢复 8593（-8 为新豁免弧），行 11694→11698（+4
  守卫）。
- mingw 离线：DBAppTest / SessionStoreTest / TcpConnection.cpp
  `-fsyntax-only -std=c++23 -Wall -Wextra -Werror` 零警告。

## login 模块分支补测：LoginApp 韧性臂收割（2026-09-28）✓

口径同上轮 control 批次：分支层收割（信息性，门禁不卡），arc 级甄别后真臂
全清、余量留档。login 起盘 40 缺失（27 缺失行），收尾 21 缺失行。

1. **arc 级甄别收尾**（对 27 缺失行逐行核 JSON 弧 + 源码）：
   - 噪声/豁免 13 行：243/354（LCOV_EXCL_BR_LINE 已标，Linux 非阻塞
     connect 恒 EINPROGRESS）、306/307/413/414/474/475/478/493/494/499/500
     （markDown/handleInvocation 行上 LogAttribute::Value 四路 variant 转换
     构造库噪声，同 control 轮定性）。
   - 结构性不可达 8 行：318/425（`!hub_` 防御守卫——tick() 的 `if (hub_)`
     包住全部 supervise 调用，stop() 置 Backoff 后 tick 不再进监督）、
     320/427（switch 隐式出口弧——LinkState 三枚举全覆盖且无 default）、
     322/429/328/435 的 null-transport 臂（PendingAck/Up 态 transport 必非
     空：attempt 成功才置态，失败/markDown 即 reset+Backoff）。
   - 真臂 4 处（6 分支）全清，见下。
2. **真臂补测（LoginAppTest）**：
   - machine 腿「臂 2b」：PendingAck 期间 transport 死（探针已发出、活性
     转假）→ 首个监督 tick 立降，不等 ack 宽限——与 probe-ack-timeout 的
     分野用 500ms 宽限 + 2ms tick 钉死（误走超时路径断言即败）；降后照常
     退避重连恢复（sends==1 复发探针）；
   - db 腿镜像「臂 2b」：同款编排（DMode::Silent→Canned）。
   - 新 TEST「machine push inside the dbRequest wait loop」：db Silent +
     machine 腿接线，推送落在 dbRequest 等待窗内 → 非 DB 源帧臂（L532 假
     臂）→ 交 handleInvocation 分发（unknown 计数 +1 锚定），查询照旧超时
     降级。
   - §8 kick 测试补活跃版 realm-mismatch 直调（原直调在会话已断后，走
     isConnected 短路——handleSessionRevoked 的 realm 比较假臂补齐）。
3. **留档余量（21 缺失行）**：全部属上述三类，不为信息性指标改产品码；
   分支缺失 186→178（login 40→32）。

--- 2026-09-28 门禁纪录 ---
- gcc14-gate ctest：117/117 全绿（含 e2e）；clang-debug 树 login 双测试绿
  （src 未动，行/函数分母不变）。
- gcovr 行/函数门禁：行 11694/11694 = 100%、函数 1644/1644 = 100%；
  分支 8415→8423（缺失 186→178，login 40→32），零新增豁免。
- mingw 离线：LoginAppTest.cpp `-fsyntax-only -std=c++23 -Wall -Wextra
  -Werror` 零警告。

## 覆盖率缺口定位 + control 模块分支补测（2026-09-28）✓

本轮按派发转覆盖率：跑覆盖率定位缺口最大模块，补 top1 单测。行/函数门禁
恒 100%（11694/1644），可定位的缺口信号在分支层（信息性口径，门禁不卡）。

1. **缺口定位（gcovr txt/json，与门禁同口径）**：分支缺失按模块聚合——
   control 134（62.9%）＞ login 40 ＞ runtime 9 ＞ foundation 8 ＞ db 1，
   合计 192（总 8601 分支）。top1 = control；文件分布 MachineDaemon.cpp 84
   ＞ OpsControlCenter.cpp 35 ＞ ProcessPortScanner.cpp 13。
2. **arc 级甄别**：逐缺失行对照 gcov JSON 的分支弧，把 43+17 行缺失分为
   三类——(a) 真产品臂；(b) 库机械噪声：log/attr 行上 `LogAttribute::Value`
   （std::variant 四路）转换构造与日志机制被归因到调用行的 0-对（约 36
   行，测试不可达、也不该为信息性指标改产品码）；(c) 结构性不可达臂（各
   有在码注释背书）。
3. **真臂补测（全清）**：
   - MachineDaemonTest「terminate guards」：pid 0 哨兵载荷（parsePidPayload
     的 pid==0 臂）+ 拒绝计数 8→9；
   - MachineDaemonTest「diagnostics profiling」：handle 0 哨兵载荷
     （parseHandlePayload 的 handle==0 臂）+ 计数表 14→15、accessRejected +6→+7；
   - MachineDaemonTest 新增「extend scope arms + entry gate」独立小盘（不动
     既有计数校准）：account 圈选未命中行（比较假臂）、"all" 首次到达循环体
     （此前全被信任门拦截）、TTL=0 续期入口关闭臂（extend 视同未知方法）、
     会话入口开启时的未知方法（短路链第三条件假臂）；
   - OpsControlCenterTest 新增「unbounded capacity (maxNodes 0)」：无界模式
     容量早退臂（默认 256，既有容量盘全部显式设上界）。
4. **确认为不可测并留档的臂**：toBytes 空串防御（全部调用点传非空串）、
   relayArtifacts/auditSink 的空 hostname 臂（MachineAgent 不可注入，真机
   hostname 恒非空）、terminate ok=false（枚举↔处置固有竞态，码注释自认
   MVP SIGTERM 语义）、profile query entityId 匹配臂（无生产者，码注释
   自认「如实落空」）。

--- 2026-09-28 门禁纪录 ---
- gcc14-gate ctest：117/117 全绿；clang-debug ctest：117/117 全绿。
- gcovr 行/函数门禁（build/gcc14-gate 树内跑）：行 11694/11694 = 100%、
  函数 1644/1644 = 100%，GATE_RC=0；分支 8409→8415（缺失 192→186，
  control 134→128），零新增豁免、零断言放宽。
- mingw 离线：OpsControlCenterTest 新增段 `-fsyntax-only -std=c++23
  -Wall -Wextra -Werror` 零警告（MachineDaemonTest 在 NOT WIN32 块内）。

## db 模块补测：DBProtocol + RemoteEntityStore 直测（2026-09-28）✓

覆盖率/台账 item 8 按指示继续暂停；本轮改挑排除 item 8 后剩余可达面中缺口
最大的模块补测试。选择口径（自假设，非交互判定）：

1. **缺口判定**：模块「src 行数/test 行数」比值 db 最低（0.88）。可达面
   过滤——MySQL/PostgreSQL 后端因本机 find_package 失败不编译（环境态，
   排除），DBProtocol.cpp（353 行）+ RemoteEntityStore.cpp（132 行）= 485
   行是剩余可达面中最大的无同名专测块；其余模块的无专测文件
   （MessageHeader/RateLimiter/SessionStore/ClientSession/SessionToken）
   均被多个测试 API 级广泛引用，无真缺口。
2. **tests/db/DBProtocolTest.cpp**（17 用例）：全消息 encode→decode 往返
   （64 位极值 id、空串、空列表、多属性 EntityData 含二进制 blob）；每个
   解码器逐前缀截断拒绝矩阵（任何严格前缀必须 false）；失败/未命中响应
   短路臂（success=false 不携带载荷、queryAccount not-found 与
   createAccount 失败置零哨兵）；listIds/listTypes/allocId 无参请求的空载
   荷契约（listIds 请求无专用解码器，按 DBApp 消费方同款 MemoryStream 读法
   往返）。
3. **tests/db/RemoteEntityStoreTest.cpp**（11 用例）：ScriptedDbTransport
   脚本化 transport（仿 LoginAppTest 的 FakeDbTransport 范式，单线程无锁）
   逐臂驱动：请求构造（组件路由/method/payload 可解回）、六操作正常往返、
   杂散应答丢弃不重发、静默超时六操作全降级（30ms 短预算，耗时下界钉住
   「真的在等」）、发送拒收立即降级（不进等待循环）、pumpFn 等待循环驱动、
   请求 flush transport。
4. CMake 挂接：两个 target 全平台无条件跑（纯 std + theseed 头，无 POSIX
   专属依赖）。不改 src——覆盖分母不变。

--- 2026-09-28 门禁纪录 ---
- gcc14-gate ctest：117/117 全绿（115 + 新增 DBProtocolTest/RemoteEntityStoreTest）。
- clang-debug ctest：117/117 全绿。
- gcovr 行/函数门禁（build/gcc14-gate 树内跑）：行 11694/11694 = 100%、
  函数 1644/1644 = 100%（分支 97.8% 信息性），GATE_RC=0。
- mingw 离线：两个新测试文件 `-fsyntax-only -std=c++23 -Wall -Wextra` 零警告。

## 规则 c 收尾：build_test 清理 + 过程端口扫描 flake 三层根因修复（2026-09-28）✓

1. **build_test/ 判定与清理**：未跟踪的 build_test/ 是手工配置的 Debug
   构建树（CMakeCache/CMakeFiles/24 个 .o，1606 文件均当日生成，非任何
   preset 的 binaryDir——CMakePresets 恒为 build/<presetName>）。已清理 +
   .gitignore 补 `build_*/`（与既有 `build-*/` 对称，注明 preset 口径）。
2. **门禁复跑实caught flake**：theseed_process_port_scanner_test 高载下
   30 连跑挂 2（此前各轮全绿——失败集中于并行会话重载窗口）。逐层取证：
   - 测试侧：轮询预算浅（10s 兜底在慢轮下只给 5-7 次尝试）→ 复验制改造
     （与 HostProbeTest::testRealProcSources 同款纪律：断言一字不动，首
     预算未命中取完整预算复验，连续两预算未命中才判失败）+ 轮询间隔退
     避 + ChildStopGuard（FAIL 路径随行 stop 子进程，防 120s×1000tick/s
     泄漏进程成为负载放大器）+ 子进程 tick 1ms 密跑加速排空；
   - 产品侧（真实缺陷）：probeProcessVersion 的阻塞 connect 无超时——
     头注释「回环 connect 不长阻塞」的前提在对端 backlog 饱和时不成立
     （SYN 重传实测单次卡 25s+，把 listProcesses 轮询整轮卡死）。改非
     阻塞 connect + poll，建连与收发共用同一超时；头注释同步。新增
     backlog 饱和注入测试钉住该臂（accept 队列塞满 → poll 满短超时）。

--- 2026-09-28 门禁复跑纪录 ---
- gcc-coverage ctest：115/115 全绿（RC=0，含 theseed_machine_daemon_test 34s），gcc-14 显式钉死。
- clang-debug ctest：115/115 全绿（RC=0，含 theseed_machine_daemon_test 33s）。
- gcovr 行/函数门禁：树内根跑行 11064/11064 = 100%、函数 1628/1628 = 100%（分支 97.6% 信息性）。
- mingw 离线：`x86_64-w64-mingw32-g++ -fsyntax-only -std=c++23 -Wall -Wextra` 零警告。
- 现状：工作树干净，仅 tests/core/EntityDefLoaderTest.cpp 诊断改进（非功能改动），git status --short 无未跟踪文件。

--- 2026-09-28 任务结束 ---
规则 c 收尾完成：无新增功能、无覆盖率工作（item 8 按指示继续暂停）、树净全绿。已 fetch --rebase 并推送。
   - **核心根因**（诊断实锤：子进程 alive、正常睡 tick、2s 长探测也
     空、手动 child-ops 却秒回）：startReplyServer 的 accept 线程是
     detach 的——负载下线程迟迟未跑时其 LISTEN fd 未关，此后
     supervisor.start() fork 把该 fd 继承给子进程；lookupPidPort 按
     「最小监听端口」反查即命中这个无主 listener（无人 accept，版本
     探测恒超时）。修法：ReplyServer 返回可 join 句柄，fork 前一律
     join（fd 随线程关闭）。
3. **验证**：外部 load 43-62 重载窗口 60 连跑 0 挂（修复前同窗口挂
   1-2）；极载（14 核烧满 + 4 并行实例 + 外载 40-60）0/8；双树全量
   gcc-coverage 115/115（gcc-14）+ clang-debug 115/115；gcovr 门禁行
   11064/11064 = 100%、函数 1628/1628 = 100%（分支 97.8% 信息性）；
   mingw 离线 `-fsyntax-only` 零警告。
4. **环境注记**：build/gcc-coverage 于 03:47 被并行会话就地重配（编译
   器改回 /usr/bin/g++、二进制与 gcda 尽失）——不对抗，门禁改用私有
   钉死树 build/gcc14-gate（gcc-14），后续轮次沿用该口径。

覆盖率/台账 item 8 按指示继续暂停；本批不含覆盖率类补测（新增的
backlog 饱和测试是产品 connect 超时分支的行为测试，随既有门禁口径）。

## 遗留事项收口：Windows 网络探针台账校准（2026-09-27，非覆盖率轮）

「遗留事项」清单里 macOS 批次留下的「Windows 网络探针仍缺，本条只承诺
Linux/macOS」尾注是陈旧记录——实现实际已由同日「Windows 等价网络流量
探针」批（85c6a58 + 40a77e7 + cbacb3d）落地并 CI 验证，本轮仅台账收口：

1. **实现现状核实**（无需新码）：`_WIN32` 胶合走 GetIfTable2 的
   MIB_IF_ROW2 64 位八位组计数（InOctets/OutOctets，static_assert 钉
   IF_TYPE_SOFTWARE_LOOPBACK），逐行经 probe_detail::windowsLinkCounters
   归一回环、aggregateLinkCounters 多网卡求和——与 Linux/macOS 同一份
   聚合口径；iphlpapi 链接在 windows 分支的 CMake 里。
2. **验证方式**（本机无 Windows，三层证据）：
   - 编译：仓库既定 windows CI job（windows-msvc-debug）以 MSVC 编译
     `_WIN32` 分支——85c6a58..cbacb3d 起历次 run 全绿（run
     36335150403 / 36344457694 均含该腿）；
   - 运行时：windows job 里 HostProbeTest 的 testRealProcSources 在
     真实 Windows 计数器上断言网络计数单调（平台中立断言 = 胶合首验），
     testWindowsLinkCounters 直测 ifType 24 归一 + 64 位透传全分支；
   - 本机离线：mingw-w64 交叉编译 `-fsyntax-only -std=c++23 -Wall
     -Wextra` 复跑零警告（真实 Windows 头复刻 MS SDK 守卫结构）。
3. **台账校准**：遗留事项「网络流量统计与多网卡聚合」补 Windows 完成
   注记；「等价主机探针」条目的「仍缺」尾注改指闭合批。
4. **顺手修 flake**（验证轮实caught）：clang 树全量跑 theseed_host_probe_test
   瞬态失败一次（复跑即绿，150 连跑不复现）。根因：testRealProcSources
   直读真实环境，单采样窗口撞上瞬态噪声（容器 veth 两读之间摘除使主机
   累计回退 / fd 短暂耗尽 / space() 瞬时错误）即误报。修法不改任何断言
   ——违例时取新鲜样本对复验同一组断言，连续两窗违例才判失败；探测臂
   不计失败，只有定论臂落 PASS/FAIL。平台中立，三腿 CI 同受益。

无功能改动（src/ 零改动）——HostProbeTest 的 Windows 归一单测为上批
既有，本轮随全量套件复跑（gcc-coverage 115/115、clang-debug 115/115、
门禁行/函数 100%）。

- 规则 c 收尾巡检（2026-09-27，@28f240a）：工作树干净、无遗留脏文件；双树全量 gcc-coverage ctest 115/115 全绿（71.9s）+ clang-debug 115/115 全绿（29.0s），gcovr 门禁（树内根跑，--merge-mode-functions=merge-use-line-min）行 11682/11682 = 100%、函数 1644/1644 = 100%、分支 97.8% 信息性——零失败零 flake，本轮无修复项。

- 规则 c 补巡检（2026-09-27，当前批）：工作树干净、无遗留脏文件；双树全量 gcc-coverage ctest 115/115 全绿（含 machine_daemon_test 179s）+ clang-debug 115/115 全绿，gcovr 门禁（树内根跑，--merge-mode-functions=merge-use-line-min）行 11052/11052 = 100%、函数 1628/1628 = 100%、分支 97.6% 信息性——零失败零 flake，本轮无修复项，覆盖率/台账 item 8 按指示继续暂停。

- 规则 c 收尾巡检（2026-09-28）：build_test/ 清理 + .gitignore 补 `build_*/`（与既有 `build-*/` 对称，注明 preset 口径）；process_port_scanner_test flake 三层根因修复——测试侧复验制 + 轮询退避 + ChildStopGuard + 子进程 tick 1ms 密跑；产品侧 probeProcessVersion 阻塞 connect 无超时（backlog 饱和时 SYN 重传卡 25s+），改非阻塞 connect + poll 统一超时；新增 backlog 饱和注入测试钉住连接超时分支。验证：双树全量 gcc-coverage 115/115 + clang-debug 115/115 全绿，gcovr 行/函数 100%（11064/11064 行、1628/1628 函数），mingw 离线零警告。覆盖率/台账 item 8 按指示继续暂停。

## §6.2 requestId 全链路关联：发起方铸造 + 中心环形按请求查询（2026-09-27）

巡检派发项。协议帧字段与 daemon 透传已由「Phase 2 余项」批落地，
本批补齐剩余半边——发起方铸造与请求级关联查询：

1. **铸造助手（协议层）**：RuntimeTransport.h +`mintRequestId()`——
   进程内单调递增、逐请求唯一，0 恒不返回（0 = 未携带哨兵）。唯一性
   口径沿用规格注记「逐请求唯一是发起方责任」；跨进程/跨重启唯一性
   由部署方保证（当前生产发起方均为单进程组件，审计环形容量有界，
   进程内唯一已覆盖对账窗口——假设已注明）。
2. **LoginApp 注册探针铸造**（生产侧唯一的 daemon 客户端帧）：探针
   machine.snapshot 发出前铸造 id，经 hub → daemon 审计条目透传
   （daemon 侧纪律不变：只透传不铸造）。snapshot 接受臂照旧不留审计
   （只读噪声纪律），拒绝臂与 machine.audit 面均携带该 id。
3. **中心本地动作铸造**：OpsControlCenter 的 queryProfiles /
   downloadProfileArtifact 每次尝试（含拒绝）铸一个 id 入审计条目
   （拒绝/接受臂共用）。当前无 Gateway 层调用方上下文（04 §4.2
   OpsCommandContext），接入后改由调用方供给——假设已注明。
4. **中心环形按请求查询**：OpsControlCenter::auditTrail(requestId)
   重载——按 §6.2 关联 id 取回同一请求在环形里的全部记录（跨节点
   聚合，追加序）；0 按哨兵语义命中未携带帧的记录（查询自然语义，
   非特殊分支——0 是可查询值）；未知 id 空。
5. **测试**：LoginAppTest 断言探针携带非零 id；OpsControlCenterTest
   补假件 id 重载 + 跨节点同 id 关联查询 + 哨兵 0 语义 + 未知 id 空。
   daemon 透传链（帧→审计条目→machine.audit JSON）既有 4242 用例
   不变。
6. **台账校准**：本文件多处「requestId 仍缺」为闭合前的陈旧记录，
   逐处改为指向闭合批，消除自相矛盾。
7. **CI 覆盖率腿加固**：build.yml Coverage gate 加
   `--merge-mode-functions=merge-use-line-min`——gcovr 8 起函数合并默认
   strict，头文件 `=default` 析构的隐式克隆按 TU 记到不同行直接抛
   GcovrMergeAssertionError（pipx 未钉版本会随上游升级踩雷）；该析构是
   LCOV 豁免的平凡空体，取最小行合并即可，行/分支计数不受影响。

- 字段形态假设：04 §6.2 只列字段名；§4.2 OpsCommandContext.requestId
  （string）是尚不存在的 Gateway 层上下文——沿用既有落地口径 u64 帧
  字段（04 落地对照、RuntimeTransport.h 注释同口径），不另设形态。
- 遗留：应答帧不带 requestId（规格未定义应答关联语义；LoginApp 以
  入站即链路活性判定，不依赖 ack↔request 对账）——Gateway 层接入时
  如需请求-应答对账再评审。

## macOS 等价主机探针 + CI 矩阵补 macOS（遗留 940 / 949 矩阵部分，2026-09-27）

闭环遗留第 940 条（主机探针缺 macOS）与第 949 条的矩阵部分：LocalHostProbe
补 Apple 胶合，跨平台 CI 矩阵新增 macos-latest job。

1. **macOS 缺口清单（探针分项逐一核对）**：CPU 使用率（Linux `/proc/stat`
   有、macOS 缺 → `host_statistics(HOST_CPU_LOAD_INFO)` 四态 ticks）；网络
   流量（Linux `/proc/net/dev` 有、macOS 缺 → `getifaddrs` AF_LINK 项，
   `IFF_LOOPBACK` 识别回环，读 64 位版 `if_data64` 计数——旧 `if_data` 的
   u_char 计数会回绕，node_exporter 同款读法）；内存使用率（Linux `sysinfo`
   有、macOS 原落 sysconf 存疑尾 → 显式 `host_statistics64(HOST_VM_INFO64)`
   的 active+wire+compressor 页 × 页大小 ÷ `hw.memsize`，与活动监视器同
   口径）；负载（getloadavg 本含 `__APPLE__`）、主机名（gethostname）、
   磁盘（filesystem::space）、平台串（detectPlatform→"macos"）、进程枚举
   （enumerateMacProcesses 走 popen ps 已有）均无缺口。端口占用扫描的
   macOS 实现属第 939 条既有边界（Windows/macOS 留空），本批不扩。
2. **口径单份：probe_detail 纯函数面（全平台编译）**：tick 拆分
   （splitCpuTicksApple）、回环排除与求和（流式 accumulateLinkCounters +
   批量 aggregateLinkCounters，排除判据只实现一份）、占比换算与分母防护
   （usagePercent）从胶合层抽出为平台无关纯函数，Linux 单测直接驱动全
   分支（覆盖门不破）；Linux 既有路径同步改走同一份函数（网络解析逐行
   走流式累积、sysinfo 内存比值走 usagePercent），行为零变化。刻意不给
   Linux 解析路径引入 std::vector——push_back 内联的增长/复用 STL 边在
   接口数少的主机上单臂可达，会新破 CI 分支 100% 门；流式累积只有用户
   侧双臂且真实 /proc/net/dev 恒同时命中两臂。macOS 胶合只剩 syscall
   读数搬运，`mach_host_self` 的 send right 每采样 `mach_port_deallocate`
   归还（不泄漏端口引用）。
3. **验证边界（如实）**：本机 Linux——macOS 胶合编译期隔离（`#if
   defined(__APPLE__)`），运行时未在本机验证过、也不声称验证过；由 CI 新增
   macos-latest job 首验（HostProbe 的真实源测试断言改为全平台中立：平台
   串 ∈ {linux,macos,windows}、内存/磁盘 ∈ (0,100]、网络单调不减，macOS
   runner 上即真实执行 Apple 胶合）。Linux 可测部分（聚合/拆分/占比/差值
   分支逻辑）新增三个纯函数直测用例。
4. **CI 矩阵（949 矩阵部分）**：linux(gcc-debug/clang-debug/gcc-sanitize)
   + coverage 门 + windows(msvc-debug) 之外新增 macos-clang-debug——复用
   既有 clang-debug preset（裸 clang++ + Ninja，macOS runner 原生满足），
   缺依赖的测试沿仓库既有环境门控自跳（MySQL 无 toolchain → 编译期回退
   FileEntityStore，与 linux/windows 同口径）。
5. **随批修复：Build and Test 的既有红基线（push 后如实暴露）**：本批
   push 时该 workflow 已连续数提交全 job 红（本地双树绿属盲区——本地无
   libpq/MSVC/macOS）。逐一根因修复：LoginApp 两处 std::byte 迭代区间
   构造 std::string（gcc-13 libstdc++ 走 char_traits::assign 直接编译
   失败，新版库的按位拷贝快速路径语义与显式 reinterpret 一致）；
   SessionStore 的 zrange 裸 -1 字面量（MSVC C4245，改 size_t 哨兵，
   与 RedisProvider.cpp 既有惯例同款）；tests 侧 -Wno-missing-field-
   initializers 未按编译器分流（MSVC D8021，加 NOT MSVC 守卫）；
   TransportHub.h 缺 #include <memory>（libstdc++ 经 unordered_map 传递
   引入，Apple libc++ 不引，全仓头文件扫描确认仅此一处）。
6. **随批修复之二：编译红修穿后各 job 首次跑到真实阶段，暴露五路既有
   问题（2026-09-27 第二轮）**：(a) coverage 门分母破约——ubuntu-24.04
   镜像自带 libpq，find_package 自动点亮 PG 后端，而 runner 无可连 PG
   服务器、测试自跳，PostgreSQL* 源码全量 0 覆盖进 gcovr 分母（实测行
   96.4%/分支 92.6%）；coverage job 显式
   -DCMAKE_DISABLE_FIND_PACKAGE_PostgreSQL=ON 恢复与本地同分母，非门禁
   job 保留 PG 参编（真实 libpq 的跨编译器编译验证在 gcc-debug 腿，该腿
   本轮实测 118/118 全绿）；(b) macOS：sysconf 兜底的 _SC_AVPHYS_PAGES
   是 Linux/glibc 扩展、Apple 头不提供，且该兜底原在平台链 #endif 之后
   全平台参编——收进 __linux__ 分支（Apple 分支本就全路径 return，行为
   不变）；(c) tests 的 -Wno-missing-designated-field-initializers 守卫
   方向写反（挂 Clang，实为 GCC 15 旗标；CI clang 18 对 -Wno-未知项直接
   -Werror）——改 check_cxx_compiler_flag 正向探测（配 -Werror 的
   CMAKE_REQUIRED_FLAGS；clang 对未知 -Wno- 只告警，负向探测不可靠）；
   (d) sanitize 红一：EntityDef 定长属性裸顺序 packing 把 Int64 摆在
   4 mod 8 偏移，PropertyBlock::get<T> 的 reinterpret 产生 misaligned
   reference binding——新增 alignmentOfType 按类型对齐摆放偏移（alignUp），
   两处 storageSize 硬编码断言均为自然紧凑布局、不受扰；线格式按
   propertyId+字节长度走、不嵌偏移，兼容性无扰；(e) sanitize 红二：
   fork→execl 与守护进程 /proc 枚举的固有竞态——exec 完成前子进程 comm
   仍是测试二进制名截 15 字符（恰为 "theseed_machine"），杀名单误拒；
   测试侧等待进程名落定再处置（Linux /proc/<pid>/comm 轮询；macOS
   proc_name，本机不可验证、由 CI macos job 首验）；(f) MSVC 双警升错：
   PostgreSQLConnection 的 PQftype size_t→int 实参（static_cast，本机无
   libpq 编译不可验证）与 RedisProviderTest 两处 zrange 裸 -1（同款
   size_t 哨兵）。以上两轮修复随批提交 52bdf63、4ea2f21；第三轮收尾
   （MSVC C4244 + libc++ <algorithm>）为 17f4bd2。
7. **随批修复之三 + coverage 门口径校准（2026-09-27 第三轮）**：(a)
   MSVC C4244——ProcessSummary.port 存储面加宽 uint32 后裸传
   probeProcessVersion(uint16)，显式收缩无损（取值恒来自端口扫描器的
   uint16，bind/htons 语义）；(b) libc++ 不经传递包含引入 <algorithm>
   ——EntityDefRegistryTest 的 std::sort 补显式包含；(c) coverage 门
   校准：`--fail-under-branch 100` 系 065a3e4 迁移期遗留配置、对现行
   代码从未绿过（先被编译红掩盖、后被 PG 分母污染掩盖）——gcc 对
   **有代码的行**同样记 STL 内联 cleanup landing pad 出边
   （`--exclude-throw-branches` 只剔带 throw 标记的边，pad 自身出边
   是 throw:false），叠加真实条件臂存量缺口，分支 100 不可达。对齐
   本文件既有口径（行/函数 100%、分支信息性）：coverage job 显式钉
   g++-14（gcc-13 给模板/默认参数残影多记 5 条伪 0 行：Entity.h
   :232/233 ×2、TickScheduler.h:56，gcc-14 起不再产出），加
   `--exclude-unreachable-branches`（纯花括号行上的编译器 pad 出边，
   真实分支不可能长在除花括号外无内容的行上），`--fail-under-branch
   100` 换 `--fail-under-function 100`（文档口径含函数门，此前反而
   未断言）。
8. **分支缺口建档（存量测试债，后续分批消化）**：gcc-14 + 双排除旗标
   后分支 97.8%（未剔时 96.5%/318 点；两旗标合计剔 132 点）。剩余
   186 点/101 行分型：(i) 编译器 cleanup pad——LogAttribute/audit
   初始化行、try_emplace 内联展开等，异常展开机制、任何测试不可达，
   约 45 点；(ii) 真实条件臂缺口约 140 点——MachineDaemon 解析短路
   链臂（213/236/281）与 ternary 失败臂（933/1151/1225/1695）、
   OpsControlCenter maxNodes/查询过滤臂、LoginApp 两腿链路监督状态机
   臂（316-326/423-433）、ProcessPortScanner 错误臂、EntityDefLoader
   trim 四类空白字符臂、SessionStore/DBApp/HostProbe 单臂——集中于
   本系列早批代码，非本批引入。后续按文件分批补测试消化，消化完
   可升回分支硬门。
9. **daemon 测试审计钉修复 + gcov 覆盖池恢复（2026-09-27 收尾轮）**：
   profile "9z" 畸形句柄拒收臂入测后漏拨中心审计环钉（13→14 条，
   `Expect[14]` 加第四条 `{"profiler.download", false}`，终止守卫臂
   换 `4294967295x` 真实垃圾尾）；此前的 gcovr 95.2% 判定为失败测试
   进程提前退出未 flush .gcda 的伪缺口（554/558 缺行全在
   MachineDaemon.cpp），非真实回退——修后全量 `-j1` 重跑覆盖池恢复
   行/函数 100%。随批外部提交：coverage 门 `--gcov-executable gcov-14`
   （6879766）、macOS 非阻塞 connect 状态机 + 全链 TCP_NODELAY
   （544e6ee）、TcpConnection recv 真实错误臂豁免（85712fa）、BaseApp
   SpaceId C4244（575fec0）、Linux 专用测试按 `UNIX AND NOT APPLE`
   门控（a2dfc91）。
10. **Windows 等价网络流量探针补齐（2026-09-27）**：胶合走 IP Helper
   API `GetIfTable2`（`MIB_IF_ROW2` 的 64 位 InOctets/OutOctets，旧
   32 位 dwInOctets 会回绕；表以 `FreeMibTableDeleter` RAII 归还），
   回环判据以 RFC 2863 ifType=24（`probe_detail::
   kIfTypeSoftwareLoopback`，Windows 胶合处 `static_assert` 对照 SDK
   宏 `IF_TYPE_SOFTWARE_LOOPBACK`）；行归一 `windowsLinkCounters` 与
   聚合口径 `aggregateLinkCounters` 全平台单份，Linux 单测直测全分支
   （回环/物理/大数透传/归一入聚合）。链接：WIN32 下 theseed_control
   链 iphlpapi（对齐 theseed_runtime 的 ws2_32 先例）。胶合验证升级：
   本机装 mingw-w64 交叉编译器对真实 Windows 头离线编译该 TU（而非
   纯盲写等 CI）——实证根因：netioapi.h 类型块整体套在 _WS2IPDEF_
   守卫下，先含 iphlpapi.h 走 `__IPHLPAPI_H__` 捷径跳过 ws2ipdef.h
   自包含即整段缺声明；规范序 winsock2→ws2tcpip→windows→iphlpapi→
   netioapi 后 `-Wall -Wextra` 零警告通过（成员名 NumEntries/Table、
   InOctets/OutOctets 均经真实头核对）。

**边界与遗留（如实记录）**：
- ~~Windows 网络等价探针仍缺~~：**已补齐**（item 10）。Windows 胶合
  本机不可编译验证，沿用 macOS 批次口径：纯函数 Linux 单测全分支直测
  已过，胶合由 CI windows leg 编译并端到端首验。
- Windows 侧 GetIfTable2 失败兜底返回 {0,0}（与 macOS getifaddrs 失败
  臂同口径），系统调用失败臂 CI 不可定向注入。
- macOS 侧 `if_data64` 字段布局、`HOST_VM_INFO64` 口径、页大小换算均为
  文档/通行实现口径，本机不可运行验证，以 CI macos job 首跑为准。
- 内存占用不含 unloaded/file-backed 页（与活动监视器"已用内存"同近似，
  非精确账单）。

验证口径：gcc-coverage 与 clang 双树 cmake --build + ctest 全绿；
gcovr 行/函数 100%（口径含带理由 LCOV_EXCL，门禁见各批记录）；
CI 五平台六 job（linux×3 + coverage 门 + windows + macos）以 push 后
gh run watch 为准。

## db 腿运行期韧性：断链退避重连 + 登录查询自愈（04 §8，2026-09-27）

把上批遗留的「db 腿传输仍是静态单次连接」闭环：LoginApp 对 DBApp 的
出站腿补与通知腿同款监督（登录数据面的传输层自愈，DBApp 重启后无需
人工重启 LoginApp）。

1. **复用同款监督（LoginApp 私有面）**：通知腿的三段状态机枚举更名
   LinkState（PendingAck/Up/Backoff）供两腿共用；db 腿五函数与通知腿
   一一对应（attempt/schedule/confirm/markDown/supervise）。断链即摘除
   死 peer + logWarn(cause=transport-lost/probe-ack-timeout) + 计数
   login_db_link_down_count；退避 base 起步、翻倍封顶 max、恢复复位
   （Config 三参数 dbReconnectBaseDelay 1s / dbReconnectMaxDelay 30s /
   dbProbeAckTimeout 5s，无魔数）；每次尝试独立重调 dbTransportFactory
   （seam 语义更新：重连每次铸造新 transport，与 machine 腿同款）；
   seam 返空/探针发不出只告警排重试、不重复计 down。init 里 db 腿首试
   失败不再中断路径（旧代码 connect 失败直接 return，跳过 ops 接线）：
   转入监督状态机运行期自愈。
2. **探针选型：db.listTypes**（不是 queryAccount）：只读、DBApp 恒
   应答；关键是其应答方法 "db.listTypes.ok" 不与任何登录请求的应答
   匹配串重叠——迟到的探针应答绝不会被在途 dbRequest 误认作登录应答
   （queryAccount 探针则与登录查询同串，存在错配窗口）。应答经
   handleInvocation 消费并记 login.db.link.ready，不落入未知 method
   计数。DBApp 侧对新连接的注册仍由其 hub 首请求 sourceComponent 自报
   （与 attachServerTransport 同机制，零新协议）。
3. **拉取式活性口径**：db 腿无推送面，Up 证实点有两处——排空面的探针
   应答（tick → drainInvocations）与 dbRequest 等待循环里收到的任何
   DBApp 应答（真实登录的应答本就是链路可用的最强证据，就地证实）。
   断连窗口内 peer 已从 hub 摘除：dbRequest 立即 NotConnected → 既有
   "database unavailable" 降级（计数口径不变），不阻塞 tick、不排队
   重放——沿用请求-应答面的超时降级语义，非通知腿的 best-effort。
4. **测试**：LoginAppTest 桩面新增 db 韧性块五臂（FakeDbTransport 补
   isConnected/alive/sends/probe 观测面，探针经 createMode 维编排：
   Canned=应答/Silent=吞/Closed=发不出）——Up 断链→退避重连→探针重发
   →恢复、ack 超时臂、seam 返空臂（失败尝试≠断链）、探针发不出臂
   （不残留空注册）、恢复后登录查询真的走通（功能面验收）；既有 db
   桩面（hit/auto-register/超时/杂散/Closed/null seam）零回退。
   **场景 H（真实 TCP）**：file 后端 DBApp 下线 → 监督检出（down 计数）
   → 同端口重启 → 无人工干预自动重连重注册（up 计数）→ 同账号重登走
   query-hit（存储行跨重启保留，应答沿恢复的双向链路回来）。场景 A–G
   零回退。
5. **计数命名口径**：派单写的是 db_link_down_count/db_link_up_count
   简写，按仓库既有 login_<腿>_link_<事件>_count 家族落为
   login_db_link_down_count / login_db_link_up_count（与通知腿成对）。

**边界与遗留（如实记录）**：
- 断连窗口内的登录请求直接失败（database unavailable），不排队重放
  ——登录是同步请求-应答面，重试属客户端语义（与通知腿"漏送不补发"
  同理：恢复的是链路不是历史）。
- 心跳仍是 NetworkTransport 层 30s 控制通道消息，非应用层探活。

验证口径：gcc-coverage 与 clang 双树 cmake --build + ctest 全绿；
gcovr 行/函数 100%（口径含带理由 LCOV_EXCL，门禁见各批记录）。

## 通知腿运行期韧性：断链退避重连 + 订阅自愈（04 §8，2026-09-27）

把上批遗留的第 1 条（hub 无重连/心跳，daemon 重启后 LoginApp 要人工重启
才恢复订阅）闭环：

1. **监督状态机（LoginApp 私有面）**：PendingAck（已连已发探针，等首个
   入站证实）→ Up（活性证实）→ Backoff（断链/超时，退避到点重连）。
   活性判定两条腿：**transport 实况**（IRuntimeTransport 补
   isConnected 虚接口——缺省真（内存实现无断开概念），NetworkTransport
   按 socket 实况 override：对端 EOF 由 hub 泵转假、TCP 层既有 30s
   心跳让无 EOF 的半开随读写错误收敛）+ **入站证实**（daemon 方向
   任何入站——探针应答或推送——都把 PendingAck 升级为 Up；注册在传输层，
   与策略门独立，machine.error 应答同样证明推送通道可用）。**connect
   返回不作数**：Linux 非阻塞 connect 恒 EINPROGRESS（连到死端口也
   "成功"），故每尝试带 machineProbeAckTimeout 应答窗，超窗判未接通。
2. **退避与 best-effort 语义**：断链即摘除死 peer（不留会持续丢推送的
   僵尸注册）+ logWarn(cause=transport-lost/probe-ack-timeout) + 计数
   login_machine_link_down_count；重连窗口 base 起步、每次翻倍封顶
   max、链路恢复复位（Config 三参数 machineReconnectBaseDelay 1s /
   machineReconnectMaxDelay 30s / machineProbeAckTimeout 5s，无魔数）。
   重连 = 重新调用 machineTransportFactory（与 db 腿同款注入 seam——
   每次尝试独立铸造，桩可脚本化失败序列）或新建 TcpConnection →
   connectPeer 覆盖注册 → 重发 machine.snapshot 探针 → daemon 侧 hub
   经首请求自报重新注册订阅。恢复计 login_machine_link_up_count。
   断连期间登录面照常（通知丢失按既有 best-effort 语义只由 daemon 侧
   丢弃告警兜底）。
3. **失败尝试 ≠ 断链事件**：seam 返空 / 探针发不出（Closed）只告警排
   重试、不重复计 down；探针发不出的尝试还须摘除刚注册的死 peer。
4. **测试**：LoginAppTest 桩面新增韧性块五臂——Up 断链→退避重连→探针
   重发→应答恢复（恢复后不再重连）、ack 超时臂（活性真不应答两连击）、
   重连遇 seam 返空、重连遇发不出 transport、恢复后 revoked 推送照常
   关闭活登录（功能面而非计数面验收）；接口缺省活性断言（内存 transport
   恒真）。**场景 G（真实 TCP）**：daemon 下线 → 监督检出（down 计数）
   → 同端口重启（SO_REUSEADDR）→ LoginApp 无人工干预自动重连重注册
   （up 计数）→ 运维单踢沿恢复的订阅送达 → 匹配连接被关闭、双端计数、
   不匹配者无感。既有桩面（探针四臂/分发/畸形/未知、联动、选领域回写）
   与场景 A–F 零回退。
5. **选型记录**：不做订阅补发协议（吊销事实以存储为准，list-sessions
   对账仍是漏送兜底——重连恢复的是订阅不是历史）；db 腿同款监督未做
   （同性质扩面，dbRequest 自带超时降级，非订阅面，风险口径不同）。

**边界与遗留（如实记录）**：
- 断链窗口内 daemon 推的通知不可恢复（无补发/重放），仅告警计数；
  运行期运维对账仍走 machine.list-sessions。
- 心跳是 NetworkTransport 层既有 30s 控制通道消息，非应用层探活：
  半开链路靠 socket 错误收敛，无独立心跳超时参数。
- db 腿传输仍是静态单次连接（见上 5 选型）。

验证口径：gcc-coverage 与 clang 双树 cmake --build + ctest 全绿；
gcovr 行/函数 100%（口径含带理由 LCOV_EXCL，门禁见各批记录）。

## 通知通道的 LoginApp 生产接线（04 §8 Phase 2 收尾，2026-09-27）

把上一批遗留的第 1 条（推送只到 daemon 为止，LoginApp 侧只有落点）接通，
并修出接通时暴露的两处口径缺陷：

1. **LoginApp 的 hub 入站分发面**（此前只有 dbRequest 的出站面）：
   - `drainInvocations()`：tick 里在 `hub_->tick()` 之后逐条排空发到本
     组件的 RuntimeInvocation（与 DBApp::processMessages 同款循环）；
   - `handleInvocation()`：machine.session.revoked → 解载荷
     （account/realm）→ `handleSessionRevoked` 关匹配的活跃登录连接；
     探针应答（machine.snapshot.ok / machine.error）记联动状态；其余
     method **记日志 + 计数**（login_unknown_invocation_count）不静默
     丢包，也不回 machine.error——回包会与 daemon 的未知方法臂互弹成环；
   - 载荷解析：紧凑 JSON 取串，转义集与 control 侧 escapeJsonString
     严格对齐（\\ \" \n \r \t），越集转义/未闭合/悬空转义一律按畸形拒收
     （login_session_notify_malformed_count，宁拒不猜——通知是
     best-effort，拒收有日志有计数，不会静默错配账号）；命中时记
     login.session.revoked（account/realm/closed 三属性，无令牌原文）。
2. **传输腿与注册（选型理由）**：LoginApp 出站连 daemon（hub
   connectPeer）并发一条 **machine.snapshot 注册探针**自报身份——daemon
   侧 hub 沿用既有 `attachServerTransport` 的「首条请求 sourceComponent
   自报注册」接缝（与 DBApp 同机制），**不新增注册协议、不新增端口**。
   探针本身即 §8 只读探活语义：应答 ok/error 顺带暴露策略接线状态。
   machineHost 空 = 不接线（缺省安全，联动是增强不是登录前提）；连不上
   只告警跳过不阻断登录面（与 db 腿的失败即弃不同：联动是 best-effort
   增益，登录是主职责）。app 可用 `--machine-host/--machine-port` 打开。
   注入 seam `machineTransportFactory` 与 dbTransportFactory 同款。
3. **修出的缺陷一：领域口径两侧不一致（联动对真实客户端会漏关）**——
   通知的 realm 取自 SessionStore 行，而登录只写基础会话（realm 空），
   绑定的 realm 却在 SelectRealm 时补上了真实领域：真实客户端
   「登录 → 选领域」后收到吊销通知，account 对上、realm 对不上
   （空 vs 真实领域），连接不会被关。修法：选领域成功时把 realm 写回
   同一会话行（先 load 再改写，**过期行不因选领域复活**；TTL 按
   config_.sessionTtl 重写，进入领域即该领域会话起点，运维续期策略
   extend-sessions 不受影响）。顺带修掉 list-sessions 的 realm 字段
   恒为空的问题（该字段此前是空壳）。
4. **修出的缺陷二**：apps/loginapp/main.cpp 在 `std::move(config)` 之后
   打印 config.listenHost（移动后字符串成员为空，打印串首段恒空）——改为
   move 前取局部副本，并补 machine-link=on/off 状态行。
5. **测试**：
   - LoginAppE2ETest **场景 F（真实 TCP 端到端，本批验收主线）**：真
     MachineDaemon（真 LocalHostProbe + LocalProcessSupervisor）+ 真
     LoginApp 出站连 daemon（注册探针 → daemon hub 注册 20）+ 两个真实
     客户端登录并各选领域 realm1 → 运维客户端（Operator + NodeOpsPolicy
     白名单）发 machine.kick-session → 断言：daemon 应答 kick-session.ok、
     machine_session_notify_count +1、**erin 的连接被推送关掉**、
     frank 的连接不受影响、login_session_revoked_count +1、会话行确已
     消失（吊销事实以存储为准）；再发未知令牌 kick → machine.error 且
     不关任何连接；最后 tick 清扫后绑定不再命中（无悬垂键）。
   - LoginAppTest 桩面：探针四臂 + 工厂空臂（ack/reject/静默/连接被拒/
     工厂返回 nullptr）、推送分发（全转义字符集账号 → 解析还原后精确
     命中并关闭）、畸形载荷五臂 + 未知 method 臂的计数断言。
   - LoginAppTest 选领域补写存储行：登录行 realm 空 → 选领域后同行为
     realm1（list-sessions 同源可见）→ 未登录连接选领域不建行 → 过期行
     不复活（advanceClock）。
   - LoginAppTest 既有桩面用例（carl@default / amy@空领域 精确命中等）
     未回退；场景 E（真实 TCP 生命周期清扫）未回退。
6. **口径沿用不改**：clear temporary bans 不做（无封禁存储前置）、长度
   指纹同长歧义不改、枚举顺序不承诺时序。

**边界与遗留（如实记录）**：
- ~~通知通道的 LoginApp 生产接线未建~~：**已完成**（本批实现：入站分发面 `drainInvocations()` + `handleInvocation()`，机器注册探针通过 `machineHost/machinePort` 配置建立出站连接，daemon 侧 `notifySessionRevoked` 通过 hub 入站分发至 `handleSessionRevoked`；领域口径已在 `handleSelectRealm` 与 `handleSessionRevoked` 间对齐，`list-sessions` 域字段恒为空由存储行决定）。本批验收通过真实 TCP E2E 场景：daemon 吊销 → 通知送达 LoginApp → 匹配的活跃登录连接被关闭、计数增量、不匹配的连接不受影响。

  本批交付内容详见交付摘要。
- 通知仍 best-effort 无重试/确认（设计如此：吊销事实以存储为准）。
- 匹配口径是 account+realm 精确匹配：一个账号同时开多个连接（多设备）
  且其中一条被单踢时，通知关的是该账号**全部**匹配领域的连接——存储行
  按 token 区分、通知面按 account+realm 聚合（协议出口令牌只回显长度
  指纹，不可逆，无法逐 token 定位）。属有意取舍，已在 §8 对照记明。
- 会话续期策略值与 LoginApp 会话 TTL（config_.sessionTtl）仍是两处
  策略：前者是运维续期上限（daemon 侧），后者是登录/进领域时签发的会话
  寿命（LoginApp 侧）——统一为单一策略源属扩面，未做。
- 畸形载荷拒收后不重试、不告警请求方（单向推送无回包语义），只记
  日志与计数。

## Phase 2 余项：会话续期策略 + 踢人联动通知 + requestId（04 §8，2026-09-26）

沿上一批的会话运维命令面继续，把简报 Phase 2 余项中前置已备的三件
（requestId 判断本轮可补，一并做了）：

1. **machine.extend-sessions（≥Operator，≙ 会话续期策略）**：
   - 作用域选择器与应答口径同 kick-sessions（all | account=<id> |
     realm=<id>，extended/missing 分列、指纹脱敏、入口关闭 = TTL 为 0
     或未配 sessionStore 视同未知方法）；
   - **续期 TTL 是策略值不是调用方参数**：refresh 到
     Config.sessionRenewalTtl（扩策略须改配置评审，与 §6.3 白名单同
     纪律）；用途 = 维护窗口前批量滑动过期，防无关批量掉线；
   - 部分失败臂（枚举后续期前会话被并发摘除/过期）用 FlakyDelProvider
     的 armFailExpire（expire 按后缀失败一次）确定性复现；
   - 计数 machine_extend_sessions_{accepted,rejected}_count 成对，
     审计 args = `scope=<sel> extended=N missing=M`。
2. **踢人联动通知（machine.session.revoked，best-effort 推送）**：
   - daemon 侧 notifySessionRevoked：单踢（load-before-revoke 先取
     account/realm，"unknown session token" 与 "session vanished before
     revocation" 具名区分）与批量踢吊销成功后，向
     Config.sessionNotifyComponent 推 JSON 载荷
     `{account, realm, session=长度指纹, reason=operator.kick |
     operator.kick.batch}`；无重试/无确认——目标未在 hub 注册即丢弃并
     logWarn（吊销事实以存储为准，重连后可经 list-sessions 对账），
     送达计数 machine_session_notify_count（只计送达，无接受/拒绝语义）；
   - LoginApp 侧落点 handleSessionRevoked（公开方法）：按
     account+realm 关闭匹配的活跃登录连接，返回关闭数，计数
     login_session_revoked_count；定位面 = 新增 bindings_ 注册表
     （登录成功登记 account，SelectRealm 成功补 realm——只在已登录
     连接上补，不给陌生键开洞；realm 空串 = 未选领域，与存储行同形），
     cleanupDisconnected 与连接同步出表（真实 TCP 生命周期在 E2E
     测试覆盖，不留悬垂键）。
3. **requestId（§6.2 审计关联 id 缺口闭合）**：
   - RuntimeInvocation 帧 +u64 requestId（0 = 未携带，进程内默认调用方
     不铸 id），InvocationCodec 编解码透传，无版本协商（同仓同版本
     对端共同演进）；AuditEntry +requestId，14 个 handler + 未知方法
     拒绝臂统一盖戳，machine.audit JSON 出 request_id 字段——§6.2
     七字段全对齐，此前的「六已对齐」边界关闭。
4. **测试**：MachineDaemonTest（续期策略 TTL 真实生效：短 TTL 会话
   越过原 1s 仍活、越过策略 30s 才没；advanceClock 模拟）、续期部分
   失败分列、三角色 × extend 正反矩阵、通知三臂（未注册丢弃不计
   数 / 单踢送达 reason=operator.kick / 批量送达 reason=
   operator.kick.batch）、通知载荷零令牌原文、requestId 透传入审计
   与 machine.audit、单踢竞争臂（armFailDel 后重试成功）；LoginAppTest
   桩面：carl@default / amy@空领域 精确命中、已关/领域不匹配/无匹配
   不计数、计数增量；LoginAppE2ETest 场景 E：真实 TCP 登录 → 吊销关
   连接 → tick 清扫摘绑定。InvocationCodecTest 补 requestId 往返。
5. **口径沿用不改**：clear temporary bans 不做（无封禁存储前置）、
   长度指纹同长歧义不改、枚举顺序不承诺时序。

**边界与遗留（已于 2026-09-27 下一批收敛，见上）**：
- ~~通知通道的 LoginApp 生产接线未建~~：本批测试直接驱动落点；该遗留项
  在「通知通道的 LoginApp 生产接线」批次完成（入站分发面 + 注册探针
  传输腿 + 领域口径修正），此处按当时口径留档。
- 通知 best-effort 无重试/确认（设计如此：吊销以存储为准）。
- 会话续期策略值与 LoginApp 会话 TTL（config_.sessionTtl）是两处
  策略：前者是运维续期上限（daemon 侧），后者是登录签发时的会话寿命
  （LoginApp 侧）——统一为单一策略源属扩面，未做。
- requestId 由发起方铸造，daemon 侧只透传不校验唯一性（逐请求唯一是
  运维控制台的纪律责任，跨进程无仲裁者）。

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
- requestId 缺口已由上批（Phase 2 余项）闭合：RuntimeInvocation 帧
  透传 u64 → 审计条目 → machine.audit JSON；
- kick 入口是进程内 SessionStore 指针而非独立 LoginApp 组件（沿上批
  记录）。

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
   requestId 当时仍缺（沿既有记录，不编造）；已由「Phase 2 余项」批
   补帧字段与透传、2026-09-27「requestId 全链路关联」批补铸造与查询。
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
- requestId 已由后续批次闭合（「Phase 2 余项」批补帧字段与透传，
  2026-09-27「requestId 全链路关联」批补铸造与查询）；Gateway 限流
  边界沿用上一批记录。

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
   同既有条目形状。requestId 字段当时仍缺（沿用既有边界记录；已由
   「Phase 2 余项」+ 2026-09-27「requestId 全链路关联」两批闭合）。
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
- requestId 字段当时仍缺（§6.2 七字段之六已对齐）；「Phase 2 余项」
  批已补帧字段与透传，2026-09-27「requestId 全链路关联」批补铸造
  与中心环形查询。
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
- §6.2 审计字段的 requestId（请求关联 id）当时协议层仍缺；已由
  「Phase 2 余项」批补帧字段与透传、2026-09-27「requestId 全链路
  关联」批补铸造与中心环形按请求查询。
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
先落只读导出面，OTel 迁移留待依赖就绪；2026-09-30 trace 半边以零依赖
OTLP/HTTP JSON 先行落地，见顶部「OTel 导出器」批）：

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
   nodeId、result ≙ accepted+ok；requestId 当时待协议层支持——「Phase
   2 余项」批已补帧字段与透传，2026-09-27 批补铸造与查询）。
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
- ~~网络流量统计与多网卡聚合~~（2026-09-26 完成，Linux；Windows 等价
  胶合 2026-09-27 补齐——GetIfTable2 64 位计数入同一 probe_detail 聚合口径）
- ~~非受控全局进程的策略化管理与权限边界~~（2026-09-26 完成：控制面
  execute 双白名单 + 主机级非 agent 进程的策略化治理——machine.processes
  / machine.terminate RPC、ProcessGovernPolicy 独立策略、全动作审计入
  中心环形）
- ~~端口占用扫描与二进制版本探测~~（2026-09-26 完成，Linux；Windows/macOS 留空）
- ~~Linux / macOS 的等价主机探针完整实现与验证（网络部分 Linux 已完成，缺 macOS）~~
  （2026-09-27 完成 macOS 实现：CPU/内存/网络 Apple 胶合 + probe_detail
  共用口径（Linux 单测直测）；Linux 运行时本机已验，macOS 运行时由 CI
  macos-latest job 首验。Windows 网络探针亦已于同日补齐（85c6a58..cbacb3d，
  见顶部「Windows 等价网络流量探针」批）——「仍缺」为该批之前的陈旧
  尾注，2026-09-27 收口校准）
- ~~`MachineAgent` 的 RPC 输出与控制面注册~~（2026-09-26 完成：MachineDaemon
  TCP 端点；控制面中心注册待 Ops Control Plane 对接时一并做）
- ~~与 `Ops Control Plane` 的注册、上报和审计对接~~（2026-09-26 完成：
  生命周期注册/注销 + pruneStale 掉线摘除自洽；execute 审计（含拒绝）
  汇入中心可查询审计环形）
- ~~与 `Telemetry` 的指标、日志、trace 联动~~（2026-09-26 完成控制面切片：
  machine/ops 双侧计数与水位仪表、结构化审计与生命周期日志、
  execute SpanScope + 日志自动 trace 关联；OTel 导出器 trace 半边
  2026-09-30 零依赖 OTLP/HTTP JSON 落地，见顶部「OTel 导出器」批，
  metrics 导出仍开放）
- 更完整的单元测试与跨平台 CI 构建矩阵（矩阵部分 2026-09-27 落地：
  新增 macos-latest job（复用 clang-debug preset），矩阵成
  linux 三连 + coverage 门 + windows + macos；「更完整的单元测试」仍开放）

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

## 规则 c 巡检 2026-09-27：115/115 测试全绿、gcovr 100%（11682/11682 行 100%，1644/1644 函数 100%）；clang 树零警告、双树门禁 Through（gcc14+merge-use-line-min）

## 规则 c 巡检 2026-09-27：115/115 测试全绿、gcovr 100%（11682/11682 行 100%，1644/1644 函数 100%）；clang 树零警告、双树门禁 Through（gcc14+merge-use-line-min）
