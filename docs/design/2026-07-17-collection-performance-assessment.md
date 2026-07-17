# Collection performance: assessment and posture — decision record

**Date:** 2026-07-17. **Status:** assessment banked; optimization work deferred by
David's decision (correctness and simplicity over absolute performance).
Measurements behind every claim: `~/workspace/tuvm/zephyr/docs/results.md` E4–E8.
Companion decision record: `2026-07-17-fastpath-opcodes-design.md` (the E6 trial).

## The question (David, 2026-07-17)

The E4 STIC table shows tuvm last on 5 of 9 rows, worst wherever collections
appear. Is that (a) synthetic benchmarks we cannot optimize away, or (b) a
fundamental limitation? And is there a non-trivial benchmark showing realistic
relative performance?

## The answer

**Neither excuse holds; the cause is precise and now measured.** The iteration
counts are real work in every VM — nothing is being elided by the competition.
And it is not allocation or GC: E5's decomposition put closure creation and
captured-variable access at 2.3 µs each on target, the *cheapest* rows. The gap
is per-element interpreter mechanics: every element visit in a combinator pays a
real method send (~7 µs) plus a real block activation (~6 µs), both dominated by
frame activation.

**The architectural reason:** the reference VMs execute collection mechanics in
C — CircuitPython's iteration protocol and list methods are native, uLisp's
`dolist` is C, Toit intrinsifies its core collections and compiles its
non-escaping blocks inline. In tuvm, `do:`, `at:`, growth policy — all of it —
is kernel.ns code running through the full send/frame machinery. That is the
design: a minimal VM, the library in the language.

**Fundamental vs implementation:** mostly implementation, with one true semantic
floor. E6 halved collectionIterate (41.9× Toit → 21.6×) with zero semantic
change, which proves headroom exists. The floor: a block passed as a parameter
(`aBlock value:` inside `do:`, kernel.ns:132) is a real object invoked late —
that *is* the language, and its cost can approach but not undercut a method
send. The honest framing of further work: **each increment toward Toit means
moving more collection mechanics from interpreted kernel code into VM fast
paths** — the same game the others already played, a purity-vs-speed dial with a
stopping point that is David's to set, not a cliff to fall off.

**Density correction:** STIC's collection rows are *pure* mechanics — element
visits and nothing else — so 21.6× is the worst case, not the expected case.
Compute rows sit at parity-to-ahead (integer level with CircuitPython, float
ahead of Toit, hanoi 2nd). Real workloads interleave both; their gap is weighted
by send-density.

## The program-shaped evidence: Richards (E7)

Richards (OS-scheduler simulation; polymorphic sends, linked structures, no hot
loop) was already ported (`nsl-tests/05_somns_benchmarks/06_richards.ns`);
`nsl-tests/00_perf/richards.ns` wraps it in a timed, verified node.

- **Host: oevm runs Richards at 1.18× primordialsoup** — on program-shaped work
  this VM is already close to a mature Smalltalk-family VM.
- **Device: 17.6 s/run**, host→device ratio ~220×, inside E5's send-heavy band —
  Richards composes the same machinery the microbenches decomposed.
- **The E6 branch is flat on Richards** (+4.8%, noise band): its hot code is `=`
  compares and parameter-block invokes, precisely what the fast-path design
  declined to lower. E6's wins are collection wins; the general send floor is
  untouched.
- **No cross-VM ESP32 Richards column exists yet.** For a defensible real-world
  ranking, run Richards on Toit and CircuitPython on the same board (Toit port:
  `~/workspaceToit/toit/tests/type_propagation/richards-test.toit`; Python
  Richards is standard in pyperformance). This is the highest-value *future*
  measurement, deferred.

## Trials run to closure (E8)

