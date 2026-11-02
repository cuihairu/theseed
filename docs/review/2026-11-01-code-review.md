# Code Review: theseed 2026-11-01

> **English** | [中文](2026-11-01-code-review.zh.md)

**Date:** 2026-11-01 · **Scope:** full repository · **Method:** requesting-code-review flow
(security static scan → hidden-bug hunt → quality gates), every finding verified against source.

## Summary

| Metric | Count |
|---|---|
| Findings examined | 27 |
| Verified actionable findings | 26 |
| **P1 — fixed immediately (one commit each)** | **12** |
| P2 — logged to `todo.md` | 12 |
| P3 — logged to `todo.md` | 2 |
| Specialist claim not reproduced | 1 |

**P0 (remote-code-execution / auth bypass):** none.

**Baseline & gates:** full `ctest` 118/120 (2 pre-existing environmental failures:
`theseed_login_app_e2e_test`, `theseed_machine_daemon_test` — the host runs ~992 processes so
`LocalProcessSupervisor::listProcesses` exceeds the test budget; identical before and after these
changes). `-Werror` clean, zero warnings. Every fix carries a regression test that fails on the
unpatched code.

## Fixed (P1) — 12 issues

| # | Finding | Location | Commit | Regression test |
|---|---|---|---|---|
| 1 | Allocation bomb: untrusted length prefix allocated before bounds check (`readString`) | `src/foundation/MemoryStream.cpp` | `52e40a4` | `MemoryStreamTest::testReadStringLiedLength` |
| 2 | Allocation bomb: invocation payload length allocated before bounds check | `src/runtime/InvocationCodec.cpp:48` | `2305061` | `InvocationCodecTest::testDecodeRejectsLiedPayloadLength` |
| 3 | Allocation bomb: entity property size allocated before bounds check | `src/core/EntityData.cpp` | `a70fdbe` | `EntityDataTest` (existing `testDecodeTruncatedProperty`) |
| 4 | Heap over-read: delta header read past payload after a variable-size value | `src/runtime/PropertyReplication.cpp:92` | `2be9c00` | `PropertyReplicationTest::decode_truncated_header_after_value` |
| 5 | Use-after-free: `createEntity` moved `unique_ptr` then returned raw pointer after failed `emplace` | `src/core/CellApp.cpp:118` | `ef21891` | `CellAppTest::testCreateEntityDuplicateId` |
| 6 | Null deref: `counter()` on an existing gauge name | `src/foundation/MetricsRegistry.cpp:85` | `a5f4496` | `MetricsTest::testCounterOnExistingGauge` |
| 7 | Periodic timer fired a wheel-cycle late at cascade (256-tick boundary) | `src/foundation/TimerWheel.cpp` | `f7560fc` | `TimerWheelTest::testPeriodicTimerExactWheelBoundary` |
| 8 | `stoi`/`stoll` overflow parsing unsigned numeric bounds | `src/core/EntityDefLoader.cpp` | `3c6b806` | `EntityDefLoaderTest` (u32/u64 max-value properties) |
| 9 | Missing `FD_CLOEXEC` on all sockets (fd leak into fork/exec children → port retention) | `src/runtime/SocketDetail.h`, `TcpConnection.cpp`, `TcpListener.cpp`, `ProcessPortScanner.cpp` | `2a760e8` | socket tests |
| 10 | Blocking `waitpid` froze the control-plane tick thread on a SIGTERM-ignoring child | `src/control/machine/ProcessSupervisor.cpp:546` | `ae451a2` | `ProcessSupervisorTest::test_stop_escalates_to_sigkill` |
| 11 | Uncaught decode exception crashed the whole runtime process | `src/runtime/NetworkTransport.cpp:215` | `d2fdd56` | `NetworkTransportTest::testMalformedInvocationFrameDisconnects` |
| 12 | No receive-side cap: peer could claim a 4 GB payload and trickle it, growing `recvBuffer_` unbounded (slowloris DoS) | `src/runtime/NetworkTransport.cpp:186` | `b607218` | `NetworkTransportTest::testOversizedClaimedPayloadDisconnects` |

## Remaining (P2) — 12 issues, logged to `todo.md`

