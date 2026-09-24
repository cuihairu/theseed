# 测试覆盖率报告 — 行级 100% 的构成与显式豁免清单

> 这份文档回答四件事：
>
> 1. 当前行覆盖率的数字与口径（怎么测、分母怎么算、为什么可信）
> 2. 从 98.35% 到 100% 的完整冲刺轨迹（覆盖率作为测试强度的探针）
> 3. 全部 131 行显式豁免（`LCOV_EXCL`）的定性与理由
> 4. 冲刺沉淀的可复用方法论

---

## 1. 总览与口径

| 指标 | 值 |
| --- | --- |
| 行覆盖率 | **100.0%**（10440 / 10440） |
| 函数覆盖率 | 100.0%（1588 / 1588） |
| 豁免行 | 131（未豁免分母 10571，未豁免口径 99.1%） |
| 分支覆盖率 | 62.7%（7530 / 12003，无既定目标，未纳入本报告口径） |
| 覆盖率构建树 | `build/gcc-coverage`（gcc + `--coverage`） |
| 统计范围 | `src/` 全部源文件与头文件（不含 `tests/`） |
| 测试规模 | gcc-coverage 树 107 个测试（含 MySQL/PG 集成段）；clang18 / gcc13 树各 103（无 libmysql，DBApp 回退 file 后端） |
| 工具 | gcovr 8.6，`--filter src/`，`--txt` 报告 |

### 1.1 口径：行级，而不是条目级

gcovr 对同一行可能记多条计数条目（同一符号的多个 ABI 变体、模板逐 TU 实例等）。

- **条目级**口径要求一行上所有条目都被执行——历史上的 "98.85%、122 miss" 就是这个口径。
  它把大量结构性不可达的符号变体（如析构的 D0/D1/D2 三符号）计成 miss，数字偏低且
  与 gcovr 官方行覆盖率定义不一致。
- **行级**口径（本报告，也是 gcovr `--txt/--html` 的默认判定）：一行上任一条目命中即覆盖。
  这是正确目标口径。

### 1.2 gcovr 的两个坑

- **`--json` 输出不应用 exclusion 标记**，`--txt/--html` 才应用。用 JSON 对账豁免行会得到
  "标记没生效"的假象——对账必须用 `--txt`。
- **gcno 残留会污染报告**：从构建中删除源文件后，旧 `.gcno`/`.o` 仍留在 build 目录里，
  gcovr 会把它们继续计入分母。删文件后要同步清理对应的 gcno/o。

### 1.3 口径约束（踩过坑之后定下的规矩）

- **分母必须对账**。libgcov 的 `overwriting an existing profile data with a different checksum`
  警告**不是无害的**——checksum 不匹配的 TU 数据缺失会让分母虚小、覆盖率虚高。
  判断覆盖率变化前先确认分母没变；触碰并重编相关 TU 后分母会跳涨属正常。
- **SQL 集成段靠环境变量门控**：`THESEED_MYSQL_*` / `THESEED_PG_*`（无 `TEST` 中缀，
  **`THESEED_PG_DATABASE=theseed_test` 必须显式设置**，默认库名不存在会导致 PG 测试全挂）。
  变量名写错会让 SQL 测试**静默跳过**、ctest 仍然全绿，但 MySQL/PG 源码覆盖率归零。
  CI 无数据库，SQL 段整个跳过——CI 绿不代表 SQL 路径被验证。
- **三棵树验证**：gcc-coverage（带 SQL env）+ clang18-debug + gcc13-debug（后两棵不带
  SQL env，带了会挂在 "mysql-backend init should fail"）。推送前必须三棵全绿。
  clang18 树构建前需 `export LD_LIBRARY_PATH=/home/cui/.local/tools/clang18-root/usr/lib/x86_64-linux-gnu`。

---

## 2. 覆盖率轨迹

2026-09-22 ~ 09-23，从 98.35%（条目级）到行级 100%：

| 提交 | 主题 | 覆盖率 | miss |
| --- | --- | --- | --- |
| （起点） | 冲刺开始前（条目级） | 98.35%（10385/10559） | 174 |
| `c2b2504` | syncRealGhosts 孤儿空间分支 + JSON 转义路径 | 98.39% | 170 |
| `31cc3ed` | Entity.h 全部 per-TU 模板 miss 清零 + HotUpdate 警告路径 | 98.68% | 139 |
| `1393a88` | ProcessSupervisor fork 失败路径 + ObjectPool per-TU miss | 98.72% | 135 |
| `2d5332f` | **产品改进**：有界 DB 请求等待 + NetworkTransport 冲刷策略 | 98.82%（10459/10584） | 125 |
| `3fd7f0f` | MySQL allocId 回读失败（查询配额法）+ zero-id 创建防御 | **98.85%**（10464/10586） | **122** |
| `6cfa8b7` | 覆盖率报告成文（122 miss 完整分类） | — | — |
| 100% 冲刺 | 三批次：测试补强与死代码清理 → 98 miss（行级 99.1%）→ 显式豁免 131 行 → **行级 100.0%**（10441/10441） | **100.0%** | **0** |

