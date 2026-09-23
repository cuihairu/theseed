# 测试覆盖率报告 — 98.85% 的构成与 122 个未覆盖行的定性

> 这份文档回答三件事：
>
> 1. 当前行覆盖率的数字与口径（怎么测、分母怎么算、为什么可信）
> 2. 覆盖率冲刺过程中顺手修掉的产品隐患（覆盖率作为测试强度的探针）
> 3. 剩余 122 个未覆盖行分别是什么、为什么测试侧不可达，以及通往 100% 的路径

---

## 1. 总览与口径

| 指标 | 值 |
| --- | --- |
| 行覆盖率 | **98.85%**（10464 / 10586） |
| 未覆盖行 | 122 |
| 覆盖率构建树 | `build/gcc-coverage`（gcc + `--coverage`） |
| 统计范围 | `src/` 全部源文件与头文件（不含 `tests/`） |
| 测试规模 | gcc-coverage 树 107 个测试（含 MySQL/PG 集成段）；clang18 / gcc13 树各 103（无 libmysql，DBApp 回退 file 后端） |
| 工具 | gcovr 8.6，`--filter src/`，JSON 输出逐行对账 |

口径约束（踩过坑之后定下的规矩）：

- **分母必须对账**。libgcov 的 `overwriting an existing profile data with a different checksum`
  警告**不是无害的**——checksum 不匹配的 TU 数据缺失会让分母虚小、覆盖率虚高。
  判断覆盖率变化前先确认分母没变；触碰并重编相关 TU 后分母会跳涨属正常。
- **SQL 集成段靠环境变量门控**：`THESEED_MYSQL_*` / `THESEED_PG_*`（无 `TEST` 中缀）。
  变量名写错会让 SQL 测试**静默跳过**、ctest 仍然全绿，但 MySQL/PG 源码覆盖率归零
  （总覆盖率掉到 89%）。CI 无数据库，SQL 段整个跳过——CI 绿不代表 SQL 路径被验证。
- **三棵树验证**：gcc-coverage（带 SQL env）+ clang18-debug + gcc13-debug（后两棵不带
  SQL env，带了会挂在 "mysql-backend init should fail"）。推送前必须三棵全绿。

---

## 2. 覆盖率轨迹

覆盖率冲刺（2026-09-22 ~ 09-23）共六批提交，miss 从 174 降到 122：

| 提交 | 主题 | 覆盖率 | miss |
| --- | --- | --- | --- |
| （起点） | 冲刺开始前 | 98.35%（10385/10559） | 174 |
| `c2b2504` | syncRealGhosts 孤儿空间分支 + JSON 转义路径 | 98.39% | 170 |
| `31cc3ed` | Entity.h 全部 per-TU 模板 miss 清零 + HotUpdate 警告路径 | 98.68% | 139 |
| `1393a88` | ProcessSupervisor fork 失败路径 + ObjectPool per-TU miss | 98.72% | 135 |
| `2d5332f` | **产品改进**：有界 DB 请求等待 + NetworkTransport 冲刷策略 | 98.82%（10459/10584） | 125 |
| `3fd7f0f` | MySQL allocId 回读失败（查询配额法）+ zero-id 创建防御 | **98.85%**（10464/10586） | **122** |

其中 `31cc3ed` 一批消掉 31 条：Entity.h 的 callCellWith/callBaseWith/callDefMethod/
onPropertyChanged 模板按 (行, TU 实例) 计数，用 gcov 逐 TU 定位（gcc≥9 的 gcno 带
源文件后缀，需在 /tmp 对目标 TU 建符号链接 `ln -s <objdir>/Name.cpp.gcno Name.gcno`
再 `gcov -b Name.cpp`），对每个 (参数组合, side, 成功/NotConnected) 组合补一次调用。

---

## 3. 覆盖率冲刺顺手修掉的产品隐患

覆盖率探针的价值不在数字本身，而在它逼出来的产品缺陷。冲刺期间修掉五处：

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

---

## 5. 122 个未覆盖行的完整分类

