# 代码深审：theseed 2026-11-01

> [English](2026-11-01-code-review.md) | **中文**

**日期：** 2026-11-01 · **范围：** 全仓 · **方法：** requesting-code-review 流程
（安全静态扫描 → 隐藏 bug 猎捕 → 质量门），每条发现均对照源码逐条验证。

## 总账

| 指标 | 数量 |
|---|---|
| 审查发现总数 | 27 |
| 经验证可行动的发现 | 26 |
| **P1 — 当场修复（一问题一 commit）** | **12** |
| P2 — 记入 `todo.md` | 12 |
| P3 — 记入 `todo.md` | 2 |
| 专项结论未复现 | 1 |

**P0（远程代码执行 / 鉴权绕过）：** 无。

**基线与质量门：** 全量 `ctest` 118/120（2 例为既有环境性失败：
`theseed_login_app_e2e_test`、`theseed_machine_daemon_test` —— 宿主机约 992 个进程导致
`LocalProcessSupervisor::listProcesses` 超出测试预算；本轮改动前后完全一致）。
`-Werror` 零警告。每项修复均带回归测试，且该测试在未打补丁的代码上必失败。

## 已修复（P1）— 12 项

| # | 发现 | 位置 | Commit | 回归测试 |
|---|---|---|---|---|
| 1 | 分配炸弹：不可信长度前缀先分配后界检查（`readString`） | `src/foundation/MemoryStream.cpp` | `52e40a4` | `MemoryStreamTest::testReadStringLiedLength` |
| 2 | 分配炸弹：invocation payload 长度先分配后界检查 | `src/runtime/InvocationCodec.cpp:48` | `2305061` | `InvocationCodecTest::testDecodeRejectsLiedPayloadLength` |
| 3 | 分配炸弹：实体属性 size 先分配后界检查 | `src/core/EntityData.cpp` | `a70fdbe` | `EntityDataTest`（既有 `testDecodeTruncatedProperty`） |
| 4 | 堆越界读：变长 value 之后 delta header 读取越过 payload 末尾 | `src/runtime/PropertyReplication.cpp:92` | `2be9c00` | `PropertyReplicationTest::decode_truncated_header_after_value` |
| 5 | Use-after-free：`createEntity` 移动 `unique_ptr` 后 `emplace` 失败仍返回裸指针 | `src/core/CellApp.cpp:118` | `ef21891` | `CellAppTest::testCreateEntityDuplicateId` |
| 6 | 空指针解引用：对同名既有 gauge 调 `counter()` | `src/foundation/MetricsRegistry.cpp:85` | `a5f4496` | `MetricsTest::testCounterOnExistingGauge` |
| 7 | 周期定时器在级联时刻晚一整个轮周期（256 tick 边界）触发 | `src/foundation/TimerWheel.cpp` | `f7560fc` | `TimerWheelTest::testPeriodicTimerExactWheelBoundary` |
| 8 | 无符号数值边界解析 `stoi`/`stoll` 溢出 | `src/core/EntityDefLoader.cpp` | `3c6b806` | `EntityDefLoaderTest`（u32/u64 maxValue 属性） |
| 9 | 全部套接字缺 `FD_CLOEXEC`（fd 泄漏进 fork/exec 子进程 → 端口滞留） | `src/runtime/SocketDetail.h`、`TcpConnection.cpp`、`TcpListener.cpp`、`ProcessPortScanner.cpp` | `2a760e8` | 套接字测试 |
| 10 | 阻塞式 `waitpid` 在忽略 SIGTERM 的子进程上冻结控制面 tick 线程 | `src/control/machine/ProcessSupervisor.cpp:546` | `ae451a2` | `ProcessSupervisorTest::test_stop_escalates_to_sigkill` |
| 11 | decode 异常未捕获，整个 runtime 进程崩溃 | `src/runtime/NetworkTransport.cpp:215` | `d2fdd56` | `NetworkTransportTest::testMalformedInvocationFrameDisconnects` |
| 12 | 接收侧无上限：对端可谎报 4GB payload 并慢速滴流，`recvBuffer_` 无界增长（slowloris DoS） | `src/runtime/NetworkTransport.cpp:186` | `b607218` | `NetworkTransportTest::testOversizedClaimedPayloadDisconnects` |

## 待办（P2）— 12 项，已记入 `todo.md`