> **分母变更记录**：`efc7c55`（TickScheduler run() 停止竞态修复）删除了 run() 中的
> `stopRequested_` 清零行——非豁免分母 10441 → 10440，覆盖率保持 100.0%。本文其余
> 章节的 10441 均为达成当时的历史数字，以本表为当前口径。

`31cc3ed` 一批消掉 31 条的手法：Entity.h 的 callCellWith/callBaseWith/callDefMethod/
onPropertyChanged 模板按 (行, TU 实例) 计数，用 gcov 逐 TU 定位（gcc≥9 的 gcno 带
源文件后缀，需在 /tmp 对目标 TU 建符号链接 `ln -s <objdir>/Name.cpp.gcno Name.gcno`
再 `gcov -b Name.cpp`），对每个 (参数组合, side, 成功/NotConnected) 组合补一次调用。

100% 冲刺的三个批次：

1. **死接口与测试补强**：删除全仓零引用的 `IRuntimePhaseHook`；给 `IRuntimeTransport::tick`
   补契约测试（内存实现空操作、PipedTransport 走基类默认）。
2. **产品侧清理**：5 处恒真防御改 `assert` 或删除（NetworkTransport decodeHeader、
   SpaceRuntime 空实体、EntityDefRegistry 空指针）；7 处 switch 全枚举后的尾部消除
   （合法 case 并入 default、或按语义用三元表达式）；6 处 gcc 归因伪影改写
   （多行 braced-init-list 单行化、链式 `<<` 拆独立语句）。
3. **显式豁免**：对实验论证不可测的 131 行加 `LCOV_EXCL` 标记（清单见 §5）。

---

## 3. 覆盖率冲刺顺手修掉的产品隐患

覆盖率探针的价值不在数字本身，而在它逼出来的产品缺陷。冲刺期间修掉六处：

1. **LoginApp::dbRequest 忙等挂死**（`2d5332f`）：原实现是 `while(true)` 无 deadline
   忙等，DBApp 停服（EOF）后 `receive` 永远返回 0 → LoginApp 100% CPU 挂死。
   现有 deadline（`dbRequestTimeout`，默认 5s）+ send 返回值检查 + 应答 method 校验。
2. **RemoteEntityStore::request 同构忙等**（`2d5332f`）：加 `requestTimeout`
   （构造第 4 参，默认 5s），超时返回空 `RuntimeInvocation`，六个调用方零改动。
3. **NetworkTransport BackPressure 死分支激活**（`2d5332f`）：send 尾部总调
   flushOutbound 使背压不可达。新增 `Config::autoFlush`（默认 true），false 时 send
   只入队、由 tick/flush 冲刷，队列积压 256 条后 BackPressure 可达。
4. **onPipeClosed 死代码删除**（`2d5332f`）：零调用者（IBytePipe 无 setOnClosed 的
   结构现实），直接删除而不是补一个永远没人调的测试。
5. **BaseRuntime::createEntity 的 allocId()==0 防御**（`3fd7f0f`）：DBApp 失联时
   RemoteEntityStore 超时返回 0，原实现会把 id=0 的实体注册进表——与"无实体"
   哨兵语义冲突。现在拒绝注册返回 nullptr。
6. **恒真防御的 assert 化**（100% 冲刺）：NetworkTransport 的 decodeHeader 返回值检查
   与 EntityDefRegistry 的空指针防御在预检后恒真，原样保留会永久 miss；改成
   `assert` + 注释说明前提，未来 decodeHeader 引入字段校验时断言会立即暴露静默破坏。

另有语义修正：DB 不可用不再计入 `challenge_failure_count`（语义污染），新增
`login_db_unavailable_count` 计数器。

---

## 4. 方法论武器库（可复用手法）

冲刺中实测有效的注入/定位手法，按性价比排序：

### 4.1 SQL DDL 失败注入（三板斧 + 配额法）

目标是构造"第 N 条语句成功、第 N+1 条失败"的窗口：

| 手法 | 适用 | 原理 |
| --- | --- | --- |
| ① 预建**缺列**的同名表 | PG / MySQL | 拦 CREATE INDEX，报 "column does not exist" |
| ② 按表授权差异 | MySQL | GRANT ALL 单表 + 库级仅 DML，让第二条 CREATE TABLE 撞权限 |
| ③ 查询配额法 | MySQL | `ALTER USER trap WITH MAX_QUERIES_PER_HOUR 2` + `FLUSH USER_RESOURCES` + KILL store 连接 |

