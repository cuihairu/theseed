# Test Coverage Report — Composition of Line-Level 100% and the Explicit Exemption List

> **English** | [中文](coverage-report.zh.md)

> This document answers four things:
>
> 1. The current line-coverage numbers and their metric basis (how it is measured, how the denominator is computed, why it is trustworthy)
> 2. The complete sprint trajectory from 98.35% to 100% (coverage as a probe of test strength)
> 3. The characterization and rationale for all 131 lines of explicit exemption (`LCOV_EXCL`)
> 4. The reusable methodology distilled from the sprint

---

## 1. Overview and Metric Basis

| Metric | Value |
| --- | --- |
| Line coverage | **100.0%** (10555 / 10555) |
| Function coverage | 100.0% (1606 / 1606) |
| Exempt lines | 131 (unexempted denominator 10571, unexempted basis 99.1%) |
| Branch coverage | **100.0%** (7194 / 7194, basis after excluding throw edges and exempted edges, see §8.4) |
| Coverage build tree | `build/gcc-coverage` (gcc + `--coverage`) |
| Statistical scope | All source files and headers under `src/` (excluding `tests/`) |
| Test scale | 115 tests in the gcc-coverage tree (including the MySQL/PG integration section); clang18 / gcc13 trees have 103 each (no libmysql, DBApp falls back to the file backend) |
| Tooling | gcovr 8.6, `--filter src/`, `--txt` report |

### 1.1 Metric Basis: gcovr 8.6's `--txt` Is Actually an "Entry-Level" Determination (settled 2026-09-25)

gcovr may record multiple count entries for the same line (multiple ABI variants of the same symbol, template instances per (TU, parameter combination),
etc.). The aggregation semantics of the three output types are **not consistent**:

- **`--lcov`** (`DA:line_number,count`): **sums** all entries on the same line — any one entry hitting the line counts it as covered.
- **The Missing column of `--txt` / `--html`** (measured on gcovr 8.6): determines entry by entry
  `is_uncovered = is_reportable and count == 0` — **if any entry is 0 the line is reported as miss**,
  even if other instances on the same line hit dozens of times in aggregate.
- **`--json`**: retains per-entry raw data (including multiple entries on the same line and pre-exemption data); suitable for per-edge / per-instance
  reconciliation, but cannot be used directly as an aggregate conclusion.

The historical 100% (10440/10440) and the current 100% (10555/10555) in this report are both the **txt basis**,
i.e. de facto entry-level full driving: every line of every template instance must be executed at least once. This explains
why "per-TU backfill of calls" in the `31cc3ed`/`1393a88` era was necessary work for line coverage; it also explains
the three-basis contradiction when re-checking the line basis after the 2026-09-25 branch sprint — "json shows count=25, lcov shows DA:286,25,
txt reports miss" — the root cause being that 4 new template instances such as `callCellWith<string,int>` were never driven (see §8.4 for details).
**Reconciliation rules: use txt for exemption reconciliation; use txt for line-coverage conclusions;
when investigating a single-line suspicion, cross-verify with all three outputs.**

### 1.2 The Two Pitfalls of gcovr

- **`--json` output does not apply exclusion markers**; only `--txt/--html` do. Reconciling exempt lines with JSON produces
  the illusion that "the marker did not take effect" — reconciliation must use `--txt`.
- **Leftover gcno pollutes the report**: after a source file is deleted from the build, the old `.gcno`/`.o` remain in the build directory,
  and gcovr continues to count them in the denominator. After deleting a file, clean up the corresponding gcno/o as well.

### 1.3 Metric Basis Constraints (rules established after stepping on the pitfalls)

- **The denominator must be reconciled**. libgcov's `overwriting an existing profile data with a different checksum`
  warning is **not harmless** — missing data for a TU with a mismatched checksum makes the denominator artificially small and the coverage artificially high.
  Before judging a coverage change, confirm the denominator has not changed; a jump in the denominator after touching and recompiling the relevant TU is normal.
- **The SQL integration section is gated by environment variables**: `THESEED_MYSQL_*` / `THESEED_PG_*` (no `TEST` infix,
  **`THESEED_PG_DATABASE=theseed_test` must be set explicitly**, since the default database name does not exist and would make all PG tests fail).
  A mistyped variable name makes the SQL tests **silently skip**, ctest still fully green, but MySQL/PG source coverage drops to zero.
  CI has no database, so the entire SQL section is skipped — CI being green does not mean the SQL paths were verified.
- **Three-tree verification**: gcc-coverage (with SQL env) + clang18-debug + gcc13-debug (the latter two without
  SQL env, otherwise they hang on "mysql-backend init should fail"). All three must be green before pushing.
  Before building the clang18 tree, run `export LD_LIBRARY_PATH=/home/cui/.local/tools/clang18-root/usr/lib/x86_64-linux-gnu`.

---

## 2. Coverage Trajectory

2026-09-22 to 09-23, from 98.35% (entry-level) to line-level 100%:

| Commit | Topic | Coverage | miss |
| --- | --- | --- | --- |
| (Starting point) | Before the sprint began (entry-level) | 98.35% (10385/10559) | 174 |
| `c2b2504` | syncRealGhosts orphan-space branch + JSON escape path | 98.39% | 170 |
| `31cc3ed` | Entity.h all per-TU template misses zeroed + HotUpdate warning path | 98.68% | 139 |
| `1393a88` | ProcessSupervisor fork failure path + ObjectPool per-TU miss | 98.72% | 135 |
| `2d5332f` | **Product improvement**: bounded DB request wait + NetworkTransport flush strategy | 98.82% (10459/10584) | 125 |
| `3fd7f0f` | MySQL allocId read-back failure (query quota method) + zero-id creation defense | **98.85%** (10464/10586) | **122** |
| `6cfa8b7` | Coverage report written up (full classification of 122 miss) | — | — |
| 100% sprint | Three batches: test reinforcement and dead-code cleanup → 98 miss (line-level 99.1%) → explicit exemption of 131 lines → **line-level 100.0%** (10441/10441) | **100.0%** | **0** |
| Post-batch-four fixes | loadEntity registered-id dangling UB defense + UInt32/64 defaultValue unsigned parsing + decodeDelta count consistency gate (10451/10451) | **100.0%** | **0** |
| Branch 100% sprint | Batches five/six: exempt ~30 artifact/unreachable arms + backfill ~24 scenarios → branch 100.0% (7194/7194, `--exclude-throw-branches` basis) | **100.0% (branch)** | **0 (branch)** |
| Line-basis regression fix | Batch-six re-check found 4 new template instances never driven (exposed by the txt entry-level determination): EntityTemplateBranchTest backfilled the `<string,int>` four-state matrix + callDefMethod Cell/default arms; ObjectPoolTest backfilled the rvalue first-allocation growth scenario + OverAlignedObj resetFn. Line 100.0% (10555/10555) + branch 100.0% (7194/7194) | **100.0%** | **0** |