按不可达机理分八类。前两类（伪影，36 条）是测量侧现象而非真实不可测；
中间两类（死防御，19 条）是产品侧可清理的真实死代码；后四类（67 条）是
当前架构下测试侧真正无法定向注入的防御分支。

### A. 进程/系统调用边界（32 条，26.2%）

| 文件 | 行 | 内容 |
| --- | --- | --- |
| ProcessSupervisor.cpp | 22 条 | 84（reap 与 /proc 枚举间的 push_back 窗口）；87-114、325（queryCurrentProcess 是匿名 namespace，唯一外部入口 325 是 /proc 枚举为空时的 fallback，而 /proc 恒非空）；390-399（fork 子进程体：execvp 成功替换映像不写 gcda，失败走 _exit 也不写）；431、480（kill 失败非 ESRCH——对 managed child 恒有权限，SIGTERM 后从 /proc 消失需竞态） |
| HostProbe.cpp | 10 条 | 51（gethostname 失败）、106-115（sysinfo 成功后 sysconf fallback 永不进——sysinfo 在 Linux 恒成功）、134（/proc/stat 解析失败）、156（getloadavg 失败） |

系统调用失败分支无法定向注入（没有 fault-injection 点），fork 子进程体的行
**执行了也不会写 gcda**。fork 失败（EAGAIN）路径已用 RLIMIT_NPROC 覆盖。

### B. 客户端库内部状态 / OOM（27 条，22.1%）

| 文件 | 行 | 内容 |
| --- | --- | --- |
| MySQLConnection.cpp | 22 条 | 169-170（mysql_init OOM）、254-255、334-335（stmt_init OOM）；308-310、385-387（bind_param）、432-435（bind_result）仅客户端内部状态/OOM 可触发——libmysql 对参数数不匹配**不**在 bind 报错（execute 才报），坏 SQL / 占位符不匹配走 prepare/execute 失败分支，均已覆盖；399-401（store_result）、443-444（fetch rc==1）在 client-side cursor（store_result 模式）下是**纯内存遍历**，fetch 无网络依赖、无 SQL-only 注入面（断连会被 ensureConnected 的 ping+重连吸收）；471（fetch_column 失败）在截断恢复路径内，代码自身参数恒合法 |
| PostgreSQLConnection.cpp | 5 条 | 164（captureError else）、248-249、275-276（PQexecParams 返 nullptr）——ensureConnected 挡在所有调用前，有效连接出错返回的是 error result 而非 nullptr |

查询配额法（第四板斧）对 stmt 路径无扩展空间——已逐条实验收口。

### C. 头文件 per-TU 伪影（20 条，16.4%）

头文件里的虚析构 `= default` 与 inline 空方法体：几十个 TU 实例化（vtable 引用），
但只有少数 TU 真正执行该行——hit 与 miss 条目并存，行级 miss 永远存在。

RuntimeTransport.h [25, 38, 55]（3 条）+ 以下各 1 条：TickScheduler.h:36、Space.h:22、
RuntimeLoop.h:44、IORuntime.h:62、IBytePipe.h:11、Controller.h:28、BehaviorTree.h:21、
AOI.h:49、RedisProvider.h:30、Logger.h:39、Channel.h:54、IAccountStore.h:16、
IEntityStore.h:18、EntityQuery.h:64、ProcessSupervisor.h:23、MachineAgent.h:20、HostProbe.h:21

对照：**代码文件的 per-TU 条目可全消**（Entity.h 30+2 条已用逐 TU 定位法清零），
头文件的虚析构/inline 空体不行——除非把定义移出类定义（见 §7）。

### D. gcc 归因伪影（16 条，13.1%）

代码确实执行过（测试断言证明），但 gcc 把计数条目挂到未执行的归因块：

- 析构函数大括号/`= default` 行：NetworkNode:18/24、NetworkTransport:23/25、
  MySQLEntityStore:74、PostgreSQLEntityStore:71、RedisProvider.cpp:16、OpsInspector:42
- 纯续行：HostProbe:177/179、LoginApp:97→103（histogram 链式调用里 Boundaries{...} 实参续行）、
  RealmApp:71、MachineSnapshotCodec:68/104（escapeJson 链式 << 续行——测试断言
  `"name":"db\"app\n"` 转义通过证明执行过）