配额法的两个关键机理（都实测过）：

- MySQL 配额检查**先加后判**：0+1=1 < 2 放行第 1 条（upsert），第 2 条
  （SELECT LAST_INSERT_ID()）2/2 被拒——配额设 2 才能精确命中"回读失败"分支；
  配额 1 会让第一条就拒（走的是 upsert-failed 分支）。
- ALTER USER 对已有连接不生效（限制值在连接建立时快照或共享 uc 即时生效，两种
  行为下配额 2 都正确），所以必须 KILL store 的连接（查
  `information_schema.processlist`）+ FLUSH，让 ensureConnected 重连。

trap 对象（用户/表/view）必须在段首 `DROP IF EXISTS`，保证 FAILED 提前退出后的幂等。

### 4.2 fork 失败注入（RLIMIT_NPROC）

`setrlimit(RLIMIT_NPROC, rlim_cur=1)`：当前 uid 已有进程数 ≥1 恒真 → fork 返回
EAGAIN。注意恢复 limit 放断言**之前**；getrlimit/setrlimit 失败时 PASS 跳过避免假红；
soft 可以任意调低（≤hard），per-process 限制不影响同机其他 ctest 进程。

### 4.3 单线程传输假件（FakeDbTransport）

驱动 db auth 路径的注入用手写 `IRuntimeTransport`（send 时同步入队 canned 应答，
单线程零死锁）。**双 InMemoryBytePipe + 双 NetworkTransport 方案在单线程下死锁**
（假 DBApp 侧的 pump 永远没机会跑），别再试。配套：LoginApp 单测用 `listenPort=0`
（TcpListener 支持随机端口）+ `dbTransportFactory` 注入。

### 4.4 psql 实验手法坑

`psql -c` 多语句字符串在**单隐式事务**里，`default_transaction_read_only` 只影响
新事务——要测事务属性必须每个 `-c` 独立语句（libpq 的 PQexec 每条是独立隐式事务，
与产品代码一致）。

### 4.5 判定前的可见性检查

写测试前先查成员可见性：HotUpdate:128/130 的 default throw 在 **private** 方法内，
外部不可达，纯浪费一轮"写测试→跑→发现没命中"。

### 4.6 trivial 虚析构是结构性不可覆盖的（重要教训）

**gcc 不为空函数体生成计数指令。** C++ ABI 要求虚析构生成三个符号：

- D0（deleting destructor）：`delete` 静态类型为该类时调用
- D1（complete destructor）：动态类型恰为该类时调用
- D2（base destructor）：被派生类析构调用——但 trivial 空体会被编译器**内联进
  派生类析构**，D2 本体没有机器码

对纯接口类的 `virtual ~X() = default;`，三个符号变体的行条目**全部恒 0**，
无论定义写在头文件里还是移到 .cpp（实测两种位置都 miss，out-of-line 化是负优化：
多出 3 个源文件还同样不可覆盖）。具体类的 outline trivial 析构
（`MySQLEntityStore::~MySQLEntityStore() = default;`）同理。

**结论**：trivial 虚析构行只能豁免，标记加在定义行上。非平凡析构（有函数体语句）不受影响。
由此引申：接口类只在有非平凡资源语义时才写非默认析构；纯标记接口接受这一行豁免。

### 4.7 标记工程的三个坑

- `LCOV_EXCL_LINE` **只豁免标记所在行**。写成独立注释行（`// LCOV_EXCL_LINE` 下一行
  才是语句）不会豁免语句行——必须把标记放在**语句行尾**，或用 START/STOP 包住区间。
- 拆行会把伪影**带走**而不是消掉：`std::visit(` 与 lambda 开括号拆成两行后，原来
  miss 的一行变成 miss 的两行（闭包包装符号的归因跟着拆行移动）。归因伪影要么
  恢复单行 + 行尾标记，要么 START/STOP 包住整个表达式。
- 豁免必须带理由注释（本仓库规矩：标记后紧跟中文理由），并在本文档 §5 留痕。

---

## 5. 131 行显式豁免的完整定性

未豁免分母 10572 行、98 miss（99.1%）→ 豁免后 10441 行、0 miss。89 个标记点
（34 个文件）按下述机理分七类。每处标记在源码中都有随行理由，此处按类汇总。

### A. C++ ABI 结构不可测（trivial 虚析构，21 行）