> **Denominator change log**: `efc7c55` (TickScheduler run() stop-race fix) deleted the
> `stopRequested_` clear line in run() — unexempted denominator 10441 → 10440, coverage remaining 100.0%. The two defect fixes after
> batch four (loadEntity registered-id defense + UInt32/64 unsigned parsing) and the decodeDelta
> count consistency gate (which also removed the implied header truncation check) together added executable lines,
> taking the denominator 10440 → 10451, coverage remaining 100.0%. During batches five/six, product evolution such as the dbRequest bounded-wait rework
> and the autoFlush strategy, plus new template instance lines, took the denominator 10451 → 10555 (functions
> 1588 → 1606), coverage remaining double 100.0%. The 10440/10441 appearing elsewhere in this document are
> historical numbers from the time they were achieved; this table is the current basis.

The technique by which the `31cc3ed` batch eliminated 31 entries: the callCellWith/callBaseWith/callDefMethod/
onPropertyChanged templates in Entity.h are counted per (line, TU instance); locate them per TU with gcov (gcc≥9 gcno files carry
a source-file suffix, so create a symlink to the target TU in /tmp: `ln -s <objdir>/Name.cpp.gcno Name.gcno`
then `gcov -b Name.cpp`), and backfill one call for each (parameter combination, side, success/NotConnected) combination.

The three batches of the 100% sprint:

1. **Dead interface and test reinforcement**: deleted the repo-wide zero-reference `IRuntimePhaseHook`; added contract tests for
   `IRuntimeTransport::tick` (in-memory implementation no-op, PipedTransport takes the base-class default).
2. **Product-side cleanup**: 5 always-true defenses changed to `assert` or removed (NetworkTransport decodeHeader,
   SpaceRuntime empty entity, EntityDefRegistry null pointer); 7 tail eliminations after exhaustive switch enumeration
   (legal cases merged into default, or replaced by a ternary expression per semantics); 6 gcc attribution artifacts rewritten
   (multi-line braced-init-list collapsed to one line, chained `<<` split into separate statements).
3. **Explicit exemption**: added `LCOV_EXCL` markers to the 131 lines experimentally demonstrated to be untestable (list in §5).

---

## 3. Product Hazards Fixed Along the Way During the Coverage Sprint

The value of a coverage probe lies not in the number itself but in the product defects it forces out. Six were fixed during the sprint:

1. **LoginApp::dbRequest busy-wait hang** (`2d5332f`): the original implementation was a `while(true)` busy-wait with no deadline,
   so after DBApp stopped serving (EOF) `receive` returned 0 forever → LoginApp hung at 100% CPU.
   Now there is a deadline (`dbRequestTimeout`, default 5s) + send return-value checks + response method validation.
2. **RemoteEntityStore::request isomorphic busy-wait** (`2d5332f`): added `requestTimeout`
   (constructor's 4th parameter, default 5s); on timeout it returns an empty `RuntimeInvocation`, with zero changes to the six callers.
3. **NetworkTransport BackPressure dead-branch activation** (`2d5332f`): send's tail always called
   flushOutbound, making back-pressure unreachable. Added `Config::autoFlush` (default true); when false, send
   only enqueues and tick/flush performs the flush, so BackPressure becomes reachable once the queue backs up to 256 entries.
4. **onPipeClosed dead-code removal** (`2d5332f`): zero callers (the structural reality that IBytePipe has no setOnClosed),
   so it was deleted outright rather than adding a test that nobody would ever call.
5. **BaseRuntime::createEntity's allocId()==0 defense** (`3fd7f0f`): when DBApp is unreachable
   RemoteEntityStore times out and returns 0, and the original implementation would register an entity with id=0 into the table — conflicting with the
   "no entity" sentinel semantics. It now refuses to register and returns nullptr.
6. **Assert-ification of always-true defenses** (100% sprint): NetworkTransport's decodeHeader return-value check
   and EntityDefRegistry's null-pointer defense are always true after pre-checks, so keeping them as-is would miss forever; they were changed to
   `assert` + a comment stating the precondition, so that when decodeHeader later gains field validation the assertion will immediately expose silent breakage.

There is also a semantic correction: DB unavailability is no longer counted in `challenge_failure_count` (semantic pollution); a new
`login_db_unavailable_count` counter was added.

---

## 4. Methodology Arsenal (reusable techniques)

Injection/localization techniques measured to be effective during the sprint, ordered by cost-effectiveness:

### 4.1 SQL DDL Failure Injection (the three-pronged approach + the quota method)

The goal is to construct the window where "the Nth statement succeeds and the (N+1)th fails":

| Technique | Applicable to | Principle |
| --- | --- | --- |
| ① Pre-create a same-named table with a **missing column** | PG / MySQL | Blocks CREATE INDEX, reporting "column does not exist" |
| ② Per-table grant difference | MySQL | GRANT ALL on a single table + database-level DML only, so the second CREATE TABLE hits a permission error |
| ③ Query quota method | MySQL | `ALTER USER trap WITH MAX_QUERIES_PER_HOUR 2` + `FLUSH USER_RESOURCES` + KILL the store connection |

Two key mechanisms of the quota method (both measured):

- MySQL quota checking **increments before it decides**: 0+1=1 < 2 lets the 1st statement (upsert) through, and the 2nd
  (SELECT LAST_INSERT_ID()) is rejected at 2/2 — only a quota of 2 precisely hits the "read-back failure" branch;
  a quota of 1 rejects the very first statement (taking the upsert-failed branch).
- ALTER USER does not take effect on existing connections (the limit is snapshotted at connection establishment, or the shared uc takes effect immediately; quota 2 is correct under either
  behavior), so the store's connection must be KILLed (query
  `information_schema.processlist`) + FLUSH, forcing ensureConnected to reconnect.

