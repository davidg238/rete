# Perf trials: push_frame + OP_AT/OP_AT_PUT — working journal

**Status: in progress, 2026-07-16 evening.** Written incrementally (power-fail risk —
David's request). If this doc ends abruptly, the git working tree + this journal are
the recovery state.

## Mandate (David, 2026-07-16)

- Review `push_frame` and an `OP_AT`/`OP_AT_PUT` fast path for SMI-indexed Array access.
- Benchmark revisions. **Trials on branches** so we can compare/contrast; main must not
  go backwards.
- Posture: clean, elegant code (simplicity = robustness); obvious optimizations only;
  target "usably fast" for a Zephyr scripting language — "close to MicroPython
  (CircuitPython column in E4) is the bar".
- Semantics must be preserved exactly: bounds errors, non-Array receivers fall back to
  the full send. No caches needing invalidation stories, no speculative state.

## Baseline (E5, already in the ledger)

`zephyr/docs/results.md` E5: activation 28 ns/5.8 µs (host/ESP32), send 26 ns/7.1 µs,
capture 34 ns/2.3 µs, closure 42 ns/2.3 µs, ocDo 172 ns/40.7 µs per element,
ocAt 188 ns/73.5 µs. Within-binary deltas only; cross-binary absolutes carry ~25%
layout noise on host, more on target.

Kernel `do:` per element (bytecode-verified): direct slot load (cheap) + `#at:` SEND
+ OP_BLOCK_INVOKE. The two targets of this work are exactly those two costs.

## Plan

1. Profile host activation path (perf) — attribute the 28 ns before cutting anything.
2. Branch `perf/push-frame-trim` (tuvm): obvious cost cuts in push_frame/OP_RET path.
3. Branch `perf/op-at` (tuvm + nsl-tuvm, paired): compiler lowers `at:`/`at:put:`
   sends to new opcodes; VM handler does vtable+SMI+bounds check inline, falls back to
   the full send path otherwise (mirror of OP_ADD's SMI fast path).
4. Gates per branch: tools/run_c_tests.sh + nsl-tests suite. Fail → report, don't bench.
5. Host compare: blockcost.ns same-binary deltas per branch + combined.
6. Device: Model B blockcost run of the winner; E6 ledger entry.

## Findings so far

- (pending) perf profile of 40M-activation binary.
- Device-side observation from E5 worth a future trial (NOT in current scope):
  baseLoop = 7.7 µs per ~5 bytecodes ≈ 370 cycles/bytecode on ESP32 vs ~35 on host —
  interpreter+bytecode likely thrashing the flash (XIP) cache. A `perf/iram-interp`
  trial (hot loop into IRAM; IRAM is only 16.6% used per E5 build) could lift EVERY
  row. Logged for David; needs its own branch and measurement.

## Artifacts

- Benchmarks: `~/workspace/nsl-tests/00_perf/blockcost.ns` (E5, uncommitted),
  scratchpad `profblock.ns` (40M activations, profiling target).
- E5 device log: `zephyr/esp32-debug-artifacts/tuvm-esp32-blockcost-96k-run1.log`.

## Review findings (task 1, complete 2026-07-16 ~23:00)

perf sampling unavailable (perf_event_paranoid=4, no sudo) → sized by code reading;
ablation/trial branches are the measurement. Ranked by expected value:

1. **Inlined loops still pay a full send per iteration for the bound check.**
   `1 to: n do:` emits `OP_SEND #<=` (verified in baseLoop bytecode: `08040102`) —
   every iteration of every inlined loop walks the whole send_resolve gauntlet.
   There are no comparison opcodes; OP_ADD..OP_DIV exist but stop at arithmetic.
   → **OP_LT/LE/GT/GE opcodes** (SMI inline, non-SMI falls to the existing prim
   logic). Numeric-only like OP_ADD. `=` deliberately NOT lowered (object equality
   semantics too broad). Opcode space free from 0x19.
2. **`is_truthy` does `strcmp("true")` per conditional jump** (`dispatch.c:283`,
   consumed by OP_JMP_IF_FALSE). Prims return the static `true_sym_obj`/
   `false_sym_obj`, so an identity fast path (2 pointer compares) covers the hot
   path; strcmp stays as fallback for blob-side `true` literals.
3. **send_resolve prologue: three receiver-identity gates are real function CALLS**
   (`serial_object()`, `udp_object()`, `mirror_object()` — cross-TU, not inlined)
   on EVERY send, plus a BoundClass obj_vtable check. → cache the three singleton
   pointers in statics after first init; three compares, zero calls.
4. **OP_AT/OP_AT_PUT** as planned — deletes the per-element `#at:` send in kernel
   `do:` and all index loops. Must mirror the at:/at:put: prim semantics exactly,
   incl. bounds error and non-Array fallback (OrderedCollection at: is a kernel
   method, must still resolve normally).
5. **push_frame micro** (smaller than hoped — it's ~20 stores): redundant per-arg
   `i+1 < num_registers` bound check; OP_BLOCK_INVOKE loads the selector const
   even on the fast path where it's unused (move into fallback branch).
6. Dispatch loop carries 3 debugger checks per bytecode (bp gate, OP_BREAK compare,
   single-step check). Candidate compile-time knob for MCU builds; NOT in scope for
   the first trials (semantic knob, needs discussion).

Trial branches:
- **A `perf/activation-trim`** (tuvm only, no ISA change): items 2, 3, 5.
- **B `perf/op-cmp-at`** (tuvm + nsl-tuvm paired, ISA + compiler): items 1, 4.
- Compare A, B, A+B against baseline on host blockcost; winner to device.

Build gotcha (recorded the hard way): `-g` on a solidified_image.c sends cc1 to
multi-GB RSS / >5 min even for a tiny image (the Model A pain is largely debug info
on giant initializers). Profiling builds: compile the image .o separately without -g.

## Results log (append-only)

- 2026-07-16 22:5x: profblock.ns (40M param-block activations) = 2.43 s host -O2
  → 61 ns/activation gross, consistent with E5's 28 ns net delta.
- perf blocked (paranoid=4); pivoted to ablation-by-branch.
- **Branch A implemented** (worktree ~/workspace/tuvm-wt-a, branch perf/activation-trim,
  uncommitted): is_truthy identity fast path (dispatch.c), capability-gate pointer
  cache in send_resolve (dispatch.c), push_frame arg-copy clamp (vm.c),
  OP_BLOCK_INVOKE selector load moved to fallback (vm.c).
- **Branch A host numbers** (blockcost, -O2 -falign-functions=64 -falign-loops=32 on
  BOTH trees to damp layout noise; medians of 3): baseLoop 366→311 (−15%),
  blockValue 618→535 (−13%), methodSend 597→521 (−13%), blockCapture 934→892 (−4.5%),
  closureMake 757→699 (−8%), ocDo 1566→1469 (−6%), ocAt 1700→1540 (−9%).
  Within-binary activation delta 28.2→20.2 ns, send delta 26.3→19.3 ns (unaligned
  builds). Gates next.
- **Branch B implemented** (worktrees ~/workspace/tuvm-wt-b + ~/workspace/nsl-tuvm-wt-b,
  branch perf/op-cmp-at in both, uncommitted): six opcodes OP_LT/LE/GT/GE/AT/AT_PUT
  (0x19–0x1E) sharing OP_SEND's layout, fast-path miss GOTOs into the OP_SEND handler
  (no duplicated semantics). Compiler lowers explicit-receiver < <= > >= and
  basicAt:/basicAt:put: (+ literal-array at:put: whose receiver is provably Array).
  Kernel: OC do:/at:/at:put:/add: index `contents` via basicAt: (emitter discipline
  widened, dispatch.c comment updated). TDD: tests/test_op_fast.c hand-assembles all
  six — watched RED ("Unknown opcode 26", abort) → GREEN. Registered in run_c_tests.sh.
- **Branch B host numbers** (aligned, medians of 3): baseLoop 366→278 (−24%),
  blockValue 618→581 (−6%), blockCapture ~flat, closureMake 757→698,
  **ocDo 1566→1067 (−32%)**, **ocAt 1700→983 (−42%)** — ocAt now BEATS ocDo (98 vs
  107 ns/elem), restoring the expected hand-loop advantage. B alone still pays
  is_truthy strcmp per conditional (A's fix) → A+B combined branch next.
- **Committed the trials**: tuvm perf/activation-trim 39a2991, tuvm perf/op-cmp-at
  bd29b1b, nsl-tuvm perf/op-cmp-at cc30a99. GOTCHA: the nsl suite writes
  OEVM_DIR/src/solidified_image.c, so the first branch commits swallowed transient
  test fixtures — resolved in the merge by restoring the 8b32e21 fixture; branch
  commits to be amended the same way after their suites finish.
- **Combined branch** perf/activation-plus-ops (worktree ~/workspace/tuvm-wt-ab,
  merge commit 2a67669; nsl side = perf/op-cmp-at). Host aligned medians:
  | variant | base | A | B | A+B |
  |---|--:|--:|--:|--:|
  | baseLoop | 366 | 311 | 278 | **250** (−32%) |
  | blockValue | 618 | 535 | 581 | **509** (−18%) |
  | methodSend | 597 | 521 | 521 | **474** (−21%) |
  | blockCapture | 934 | 892 | 927 | 928 (flat — Context walks dominate) |
  | closureMake | 757 | 699 | 698 | **650** (−14%) |
  | ocDo | 1566 | 1469 | 1067 | **1069** (−32%) |
  | ocAt | 1700 | 1540 | 983 | **855** (−50%) |
  Baseline nsl suite: 195/0/12. A and B suites pending; AB gates next; device run
  of A+B after gates.
- **Host STIC (performance.ns), baseline vs A+B, aligned, medians of 4**:
  collectionIterate 3019→1913 (−37%), collectionWrite 373→265 (−29%),
  arrayWrite 628→485 (−23%), dictionaryWrite 154→129 (−16%), alloc 174→138,
  integerMath ~flat (arith already had opcodes), stringCompare ~flat,
  floatMath 126→145 (+15%, CONSISTENT — suspect layout/arena placement, floats
  never touch the new fast paths; device run must arbitrate before quoting).
- AB C gate: 26/26 PASS (incl. test_op_fast).
- **Gate results**: Branch A nsl suite 195/0/12 == baseline → A fully gated; commit
  amended to drop transient fixture churn (now dispatch.c+vm.c only, 39a2991 rewritten).
- **False alarm on B**: first B suite run showed 109 FAIL — root cause is the test
  runner's SHARED /tmp artifacts (`/tmp/oevm_img`, `/tmp/oevm_layout.json`,
  `/tmp/*.oimg`): I ran A's and B's suites CONCURRENTLY, so B's new-opcode .oimg
  files raced against A's old-opcode /tmp/oevm_img → "unknown opcode". A's suite was
  immune (old opcodes run on both VMs). **Never run two nsl suites concurrently.**
  B re-running solo.
- **B solo: 195/0/12 == baseline.** One residual FAIL (debugger-integration) traced to
  tools/test_debugger_integration.py and test_supervisor_grant.py HARDCODING
  `OEVM = ../tuvm` (ignoring OEVM_DIR) — they compiled with the branch compiler but
  linked the MAIN tree's old VM → "Unknown opcode 28". Patched both harnesses to
  honor OEVM_DIR (part of nsl-tuvm perf/op-cmp-at); both gates then PASS against
  wt-b. Grant had passed before only by luck.
- Commits amended (pristine fixture; harness fixes folded in): tuvm A=7637978,
  tuvm B=b9f893f, nsl-tuvm B amended; combined branch rebuilt cleanly from amended
  heads (merge 8e70ba5). AB C gate 26/26 again. AB nsl suite running SOLO; AB
  firmware (blockcost_ab.oimg, Model B) built — 168816 B dram (85.86%), identical
  to E4/E5. Flash + capture follows a green suite.
- **AB fully gated** (2026-07-17): nsl 195/0/12 + grant/debugger-integration/
  portability-32/boot-gc-asan all PASS.
- **AB blockcost ON DEVICE** (log archived as tuvm-esp32-blockcost-AB-96k-run1.log),
  vs E5 baseline ms: baseLoop 76,856→49,640 (−35%), blockValue 135,137→104,753
  (−22%), methodSend 147,960→119,743 (−19%), blockCapture 158,365→128,065 (−19%),
  closureMake 100,078→72,945 (−27%), **ocDo 407,131→156,058 (−62%, 15.6 µs/elem)**,
  **ocAt 735,069→375,641 (−49%, 37.6 µs/elem)**. Mechanism deltas unchanged where
  untouched (activation 5.5 vs 5.8 µs, capture 2.3, closure 2.3) — the wins are the
  deleted per-element sends/frames and the cheaper loop skeleton, exactly as
  designed. NOTE: on device ocAt is still 2.4× ocDo (host had them flipped) —
  OC>>at:'s kernel-method frame per element dominates on Xtensa; hand index loops
  over OCs remain the slow way on target.
- **STIC AB on device complete** (2026-07-17, log tuvm-esp32-STIC-AB-96k-run1.log):
  collectionIterate 532.6→274.8 s (−48%), collectionWrite 97.0→81.3 (−16%),
  arrayWrite 138.9→117.0 (−16%), integerMath 15.3→14.6 (parity with CircuitPython
  14.4), floatMath 35.1→34.3 (host's +15% refuted — layout noise), stringCompare
  5.2→4.8, alloc/hanoi flat, dictionaryWrite 43.3→45.9 (+6%, untouched row, noise
  band). Full E6 section banked in zephyr/docs/results.md.
- **Status: all six tasks done.** Branches ready for David's review:
  tuvm perf/activation-trim (7637978), perf/op-cmp-at (b9f893f), combined
  perf/activation-plus-ops (8e70ba5); nsl-tuvm perf/op-cmp-at (amended cc30a99).
  Worktrees left in place: ~/workspace/tuvm-wt-{a,b,ab}, ~/workspace/nsl-tuvm-wt-b.
  Nothing merged to master/feat branches; main trees untouched.
- **Follow-up candidates surfaced but out of scope** (for the next rung):
  (1) the remaining 4th-place STIC rows all reduce to one public kernel-method
  send + frame per element — push_frame cost, so either cheapen push_frame's
  layout or give at:put:/add: the do: treatment;
  (2) `perf/iram-interp` — interpreter hot loop into IRAM (16.6% used), attacks
  the ~370 cycles/bytecode dispatch floor every row pays;
  (3) compile-time knob for the 3 per-instruction debugger checks on MCU builds;
  (4) the E4-vs-E5 26.6-vs-40.7 µs/elem spread is cross-binary flash-cache noise —
  benchmark discipline: same-binary deltas only, quote ranges for absolutes.