18 个纯接口/基类的 `virtual ~X() = default;`（头文件定义）：
RuntimeTransport.h、TickScheduler.h、Space.h、RuntimeLoop.h（IServiceApp）、
IORuntime.h、IBytePipe.h、Controller.h、BehaviorTree.h、AOI.h（RangeTrigger）、
RedisProvider.h、Logger.h、Channel.h、IAccountStore.h、IEntityStore.h、
EntityQuery.h（IEntityQueryStore）、ProcessSupervisor.h、MachineAgent.h、HostProbe.h
——D0/D1/D2 三符号变体恒 0（§4.6）。

外加 3 个具体类的 outline trivial 析构：MySQLEntityStore、PostgreSQLEntityStore、
InMemoryRedisProvider（.cpp 定义，同样无机器码无计数）。

### B. gcc 归因伪影（12 行）

代码确实执行过（测试断言与函数体行计数可证），gcc 把计数条目挂到恒 0 的位置：

| 位置 | 内容 |
| --- | --- |
| NetworkNode.cpp 18-25 | 析构签名行与大括号行（函数体各行均有计数） |
| NetworkTransport.cpp 26 | 单行化后的析构行（close() 有计数） |
| EntityDefRegistry.cpp 59 | `std::function` 赋值行的闭包包装符号 |
| MetricsRegistry.cpp 229 | `std::visit` 行的闭包包装符号（lambda 体各分支有计数） |

### C. 进程/系统调用边界（约 40 行）

| 文件 | 段 | 内容 |
| --- | --- | --- |
| ProcessSupervisor.cpp | 8 段 | reap 与 /proc 枚举间的 push_back 窗口；queryCurrentProcess（匿名 namespace 且 /proc 恒非空，325 的 fallback 不可达）；fork 子进程体（execvp 成功不写 gcda）；kill 失败非 ESRCH（对 managed child 恒有权限） |
| HostProbe.cpp | 4 段 | gethostname 失败；sysinfo 恒成功后的 sysconf fallback；/proc/stat 解析失败；getloadavg 失败兜底 |
| TcpListener.cpp | 1 段 | bind 成功后 listen 失败本地无法稳定触发（fd 耗尽先死在 socket()） |
| LoginApp.cpp / NetworkNode.cpp | 各 1 段 | 非阻塞 connect 对无服务端口恒 EINPROGRESS；inet_pton 无 DNS，坏主机名 → 0.0.0.0 → 仍"成功" |

系统调用失败分支没有 fault-injection 点，无法定向注入；fork 失败（EAGAIN）已用
RLIMIT_NPROC 真实覆盖（不在豁免之列）。

### D. 客户端库内部状态 / OOM（约 30 行）

| 文件 | 段 | 内容 |
| --- | --- | --- |
| MySQLConnection.cpp | 9 段 | mysql_init/stmt_init OOM；bind_param/bind_result 内部状态失败（libmysql 对参数数不匹配不在 bind 报错，坏 SQL 走 prepare/execute 失败分支且已覆盖）；client-side cursor 下 store_result/fetch 是纯内存遍历，无 SQL-only 注入面（断连被 ensureConnected 的 ping+重连吸收） |
| PostgreSQLConnection.cpp | 3 段 | captureError else；PQexecParams 返 nullptr（ensureConnected 挡在所有调用前，有效连接出错返回 error result 而非 nullptr） |
| Tracing.cpp | 1 段 | 真随机数全 0 才进的重试循环体 |

查询配额法对 stmt 路径无扩展空间——已逐条实验收口。

### E. 语义防御 / 不可达兜底（约 12 行）

| 位置 | 内容 |
| --- | --- |
| HotUpdate.cpp 2 段 | private 方法内的防御 throw（外部不可达）；语义 default |
| EntityDefLoader.cpp | switch 的 default（Vector3 被上方分支拦截、String/Blob 被 isVariableSized 拦截、非法类型名直接 throw——三重拦截，仅 -Wswitch 完整性兜底） |
| PostgreSQLEntityStore.cpp 110-112 | createSchema 第 2 条 DDL 失败分支——PG 16 下三重实验封死（见 §6） |
| EntityQuery.cpp / EntityData.cpp / EntityDef.cpp / Logger.cpp / MetricsRegistry.cpp | switch 全枚举后按语义合并的 default 臂（合法类型已穷举，default 同时承担"变长类型记 0"等语义） |

### 豁免的原则

每一处豁免都必须同时满足：① 有可复现的实验论证（本节与 §6）；② 标记处带随行理由；
③ 豁免的是"测量不可达"而非"懒得测"——任何能写出测试的分支一律写测试，不动标记。
排除后分母变小，"100%"的含义是"**全部可测代码 100% 覆盖 + 不可测代码显式豁免**"。