The trap objects (user/table/view) must be `DROP IF EXISTS` at the start of the section, ensuring idempotence after a FAILED early exit.

### 4.2 Fork Failure Injection (RLIMIT_NPROC)

`setrlimit(RLIMIT_NPROC, rlim_cur=1)`: the current uid already having ≥1 process is always true → fork returns
EAGAIN. Note that restoring the limit must come **before** the assertion; when getrlimit/setrlimit fail, PASS-skip to avoid a false red;
soft may be lowered arbitrarily (≤hard), and a per-process limit does not affect other ctest processes on the same machine.

### 4.3 Single-Threaded Transport Fake (FakeDbTransport)

Injection for driving the db auth path uses a hand-written `IRuntimeTransport` (on send it synchronously enqueues a canned response,
single-threaded, zero deadlock). **The dual InMemoryBytePipe + dual NetworkTransport scheme deadlocks under single-threading**
(the pump on the fake DBApp side never gets a chance to run), so don't try it again. Companion: LoginApp unit tests use `listenPort=0`
(TcpListener supports random ports) + `dbTransportFactory` injection.

### 4.4 Pitfalls of the psql Experimentation Technique

A `psql -c` multi-statement string is in a **single implicit transaction**, and `default_transaction_read_only` only affects
new transactions — to test transaction properties, each `-c` must be a separate statement (libpq's PQexec makes each one an independent implicit transaction,
consistent with the product code).

### 4.5 Visibility Check Before Deciding

Check member visibility before writing a test: the default throw at HotUpdate:128/130 is inside a **private** method,
unreachable from outside, so it purely wastes a round of "write test → run → find no hit".

### 4.6 Trivial Virtual Destructors Are Structurally Uncoverable (important lesson)

**gcc does not generate counting instructions for empty function bodies.** The C++ ABI requires a virtual destructor to generate three symbols:

- D0 (deleting destructor): called when the static type of `delete` is that class
- D1 (complete destructor): called when the dynamic type is exactly that class
- D2 (base destructor): called by the derived class destructor — but a trivial empty body is **inlined into
  the derived class destructor** by the compiler, so D2 itself has no machine code

For a pure interface class's `virtual ~X() = default;`, the line entries for all three symbol variants are **permanently 0**,
whether the definition is written in the header or moved to a .cpp (both positions measured to miss; out-of-lining is a negative optimization:
3 more source files and still equally uncoverable). A concrete class's outline trivial destructor
(`MySQLEntityStore::~MySQLEntityStore() = default;`) is the same.

**Conclusion**: trivial virtual destructor lines can only be exempted, with the marker placed on the definition line. Non-trivial destructors (with function-body statements) are unaffected.
This leads to: an interface class should only write a non-default destructor when it has non-trivial resource semantics; a pure marker interface accepts this one-line exemption.

### 4.7 Three Pitfalls of Marker Engineering

- `LCOV_EXCL_LINE` **only exempts the line the marker is on**. Writing it as a standalone comment line (where `// LCOV_EXCL_LINE` is followed on the next line
  by the statement) does not exempt the statement line — the marker must be placed at the **end of the statement line**, or the range must be wrapped with START/STOP.
- Splitting a line **carries the artifact along** rather than eliminating it: after splitting `std::visit(` and the lambda's opening brace into two lines, the one
  miss line becomes two miss lines (the attribution of the closure wrapper symbol moves with the split). An attribution artifact must either
  be restored to a single line + end-of-line marker, or have the whole expression wrapped with START/STOP.