| # | 发现 | 位置 | 证据 |
|---|---|---|---|
| 1 | `write()` 向 `sendBuffer_` 追加无上限；对端滞止时无界增长 | `src/runtime/TcpConnection.cpp:76` | `sendBuffer_.insert(...)` 无水位检查 |
| 2 | `createAccount` 两阶段写非原子：主表行先落库再写索引行；中间失败/崩溃遗留孤儿 Account | `src/db/MySQLEntityStore.cpp:324`、`src/db/PostgreSQLEntityStore.cpp:329` | 先 `save()` 再独立 `INSERT ... _account_index`；无事务 |
| 3 | `Impl` 构造函数调用 `mysql_init`；`connect()` 对同一 `MYSQL` 重复 init 且未先 close → 首个句柄泄漏 | `src/db/MySQLConnection.cpp:141`（构造）、`:164`（connect） | `Impl()` 与 `connect()` 两处 `mysql_init` |
| 4 | `handleListIds` 对 `payload.size() < 4` 静默返回、不发错误响应 → 调用方永久等待 | `src/db/DBApp.cpp:305` | 任何 `sendResponse` 之前 `if (payload.size() < 4) return;` |
| 5 | `RemoteEntityStore` 按 method 字符串而非 requestId 关联响应；乱序重复可能错配 | `src/db/RemoteEntityStore.cpp:35` | `resp.method == std::string(method) + ".ok"` |
| 6 | `MachineAgent::execute` 用 `std::from_chars` 解析 pid 到 `uint32_t`；负数 pid 回绕成巨大值 | `src/control/machine/MachineAgent.cpp:74` | `std::uint32_t pid; from_chars(...)` 无符号检查 |
| 7 | `TimerWheel::cancel` 只置 `cancelled` 从不删除条目 → 已取消定时器持续累积 | `src/foundation/TimerWheel.cpp:86` | 循环内 `entry.cancelled = true`；无 erase |
| 8 | `RateLimiter::tryConsume` 按毫秒除以 `refillInterval`；`< 1 ms` → count 0 → 除零（限流器静默失效） | `src/foundation/RateLimiter.cpp:41` | `1.0 / duration_cast<ms>(refillInterval).count()` |
| 9 | `CellRuntime` 分发路径无异常隔离；抛异常的 handler 逃逸 `pumpInbound` | `src/runtime/CellRuntime.cpp:354` | 无界循环内 `dispatchInvocation(batch[index])`，无 try/catch |
| 10 | `FileEntityStore::allocId` 跨文件 I/O 的非原子读改写；并发调用可拿到同一 id | `src/core/FileEntityStore.cpp:102` | 读 `nextId`、`++`、写回 —— 无锁 |
| 11 | `RateLimiter::tryConsume` 跨 Redis 往返的读改写非原子；并发请求超发 | `src/foundation/RateLimiter.cpp:27` | `redis_->get` / `redis_->set` 无 WATCH/MULTI |
| 12 | `EntityDef::mergeFrom` 增量改动 `properties_`/`methods_`，中途属性冲突即返回 false，遗留半合并定义 | `src/runtime/EntityDef.cpp:33` | 先 push 属性、冲突处 `return false`；`inherited_` 保持 false |

## 待办（P3）— 2 项

| # | 发现 | 位置 |
|---|---|---|
| 1 | `trySendBuffered` 将 `sendBuffer_.size()` 强转为 `int` 传给 `send()`；超过 2GB 截断/UB | `src/runtime/TcpConnection.cpp:160` |
| 2 | 各 app `main` 用 `std::stoi` 解析数字命令行参数且无 catch，并以 `volatile bool` 替代 `sig_atomic_t` | `src/` 各入口 |

## 未复现

- **B-F3（OpsServer 截断大 HTTP 响应）：** 该专项结论未复现。
  `OpsServer::respond`（`src/ops/OpsServer.cpp:139`）完整写出 body 且 `Content-Length`
  正确，不存在响应截断。该路径的残余风险是 P3-1（`TcpConnection::trySendBuffered` 的
  `int` 截断），对包括 OpsServer 在内的所有发送生效。已并入该条，不作为独立缺陷上报。

## 验证说明

- 每个回归测试均在未修复源码上确认失败、修复后通过。
- 12 项修复各自独立成 commit（英文、简短、无 AI 署名）。
- 覆盖率红线：新增守卫分支均由回归测试覆盖；确实不可达的臂按仓内惯例登记
  `LCOV_EXCL_BR_LINE`/`LCOV_EXCL_LINE`。修复后全量 `gcc-coverage` 树清扫以确认行/函数
  双 100% 门，延后至覆盖率任务执行（见 `docs/KNOWN_UNCOVERABLE_ARMS.md`）。

## Commit 清单

```
b607218 fix: reject frames claiming more than maxMessageSize on receive
d2fdd56 fix: drop malformed invocation frames instead of crashing
ae451a2 fix: bound waitpid with SIGKILL escalation in process supervisor
2a760e8 fix: set FD_CLOEXEC on all sockets
3c6b806 fix: parse unsigned numeric bounds without stoi/stoll overflow
f7560fc fix: fire due timer at cascade in TimerWheel advance
a5f4496 fix: check metric type in MetricsRegistry counter
ef21891 fix: guard duplicate id in CellApp createEntity
2be9c00 fix: bound delta header before memcpy in decodeDelta
a70fdbe fix: bound property size before allocation in entity decode
2305061 fix: bound payload length before allocation in invocation decode
52e40a4 fix: guard string length before allocation in readString
```