---

## 6. 深挖案例：PostgreSQLEntityStore 110-111 的三重实验

createSchema 顺序执行三条 DDL（_entity_ids / _account_index / 索引）。
要覆盖第 2 条的失败分支，需要构造"第 1 条成功或跳过、第 2 条失败"的窗口。
三条路线逐一实验（PG 16.15），全部封死：

1. **独立受限用户**：无 schema CREATE 权限的用户，对**已存在**的表执行
   `CREATE TABLE IF NOT EXISTS` 也 permission denied——schema ACL 检查**先于**
   if_not_exists 存在性跳过（与 MySQL 行为相反）。
2. **默认表空间**：`SET default_tablespace='不存在'` 在 SET 时即被 GUC check hook
   拒绝；指向真实表空间的方案需要容器文件系统预建目录，不自包含。
3. **read-only 事务**：`SET default_transaction_read_only=on` 后对已存在表的
   `CREATE TABLE IF NOT EXISTS` 同样报 "cannot execute CREATE TABLE in a
   read-only transaction"——readonly 检查在 analyze **之前**按语句类型做
   （check_xact_readonly），根本到不了 if_not_exists 跳过。

结论：SQL-only 无法构造该窗口。若要覆盖只能改产品结构（逐条独立事务 + 中间
状态注入），收益不成比例，予以豁免。

---

## 7. 结论与维持口径的守则

**行级 100.0% 已达成**（10440/10440，另有 131 行显式豁免；函数级 1588/1588）。
这个数字的可信度建立在：

1. 每一行豁免都有实验论证与随行理由（§5、§6）；
2. 分母对账（checksum 警告不忽略、gcno 残留清理）；
3. SQL 集成段带全环境变量实跑（`THESEED_PG_DATABASE` 不可漏）；
4. 三棵树全绿后才推送。

**守则**：新增产品代码不允许留下无理由的 miss——要么写测试，要么按 §5 的格式
加带理由的豁免标记并在本文档补一行定性。评审覆盖率变化时先看分母、再看豁免
清单 diff，最后才看百分比本身。

---

## 8. 分支覆盖：基线与定性（2026-09-24）

分支口径**不设 100% 目标**，本节记录基线与 miss 构成定性，供后续增量冲刺参考。

| 口径 | 值 |
| --- | --- |
| 原始分支覆盖率 | 62.7%（7530 / 12003） |
| 异常边（gcov `throw` 分支） | 3567 条，其中 3533 miss——gcc 给每条可抛语句生成 normal/exception 两条边，exception 边无故障注入不可达 |
| **剔除异常边后的业务分支覆盖率** | **88.2%**（7507 / 8514） |

业务分支 miss（1014 个）构成定性：

1. **短路链防御臂不可达**：`!p || !p->isValid()` 类的"非空但无效"臂——如
   `Entity::bindBaseEntityCall` 必带 targetComponent（emplace 有值）、clear 即
   reset，"存在但 optional 无值"状态公共 API 不可构造（CellRuntime 约 15-20 个
   miss 属此类：220/927/958/1016 等行）。
2. **STL 内联归因**：`std::vector` 构造、`map::emplace` 等调用行上的异常边变体。
3. **真业务失败臂**：`||` 短路中间臂（实体非空但 state 不符）、AoI observer/
   target 在事件产生后被移除等时序状态——可测但需针对性场景，按需补充。

泵路径（`syncToBases` / `flushClientEvents` / `flushAoIEvents` / `flushWitnessSync`）
的端到端驱动已由 `CellRuntimeBranchTest` P 组建立先例：createCell 造 ownedEntities_
实体 + `bindBaseEntityCall` + `emitToClient`/`setProperty`/`ensureWitness` +
`scheduler.runOnce()` + drain 断言。

分析工具：`gcovr --json` 的 branches 数组带 `throw` 属性，可精确分离异常边与
业务分支（`--json` 不应用 LCOV_EXCL，与 §1.2 的 txt 口径坑不冲突——本节只做
miss 分类，不做豁免对账）。

### 8.1 第二批：BaseRuntime / EntityDefLoader（2026-09-24）

新增 `tests/core/BaseRuntimeBranchTest.cpp`（18 场景）与
`tests/core/EntityDefParserBranchTest.cpp`（20 场景），并对全部剩余 miss 逐行
核对 gcov 文本定性。

| 文件 | 起始 miss 行数 | 终值 | 新增场景可清 | 定性留置 |
| --- | --- | --- | --- | --- |
| BaseRuntime.cpp | 70 | 15 | 55 | 15 |
| EntityDefLoader.cpp | 58 | 11 | 47 | 11 |
| CellRuntime.cpp（第一批） | — | 60 | — | 60（多为 isValid 族，见 §8 上文） |

