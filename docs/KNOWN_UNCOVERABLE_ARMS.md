# KNOWN_UNCOVERABLE_ARMS — Registry of Unreachable/Noise Arms in Branch Coverage

> **English** | 中文

The residual ledger at the branch layer (informational basis) after the line/function gates have settled at a constant 100%. An exclusion list from three rounds of branch harvesting
(control / login / runtime+foundation+db, see the respective batches in todo.md), after line-by-line arc-level screening:
**the arms below are not forced to pass by padding tests** — they are untestable or purely library-mechanical arcs.
Each entry carries a qualitative category and a source of endorsement; exemptions where product code is present all carry inline
`LCOV_EXCL_BR_LINE` / code comments.

Measurement basis [added 2026-09-30]: the line numbers in this ledger are based on "the union of two consecutive full
ctest runs after clearing gcda" — a single-round sweep will falsely report gaps because the timing arms of the in-process e2e tick loop are entirely
absent (the first round once falsely reported 88 entries for DBApp, and the union of the second round went to zero); do not pad the registry or force tests based on single-round
data.

[2026-10-01 re-review reversal round] Item-by-item re-review of the largest holders: MachineDaemon 77→74 arcs
(the three structural arms 25/545/1011 were closed out by added tests and removed), OpsControlCenter 34→33 (the 311
entityId hit arm closed out by added tests and removed), ProcessPortScanner 13→11 (the 172 fd exhaustion
arm and the 255 >64KB truncation arm were closed out by added tests and removed, with the two LCOV_EXCL regions in the source removed accordingly
and subsequent line numbers renumbered by -2), LoginApp 32 unchanged (argument strengthened and kept on record). The 34 OTel
noise entries are untouched. Total gap 198→192, and the line/function gates do not regress from their double 100%.

[2026-10-01 noise-ledger re-check follow-up round] Undecided structural arms/line batch: HostProbe 318
reversed and closed out and removed (sumNetworkBytes moved out of the anonymous namespace as a probe_detail test
seam + four defensive arms directly tested with a synthetic stream, the three LCOV_EXCL regions in the source removed and converted to real tests — total line count
12212→12215, branch denominator 9008→9018, all newly added arcs covered, zero-arc count unchanged);
SessionStore 44 re-recorded as-is (the original description wrongly pointed at the concatenation on line 41, while line 44 is actually the zadd
index-key fixed-literal construction); TcpConnection 165 reclassified from inline-noise to
structural platform window (the ENOTCONN true arm, empirically unreachable on this Linux host); Ops 150,
TickDiagnostics 39, TcpConnection 116, the LoginApp structural group, and the Scanner platform
group gained instruction-level endorsement (gcov -b -c per-arm taken counts). [2026-10-01 re-recorded
as-is] "Entity.h 12 template inline copy edges, total gap 192→204" was a non-gate single
sweep — the current two-round union gate actually measures those 12 line arc pairs as all non-zero (231 taken
[2,26], 326-331 [18]), while Entity.h's 21 true zero arcs (188/252/287/295/
305/323, non-throw) are all excluded by --exclude-unreachable-branches and do not enter
the gap; the total gap stays at 192 (gate branches 8826/9018, lines 12215/functions 1689
double 100%, GATE_EXIT=0).

[2026-11-15 G2-B re-baseline] Gate denominators as of the 2026-11-09 daemon split plus this round's
ProcessPortScanner 105/106 inline exclusion: **lines 12501/12501 and functions 1721/1721, double 100%
(GATE_EXIT=0)**; branches informational at 9119/9347 (local two-round union; gcc-coverage tree configured
at CI parity: gcc-14 + CMAKE_DISABLE_FIND_PACKAGE_PostgreSQL=ON). Reproduced on CI at run 43671280096
(12501/12501, 1721/1721, branches 9116/9343). The 4-arc branch-denominator delta between local (9347) and
CI (9343) is pre-existing (also present at baee29a) and was not chased — branch is informational. Scope note
for the figures above: only the six MachineDaemon family TUs were re-measured this round, so the other
entries keep their 2026-10-01 coordinates and arc counts un-re-measured; and the gcda snapshots were not
retained, so the "total gap 192" figure above remains the 2026-10-01 basis and is **not** today's count —
do not read it as current.