- An exemption must carry a rationale comment (this repo's rule: the marker is immediately followed by a Chinese rationale), and leave a trace in §5 of this document.

---

## 5. Full Characterization of the 131 Lines of Explicit Exemption

Unexempted denominator 10572 lines, 98 miss (99.1%) → after exemption 10441 lines, 0 miss. The 89 marker points
(34 files) fall into seven classes by the mechanisms below. Each marker in the source has an accompanying rationale; here they are summarized by class.

### A. C++ ABI Structurally Untestable (trivial virtual destructors, 21 lines)

The `virtual ~X() = default;` of 18 pure interfaces/base classes (header-defined):
RuntimeTransport.h, TickScheduler.h, Space.h, RuntimeLoop.h (IServiceApp),
IORuntime.h, IBytePipe.h, Controller.h, BehaviorTree.h, AOI.h (RangeTrigger),
RedisProvider.h, Logger.h, Channel.h, IAccountStore.h, IEntityStore.h,
EntityQuery.h (IEntityQueryStore), ProcessSupervisor.h, MachineAgent.h, HostProbe.h
— the D0/D1/D2 three-symbol variants are permanently 0 (§4.6).

Plus the outline trivial destructors of 3 concrete classes: MySQLEntityStore, PostgreSQLEntityStore,
InMemoryRedisProvider (.cpp-defined, likewise no machine code and no counting).

### B. gcc Attribution Artifacts (12 lines)

The code did execute (provable from test assertions and function-body line counts); gcc attached the count entries to positions that are permanently 0:

| Location | Content |
| --- | --- |
| NetworkNode.cpp 18-25 | Destructor signature line and brace line (every function-body line has a count) |
| NetworkTransport.cpp 26 | The destructor line after single-lining (close() has a count) |
| EntityDefRegistry.cpp 59 | The closure wrapper symbol of the `std::function` assignment line |
| MetricsRegistry.cpp 229 | The closure wrapper symbol of the `std::visit` line (each branch of the lambda body has a count) |

### C. Process/System-Call Boundaries (~40 lines)

| File | Segments | Content |
| --- | --- | --- |
| ProcessSupervisor.cpp | 8 segments | The push_back window between reap and /proc enumeration; queryCurrentProcess (anonymous namespace and /proc is never empty, so the fallback at 325 is unreachable); the fork child body (execvp succeeds without writing gcda); kill failure that is not ESRCH (a managed child always has permission) |
| HostProbe.cpp | 4 segments | gethostname failure; the sysconf fallback after sysinfo always succeeds; /proc/stat parse failure; the getloadavg failure fallback |
| TcpListener.cpp | 1 segment | listen failure after a successful bind cannot be triggered reliably locally (fd exhaustion dies first in socket()) |
| LoginApp.cpp / NetworkNode.cpp | 1 segment each | A non-blocking connect to a port with no service is always EINPROGRESS; inet_pton has no DNS, so a bad hostname → 0.0.0.0 → still "succeeds" |

The system-call failure branches have no fault-injection point and cannot be injected in a targeted way; fork failure (EAGAIN) is already
genuinely covered using RLIMIT_NPROC (not among the exemptions).

### D. Client-Library Internal State / OOM (~30 lines)

| File | Segments | Content |
| --- | --- | --- |
| MySQLConnection.cpp | 9 segments | mysql_init/stmt_init OOM; bind_param/bind_result internal-state failure (libmysql does not error in bind on a parameter-count mismatch, and bad SQL takes the prepare/execute failure branch, which is already covered); under a client-side cursor, store_result/fetch is a pure in-memory traversal with no SQL-only injection surface (disconnection is absorbed by ensureConnected's ping+reconnect) |
| PostgreSQLConnection.cpp | 3 segments | captureError else; PQexecParams returning nullptr (ensureConnected guards all calls, and an error on a valid connection returns an error result rather than nullptr) |
| Tracing.cpp | 1 segment | The retry loop body entered only when the true random numbers are all 0 |

The query quota method has no room for extension on the stmt path — it was closed out by experimenting statement by statement.

### E. Semantic Defenses / Unreachable Fallbacks (~12 lines)

| Location | Content |
| --- | --- |
| HotUpdate.cpp 2 segments | Defensive throw inside a private method (unreachable from outside); semantic default |
| EntityDefLoader.cpp | The switch's default (Vector3 is intercepted by the branch above, String/Blob are intercepted by isVariableSized, an invalid type name throws outright — triple interception, with only -Wswitch completeness as fallback) |
| PostgreSQLEntityStore.cpp 110-112 | The 2nd DDL failure branch of createSchema — sealed off by three experiments under PG 16 (see §6) |
| EntityQuery.cpp / EntityData.cpp / EntityDef.cpp / Logger.cpp / MetricsRegistry.cpp | The default arm merged per semantics after exhaustive switch enumeration (legal types are fully enumerated, and default simultaneously carries semantics such as "record 0 for variable-length types") |

### Principles of Exemption

Every exemption must simultaneously satisfy: ① a reproducible experimental demonstration (this section and §6); ② an accompanying rationale at the marker;
③ the exemption is for "measurement-unreachable" rather than "too lazy to test" — any branch for which a test can be written gets a test, without touching the marker.
After exclusion the denominator shrinks, and the meaning of "100%" is "**100% coverage of all testable code + explicit exemption of untestable code**".

---

## 6. Deep-Dive Case: The Three Experiments for PostgreSQLEntityStore 110-111

createSchema executes three DDLs in order (_entity_ids / _account_index / index).
To cover the 2nd one's failure branch, we need to construct the window where "the 1st succeeds or is skipped and the 2nd fails".
Three routes were experimented one by one (PG 16.15), all sealed off:

1. **Dedicated restricted user**: for a user without schema CREATE privilege, executing
   `CREATE TABLE IF NOT EXISTS` on an **existing** table is also permission denied — the schema ACL check happens **before**
   the if_not_exists existence skip (opposite to MySQL's behavior).
2. **Default tablespace**: `SET default_tablespace='nonexistent'` is rejected by the GUC check hook
   at SET time; pointing at a real tablespace requires pre-creating a directory in the container filesystem, which is not self-contained.
3. **read-only transaction**: after `SET default_transaction_read_only=on`, `CREATE TABLE IF NOT EXISTS`
   on an existing table likewise reports "cannot execute CREATE TABLE in a
   read-only transaction" — the readonly check is done by statement type **before** analyze
   (check_xact_readonly), so it never reaches the if_not_exists skip.

Conclusion: SQL-only cannot construct this window. Covering it would require changing the product structure (per-statement independent transactions + intermediate
state injection), which is disproportionate to the benefit, so it is exempted.

---

## 7. Conclusion and Rules for Maintaining the Metric Basis

**Line-level 100.0% has been achieved** (10555/10555, plus 131 lines of explicit exemption; function-level 1606/1606;
branch-level 7194/7194, `--exclude-throw-branches` basis).
The credibility of this number rests on:

1. Every line exemption has an experimental demonstration and an accompanying rationale (§5, §6);
2. Denominator reconciliation (checksum warnings are not ignored, leftover gcno is cleaned up, gcda is cleared and a full rerun done before concluding);
3. The SQL integration section is really run with the full set of environment variables (`THESEED_PG_DATABASE` must not be omitted);
4. All three trees are green before pushing;
5. Line-basis conclusions always follow `--txt` (§1.1: txt is the entry-level determination, json/lcov will be artificially high).

**Rules**: New product code is not allowed to leave an unjustified miss — either write a test, or add a rationale-bearing
exemption marker in the format of §5 and add a characterization line to this document. When reviewing a coverage change, look at the denominator first, then the exemption
list diff, and only then the percentage itself.

---

## 8. Branch Coverage: Baseline and Characterization (2026-09-24)

The branch basis initially **set no 100% target** — sections 8.1-8.3 here record the baseline and the characterization of the miss composition;
after the batches five and six sprints on 2026-09-25 it has reached **100%** (see §8.4).

| Basis | Value |
| --- | --- |
| Raw branch coverage | 62.7% (7530 / 12003) |
| Exception edges (gcov `throw` branches) | 3567, of which 3533 miss — gcc generates normal/exception edges for every throwable statement, and the exception edge is unreachable without fault injection |
| **Business branch coverage after excluding exception edges** | **88.2%** (7507 / 8514) |

Characterization of the business branch miss composition (1014 of them):

1. **Unreachable short-circuit defense arms**: the "non-null but invalid" arm of the `!p || !p->isValid()` family — e.g.
   `Entity::bindBaseEntityCall` always carries a targetComponent (emplace has a value) and clear means
   reset, so the "present but optional has no value" state cannot be constructed through the public API (about 15-20
   miss in CellRuntime fall into this class: lines 220/927/958/1016, etc.).
2. **STL inline attribution**: exception-edge variants on call lines such as `std::vector` construction and `map::emplace`.
3. **Genuine business failure arms**: the middle arm of an `||` short-circuit (the entity is non-null but the state does not match), and timing states such as an AoI observer/
   target being removed after the event is produced — testable but requiring targeted scenarios, to be backfilled as needed.

End-to-end driving of the pump paths (`syncToBases` / `flushClientEvents` / `flushAoIEvents` / `flushWitnessSync`)
has been precedented by the `CellRuntimeBranchTest` P group: createCell builds an ownedEntities_
entity + `bindBaseEntityCall` + `emitToClient`/`setProperty`/`ensureWitness` +
`scheduler.runOnce()` + drain assertion.

Analysis tool: the branches array of `gcovr --json` carries a `throw` attribute, which precisely separates exception edges from
business branches (`--json` does not apply LCOV_EXCL, which does not conflict with the txt basis pitfall of §1.2 — this section only does
miss classification, not exemption reconciliation).

### 8.1 Batch Two: BaseRuntime / EntityDefLoader (2026-09-24)

Added `tests/core/BaseRuntimeBranchTest.cpp` (18 scenarios) and
`tests/core/EntityDefParserBranchTest.cpp` (20 scenarios), and checked the gcov text line by line to characterize
all remaining miss.

| File | Initial miss lines | Final value | Clearable by new scenarios | Characterized and retained |
| --- | --- | --- | --- | --- |
| BaseRuntime.cpp | 70 | 15 | 55 | 15 |
| EntityDefLoader.cpp | 58 | 11 | 47 | 11 |
| CellRuntime.cpp (batch one) | — | 60 | — | 60 (mostly the isValid family, see §8 above) |

Characterization of BaseRuntime's remaining 15 lines:

1. **The `findEntity` null arm of the timer callback lambda** (172-180 / 211-219, 8 lines):
   `completeBaseDestruction` calls `cancelEntityTimers` before erase, and
   `TimerWheel::advance` checks cancelled item by item on dequeue — the window where "the timer is alive but the entity has been deregistered"
   does not exist, so this is defense in depth.
2. **The always-true family of `EntityCall::isValid()`** (250 / 624 / 653 / 795, 4 lines):
   `isValid = targetComponent_.has_value()`, and `bindCellEntityCall` is the only
   emplace path and any ComponentId (including 0) sets a value, so a "non-null but invalid" EntityCall
   cannot be constructed through the public API — the same family as class 1 of §8.
3. `completeBaseDestruction` idempotent early exit (264), pumpInbound loop CFG attribution
   (376), syncToCells same-condition multi-copy edges (666).

Characterization of EntityDefLoader's remaining 11 lines:

1. **Exit phi / short-circuit chain copy attribution** (40 / 71 / 114 / 145 / 220 / 412): all three
   break logics and all parse exits have scenarios; the miss are gcc's multi-copy edges for the `||` chain and
   the function-exit phi.
2. **The defaultValue switch defense arms** (306 / 308 / 310 / 367 / 374): on the reachable type
   set `fixedSize` is always >0, String/Blob are intercepted before the switch, and the switch's
   default jump corresponds to an unreachable value.

Technique lessons (added in this batch):

- **gcovr's F/J direction cannot be read by intuition**: defense/failure arms are often placed in the fall-through position, where
  the miss edge with `fallthrough=true` is precisely the "condition-true early-exit arm". A conclusion may only be drawn after cross-checking
  against the line execution count in the `branch N taken X%` text of `gcov -b`.
- **ctest must run coverage serially**: with `-j` in parallel, multiple test processes concurrently write the same shared library's
  gcda, corrupting the data extensively and non-reproducibly manifesting as "some edges zeroed out".
- After changing tests you must `find . -name '*.gcda' -delete` and then do a full rerun, otherwise gcda with a mismatched
  checksum is silently refused for writing.
- For this repo gcovr must also add
  `--gcov-ignore-errors=no_working_dir_found --gcov-ignore-parse-errors=negative_hits.warn_once_per_file`
  (the former is the working directory recorded by gcda being lost, the latter is the NegativeHits of gcc bug 68080).

Two defects found along the way have been fixed (2026-09-24, after batch four): ①`loadEntity` previously had no defense against an
already-registered id — on a `map::emplace` collision the new entity has already been moved and the temporary pair destructed, so continuing to use the returned
pointer is dangling UB; it now directly refuses a registered id and returns nullptr (`testFactoryHook` was changed to take
the real load path after destroy, and a registered-id rejection scenario was added). ②EntityDefLoader's
UInt32/UInt64 defaultValue previously shared `stoi`/`stoll`, so legal in-domain values (>INT32_MAX /
>INT64_MAX) threw an uncaught `std::out_of_range`; the cases were split to use `stoull` (Int32/Int64
keep the fail-fast semantics of signed parsing), and the defaultValue matrix test was changed to use UINT32_MAX /
UINT64_MAX to lock down the full domain. Subsequently the ASan tree also exposed decodeDelta's missing count consistency
(a malformed count made `reserve` request a GB-scale allocation, a memory amplification) — after adding the count gate,
the header truncation check inside the original loop is mathematically implied by that gate (a minimum of 8 bytes per entry ⇒ on entering each entry,
cursor + remaining entry count × 8 ≤ payload length, provable by induction), so the redundant check was removed. The denominator
went 10440 → 10451, with line coverage remaining 100.0%.

### 8.2 Batch Three: EntityQuery / BaseApp (2026-09-24)

Added `tests/core/EntityQueryBranchTest.cpp` (6 scenarios) and
`tests/core/BaseAppBranchTest.cpp` (5 scenarios).

| File | Initial miss | Final value | Clearable by new scenarios | Characterized and retained |
| --- | --- | --- | --- | --- |
| EntityQuery.cpp | 45 edges / 24 lines | **0 edges / 0 lines (fully cleared)** | 45 edges | 0 |
| BaseApp.cpp | 41 edges / 30 lines | 12 edges / 9 lines | 29 edges | 12 edges |

The technique for fully clearing EntityQuery:

1. **decodeNative size-mismatch matrix** (24 lines × 9 instances): the template generates a CFG per instance of T,
   so for each of the 11 fixed-length types one shot with an empty lhs rawValue + a truncated rhs rawValue is needed.
2. **The three edges of the `!decodeNative(lhs) || !decodeNative(rhs)` short-circuit chain** (43-93): lhs
   failure takes the jump edge, rhs failure takes both edges of the second condition — two shots fully clear the 3
   miss per type.
3. **Floating-point `<=>` four-result matrix** (84/89): one shot each for less/greater/equal/unordered,
   with NaN triggering unordered (both directions); Bool (94) and String (97) likewise have their three result edges filled in.
4. The false arm of `QueryFilter::of(bool)` (194).

Characterization of BaseApp's remaining 12 edges:

1. **The always-true family of `EntityCall::isValid()`** (310 / 365): same characterization as §8.1 BaseRuntime
   — bind always sets a value, so "non-null but invalid" cannot be constructed publicly.
2. **init factory/restore loop defenses** (50 / 88): the loop iterates over `registry_.entityTypes()`
   so `createFactory` is always non-null; on a brand-new runtime `findEntitiesByType` is always empty.
3. **Always-non-null defenses and ops attribution** (96 ×4 / 104 / 108 / 114 / 147): constructor
   validation guarantees transport_/runtime_ are always non-null; the rest are copy edges from the aggregate initialization of ProcessInfo /
   OpsServer::Config.

New technique lessons from this batch:

- **A "bad size" payload must be constructed per type**: a fixed 2 bytes is exactly
  a legal size for Int16/UInt16, so after a successful decode it takes a real comparison rather than unordered. The correct approach is to truncate
  one byte per type.
- **A zero-property def cannot open the EnterGame empty-snapshot arm**: `buildFullPropertySnapshot`
  throws `invalid_argument` directly for a null store (the store is allocated per property layout, and zero properties means
  null). The correct approach is a single property with `flags="Base"` — the semantics of `buildFullPropertySnapshot
  (PropertyFlag::Base)` is to **exclude** Base-flagged properties.
- **`requestCreateCell` does not bind a cellCall**: after EnterGame the entity has no cell binding,
  so at this point `destroyEntity` destroys immediately and synchronously triggers `onEntityDestroyed`, clearing the session
  mapping — if the test then gets null from `findSessionByEntity` and calls `bindSessionToEntity`
  it binds null into the session table, and the next tick's flush segfaults. Deferred destruction requires an explicit
  `setCellEntityCall` first.
- **Zombie session injection method**: `takeClientSession(a disconnected ClientSession)` +
  `bindSessionToEntity(raw, id)` in one shot covers the "session already disconnected" early-exit
  arm of five classes of downlink callbacks and the `findEntity` null arm of cleanupClients, without any real TCP.

At this point all branch miss in the src/core directory are either zeroed or characterized (BaseApp's 12 edges
retained). Candidates for the next batch: SQL backend error arms such as PostgreSQLEntityStore (335 edges) /
MySQLEntityStore (313 edges).

### 8.3 Batch Four: SQL Backends and FileEntityStore (2026-09-24)

Added `tests/db/MySqlBranchTest.cpp`, `tests/db/PgBranchTest.cpp` (require the
`THESEED_MYSQL_HOST` / `THESEED_PG_HOST` environment, skipped when unset) and
`tests/core/FileEntityStoreBranchTest.cpp` (no external dependencies). FileEntityStore
and RemoteEntityStore already had high coverage from the existing loopback tests, so this batch only filled gaps.

| File | Final value (gcov text basis) | Composition |
| --- | --- | --- |
| FileEntityStore.cpp | **0 (fully cleared)** | — |
| MySQLConnection.cpp | 32 lines | 17 lines of exemption-region entry/never-exec + 15 lines characterized and retained (see below) |
| MySQLEntityStore.cpp | 14 lines | All characterized and retained |
| PostgreSQLEntityStore.cpp | 20 lines | 2 lines of exemption region + 18 lines characterized and retained |
| RemoteEntityStore.cpp | 1 line | Function-exit aggregate block (the timeout return path, L45) |

Starting-basis note: before the batch, business miss edges counted with `gcovr --json` were MySQLConnection 39 /
MySQLEntityStore 41 / PGStore 54 / FileEntityStore 5 / RemoteEntityStore 4
(143 total). **The json basis contains two classes of artifact**: all edges of uncalled instances of `-O2` function clones (constprop/partial)
count as 0; and when checking gcov text, `"0%" in line` misjudges
`taken 100%` as miss (because "100%" contains the substring "0%").
Final values always follow the basis of `gcov -b` text, the exact regexes `taken N%` / `never executed`, and exclusion of
`(throw)`.

Scenarios cleared in this batch:

1. **FileEntityStore 5 edges fully cleared**: allocId metafile read failure (chmod 000) and write failure
   (the `fs::create_directory(meta)` directory trick) — note that under non-root a chmod 000
   file is still readable by its owner, so the assertion must use the two-state `idA == 1 || idA == 6`; the directory-entry skip chain of listIdsByType
   / listEntityTypes uses a mixed directory of "entity + subdirectory + junk file".
2. **bytesToUint64 low-direction break** (MySQLConnection 26): `SELECT '-42'` —
   the first character `'-' < '0'` hits the low-direction arm; `'not-a-number'`'s first character `n > '9'` only takes the
   high direction (the `||` short-circuit chain's break is the same one, but gcc lays it out as two edges).
3. **sanitizeForTable full-spectrum characters** (46-48 of both stores): `"AZ[az{09:_-"`
   a single string covers both directions of each comparison segment — uppercase, `'['` after `'Z'`, lowercase, `'{'` after
   `'z'`, digits, `':'` after `'9'`, legal `'_'`, illegal `'-'`. The string is legal and not
   overlong, ensureTable builds the table normally (the sanitize branch is unrelated to the DDL result), and it ends with a DROP
   of `tbl_AZ_az_09___`.
4. **MySQL-specific**: the un-init defense family, overlong table name DDL failure (80 characters > the 64 limit,
   MySQL **errors**), empty blob refused on load, the knownTables_ cache-hit failure family after DROP,
   chained failure of DROP `_entity_ids` / `_account_index` / `tbl_Account`, blank charset,
   second disconnect, asBytes defense, empty parameter list, and wait_timeout=1 disconnect-and-
   reconnect (autoReconnect true → transparent reconnect succeeds / false → execute fails).
5. **PG mirror**: the feasible subset of the above. PG's ensureTable failure arm **cannot be injected
   SQL-only** — a 63-byte identifier is **truncated without error** (opposite to MySQL's error), so the DDL failure arm
   and the `!ensureTable()` short-circuit arm (load 184 / listIdsByType 277) are retained.

Characterization of the remaining miss (all retained):

1. **never-exec library inline edges** (the save/remove/queryAccount/
   createAccount statement lines and closing lines of both stores, MySQLConnection 313/314/341/394/395/
   410/411/445-447/458, etc.): private edges from inlining `std::string`/`ostringstream`/`unique_ptr`
   and the mysql API, `never executed` and with no business semantics.
2. **Always-true/always-false defenses**: `ScopedMsTimer`'s `if (emitter_)` (22/21 of both stores,
   both instantiations pass a literal lambda), allocId's `!result->next()` (SELECT
   LAST_INSERT_ID / RETURNING is always one row), the false arm of `starts_with("tbl_")`
   (LIKE / information_schema queries guarantee the prefix), the advance arm of MySQLResult constructed with a null res
   (74) and `cursor > rows.size()` (106, next() reaching the top does not
   advance, so cursor's upper bound is size, a redundant defense).
3. **EXCL exemption-region entry**: the fallthrough edge of the mysql_init / stmt_init failure arm
   (168/255/339, etc.) points into the already-exempted OOM region.
4. **Bind/retrieve failure arms** (bind_param / bind_result / store_result /
   fetch_column != 0): require driver-level fault injection, SQL-only unreachable.
5. **The multi-statement drain `more != nullptr` arm** (MySQLConnection 229): independent probes
   and fprintf instrumentation both prove it reachable (the subsequent result sets of `SELECT 1; SELECT 2; SELECT 3` are
   non-null), but under the full test load it misses intermittently — an environment timing-sensitive edge, not pursued for
   stable hits (the same treatment as the batch-three TickScheduler run race).
6. **remove's DELETE failure arm** (238/245 of both stores): once a table is DROPped it is no longer in
   the `listEntityTypes` result (SHOW TABLES / information_schema is a live query), so the two preconditions of "DELETE fails inside the loop"
   are mutually exclusive; the driver-level fault injection surface is not in the SQL layer.

New technique lessons from this batch:

- **SQL table-name casing triple**: `tableName("Avatar")` returns `tbl_Avatar` (preserving
  the uppercase); PG folds unquoted identifiers to lowercase and MySQL on Linux is case-sensitive — test SQL must
  write `"tbl_Avatar"` (PG, with double quotes) / `` `tbl_Avatar` `` (MySQL, backticks);
  the lowercase variant operates on a nonexistent table (all 9+7 failures in the first round were this).
- **knownTables_ cache semantics**: a successful ensureTable caches, and after a DROP it still hits the cache and skips
  the DDL → subsequent SQL fails outright — this is exactly the principle of the "cache-hit failure family after DROP" scenario;
  a defense scenario must finish its happy-path regression with a **new store instance** (empty cache → ensureTable rebuilds).
- **libmysql client four-way comparison method**: before asserting "some edge is unreachable", compare with an independent C probe
  (mariadb connector vs Oracle libmysqlclient × with/without OPT_RECONNECT) —
  this batch used probes to prove 229 reachable, thereby avoiding a wrong code change. Note that vcpkg's
  `unofficial-libmysql` actually installs **libmariadb**, whereas the build tree manifest installs
  **Oracle libmysqlclient** (a C++ implementation; linking needs g++ + zstd); probe compilation
  must explicitly use `/usr/bin/g++` (`~/.local/bin/g++` is a
  crippled shadow).
- **gcda accumulation illusion**: if gcda is not cleared between test-modification iterations, old scenario edge counts are added into the new
  gcda (L227 loops 3 times vs actually 2). Before the final check, `find . -name '*.gcda'
  -delete` + a full serial rerun is the only trustworthy basis.

Three-tree verification: gcc-coverage 114/114 (SQL env), clang18 108/108, gcc13
108/108 (the SQL backend is disabled in the latter two trees, so the branch test targets naturally do not exist).

### 8.4 Batches Five and Six: Branch Coverage Reaches 100% (2026-09-25)

Batch five (around the host crash) and batch six (two closing rounds) combined: about 30 exemptions (artifact/structurally
unreachable arms, `LCOV_EXCL_BR_LINE` line-level annotation) + about 24 backfilled scenarios, clearing business branch
miss from 78 to **0**:

| Basis | Value |
| --- | --- |
| **Branch coverage (final)** | **100.0%** (7194 / 7194) |
| Denominator basis | `gcovr --exclude-throw-branches` + after removing all exempted edges |
| Test scale | gcc-coverage tree 115/115 fully green (SQL env), never regressed throughout |

#### Exemption Mechanism Settled (three iron rules + one supplement)

1. **Function-tail merge pseudo-edges are attributed to the `}` line**: the merge pseudo-edge of `return x;` is attributed to the closing-brace line rather than the
   return line (TimerWheel 71/84, the tail of LoginApp dbRequest). The exemption must land on the
   `}` line.
2. **`LCOV_EXCL_BR_START/STOP` region exemption does not take effect for the branch metric** (measured on gcovr 8.6)
   — only the line-level `LCOV_EXCL_BR_LINE` works.
3. **Stale gcno causes false invalidation**: a header exemption requires recompiling all dependent TUs; when miss persists
   after changing an exemption, touch-recompile first before concluding (SocketDetail.h, Tracing settled this way).
4. **Supplement (batch six): the exemption line ≠ the gcc edge attribution line, and may be off by 1 line**. Typical: condition A's
   false arm is attributed to the **next physical line** (PropertyBlock 30's `!empty()` false arm
   is actually attributed to line 31, block 20 — only discovered by checking edge by edge under clean unit-test data); a signature-line exemption
   does not cover the return line's construction edge (TcpConnection.h 19→20). Four corrections: TcpConnection.h
   19→20, CellRuntime 967→968, LoginApp 50→52 (+70/+154), RealmApp 44→+42.

#### The Second Metric-Basis Pitfall of json vs txt

The start of §8 said "`--json` does not apply LCOV_EXCL" — batch six found a finer layer: **json's
branches are per-edge instance raw data** (the same logical edge may have multiple cloned instances, including miss edges
of exempt lines), while **txt aggregates per (line, branch) across instances and applies exemptions**. At br8, after removing
exemptions json still had 29 remaining vs txt's 8, and the difference set was entirely "single-instance miss, taken after cross-instance aggregation".
**Conclusion: json can only show single-edge direction and block numbering; miss counts and exemption reconciliation always
follow txt.**

#### The Three Truths of "Still miss after backfill testing" (batch six triage method)

For lines that still miss after clearing gcda and doing a full rerun, use the **five-step triage** to characterize edge by edge:

1. Clear all gcda → run the suspect test alone → full gcovr (clean data, consistent basis);
2. Read the taken/miss of **all edges** of that line from json (not just the miss edges);
3. Infer the business semantics of each edge from the block merge direction (combined with upstream invariants such as addProperty);
4. When necessary, write an independent probe program (a 10-line main linking the product library directly) to verify the invariant;
5. Handle by class: backfill testing for a genuine gap / EXCL for an artifact or unreachable arm.

Three examples:

- **PropertyBlock 30**: "still miss after backfilling fixed empty default value" — checking edge by edge, the missing
  `21->25 nt` is the **false arm of segment B's `desc.size > 0`** (A's false arm is attributed to line 31 and
  was already taken 7 times). `EntityDef::addProperty` always corrects the size of a fixed-length type to
  `fixedSizeOfType(type)` (probe-confirmed size=4), and variable-length types do not enter this branch →
  **structurally unreachable, EXCL**.
- **LoadProfiler 104**: the miss edge is the "brand-new entry + empty type" combination (existing scenarios all
  set the type first then pass empty) — a genuine gap, cleared by a single `scope(31, "")`.
- **TransportHub 67**: the two false arms of the awaitingIdentity short-circuit `count > 0 && source != 0`
  were never constructed — backfill the "empty transport (count==0) + message sourceComponent==0"
  two-arm scenario.

#### New Artifact Characterization from Batch Six (EXCL)

- **LoginApp 204**: a gcc cold-block copy from constructing the `"login:" + account` temporary (both arms permanently
  0, in the high-block-number region); the true and false arms of tryConsume are already covered by the rate-limit/pass scenarios.
- **LoadProfiler 125**: the inline branch constructing the string member inside the `return EntityTypeLoadSnapshot{}` aggregate
  (the same family as RealmApp ProcessInfo); the end() early-exit arm is already covered.

#### Batch Six Backfill Testing List

- LoginApp: rate-limiter configuration + empty account (the 203 second-segment short-circuit arm);
- PropertyBlock: fixed-length empty default value (stays zero) / fixed-length overlong default value (write refused);
- ClientSession: send after close (the isConnected false arm) + repeated close idempotence;
- TcpConnection: pump with no callback set (recv consumption counting, no delivery);
- NetworkNode: the true arm of the first condition listenPort>0 (binding an unroutable address fails reliably);
- LoadProfiler: brand-new entry with empty type; TransportHub: the two-false-arm scenario.

Three-tree verification: gcc-coverage 115/115 (SQL env), with clang18 and gcc-sanitize tree regressions
run separately (see the basis note in §8.3).

#### Line-Basis Regression and Settlement After the Branch Sprint (2026-09-25)

After the final branch-100% check, re-checking the line basis revealed `TOTAL 10554 10543 99%` — 11 lines miss
(Entity.h 286-288/290/308-309/312-313 + ObjectPool.h 155/165/176). Triage conclusion:

1. **Not stale gcda**: after clearing all gcda and doing a full rerun the 11 lines persisted (the third round confirmed that gcda must be
   cleared before triage, but one likewise cannot assume "clearing makes it disappear").
2. **Not deleted test calls**: the only deleted line on the tests side in migration commit `065a3e4`,
   `-    off += 4;`, is a decode bug fix (readU32 already advances internally), with zero logic deletion.
3. **The root cause is the txt entry-level determination settled in §1.1**: a per-entry dump of gcovr json shows that each miss
   line has exactly one count=0 **new template instance** —
   - `Entity::callCellWith<std::string,int>`: the `(string,int)` combination had previously only been driven via
     `callDefMethod("noSuchMethod",...)` (unknown-method early exit) and the Base arm
     (`addScore`→callBaseWith), and the Cell arm's inline chain and the four-state connection matrix were never reached;
   - `Entity::callDefMethod<std::string,int>`: same as above, missing the Cell arm (case lines
     308 + 309) and the default arm (312/313);
   - `ObjectPool<SimpleObj>::acquire<int,double,std::string>`: the rvalue instance
     was acquired only once when the pool had a free slot — the freeList-empty true arm (155 addBlock) and the
     water-mark-record true arm (165) were structurally untriggered;
   - `ObjectPool<OverAlignedObj>::release`: that scenario's pool did not set a resetFn, so
     176's true arm is permanently 0 for this T instance.
4. **Three-basis cross-verification method**: json (per entry, a 0 entry is immediately apparent) → lcov (`DA:line,sum`,
   a sum >0 creating the "covered" illusion) → txt (entry-level miss) — the contradiction among the three is itself
   the signature of "new instances not fully driven".

Fix (all backfill testing, zero new exemptions):

- EntityTemplateBranchTest: backfilled the `<std::string, std::int32_t>` combination for each of the four-state connection matrix (no transport / no cellCall / valid /
  invalid); callDefMethod backfilled the Cell arm of `(string,int)` (`castSpell`→Accepted) and the default arm
  (`announce`→NotConnected).
- ObjectPoolTest: before the rvalue scenario, insert a blockSize=1 standalone pool's rvalue first-allocation block
  (freeList empty → addBlock → active breaks zero → water-mark refresh); the OverAlignedObj pool adds a
  resetFn and asserts the callback count (incidentally strengthening the original
  alignment assertion).

Final check: line 10555/10555, branch 7194/7194, function 1606/1606, all three bases at 100%, 115/115 fully green.
Also: under full ctest concurrency with `-j 8`, MySQLEntityStoreTest fails occasionally (libmysql's SET NAMES
grabs the connection quota; it passes when run alone) — the line/branch final check always uses `-j 4`.