BaseRuntime 剩余 15 行定性：

1. **timer 回调 lambda 的 `findEntity` null 臂**（172-180 / 211-219，8 行）：
   `completeBaseDestruction` 先 `cancelEntityTimers` 再 erase，且
   `TimerWheel::advance` 出队时逐项检查 cancelled——"timer 存活但实体已出册"
   的窗口不存在，属纵深防御。
2. **`EntityCall::isValid()` 恒真族**（250 / 624 / 653 / 795，4 行）：
   `isValid = targetComponent_.has_value()`，`bindCellEntityCall` 是唯一
   emplace 路径且任意 ComponentId（含 0）都置值，"非空但无效"的 EntityCall
   公共 API 不可构造——与 §8 第 1 类同族。
3. `completeBaseDestruction` 幂等早退（264）、pumpInbound 循环 CFG 归因
   （376）、syncToCells 同条件多副本边（666）。

EntityDefLoader 剩余 11 行定性：

1. **出口 phi / 短路链副本归因**（40 / 71 / 114 / 145 / 220 / 412）：三种
   break 逻辑与全部解析出口均有场景，miss 是 gcc 对 `||` 链与函数出口 phi
   的多副本边。
2. **defaultValue switch 防御臂**（306 / 308 / 310 / 367 / 374）：可达类型
   集合上 `fixedSize` 恒 >0、String/Blob 在 switch 前被拦截、switch 的
   default 跳转对应不可达值。

手法教训（本批新增）：

- **gcovr 的 F/J 方向不能按直觉读**：防御/失败臂常被放在直落位，此时
  `fallthrough=true` 的 miss 边恰是"条件为真的早退臂"。结论必须以
  `gcov -b` 文本的 `branch N taken X%` 对照行执行计数核对后才可下。
- **ctest 必须串行跑覆盖率**：`-j` 并行时多个测试进程并发写同一份共享库
  gcda，数据大面积损坏且不可复现地表现为"某些边归零"。
- 改测试后必须 `find . -name '*.gcda' -delete` 再全量重跑，否则 checksum
  不匹配的 gcda 被静默拒写。
- gcovr 对本仓库需同时加
  `--gcov-ignore-errors=no_working_dir_found --gcov-ignore-parse-errors=negative_hits.warn_once_per_file`
  （前者是 gcda 记录的工作目录丢失，后者是 gcc bug 68080 的 NegativeHits）。

顺带发现、记录在案未修（既有行为，修复属功能变更）：`loadEntity` 对已在册
id 无防御（`map::emplace` 冲突致指针悬垂 UB）；EntityDefLoader 的
UInt32/UInt64 defaultValue 超出 `stoi`/`stoll` 上限时抛 `std::out_of_range`。

### 8.2 第三批：EntityQuery / BaseApp（2026-09-24）

新增 `tests/core/EntityQueryBranchTest.cpp`（6 场景）与
`tests/core/BaseAppBranchTest.cpp`（5 场景）。

| 文件 | 起始 miss | 终值 | 新增场景可清 | 定性留置 |
| --- | --- | --- | --- | --- |
| EntityQuery.cpp | 45 边 / 24 行 | **0 边 / 0 行（全清）** | 45 边 | 0 |
| BaseApp.cpp | 41 边 / 30 行 | 12 边 / 9 行 | 29 边 | 12 边 |

EntityQuery 全清手法：

1. **decodeNative 尺寸错配矩阵**（24 行 ×9 实例）：模板按 T 逐实例生成
   CFG，需对 11 个定长类型各发一发 lhs 空 rawValue + rhs 截短 rawValue。
2. **`!decodeNative(lhs) || !decodeNative(rhs)` 短路链三边**（43-93）：lhs
   失败打跳转边、rhs 失败打第二条件两边——两发场景即全清每类型的 3 条
   miss。
3. **浮点 `<=>` 四结果矩阵**（84/89）：less/greater/equal/unordered 各一发，
   NaN 触发 unordered（双向）；Bool（94）与 String（97）同样补齐三结果边。
4. `QueryFilter::of(bool)` 的 false 臂（194）。

BaseApp 剩余 12 边定性：

1. **`EntityCall::isValid()` 恒真族**（310 / 365）：同 §8.1 BaseRuntime
   定性——bind 恒置值，"非空但无效"公共不可构造。
2. **init 工厂/恢复循环防御**（50 / 88）：循环遍历 `registry_.entityTypes()`
   故 `createFactory` 恒非空；全新 runtime 上 `findEntitiesByType` 恒空。