Categories: `variant-noise` (the 0-pairs of LogAttribute::Value's four-way std::variant conversion construction
attributed to the calling line, inlined by the gcc library); `inline-noise` (std-library inlined mechanical
arcs: hash buckets / string SSO copies / ternary merge copies); `structural` (structurally
unreachable: guard arms sealed off by upstream invariants, platform windows, the implicit exit of an
exhaustive enum, and exception-unwind landing pads — cleanup dispatch reachable only when a bad_alloc-level
exception is thrown inside the statement, untestable unless the test injects an exception).

## control

### MachineDaemon family (MachineDaemon.cpp + Process/Session/Diagnostic/Config/Detail)

> [2026-11-15 G2-B re-harvest] The 2026-11-09 split (commit 62fedfa) redistributed the former
> single 1861-line MachineDaemon.cpp verbatim (zero logic change) into six TUs under
> `src/control/machine/`. The 74-arc set was re-harvested on the ledger basis: gcda swept,
> then two consecutive full ctest runs (125/125 green, PID namespace) with a gcovr snapshot
> after each — the zero-arc sets are identical across both rounds (no first-round pseudo
> gaps this time). Conservation is exact: 74 arcs on 35 lines before and after the split.
> Gate basis after this round: 12501 lines / 1721 functions, double 100% (GATE_EXIT=0),
> branches 9119/9347 informational.
> The signature spot-check on the union set repeats the legacy shape: every gap line
> executed (minimum 1, among the machine.config.applied lines), all zero arcs paired never
> blocks (2/2 attr+call pattern; 4/4 on ternary/concatenation lines), zero throw-flagged
> arcs, all semantic arms taken in both rounds (Process:127 ternary non-zero arcs
> 11/4/4/4/11; Session:442 nested ternary 4/2/3/1/4/4/2/4/2; Config:53 concatenation
> 10/1/10/10/1/1/10/10/1; Diagnostic:171 ternary 6/4/6/4/4/6). The 2026-10-01
> classification (structural exception-unwind landing pads; objdump endorsement of the
> pre-split 171/373 dead blocks — endbr64 + construction-complete flag test/je +
> conditional ~basic_string) carries over unchanged: same lines, same codegen mechanism,
> only the coordinates moved.

Post-split coordinates, per TU [structural]:

- `MachineDaemon.cpp` 43/44/45 (logExecuteRejected attrs + "machine.execute.rejected" warn),
  90/91 (start "machine.daemon.started"), 113 (stop "machine.daemon.stopped"), 190/195
  (relayArtifacts "machine.profile.relayed"), 456/458 (execute audit "machine.execute"),
  544 ("machine.unknown_method" warn) — 22 arcs / 11 lines.
- `MachineDaemonProcess.cpp` 127 (terminate args ternary
  `parsed.ok ? std::to_string(...) : ""`), 188 ("machine.terminate" call) — 4 arcs / 2 lines.
- `MachineDaemonSession.cpp` 102/103 (kick-session), 173/174 (list-sessions), 292/293
  (kick-sessions), 406/407 (extend-sessions) attr+call pairs, 442/443 (draining nested
  ternary), 474 ("machine.set-draining" call), 530 ("machine.shutdown" call),
  573 ("machine.session.notify.dropped" warn) — 28 arcs / 13 lines.
- `MachineDaemonDiagnostic.cpp` 93 ("machine.profile.trigger" call), 171 (profile-download
  args ternary) — 4 arcs / 2 lines.
- `MachineDaemonConfig.cpp` 53 (config.apply args concatenation
  `parsed.key + "=" + parsed.value`), 102/103/104 ("machine.config.applied" attrs + call)
  — 10 arcs / 4 lines.