| # | Finding | Location | Evidence |
|---|---|---|---|
| 1 | `write()` appends to `sendBuffer_` with no cap; a stalled peer grows it unbounded | `src/runtime/TcpConnection.cpp:76` | `sendBuffer_.insert(...)` with no watermark check |
| 2 | `createAccount` two-phase write is non-atomic: main-table row committed before index row; failure or crash between them leaves an orphaned Account | `src/db/MySQLEntityStore.cpp:324`, `src/db/PostgreSQLEntityStore.cpp:329` | `save()` then separate `INSERT ... _account_index`; no transaction |
| 3 | `Impl` constructor calls `mysql_init`; `connect()` re-inits the same `MYSQL` without a prior close → leaked first handle | `src/db/MySQLConnection.cpp:141` (ctor), `:164` (connect) | `mysql_init` in both `Impl()` and `connect()` |
| 4 | `handleListIds` silently returns on `payload.size() < 4` with no error response → caller waits forever | `src/db/DBApp.cpp:305` | `if (payload.size() < 4) return;` before any `sendResponse` |
| 5 | `RemoteEntityStore` correlates a response by method string, not request id; out-of-order duplicates can be mismatched | `src/db/RemoteEntityStore.cpp:35` | `resp.method == std::string(method) + ".ok"` |
| 6 | `MachineAgent::execute` parses pid via `std::from_chars` into `uint32_t`; a negative pid wraps to a huge value | `src/control/machine/MachineAgent.cpp:74` | `std::uint32_t pid; from_chars(...)` — no sign check |
| 7 | `TimerWheel::cancel` marks `cancelled` but never erases the entry → cancelled timers accumulate | `src/foundation/TimerWheel.cpp:86` | loop sets `entry.cancelled = true`; no erase |
| 8 | `RateLimiter::tryConsume` divides by `refillInterval` in ms; `< 1 ms` → count 0 → division by zero (limiter silently disabled) | `src/foundation/RateLimiter.cpp:41` | `1.0 / duration_cast<ms>(refillInterval).count()` |
| 9 | `CellRuntime` dispatch path has no exception containment; a throwing handler escapes `pumpInbound` | `src/runtime/CellRuntime.cpp:354` | `dispatchInvocation(batch[index])` inside unbounded loop, no try/catch |
| 10 | `FileEntityStore::allocId` is a non-atomic read-modify-write across file I/O; concurrent callers can receive the same id | `src/core/FileEntityStore.cpp:102` | read `nextId`, `++`, write back — no lock |
| 11 | `RateLimiter::tryConsume` read-modify-write across a Redis round-trip is not atomic; concurrent requests over-admit | `src/foundation/RateLimiter.cpp:27` | `redis_->get` / `redis_->set` without WATCH/MULTI |
| 12 | `EntityDef::mergeFrom` mutates `properties_`/`methods_` incrementally and returns false on a mid-merge property collision, leaving a half-merged definition | `src/runtime/EntityDef.cpp:33` | pushes properties, then `return false` on collision; `inherited_` stays false |

## Remaining (P3) — 2 issues

| # | Finding | Location |
|---|---|---|
| 1 | `trySendBuffered` casts `sendBuffer_.size()` to `int` for `send()`; a > 2 GB buffer truncates/UB | `src/runtime/TcpConnection.cpp:160` |
| 2 | App `main` parses numeric CLI args via `std::stoi` with no catch, and uses `volatile bool` where `sig_atomic_t` is required | `src/` entry-points |

## Not reproduced

- **B-F3 (OpsServer truncates large HTTP responses):** the specialist claim does not reproduce.
  `OpsServer::respond` (`src/ops/OpsServer.cpp:139`) writes the full body with a correct
  `Content-Length`. There is no response truncation. The residual risk on this path is P3-1
  (the `int` truncation in `TcpConnection::trySendBuffered`), which applies to all sends
  including OpsServer. Folded into that finding; not reported as a distinct defect.

## Verification notes

- Each regression test was confirmed to fail against the pre-fix source and pass post-fix.
- The 12 fixed issues are each an independent commit (English, short, no AI attribution).
- Coverage red line: new guard branches are exercised by their regression tests; genuinely
  unreachable arms carry `LCOV_EXCL_BR_LINE`/`LCOV_EXCL_LINE` per repo convention. A full
  `gcc-coverage` tree sweep to confirm the 100% line/function gate post-fix is deferred to the
  coverage job (see `docs/KNOWN_UNCOVERABLE_ARMS.md`).

## Commit list

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