3. **恒非空防御与 ops 归因**（96 ×4 / 104 / 108 / 114 / 147）：构造函数
   校验保证 transport_/runtime_ 恒非空；其余为 ProcessInfo / OpsServer::Config
   聚合初始化的副本边。

本批新手法教训：

- **「坏尺寸」payload 必须逐类型构造**：固定 2 字节对 Int16/UInt16 恰是
  合法尺寸，decode 成功后走真实比较而非 unordered。正解是每类型截短
  一字节。
- **零属性 def 打不开 EnterGame 空快照臂**：`buildFullPropertySnapshot`
  对 null 存储直接抛 `invalid_argument`（存储按属性布局分配，零属性即
  null）。正解是唯一属性带 `flags="Base"`——`buildFullPropertySnapshot
  (PropertyFlag::Base)` 的语义是**排除** Base 标记属性。
- **`requestCreateCell` 不绑定 cellCall**：EnterGame 后实体并无 cell 绑定，
  此时 `destroyEntity` 是立即销毁并同步触发 `onEntityDestroyed` 清空会话
  映射——测试随后 `findSessionByEntity` 拿到 null 再 `bindSessionToEntity`
  会把 null 绑进会话表，下一拍 flush 段错误。延迟销毁需先显式
  `setCellEntityCall`。
- **僵尸会话注入法**：`takeClientSession(未连接的 ClientSession)` +
  `bindSessionToEntity(raw, id)` 一发覆盖五类下行回调的"会话已断连"早退
  臂与 cleanupClients 的 `findEntity` null 臂，无需任何真实 TCP。

至此 src/core 目录全部分支 miss 归零或定性（BaseApp 12 边留置）。下一批
候选：PostgreSQLEntityStore（335 边）/ MySQLEntityStore（313 边）等 SQL
后端错误臂。

### 8.3 第四批：SQL 后端与 FileEntityStore（2026-09-24）

新增 `tests/db/MySqlBranchTest.cpp`、`tests/db/PgBranchTest.cpp`（需
`THESEED_MYSQL_HOST` / `THESEED_PG_HOST` 环境，未设置时跳过）与
`tests/core/FileEntityStoreBranchTest.cpp`（无外部依赖）。FileEntityStore
与 RemoteEntityStore 用既有回环测试已有较高覆盖，本批只补缺口。

| 文件 | 终值（gcov 文本口径） | 构成 |
| --- | --- | --- |
| FileEntityStore.cpp | **0（全清）** | — |
| MySQLConnection.cpp | 32 行 | 17 行豁免区入口/never-exec + 15 行定性留置（见下） |
| MySQLEntityStore.cpp | 14 行 | 全部定性留置 |
| PostgreSQLEntityStore.cpp | 20 行 | 2 行豁免区 + 18 行定性留置 |
| RemoteEntityStore.cpp | 1 行 | 函数出口聚合块（超时返回路径，L45） |

起点口径说明：批前用 `gcovr --json` 统计业务 miss 边为 MySQLConnection 39 /
MySQLEntityStore 41 / PGStore 54 / FileEntityStore 5 / RemoteEntityStore 4
（合计 143）。**json 口径含两类伪影**：`-O2` 函数克隆（constprop/partial）
未调用实例的全部边计 0；以及 gcov 文本核对时 `"0%" in line` 会把
`taken 100%` 误判为 miss（"100%" 含 "0%" 子串）。终值一律以
`gcov -b` 文本、精确正则 `taken N%` / `never executed`、剔除 `(throw)` 后
的口径为准。

本批清掉的场景：

1. **FileEntityStore 5 边全清**：allocId 元文件读失败（chmod 000）与写失败
   （`fs::create_directory(meta)` 目录 trick）——注意非 root 下 chmod 000
   的文件 owner 仍可读，断言须写 `idA == 1 || idA == 6` 双态；listIdsByType
   / listEntityTypes 的目录项跳过链用"实体 + 子目录 + 垃圾文件"混合目录。
2. **bytesToUint64 低向 break**（MySQLConnection 26）：`SELECT '-42'`——
   首字符 `'-' < '0'` 命中低向臂；`'not-a-number'` 首字符 `n > '9'` 只走
   高向（`||` 短路链的 break 是同一个但 gcc 布局成两条边）。
3. **sanitizeForTable 全谱系字符**（两 store 的 46-48）：`"AZ[az{09:_-"`
   一串覆盖每个比较段的高低两向——大写、`'Z'` 之后 `'['`，小写、`'z'`
   之后 `'{'`，数字、`'9'` 之后 `':'`，合法 `'_'`，非法 `'-'`。串合法不
   超长，ensureTable 正常建表（sanitize 分支与 DDL 结果无关），收尾 DROP
   `tbl_AZ_az_09___`。