- `MachineDaemonDetail.cpp` 67/68/69 (rejectControlAction attrs + call) — 6 arcs / 3 lines.

History: [2026-09-30 harvest] 77 arcs registered; [2026-10-01 reversal round] three structural
arms closed out by added tests and removed (toBytes empty-string defensive arm; auditSink
publish empty-identity skip arm; the `ok ? 0x01 : 0x00` false arm — ESRCH race semantics with a
deterministic stand-in), 77→74; the same day the mechanism attribution was reversed from
variant-noise/inline-noise to structural EH landing pads (instruction-level evidence), which
this re-harvest re-confirms at the new coordinates.

### OtlpTraceExporter.cpp
[2026-09-30 fifth batch of branch harvesting] Of the 21 registered gaps, 3 true arms were closed out by added tests:
315 the from_chars parse-success-but-trailing-remainder arm (a `http://…:6553x` rejection-table entry added),
375 the break arm for a character >'9' immediately after the status code (an `HTTP/1.1 200x` pure-function assertion
added), 409 the ≥200 false arm for a real transport receiving a 1xx status (a loopback collector-side Reply100
directly testing postViaTcp added) — after re-sweeping the remainder, it is registered as follows:
- 222-223/250-252/256/503/506 [variant-noise]: log attr lines (invalid
  endpoint warn, enabled six attributes, failed warn).
- 337 the 2 arcs at the head and tail of the single-span overload delegation line [inline-noise]: std::vector list
  initialization mechanical arcs, the line is executed; the semantic arms are covered by the shared implementation of the batch overload.

### OtlpMetricsExporter.cpp
- 161-162/170-175/250/253 [variant-noise]: log attr lines (invalid
  endpoint warn, enabled four attributes, failed warn). [2026-09-30 fifth batch
  sub-harvest registration]

### OpsControlCenter.cpp
- 62-64/103/121/210/230/259/286/297/337/358/380 [variant-noise].
- 98 the six 0-arcs of try_emplace [inline-noise]: std-library inlining (hash collision/rehash),
  both the inserted true/false semantic arms are tested (12 true/1 false).
- 150 the `!insertionOrder_.empty()` false arm [structural]: order and nodes_ are maintained in
  sync (insertion grows in pairs in sync, eviction pops in pairs), and the eviction path is reachable only after nodes_ grows past
  capacity, at which point order is necessarily non-empty — the empty-table defensive arm has no path. [2026-10-01
  follow-up round instruction-level endorsement] All 16 eviction evaluations took the true arm, and the false arm taken 0.

### ProcessPortScanner.cpp
- 96 the mid-iteration error arm [structural]: process disappearance/permission race cannot be stably injected
  (LCOV_EXCL region). [2026-10-01 follow-up round instruction-level endorsement] Across 3,768,066 iterations
  the error arm evaluated taken 0.
- 105/106 the fd-close race arm (`if (linkError)` true arm and its `continue`)
  [2026-11-15 G2-B round addition, structural platform window]: read_symlink fails only when the scanned
  process closes an fd mid-scan — not stably injectable; under the PID-namespace sandbox /proc is quiet
  and the true arm evaluated taken 0 across three full local rounds (4,151-8,836 iterations each), while
  CI's real-host runner observes the race at least once per run (line gate 12502/12502, run 43626120187).
  The `continue;` line carries an inline LCOV_EXCL_LINE since this round (same treatment as the 96
  iteration-error arm, line gate only; inline so the 105/106 coordinates stay stable); the 105 true arm
  remains a live registered branch gap. A local gate reading 12501/12502 = 99.9% before this round was
  this arm, not a regression — do not pad tests.
- 111 the `native.back() != ']'` true arm [structural]: /proc fd links are always well-formed
  (socket:[inode]), a malformed-form defense. [2026-10-01 follow-up round instruction-level endorsement]
  Across 3,487,977 reads the true arm's 2 arcs evaluated taken 0.