1. **IRAM placement of the interpreter hot set — hypothesis refuted, but a
   real combinator win surfaced.** The E5 follow-up hypothesis ("~370
   cycles/bytecode is interpreter I-fetch through the flash XIP cache") was
   tested directly: `perf/iram-interp` routes `run_interpreter`, `push_frame`,
   `send_resolve`, `is_truthy` (~8 KB) into IRAM behind an env knob. Device
   result: Richards −0.2% and all five blockcost *mechanism* rows −0.7…−0.9%
   — the dispatch floor is NOT interpreter I-fetch; the hot loop was already
   cache-resident on single-path workloads. But both *path-alternating* rows
   moved by a third: **ocDo −30% (40.7→28.7 µs/elem), ocAt −33% (73.5→49.6)**,
   with the flat mechanism rows as the internal control against layout luck.
   That matches E5's open puzzle (oc rows cost ~2× their component sums;
   alternating send paths lose cache locality): IRAM placement ends the
   interpreter-vs-data cache eviction fight exactly where paths alternate —
   i.e. in every real combinator loop — and makes interpreter placement
   deterministic, removing a known cross-binary noise source (the E4-vs-E5
   26.6-vs-40.7 spread). Richards staying flat says its bottleneck is D-side:
   bytecode and kernel objects fetched from the DROM-mapped blob — Model B's
   own trade ("the program costs flash, not RAM") seen from its latency side.
   Cost of the knob: ~8 KB of IRAM (16.6%→20.1%), zero semantics.
2. **Debug hooks off (`OEVM_NO_DEBUG_HOOKS`) — a genuine per-bytecode win.**
   The dispatch loop carries three per-bytecode debugger checks (breakpoint
   gate, OP_BREAK dispatch, single-step hook). The knob compiles them out; a
   build so configured cannot host the on-target debugger — a disclosed
   semantic knob, never a silent default, and benchmark numbers quoted from
   such a build must say so. Device result: −10…−16% on every blockcost
   mechanism row, −5.7% on Richards — the only knob that moves Richards.
3. **Both knobs together are the recommended production config** and are
   additive (results.md E8): Richards −6.3%, combinator iteration −36…−38%
   (ocDo 40.7→26.0 µs/elem — equal to the best cross-binary endpoint ever
   measured, now by construction rather than layout luck). Cost: ~8 KB IRAM,
   no on-target debugger. Both default-off, env-gated, on branch
   `perf/iram-interp` (90ae47e + 333f4f3), fully gated (C 25/0, nsl 195/0/14,
   grant/debugger-integration/portability-32/boot-gc-asan PASS).

## Decision (David, 2026-07-17): document and defer

Priorities are **correctness and simplicity**; absolute performance is not the
goal. "Usably fast for a Zephyr scripting language" (the E6 mandate's bar —
close to the CircuitPython column) is met-or-near on compute and program-shaped
work; the collection rows are understood, not mysterious. Deferred, in the
order they earn their keep if ever picked up:

1. **Widen the `basicAt:` emitter discipline** to the remaining kernel hot
   paths — Dictionary's probe loop (`slotFor:`'s `table at: i` per probe) and
   OC `add:`'s `ensureCapacity` chain. Applies an existing, documented rule to
   more sites; no new mechanism. Targets the three still-4th STIC rows.
2. **Sealed-receiver `=` fast path** — same fall-into-OP_SEND shape as E6's
   opcodes, inlining only receivers whose kernel `=` is definitionally identity
   (SMI×SMI, nil, booleans, Symbol). The only sub-JIT lever that reaches the
   send floor Richards runs on. **Gate:** verify those receivers' `=` cannot be
   overridden; amends fastpath Decision 2, so it needs David's sign-off.
3. **push_frame remainder** — re-measure only; E5 already found it smaller than
   hoped, and the IRAM result says cache effects were not hiding anything.

Explicitly rejected, posture-level: benchmark special-casing of any kind;
wholesale `=` or public-`at:` lowering (fastpath Decisions 2/3 stand);
per-call-site inline caches (an invalidation story, excluded by mandate).

## Where the durable knowledge lives

- Measurements + logs: `zephyr/docs/results.md` E4–E8 (+ `esp32-debug-artifacts/`).
- Fast-path ISA decisions: `2026-07-17-fastpath-opcodes-design.md`.
- Benchmark discipline (within-binary deltas, host-absolute distrust, the
  literal-array and counting traps): auto-memory + results.md caveats.
- Trial branches for review: tuvm `perf/activation-trim`, `perf/op-cmp-at`,
  `perf/activation-plus-ops`, `perf/iram-interp` (90ae47e + 333f4f3);
  nsl-tuvm `perf/op-cmp-at`. Nothing merged.