4. **MySQL 专属**：未 init 防御族、超长表名 DDL 失败（80 字符 > 64 上限，
   MySQL **报错**）、空 blob 拒载、DROP 后 knownTables_ 缓存命中失败族、
   DROP `_entity_ids` / `_account_index` / `tbl_Account` 连锁失败、charset
   留空、二次 disconnect、asBytes 防御、空参数列表、wait_timeout=1 掉线
   重连（autoReconnect true → 透明重连成功 / false → execute 失败）。
5. **PG 镜像**：上列可行子集。PG 的 ensureTable 失败臂**无法 SQL-only
   注入**——63 字节标识符**截断不报错**（与 MySQL 报错相反），DDL 失败臂
   与 `!ensureTable()` 短路臂（load 184 / listIdsByType 277）留置。

剩余 miss 定性（全部留置）：

1. **never-exec 库内联边**（两 store 的 save/remove/queryAccount/
   createAccount 语句行与闭合行，MySQLConnection 313/314/341/394/395/
   410/411/445-447/458 等）：`std::string`/`ostringstream`/`unique_ptr`
   与 mysql API 内联展开的私有边，`never executed` 且无业务语义。
2. **恒真/恒假防御**：`ScopedMsTimer` 的 `if (emitter_)`（两 store 22/21，
   两处实例化都传字面 lambda）、allocId 的 `!result->next()`（SELECT
   LAST_INSERT_ID / RETURNING 恒一行）、`starts_with("tbl_")` false 臂
   （LIKE / information_schema 查询保证前缀）、MySQLResult 以 null res
   构造的 advance 臂（74）与 `cursor > rows.size()`（106，next() 到顶不
   推进，cursor 上限即 size，防御冗余）。
3. **EXCL 豁免区入口**：mysql_init / stmt_init 失败臂的 fallthrough 边
   （168/255/339 等）指向已豁免的 OOM 区。
4. **绑定/取回失败臂**（bind_param / bind_result / store_result /
   fetch_column != 0）：需驱动级故障注入，SQL-only 不可达。
5. **多语句排空 `more != nullptr` 臂**（MySQLConnection 229）：独立探针
   与 fprintf 插桩均证明可达（`SELECT 1; SELECT 2; SELECT 3` 的后续结果
   集非 null），但在全量测试负载下间歇 miss——环境时序敏感边，不追求
   稳定命中（同第三批 TickScheduler run 竞态的处理）。
6. **remove 的 DELETE 失败臂**（两 store 238/245）：表被 DROP 后即不在
   `listEntityTypes` 结果里（SHOW TABLES / information_schema 实时查询），
   "循环里 DELETE 失败"的两个前提互斥；驱动级故障注入面不在 SQL 层。

本批新手法教训：

- **SQL 表名大小写三连**：`tableName("Avatar")` 返回 `tbl_Avatar`（保留
  大写）；PG 无引号标识符折叠小写、MySQL Linux 大小写敏感——测试 SQL 必
 须写 `"tbl_Avatar"`（PG 带双引号）/ `` `tbl_Avatar` ``（MySQL 反引号），
  小写变体操作的是不存在的表（首轮 9+7 个失败全是它）。
- **knownTables_ 缓存语义**：ensureTable 成功即缓存，DROP 后仍命中缓存跳
  过 DDL → 后续 SQL 直接失败——这正是"DROP 后缓存命中失败族"场景的原理；
  防御场景收尾必须用**新 store 实例**（缓存空 → ensureTable 重建）做
  happy path 回归。
- **libmysql 客户端四路对照法**：断言"某边不可达"前先用独立 C 探针对照
  （mariadb connector vs Oracle libmysqlclient ×有无 OPT_RECONNECT）——
  本批凭探针证明 229 可达从而避免了一次错误改码。注意 vcpkg 的
  `unofficial-libmysql` 实际装的是 **libmariadb**，而 build 树 manifest 装
  的是 **Oracle libmysqlclient**（C++ 实现，链接需 g++ + zstd）；探针编译
  必须显式 `/usr/bin/g++`（`~/.local/bin/g++` 残废遮蔽）。
- **gcda 累积假象**：改测试的迭代轮次若未清 gcda，旧场景边数会叠加进新
  gcda（L227 循环 3 次 vs 实际 2 次）。终核前 `find . -name '*.gcda'
  -delete` + 全量串行重跑是唯一可信口径。

三棵树验证：gcc-coverage 114/114（SQL env）、clang18 108/108、gcc13
108/108（后两树 SQL 后端 disabled，分支测试 target 天然不存在）。