- std::visit / std::function 构造行：MetricsRegistry:229、EntityDefRegistry:61（63 行 lambda 体 hit）
- 真随机不可达：Tracing:156/157（随机数全 0 才进的重试循环体）

### E. switch 全枚举后的尾部（9 条，7.4%）

所有 case 全枚举后编译器仍要求 return，落进永不执行的 default/尾部：
Entity:28、EntityDef:68、EntityData:27、Logger:72（"UNKNOWN"）、
MetricsRegistry:185（"untyped"）、EntityQuery:103/132、HotUpdate:27/28（default→L4）。

### F. 死防御 / 恒真分支（10 条，8.2%）

| 位置 | 为什么恒不触发 |
| --- | --- |
| NetworkTransport:183-185 | parseOneMessage 预检 kEncodedSize 后 decodeHeader 只读字段无 magic/version，不可能失败 |
| EntityData:119 | decodeProperty 的同构恒真防御 |
| EntityDefRegistry:17-18 | loadFromFile 要么 throw 要么返回非空，永不返回 nullptr |
| EntityDefLoader:350/351 | 306 行 isVariableSized 先拦掉 String/Blob，进 switch 的只有定长数值类型且 case 全枚举；parsePropertyType 对非法类型名直接 throw——三重拦截 |
| SpaceRuntime:202 | Space::entities_ 的 Member.entity 恒为 &entity（引用参数），从 map 构建不可能产 null |
| HotUpdate:128/130 | private 方法内的防御 throw，外部不可达 |

### G. 网络/时序/随机（6 条，4.9%）

- TcpListener:51/52（listen 失败）：bind 成功后 listen 本地无法稳定触发
  （fd 耗尽会先死在 socket()，同 socket 重复 listen 在 Linux 返回成功）
- LoginApp:45、NetworkNode:41（connect 失败）：Linux 非阻塞 connect 对无服务端口
  返回 EINPROGRESS→true；且 TcpConnection::connect 用 `inet_pton`（**无 DNS 解析**），
  非法主机名 → 地址全 0（0.0.0.0）→ 视作 localhost → 仍 EINPROGRESS→true——
  "坏主机名"路线也封死
- Tracing:156/157：真随机数下不可达的全 0 重试

### H. PG 结构性限制（2 条，1.6%）

PostgreSQLEntityStore:110-111（createSchema 第 2 条 DDL 失败分支）——三条路线
全部实测封死（PG 16.15，见 §6）。

---

## 6. 深挖案例：PostgreSQLEntityStore 110-111 的三重实验

createSchema 顺序执行三条 DDL（_entity_ids / _entity_index / _entity_properties）。
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
状态注入），收益不成比例。

---

## 7. 结论与通往 100% 的路径

**98.85% 是"不动产品结构、不做排除标记"约束下测试侧的可达上限。**
122 个 miss 每一行都有可复现的实验论证（本文 §5-§6）。

若以 100% 为目标，路径分三层，成本递增：

| 层 | 对象 | 手段 | 条数 |
| --- | --- | --- | --- |
| 1. 机械重构 | C+D 伪影（36 条） | 虚析构/inline 空体移出类定义（out-of-line 化），只剩一份被正常覆盖的定义 | 36 |
| 2. 死代码清理 | E+F 死防御（19 条） | 删除恒真防御 / 改写 switch 尾部（C++23 `std::unreachable()`） | 19 |
| 3. 显式排除 | A+B+G+H 真不可测（67 条） | `// LCOV_EXCL_LINE` 标记 + 理由注释，gcovr 8.6 原生识别 | 67 |

第 3 层是行业标准做法（codecov/coveralls 生态通用），前提是**透明**：每一处
排除都必须带注释说明为什么不可测，并且本文档同步更新。排除后分母变小，
"100%"的含义是"**全部可测代码 100% 覆盖 + 不可测代码显式豁免**"——这个数字
比掺水的 100% 有意义，也比含糊的 98.85% 可执行。