- 134/138/142/187/189/198/200/212/219/221/229/230/232/233/238/240
  [structural]: resource-exception and platform-window arms (/proc always present, fcntl/getsockopt/setsockopt on a
  legitimate fd does not fail, Linux non-blocking TCP connect always
  EINPROGRESS, a failed send on a connected loopback socket requires an immediate RST). [2026-10-01 reversal
  round renumbering] The original 172 fd<0 arm and 255 >64KB truncation arm were closed out by added tests
  (setrlimit lowering RLIMIT_NOFILE to inject fd exhaustion; a 70KB loopback reply to inject read-loop out-of-bounds truncation —
  the total-size guard's semantics are precisely a fallback for abnormally large responses), with the two LCOV_EXCL regions in the source
  removed and the two lines converted to real tests, and subsequent line numbers shifted by -2 (this entry's line numbers are already calibrated to the new source).
  The erratum that inline exemptions are effective for gcovr 8.6 (2026-09-30 batch, see todo.md for details)
  is unchanged: the registry remains a qualitative/endorsement ledger, not the sole basis for gcovr exclusion. [2026-10-01
  follow-up round instruction-level endorsement] Line 187 across 26 evaluations has 3 arcs, and 142/212/219/238 with 1 arc each
  all evaluated taken 0.
- 197 the connect synchronous-success arm (0F) [structural]: Linux non-blocking connect returns 0 only after the
  handshake completes, loopback handshakes are asynchronous, so it is always EINPROGRESS (2026-10-01 on-host evidence:
  non-blocking connect to 127.0.0.1:9/:1 and 240.0.0.1 always yields errno 115).
  [2026-10-01 follow-up round instruction-level endorsement] Across 26 connect evaluations the synchronous-success arm taken 0.

### HostProbe.cpp
- 318 the all-whitespace-before-colon defensive arm [2026-10-01 reversed and closed out and removed]: sumNetworkBytes
  is a pure string-computation surface (istream injectable), and anonymous-namespace privacy was the only
  obstacle to adding tests — after moving it out as `probe_detail::sumNetworkBytes` (declared in HostProbe.h,
  compiled on all platforms), four defensive arms are directly tested with a synthetic stream (header without colon / all-whitespace before colon / rx column
  non-numeric / tx column missing truncation, HostProbeTest::testSumNetworkBytesSyntheticStream).
  The three LCOV_EXCL regions in the source (BR_LINE/EXCL_LINE/EXCL_START/STOP) were removed accordingly
  and converted to real tests: gate line count 12212→12215, branch denominator 9008→9018, all 10 newly added arcs
  covered, zero-arc count unchanged at 192, and the line/function double 100% does not regress. The original claim "the anonymous-namespace
  parser has no injection seam and data lines always have an interface name" is invalidated once the test seam holds.

## login

### LoginApp.cpp
- 243/354 [structural]: LCOV_EXCL_BR_LINE already marked — Linux non-blocking connect
  is always EINPROGRESS, so the factory/real connect failure arm is unreachable (2026-10-01 on-host
  evidence endorsement: non-blocking connect to 127.0.0.1:9/:1 and 240.0.0.1 always yields
  errno 115, corroborating ProcessPortScanner:197).
- 306-307/413-414/474-475/478/493-494/499-500 [variant-noise].
- 318/425 the `!hub_` arm [structural]: the `if (hub_)` in tick() wraps all
  supervise calls, and after stop() sets Backoff, tick no longer enters supervision. [2026-10-01
  follow-up round instruction-level endorsement] 318 executed 1039 times: the machineHost empty arm 390/non-empty 649
  are all taken, and the `!hub_` true arm taken 0.
- 320/427 the switch implicit-exit arc [structural]: LinkState's three enums are fully covered with no
  default. [2026-10-01 follow-up round instruction-level endorsement] 320's three case counts 52/504/93,
  implicit exit taken 0.
- 322/429/328/435 the null-transport arm [structural]: in the PendingAck/Up states
  transport is necessarily non-null (the state is set only when an attempt succeeds; failure/markDown immediately resets + Backoff).
  [2026-10-01 follow-up round instruction-level endorsement] 322 across 52 PendingAck and 328 across 504
  Up evaluations both have the null-transport arm taken 0; the isConnected true/false arms are covered
  (50/2 and 494/10).

## foundation

### SessionStore.cpp
- 44 the 4 trailing zero arcs [inline-noise] [2026-10-01 follow-up round re-recorded as-is]:
  the index-key fixed-literal construction arc of `redis_->zadd(std::string(kIndexKey), token, indexScore())`
  — kIndexKey is a 14-character constexpr (always takes SSO, so the heap
  allocation/copy alternative blocks are dead ends; token is passed by const reference and is not constructed on this line).
  gcov instruction-level: the line executed 302 times, both sides of && and each semantic arm pair are all non-zero, branches
  19/20/22/23 + call 21 never executed. The original description "`kSessionKeyPrefix +
  token` concatenation SSO/heap copy library arcs" wrongly pointed at the concatenation statement on line 41 (which has no zero arcs), hereby
  corrected; the dual-write contract semantic arms are all tested (SessionStoreTest).

## runtime

### TickDiagnostics.cpp
- 39 [variant-noise]: the logWarn three-attribute line. [2026-10-01 follow-up round instruction-level endorsement]
  The line executed 42 times; calls 14/15 + branches 16/17 + call 18 never executed
  = the unused-alternative path of the std::variant construction (the three attributes are fixed as int64 in the source, so the construction blocks
  of the other alternatives are never executed).

### TcpConnection.cpp
- 116 the pending idle-spin arm [structural]: SO_ERROR ∈ EINPROGRESS only appears on a real network where the handshake
  has not settled across ticks, whereas the kernel loopback handshake completes synchronously; the semantic set is anchored
  by TcpConnectionTest directly testing connectStillPending, with a code-comment endorsement.
  [2026-10-01 follow-up round instruction-level endorsement] All 4 SO_ERROR≠0 evaluations took the disconnect arm,
  and the EINPROGRESS re-check false arm taken 0.
- 165 [2026-10-01 follow-up round reclassification: inline-noise → structural platform window] The zero
  arc is the true arm of notConnectedYet() at the tail of the `||` chain (the ENOTCONN fallback return false):
  the wouldBlock false side fell to the false arm all 20 times (the EPIPE true-error disconnect is pinned down by the RST send-arm assertion),
  and the true arm taken 0 — a non-blocking send within the Linux handshake window is always EAGAIN
  (2026-10-01 on-host evidence errno 115, corroborating LoginApp:243/Scanner:197),
  and the ENOTCONN hit surface is on macOS (the code comment itself admits "high hit rate, an extremely narrow window on Linux"),
  making it a platform-window structural arm rather than a library-mechanical arc.

### Entity.h
- 231-232/326-331 (paired) [inline-noise]: the copy edges of the multiple-instantiation inline expansion of the templates `onPropertyChanged` and
  `bindTypedMethodHandler` — the source-fixed parameters
  trigger only the already-tested instantiations, and the construction/branch blocks of the other alternative paths are dead code. gcov instruction-level
  shows the corresponding lines' execution count >0 but their branch/call counts all 0. [2026-10-01 re-recorded
  as-is] This entry's 12 arcs cannot be reproduced under the current two-round union gate basis — the 231/232/326-331
  arc pairs are all non-zero (231 taken [2,26], 326-331 [18]), with line execution >0 and the arcs also
  >0; Entity.h's current 21 true zero arcs are at 188/252/287/295/305/323 (non-
  throw), all excluded by --exclude-unreachable-branches and not entering the gate
  gap. "Total gap 192→204" is hereby corrected as-is: the gate basis stays at 192 (measured
  branches 8826/9018).

---

Maintenance discipline: new harvesting rounds are appended to this file (file:line + category + a one-sentence endorsement),
and cross-referenced in the corresponding batch in todo.md; when an entry is removed (the arm becomes testable and is covered by added tests), it is deleted in sync.
