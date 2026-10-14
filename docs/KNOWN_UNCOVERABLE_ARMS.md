# KNOWN_UNCOVERABLE_ARMS — Registry of Unreachable/Noise Arms in Branch Coverage

> **English** | [中文](KNOWN_UNCOVERABLE_ARMS.zh.md)

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

Categories: `variant-noise` (the 0-pairs of LogAttribute::Value's four-way std::variant conversion construction
attributed to the calling line, inlined by the gcc library); `inline-noise` (std-library inlined mechanical
arcs: hash buckets / string SSO copies / ternary merge copies); `structural` (structurally
unreachable: guard arms sealed off by upstream invariants, platform windows, the implicit exit of an
exhaustive enum, and exception-unwind landing pads — cleanup dispatch reachable only when a bad_alloc-level
exception is thrown inside the statement, untestable unless the test injects an exception).

## control

### MachineDaemon.cpp
- 171-173/191-193/373-374/396/473/478/731/733/819/949/1010/1089/1167/
  1241/1290-1292/1371-1372/1442-1443/1561-1562/1675-1676/1711-1712/
  1743/1799/1842 [variant-noise / inline-noise]: log attr lines and ternary/
  string-concatenation merge arcs. [2026-09-30 renumbering] After the asynchronous batch wiring, the source file's line numbers
  shifted down as a whole (same values from 171, 362-363→373-374 and 385→396 are +11, and 457 onward
  +16), with the classification unchanged. [2026-10-01 re-check] The remaining 35 lines all satisfy the noise signature
  (the line is executed + a 0-arc exists); 396 is the logInfo("machine.daemon.stopped")
  line, whose 0-arc pair is the construction arc of an unused attr variant alternative (a string argument always takes
  the string alternative), while the stop() guard itself is at 395 and the sink×identity four quadrants are all tested.
   [2026-10-01 follow-up round instruction-level endorsement] gcov-14 -b -c per-arm taken basis, all 35 lines
   (74 arcs) screened item by item: all surviving non-throw arms have taken >0 (no semantic true arm with taken 0
   — no reversal candidate, so no cases are fabricated), and all zero arcs are never-executed
   dead blocks. Split and classified by dead-block origin: variant-noise 60 arcs — LogAttribute single-value
   construction lines (171/172/191/192/373/473/731/1290/1291/1371/1442/1561/1675:
   branches 6/7 + call 8 dead blocks, i.e. unused variant alternative construction) and log call
   lines (173/193/374/396/478/733/819/1010/1089/1292/1372/1443/1562/
   1676/1743/1799/1842: calls 14/15 + branches 16/17 + call 18 dead blocks,
   1712 is a ternary continuation line with a slightly different form but belongs to the same dead blocks); inline-noise 14 arcs — ternary/
   string-concatenation lines (949/1167 have 2 arcs each as the to_string-side SSO/heap copy dead blocks,
   with the ternary true/false semantic arms 22/8 and 12/8 all taken; 1241's four arcs are the key+"="+value concatenation
   double dead blocks, with the ternary 20/2 all taken; 1711's four arcs + 1712's two arcs are the draining nested ternary,
   with parsed 8/4 and draining 6/2 all taken). The lowest line execution count is 2 (1290/
   1291), all the rest ≥6.
   [2026-10-01 follow-up round re-check: 74-arc mechanism reversed, variant-noise/inline-noise →
   structural] The per-arm taken count facts in the preceding entry are unchanged (all semantic arms >0, all zero arcs
   never), but the "unused variant alternative/SSO copy" mechanism attribution does not hold — objdump
   instruction-level evidence shows all 74 arcs are exception-unwind landing pads (EH landing pad cleanup dispatch):
   the attr-line variant conversion construction is an out-of-line call (170→20b7a8, 171→
   20e78c), the library body is not attributed to this line, and the conversion construction is fixed at compile time with no code generated for unused alternatives;
   the log call line has no variant construction on the line itself, and the content of the dead blocks calls 14/15/18 is
   ~vector/~LogAttribute/~basic_string conditional destruction, located in the endbr64 pad
   entry region; the ternary/concatenation lines have no variant, and if the SSO/copy predicate were on the normal flow it would necessarily produce
   taken 0 arc pairs, whereas the measured in-line zero arcs are all paired never with not a single non-throw
   taken 0 (nothing a guard could block). 171 (1e5683) and 373 (1e88cd) dead blocks are instruction-level
   isomorphic = endbr64 + r12b construction-complete flag test/je + conditional ~basic_string,
   executed only when a bad_alloc-level exception is thrown inside the statement and stack unwinding cleans up an already-constructed temporary,
   which tests cannot inject. The 74-arc registry is unchanged, and the category is re-judged as structural exception path;
   variant-noise lines with the same signature (TickDiagnostics 39 etc.) are suspected of the same mechanism, left for
   the next round of file-by-file re-review.
[2026-10-01 reversal] The original three structural arms were closed out by added tests and removed: 25 the toBytes empty-string defensive arm
(empty-payload profile download — EmptyPayloadProfiler true + empty string drives memcpy to skip,
with a legitimate zero-byte response), 545 the auditSink publish empty-identity skip arm (empty hostname fake
agent + dual sinks, publish skipped and the center ring stays empty), 1011 the `ok ? 0x01 : 0x00`
false arm (a fake agent's disposition always fails — a deterministic stand-in for ESRCH race semantics, with the 0x00 response
reachable; the claim "the race can never be stably injected" does not hold).

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
