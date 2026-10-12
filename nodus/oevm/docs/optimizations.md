# OEVM interpreter optimization log

Tracks performance work on the OEVM interpreter core (`src/dispatch.c`, `src/vm.c`),
with before/after measurements and the commit that landed each change so
improvements are traceable.

**Design rationale** for the decisions referenced here lives in `nodus/oevm/docs/topics.md`:
[selector-dispatch-caching](topics.md#selector-dispatch-caching) (Opt #1/#2),
[reflective-frame-access](topics.md#reflective-frame-access) (the post-#2
bottleneck), [block-inlining](topics.md#block-inlining) and
[scope-vs-frame](topics.md#scope-vs-frame) (the structural fix in progress).

## Ledger: every attempt, kept or not (from 2026-10-10)

One row per thing tried, including the dead ends, so "why did you …" has an answer. Add a row whenever an attempt is
measured, whether or not it lands. The numbers and the reasoning live in the record named in each row.

| date | attempt | result | verdict | record |
|---|---|---|---|---|
| 10-10 | hot path in SRAM on the RP2350 (`OEVM_HOT`), the image left in flash | XIP fetches cost 2–2.7× once the image outgrew the 16 KB cache; 8.4 KB in SRAM gave it back | kept (#403, #412) | `docs/results/2026-10-10-296-image-in-sram.md` |
| 10-10 | the image itself in SRAM (a second, RAM-heavy build for speed) | 0–5 % | **dead end**: no RAM split for speed (#296 §7) | same |
| 10-11 | caching the method lookup harder | a send is ~707 cycles on the C6, the lookup ~85 of them | **dead end**: the cost is the frame, not the lookup | `docs/spikes/2026-10-11-37-c6-send-cost/` on branch `37-c6-send-cost-spike` |
| 10-11 | `oc do:` lowered by hand, without a fused iterate step | does not reach MicroPython | **dead end**; the fused step became Opt #7 | `docs/spikes/2026-10-11-37-ocdo-lowered/` |
| 10-11 | `OP_ITER_AT`, the kernel's `do:` in one dispatch per element | ocDo 7668 → 5594 ms (WeAct) | kept, Opt #7 | Opt #7 below |
| 10-11 | literal iterator blocks inlined behind a class guard (`OP_IS_VT`) | collectionIterate 100.8 → 15.8 s | kept, Opt #8 | `docs/spikes/2026-10-11-37-do-inline/`, Opt #8 |
| 10-11 | the send levers (fast opcodes in kernel collections, prim 163) | arrayWrite 67.7 → 22.9 s | kept, Opt #9 | `docs/spikes/2026-10-11-37-send-levers/`, Opt #9 |
| 10-11 | per-instruction checks removed, a load-time verifier added | every row 2.4–17.8 %; the verifier costs 7–11 ms a boot | kept, Opt #10 | `docs/results/2026-10-11-37-lean-dispatch/` |
| 10-11 | `oevm_int_demote` moved to image build | impossible: image SMIs are 30-bit, the VM's 31 | **dead end**; the SMI test is inline instead | Opt #10 |
| 10-11 | operands decoded per case; SMI + − × on tagged words | every row 0.7–24 % | kept, Opt #11 | `docs/results/2026-10-11-37-dispatch-head/` |
| 10-11 | the arithmetic slow path inlined in the loop | floatMath fixed, every other row +2–3 % (register pressure) | **dead end**: out of line, Float first | same |
| 10-11 | peephole result forwarding (`op t; LD_REG v,t` → `op v`) | integerMath −2.3 %, alloc/dictionaryWrite +2 %, fixture churn | **dead end** for now | same, branch `37-dispatch-head` |
| 10-12 | L1: the access mechanism deleted (check, field, builtin #14, patch line) | AWFY: C6 0.936, WeAct 0.961, host 0.963; STIC: C6 0.986, WeAct 0.997; MCU text −754 B | kept, Opt #12 | `docs/results/2026-10-12-37-send-frame/` |
| 10-12 | L2: `push_frame` answers its method, one prologue, gcc left to inline | host 1.051 against L1 (gcc inlined 3 of 7 sites); C6 AWFY 0.995, STIC 1.021; WeAct AWFY 1.004, STIC 1.021 | **dead end** | same |
| 10-12 | L2 with `push_frame` always inlined | host 0.983 against L1; boards: C6 AWFY 0.981, STIC 1.022; WeAct AWFY 1.024, STIC 1.035 | **dead end** on the boards; reverted (Opt #12) | same |
| 10-12 | L4(c): `gen_tag` dropped (the frame word, both `home_generation` fields, the global increment per frame) | AWFY: C6 0.988, WeAct 0.981; STIC: C6 0.995, WeAct 0.978; MCU text −28 B | kept, Opt #15 | `docs/results/2026-10-12-37-l4/` |
| 10-12 | L3-lite: `push_frame` and the return read the frame globals once into locals; `ctx` from `push_frame` | against L4(c): C6 AWFY 1.000, STIC 0.999; WeAct AWFY 1.024, STIC 1.037 | **dead end** on the WeAct (the L2 pattern) | same |
| 10-12 | L5 with the SMI arm before the lookup (no copy) | AWFY: C6 0.985, WeAct 0.963; STIC: C6 0.987, WeAct 0.978; MCU text −11 B | merged first, then superseded by the row below | `docs/results/2026-10-12-37-l5/` |
| 10-12 | L5: `send_resolve`'s head — the kernel instance set at launch, the receiver's vtable read once (SMI arm at the top) | AWFY: C6 0.978, WeAct 0.949; STIC: C6 0.980, WeAct 0.969; MCU text +860 B over the first merge, banked | kept, Opt #16 (David: give the ratchet the memory) | same |
| 10-12 | the arena header in one word (size + mark bit; a free block's link in its payload) | level on every board (1.000 ± 0.002); −8 B per object on the boards; the smallest arena that verifies −12 % (board profile, Storage) to −19 % (Json) | kept, Opt #17 (a memory lever) | `docs/results/2026-10-12-37-alloc-hdr/` |
| 10-12 | L5 rest R1: Symbol `=` inside the `=` prim's case; `platform_object()` resolved once | AWFY C6 0.977, WeAct 0.984; STIC C6 0.979, WeAct 0.986; MCU text +940 B (`-Os` copies the prim block with the arm in it) | kept, part of Opt #18 | `docs/results/2026-10-12-37-l5-rest/` |
| 10-12 | L5 rest R2: the capability receivers tested on the lookup miss (empty vtables) | cumulative AWFY C6 0.970, WeAct 0.966; STIC C6 0.982, WeAct 0.974; MCU text −384 B | kept, part of Opt #18 | same |
| 10-12 | L5 rest R3: a send's selector proved a Symbol at load, the prim test one `prim_id` load | cumulative AWFY C6 0.918, WeAct 0.926; STIC C6 0.941, WeAct 0.939; MCU text +890 B over master, inside the 2 % | kept, Opt #18 | same |
| 10-12 | Symbol `=` out of line (`noinline`), to shrink R1's copy | MCU text +44 B worse | **dead end** | same |
| 10-12 | L4(b): a method frame's `home_ctx` NULL (the closure's, passed through), `homeOf:` answers the frame | geomean AWFY C6 0.992, WeAct 0.998–1.000; STIC C6 0.986, WeAct 0.994 — placement: Richards 1.007, arrayWrite 0.928; MCU text −28 B | kept, Opt #19, for simplicity; speed level | `docs/results/2026-10-12-37-l4b/` |
| 10-12 | L4(a): the frame's `vtable` left unset | not built: a stale `MethodContextVtable` would make a running frame restartable; the safe form is the same store | **dead end** | same |
| — | the jump table's bound check (`cmp`/`bhi`) | GCC 12 keeps it despite `__builtin_unreachable()` | open: only computed goto removes it; untried | Opt #11 |

**#37 closed for 0.1 (David, 2026-10-12): "good enough", and no hacks for benchmarks.** Where it stands on the C6
(c6spare, stic sources, `docs/results/2026-10-12-37-c6-cross-vm/` update): Richards 5,080 ms (Toit 1,178, MicroPython
7,471), DeltaBlue 336 ms (Toit 77, MicroPython 326–359); on the host, AWFY geomean nsl ÷ Primordial Soup 1.01 (morning
1.26; `docs/results/2026-10-12-37-awfy-soup-opt18/`). Every lever kept removes work any program pays; the rows that do
not send stayed level through the send levers.

Deferred past 0.1 — design work, not levers, each with the evidence that would earn it:
- **Instruction dispatch and bytecode density.** The base instruction is ~36 C6 cycles, and Richards' instructions
  alone cost twice Toit's whole run (`docs/results/2026-10-12-37-M-counts/`). Fewer, denser instructions
  (superinstructions, fused compare-branch, add-immediate) or a different interpreter shape; computed goto for the
  jump table's bound check (row above). Most of Toit's remaining lead (~4.3× on Richards and DeltaBlue).
- **The allocator** (List 2.67×, Storage 1.65× Soup's time on the host; Soup has a bump nursery and generational GC).
  First a measurement: the time in `gc_allocate` per row on the C6. Opt #17 took the header.
- **Per-send-site inline caches** (W3): the image is read-only in flash, so a RAM side table; the cache probe is
  ~85 cycles a send. The L2 lesson applies: a test at the loop's send sites has lost on the boards.
- **Unboxed floats** (W2b); GC pause and throughput (#421); Dictionary as a prim.

## Method

- **Benchmark:** `./bench.sh <test.ns> [runs]` compiles the `.ns` via the canonical
  ns-oevm compiler, builds the VM with `gcc -O2`, runs N times, reports the best
  wall-clock (`/usr/bin/time -f %e`, process startup included). Primary metric is
  `fib(32)` (= 2178309), which does ~3.5M sends and is the clearest signal;
  queens/sieve run in <10 ms and are below `time` granularity.
- **Regression gate:** the canonical ns-oevm conformance suite
  (`cd ../ns-oevm && python3 tools/run_tests_oevm.py`), baseline **19 passing /
  10 failing / 1 skipped** of 29. Every optimization must keep the exact same
  pass/fail set (diffed against a saved baseline). The `oevm/tools` copy is stale
  (14/29) — do not use it as the gate.

## Baseline (commit `7313f89`, pre-optimization, -O2)

| Benchmark   | Time   | Notes                                  |
|-------------|--------|----------------------------------------|
| fib(32)     | 4.06 s | ~1 µs/call, ~3.5M calls                |
| queens      | <10 ms | below `time` granularity               |
| sieve       | <10 ms | below `time` granularity               |

Root cause: `send()` ran a chain of 47 `strcmp(selector->name, "...")` to
intercept primitives before the vtable lookup, on **every** message send.

## Opt #1 — skip the strcmp gauntlet for non-primitive selectors

Selectors are interned (one stable `Symbol` pointer per name). Added a
pointer-keyed cache (`selector_prim_id`, formerly a boolean) that classifies each
symbol once; non-primitive sends (e.g. `fib:`) now bypass the entire gauntlet and
go straight to vtable lookup.

| Benchmark | Before | After  | Speedup |
|-----------|--------|--------|---------|
| fib(32)   | 4.06 s | 3.45 s | 1.18×   |

Regression gate: 19/29, no change. (Folded into the same commit as Opt #2.)

## Opt #2 — pointer-id dispatch instead of strcmp for primitives

Extended the per-symbol cache to return a small integer primitive id. Compute the
id once per send, then the gauntlet compares `pid == PRIM_X` (one cycle) instead
of `strcmp(...)`. Primitive-heavy sends (`<`, `ifTrue:ifFalse:`) that previously
walked deep into the strcmp chain are now near-free.

| Benchmark | Before | After  | Speedup vs baseline |
|-----------|--------|--------|---------------------|
| fib(32)   | 3.45 s | 1.31 s | **3.10×**           |

Regression gate: 19/29, no change.

**Commit:** `67ebc52` — _oevm: gate primitive dispatch by cached selector id_ (Opt #1 + #2).

### Opt #2b — jump-table the primitive gauntlet

Converted the flat `if (pid == PRIM_X)` tail of the gauntlet into a real
`switch (pid)` so the compiler emits a jump table (O(1) dispatch for all 41
non-mock primitives) instead of a linear scan.

| Benchmark | Before | After  |
|-----------|--------|--------|
| fib(32)   | 1.31 s | 1.28 s |

Marginal on its own (the pid chain wasn't the bottleneck) but strictly cleaner
and removes linear scan depth. Regression gate: 19/29, no change.

## Profile after Opt #1/#2 (gprof, -O2, fib(32))

| Function                         | self % | calls       |
|----------------------------------|--------|-------------|
| `run_interpreter`                | 65%    | (recursive) |
| `send`                           | 30%    | 70,491,552  |
| `gc_allocate`                    | 3%     | 14,098,314  |
| `primitive_closure_home`         | 1%     | 17,622,886  |
| `primitive_method_context_reg_at`| —      | 17,622,886  |
| `is_truthy`                      | 1%     | 7,049,155   |
| `check_access_allowed`           | —      | 7,049,158   |

**Key finding:** ~70M sends for ~3.5M fib calls (~20/call). The codegen accesses
block locals/`self` via *reflective sends* — `home`, `regAt:`, `regAt:put:`
(17.6M each); see [reflective-frame-access](topics.md#reflective-frame-access).
The dominant remaining cost is the interpreter loop plus this flood of reflection
sends.

## Opt #3 — structural fix: block inlining (in progress)

The original #3 (inline-cache `OP_SEND_IC`) was superseded by the profile: the
real cost is the *number* of reflective sends, not dispatch. The structural fix
is **block inlining** — control-flow blocks stop being frames, eliminating their
`home`/`regAt:` sends entirely. See
[block-inlining](topics.md#block-inlining) /
[scope-vs-frame](topics.md#scope-vs-frame) and the design spec at
`ns-oevm/docs/design/2026-06-16-block-inlining-design.md`. This is
compiler work (ns-oevm) plus one VM truthiness fix
([jmp-if-false-truthiness](topics.md#jmp-if-false-truthiness)). Measurements will
be appended here as phases land.

### Phase A — foundation landed (no-op)

Scope/frame model (`Scope` class), label/backpatch jump helpers, and the
`OP_JMP_IF_FALSE` truthiness fix landed as a behaviour-preserving no-op: every
`.ns` test produced a byte-identical `solidified_image.c` before/after (proved
with `ns-oevm/tools/snapshot_images.py` + `diff -r`), conformance unchanged at
19/29. No perf change expected yet (no inlining). Next: Phase B (if-family
inlining).

The remaining-after-inlining piece — the closure representation for *non-inlined*
blocks — is tracked as `oevm#1`.

### Phase B — if-family inlining

`ifTrue:` / `ifFalse:` / `ifTrue:ifFalse:` / `ifFalse:ifTrue:` with literal
zero-arg block arguments now compile inline into the enclosing frame:
`compile_if_inline` emits `OP_JMP_IF_FALSE`/`OP_JMP` (Phase A jump helpers) and
`compile_inlined_block` compiles each block body under a child `Scope` on the
same frame, so enclosing variables resolve as direct registers — no closure, no
`home`/`regAt:` reflective sends. `^` inside an inlined arm becomes a plain
`OP_RET` for free. Non-literal-block sends (`cond ifTrue: aBlockVar`) still use
the closure+send path.

Effect: fib(32)'s `solidified_image.c` dropped from 2 block closures + 3
`createClosure:` + `home`/`regAt:` sends to **zero** of each; fib(32) time
**1.28s → 0.30s**. Conformance 19/29 → **20/29**: the new
`04_somns_basic/09_if_inlining.ns` acceptance test passes and `ifFalse:ifTrue:`
(previously a runtime doesNotUnderstand — no primitive) now works via inlining.
No previously-passing test regressed. Next: Phase C (`and:`/`or:` short-circuit).

### Phase C — and:/or: short-circuit inlining

`and:` / `or:` with a literal zero-arg block now compile inline via
`compile_logic_inline` (the Phase B `compile_inlined_block` machinery + an
`OP_JMP_IF_FALSE` short-circuit), matching `PRIM_and`/`PRIM_or` exactly: `and:`
truthy → block value / falsy → `false` symbol; `or:` truthy → `true` symbol /
falsy → block value. Non-literal-block args (`cond and: aBlockVar`) keep the
closure+send path. A pure-`and:`/`or:` fixture dropped from `createClosure: 3`
to `0`. Conformance held at 20/29 passing (the new
`04_somns_basic/10_and_or_inlining.ns` regression test — short-circuit
side-effect proofs, chains, `and:`-result-into-inlined-if — passes via inlining;
no test regressed). fib is unaffected (no `and:`/`or:`). Next: Phase D (loops:
`whileTrue:`/`whileFalse:`/`to:do:`/`to:by:do:`).

### Phase D — loop inlining + superInit fix (2026-06-16)

Four loop forms now compile inline; no loop sends remain in any conformance test
or benchmark. The `compile_inlined_block` helper gained two new capabilities
that all loop forms share:

- **`param_regs`** — binds a block parameter (e.g. the induction variable) to a
  caller-allocated register; the block body reads it as an ordinary register, not
  a closure capture.
- **`init_temps`** — emits `OP_LD_CONST nil` for every block-local temp at the top of
  each iteration, faithfully reproducing block-activation semantics (fresh nil per
  activation) without allocating a closure.

**`to:do:` with a literal one-param block** compiles to a counting loop: the
receiver integer is the start, the argument integer is the limit, the induction
register is bumped by 1 at the bottom and compared with `OP_JMP_IF_FALSE` against
the limit; the loop exits leaving the receiver as the expression result (matching
Newspeak semantics). The `to:do:` selector is consumed entirely — zero sends remain.

**`to:by:do:` with a literal integer step and a literal one-param block** works
the same way but the direction of the limit comparison is fixed at compile time:
`<=` when step > 0, `>=` when step < 0. A non-literal step falls back to the
`PRIM_to_by_do` primitive (unchanged behaviour). A literal `by: 0` step also
falls back: the primitive does zero iterations and returns the receiver, but an
inlined `i := i + 0` loop would never terminate, so a zero step is not inlined.

**`whileTrue:` / `whileFalse:`** with two literal zero-arg blocks compile to a
back-edge jump loop: the condition block is inlined first, its result drives an
`OP_JMP_IF_FALSE` (or negated for `whileFalse:`), the body block is inlined with
`init_temps`, and the bottom emits an unconditional `OP_JMP` back to the
condition. The result of the whole expression is `nil`.

**Measured on queens (`01_queens.ns`)** — the primary loop-heavy benchmark:

| Probe | Count |
|---|---|
| `StaticSymbol_to_do_` remaining in queens.c | **0** |
| `createClosure` sites in queens.c | **0** |

**Measured on `11_loop_inlining.ns`** (the new Phase D acceptance test):

| Probe | Count |
|---|---|
| `to:do:` / `to:by:do:` / `whileTrue:` / `whileFalse:` sends | **0** |
| `createClosure` sites | **0** |

All four loop forms are fully inlined; no closures are created.

**Unplanned but necessary: `superInit` inheritance slot-layout fix.** Loop
inlining changed register and heap allocation patterns, which exposed a latent
slot-layout bug: a child class that inherits more parent slots than it declares
own params ended up with a too-short slot layout, so inherited methods read slots
out of bounds. Symptom: `08_field_access.ns` (which exercises `whileFalse:` over
an object with inherited slots) entered an infinite loop under loop inlining;
after the fix it correctly outputs 33. The fix is in
`ns-oevm/tools/ns_compile_oevm.py`:
- `collect_slots` now merges parent slots before child slots so the layout is
  wide enough for all inherited fields.
- `compile_initializer` inlines a single-level parent `new:` call instead of
  emitting a `superInit` send.

**Known limitations (documented in compiler comments):** parent must be declared
before child in source order (silent no-op skip otherwise, intentional to keep
the compiler single-pass); single-level inheritance only — grandparent chains are
not walked.

Conformance: **21/29 → 23/29** (two new tests: `11_loop_inlining.ns` and
`12_inheritance_slots.ns`; no previously-passing test regressed). Breakdown:
23 passing / 10 failing / 1 skipped.

## Block inlining — Phase E: `Array>>do:` primitive

The `do:` collection-iteration method is a **library** operation, distinct from
the control-flow block inlining of Phases B–D. It was the sole remaining blocker
for `04_somns_basic/02_block_inlining.ns` (which failed with
`Message 'do:' not understood`). Note: `to:do:` (a counting loop) always worked
and is now inlined — it is unrelated to `do:`.

`{ e1. e2. ... }` array literals lower to `Array new:` + `at:put:`, producing an
`ArrayMockVtable` instance whose `slots[0]` holds the size and `slots[1..n]` the
elements. `do:` is implemented as a VM primitive (`src/dispatch.c`, `PRIM_do`) in
the array-instance interception block alongside `at:`/`at:put:`/`size`: it runs
the one-arg block once per element via `run_interpreter(block, block, 1, &elem)`,
honoring non-local-return unwinding (`if (unwind_target_reached) break;`) exactly
as `to:do:` does. Returns the receiver.

This also covers Phase E's "deep-nesting / non-local-return corners" strand: with
`do:` present, `02_block_inlining.ns` passes in full — including
`testDeepDeepNested` (NLR out of a non-inlined `block4` through multiple inlined
`ifTrue:ifFalse:` arms), the mixed inlined/non-inlined block trees, and
`testToDoNestDoNestIfTrue` (`to:do:` nesting `do:` nesting `ifTrue:`). No new
corner-case codegen was needed; Phases A–D already handled them.

Conformance: **23/29 → 24/29** (`02_block_inlining.ns` now passes; no regression).
Breakdown: 24 passing / 9 failing / 1 skipped. The 9 remaining failures are
unrelated to block inlining (outer/super sends, several benchmarks, deep-nesting
class resolution, integer overflow). Adding `do:` advanced the benchmarks too:
e.g. `03_towers.ns` now reaches a 0-based-Vector-indexing error instead of the
`do:` DNU.

## Conformance correctness fixes (post-block-inlining, 24/29 → 29/29 pass)

With block inlining complete, the SOMns benchmarks became the driver. These are
bug fixes in the ns-oevm compiler (and one VM addition), not interpreter
optimizations; each shipped with a focused regression test and a
`snapshot_images.py` byte-diff to bound its blast radius. The two codegen bugs
below (nested-send args, outer pointer) are generalized as durable rules in
`topics.md#calling-convention-invariants`.

### Primary-factory body statements (parser)

Newspeak/SOMns class primary-factory bodies may contain executable statements
after the slot section, run at instantiation (e.g. richards' Packet runs
`1 to: 4 do: [...]`; deltablue's EditConstraint factory is a bare
`addConstraint`). The grammar only allowed `( SlotSection? )`. Fix spans three
layers: `grammar/ns.ohm` (`ClassBody = "(" SlotSection? Statements ")" ...`),
`grammar/ns-parse.js` (ClassBody/ClassDef carry an `initBody` list), and
`ns_compile_oevm.py` `compile_initializer` (emit `initBody` after slot/param/
nested init, before `RET self`). No-op for slot-only bodies. Test
`13_factory_body.ns`. **24/29 → 25/29.**

### Root `Object` superclass → NULL delegate

`class Foo = Object (...)` inherits the builtin root, not a user class.
`resolve_class_ref` couldn't find it, so the delegate fell back to a bare name
and the solidifier emitted undeclared `(oop)&Vtable_Object` → C build failure.
Object maps to a NULL delegate (the VM root), same as no superclass; its
protocol is served by primitive interception. Test `14_object_super.ns`.
**25/29 → 26/29** (richards builds).

### Nested-send argument corruption (codegen)

A keyword/binary send evaluates arguments into registers contiguous above the
receiver. The arg loop allocated each slot with `alloc_reg()`, but when an
argument was itself a send, the nested send restored `next_free_reg` to its own
higher saved value rather than to `arg+1`; the next argument then took a
non-contiguous register and the VM read stale scratch (the nested send's
leftover `self`) in the gap. Symptom: `self pushDisk: (self popDiskFrom: x)
onPile: y` passed a disk OBJECT as `onPile:` → `at:` index-0 crash. Fix: after
each argument, reset `next_free_reg = areg + 1` (the arg result sits in `areg`,
below the reclaimed scratch). Applied to all three arg loops (normal, implicit,
class-init). Test `15_nested_send_args.ns`. **26/29 → 28/29** (`03_towers.ns`
passes).

### Nested-class outer pointer (codegen)

Instantiating a nested class always stored current `self` into the new object's
outer slot (slot 0). That is correct only when the nested class is defined in the
current class. When instantiating a class defined in an ANCESTOR (e.g. a sibling
nested class sharing a container), outer must be the shared container instance,
reached by walking self's outer chain. `resolve_class_ref` already returns
`hops`; the instantiation site now uses it (hops==0 keeps `self→slot0`
byte-identical; hops>0 walks `hops` outer links). Symptom: richards'
`RichardsBenchmarks new` is called from a sibling, so reading an outer-class
constant (`Idler`) read garbage → index-0 into a size-6 array. Test
`16_outer_sibling_new.ns`. **28/29 → 29/29 pass** (richards now runs to
completion; its remaining failure is a separate simulation-correctness issue —
outputs `false` vs `true`).

### richards: `Object>>=`, integer message ops, and class-side methods (2026-06-17)

`06_richards` ran to completion but verified `false`. Probing the actual
`queuePacketCount`/`holdCount` showed both `0` — the scheduler did nothing.
Three layered bugs:

1. **`Object>>=` (`PRIM_eq` in `dispatch.c`)** coerced any non-numeric operand
   to `0.0`, so every pair of heap objects — and `nil = anyObject` — compared
   equal. `NoTask = currentTask` (NoTask is nil) was always true, so the
   scheduler loop body never ran. Fixed: both-numeric compare by value;
   otherwise identity (default `Object>>=`).
2. **Integer message ops.** `&`/`|` were boolean-only, but richards uses
   `control & 1` as bitwise AND; `//`, `bitXor:`, `<<` hit `doesNotUnderstand`
   (sent as messages, not lowered to arith bytecodes like `+ - * /`). Made
   `&`/`|` bitwise on SMI; added `//` (floored), `bitXor:`, `<<`.
3. **Class-side (metaclass) methods** were parsed-then-dropped, and every
   `ClassName sel` send was treated as instantiation, so `TaskState
   waitingWithPacket` became `TaskState new` (nil flags). Implemented real
   metaclasses (see `ns-oevm/docs/compiler.md`): a `Meta_<vt>` vtable holds the
   class-side methods and is wired as the class vtable's header (`base.vtable`);
   sends to a class route there when the selector is a class-side method (super
   chain walked), else instantiate; `self new` in a class-side method
   instantiates the enclosing class.

Tests `17_object_equality_intops`, `18_class_side_methods`. **richards now
passes.**

### floats: literal vtable, asFloat/sqrt, exact `/` (2026-06-17)

mandelbrot/nbody crashed and `1 = 1.0` returned false. Three causes:

1. **Float literal vtable.** The solidified image boxed each static `Float`
   with `(oop)&FloatVtable` — the address of the global *pointer variable* —
   instead of `(oop)&Vtable_FloatVtable` (the form Symbol/Closure literals
   already use). So a literal float's header ≠ `FloatVtable` and the VM treated
   it as a bare object everywhere that checks the vtable (`printString` →
   `[Object …]`; `<`/`=`/`>` coerced it to `0.0`). Arithmetic accidentally
   worked because `OP_ADD` etc. read `((struct Float*)x)->value` without
   checking the header. Fixed in `solidify.py`. Test `19_float_literals`.
2. **Missing `asFloat`/`sqrt`.** Both sent to SMI/Float receivers → DNU (and
   the DNU handler then segfaults dereferencing a SMI). Added as primitives
   that box a Float; `sqrt` via `__builtin_sqrt`, so builds link `-lm` (test
   runner + `bench.sh`). Fixed nbody.
3. **Exact `/`.** `OP_DIV` truncated integer division, but Smalltalk `/` is
   exact — Integer when evenly divisible, else a rational (a Float here, no
   Fraction type). mandelbrot's `(2 * y / size) asFloat` was ~0 with
   truncation. `//` stays flooring integer division. Zero blast radius (the
   only integer `/` in the suite is mandelbrot's). Fixed mandelbrot. Test
   `20_exact_division`.

Latent issue noted (not yet fixed): `does_not_understand` segfaults when the
receiver is a SMI (line ~130 dereferences `receiver->vtable`).

Current breakdown: **36 passing / 5 failing / 1 skipped.** Remaining failures
(deeper workstreams): `06_outer_sends`/`07_super_sends` (more outer/super
resolution), `07_deltablue` (runtime crash — `DeltaBlueBenchmark` DNU),
`01_deep_nesting` (nested-class resolution), `04_integer_overflow` (SMI→largeint
promotion).

## Opt #4 — global method lookup cache (2026-06-17)

**Commits:** oevm `f7cc37d..b3fd369` (branch `feat/global-method-cache`, merged to
master `b3fd369`, pushed). Design/plan: `docs/design|superpowers/plans/2026-06-17-global-method-cache.*`.

A RAM-resident, direct-mapped (4096-entry) cache keyed by `(receiver_vtable,
selector) → (method, defining_vt)` in `src/dispatch.c`. `send()` resolves via
`lookup_method_cached`, which probes the table and falls back to
`lookup_defining_vtable` (the vtable-chain walk) only on a miss; successful
results are cached, does-not-understand results never are. `primitive_vtable_bind`
(the only runtime binding-mutation site) flushes the whole table, so a replaced
method can never be served stale. Gated `-DOEVM_CACHE_STATS` counters
(hits/misses/fills/collisions/flushes) print at exit.

**Why a global cache, not the `OP_SEND_IC` machinery.** The VM already has
`struct InlineCache` + `send_ic` + an `OP_SEND_IC` handler, but that design
embeds a per-call-site cache *in the instruction stream* and **writes** to it.
Under Model B (the default since the OIMG work) instruction streams live in a
`PROT_READ` mmap'd blob, so emitting `OP_SEND_IC` would SIGSEGV. The global cache
lives entirely in process RAM — Model-A/B safe, no bytecode/compiler/ABI/`.oimg`
change. `send_ic` is left as commented dead code. (Per-call-site IC via a RAM
side-table is the deferred "lever B".)

### Results (Model A -O2, before = cache dormant @`30db3db`, after = live @`e940e4a`)

| Benchmark | Before | After | Speedup | hit_rate | collisions | flushes |
|-----------|--------|-------|---------|----------|------------|---------|
| fib(32)   | 0.45 s | 0.42 s | 1.07× | 100.0%  | 0          | 0       |
| richards  | 0.06 s | 0.05 s | 1.20× | 90.6%   | ~96,934    | 0       |
| deltablue | —      | —     | —     | —        | —          | —       (pre-existing EXEC_ERR) |

`flushes = 0` on both ⇒ `bind:to:` is not called at steady state (classes are
pre-built in the static image), so invalidation is effectively free.

### Honest read of the gain (7–20%) — modest, and expected

1. **Dispatch was already dethroned as the bottleneck.** The earlier profiling
   (see "Profile after Opt #1/#2" above) showed fib's cost moved to the reflective
   frame-access send flood, since fixed by block inlining. A dispatch-lookup cache
   was never going to give a big win on dispatch-light code. fib's 100% hit / 0
   collisions confirms the cache works perfectly — there is simply very little walk
   left to save (monomorphic `fib:`, shallow `IntegerVtable`).

2. **richards is where it helps (deeper/polymorphic vtables) — but the table is
   saturated.** 90.6% hit *with ~97k collisions* means the 4096-entry direct-mapped
   table is thrashing: hot entries get evicted by diverse `(vtable, selector)`
   pairs. We are leaving gain on the table. Bumping to 8192 or 2-way associative
   would likely push richards higher; the `collisions` counter exists to measure
   exactly this. **This empirically validates the single-shared-table interference
   concern raised during design.**

3. **Measurement caveat — we measured in the config where the cache helps least.**
   `bench.sh` builds **Model A -O2**, the gcc-static-image path where the compiler
   already inlines/optimizes the vtable walk. The cache exists for **Model B** (the
   actual default deployment), where gcc cannot optimize the static image and the
   walk is a real indirect-heavy cost. Model B perf was **not** measured; the real
   shipping gain is plausibly larger than the numbers above, but that is an
   unmeasured claim.

### Conclusion — the real gap is float boxing, not dispatch

The ~1.9× geomean gap to PrimordialSoup is dominated by **float boxing**: nbody
(1.99×) and mandelbrot (2.21×) heap-allocate a `Float` on every arithmetic op.
That is a per-operation allocation, not a lookup — a far more targeted and likely
larger win than method caching. **Decision (2026-06-17): unboxed/tagged floats is
the next perf priority; until the float-boxing cost is eliminated, chasing other
levers (cache table tuning, Model B perf measurement, per-site IC) is the wrong
order.** The global method cache was the low-risk, broad-but-shallow lever — and
partly diagnostic: it confirmed dispatch is not the main gap and surfaced the
table-sizing issue.

### Deferred follow-ups (after floats)
- Re-size / set-associativity for the global cache (richards saturation).
- Measure Model B perf to quantify the cache's real shipping benefit.
- Per-call-site inline cache via RAM side-table ("lever B").

## Opt #5 — float bump arena (2026-06-17)

P0 Phase 1 (`docs/design/2026-06-17-unboxed-floats-design.md`). Every
float result was boxed via `gc_allocate` — a full `malloc` + heap-list insert per
op (`src/gc.c:49`). The four float allocation sites (`OP_ADD/SUB/MUL/DIV` and the
inexact `OP_DIV` in `src/vm.c`; `PRIM_asFloat`/`PRIM_sqrt` in `src/dispatch.c`)
now call `float_alloc` (`src/gc.c`), which hands out `struct Float` cells by
bumping a cursor inside a 4096-cell slab (chaining a new slab when one fills) —
~2 instructions vs. a `malloc`.

`oop` stays pointer-sized and `struct Float` is unchanged, so there is **no**
32-bit, dispatch, or `.oimg` impact. NaN-boxing was rejected for exactly this
reason (it needs a 64-bit `Value` everywhere — breaks `-m32` and the image
serializer). See the design doc's Constraints section.

### Results (Model A, `-O2 -DOEVM_APPLIANCE -lm`, best of 5)

Self-verifying workloads (both still print `true`): nbody at
`innerBenchmarkLoop: 250000`, mandelbrot at `size 750` (its shipped `main`).

| Benchmark | Before (malloc) | After (arena) | Speedup |
|---|---|---|---|
| nbody @250000 | 4.48 s | 2.93 s | **1.53×** (−35%) |
| mandelbrot @750 | 5.75 s | 3.04 s | **1.89×** (−47%) |

fib and richards are non-float (the SMI path never calls `float_alloc`) and are
unchanged by construction. The mandelbrot win is larger because it allocates a
float on essentially every inner-loop iteration (`2 * y / size`, `asFloat`, the
complex-square arithmetic), so it is the most allocation-bound of the two.

### Honest scope — what this does NOT do

The arena is **not reclaimed**. Nothing is reclaimed at runtime today — `gc_collect`
is never triggered (**issue #3**) — so this matches the existing "grow until exit"
memory profile while removing the `malloc` cost. It does not eliminate boxing: a
`Float` is still materialised per op, just cheaply. Eliminating the box for
non-escaping intermediates is **Phase 2** (Opt #6, compiler-level unboxed
temporaries), scoped separately. On the embedded target (nRF52840 / 256K) the
unbounded arena, like the rest of the heap, depends on issue #3 being fixed.

### Gates (all green)

- Conformance **36 pass / 5 fail / 1 skip** on Model A *and* Model B
  (`OEVM_PATH=AB`), unchanged from baseline.
- `-m32` portability (`tests/test_portability_32.sh`) passes.
- New unit test `tests/test_float_arena.c` (slab-boundary correctness).

---

## The C6 build is -O2 (`CONFIG_SPEED_OPTIMIZATIONS=y`, rete#329, 2026-10-05)

- **Why:** 1.14–1.40× faster on Hanoi, richards and every blockcost row (#324), for +35 kB flash and +5 kB sram0.
- **Gated by:** all fourteen board scenarios (`nodus/oevm/tests/target/run.sh`) pass at -O2 on `c6spare`
  (nine on 2026-10-04, the five radio ones on 2026-10-05; #329's comments name the builds).
- **Stack:** `ot_radio_workq` keeps 156 B free of its 512 at -O2 (168 at -Os); David accepted that headroom on
  2026-10-05. A passing scenario never prints the thread table (#353), so re-reading it means a forensics build with
  a synthetic wedge.
- **Not the RP2350,** until #296: its rows swing 0.40–2.63× on code placement alone.

## Opt #6 — the allocator's bin walk, the compiler peephole, the send and allocation paths in SRAM (rete#412, 2026-10-10)

Three changes, measured together on the WeAct RP2350B in one sitting (`rpi_pico2` image, `-O2`, blob in flash,
`Spike296Node.nsl`, three boots per image, the median shown and the spread when it exceeds 2 ms; transcripts in
`docs/spikes/2026-10-10-296-image-in-sram/logs-412/`, driver `run_412.sh`). Each column adds one change to the one
before. ms per row.

| row (ms) | master | + allocator | + peephole | + peephole, arm 2 | arm 2 / master |
|---|---:|---:|---:|---:|---:|
| hanoi | 2216 | 2219 | 1934 | 1906 | 0.86 |
| baseLoop | 2742 | 2775 | 2581 | 2494 | 0.91 |
| blockValue | 6688 | 6828 | 8185 | 6301 | 0.94 |
| methodSend | 7543 | 7569 | 7176 | 7042 | 0.93 |
| blockCapture | 8980 | 7215 | 7012 | 6994 | 0.78 |
| closureMake | 5807 | 3869 | 3794 | 3762 | 0.65 |
| ocDo | 10121 | 7995 | 7726 | 7620 | 0.75 |
| ocAt | 12340 | 12399 | 11232 | 11058 | 0.90 |
| richards | 9132 (9131–9252) | 8986 (8978–8998) | 8888 (8885–8897) | 8588 (8584–8600) | 0.94 |

- **The allocator** (`gc.c`). The sweep binned each interior dead run by size class, so the one big coalesced run
  sat in the catch-all bin. Once a live object near `heap_top` shrank the bump tail, every allocation that missed
  its exact bin walked the ~57 empty bins up to the catch-all, split the big block, and walked again next time: 2.17 M
  cross-bin searches and 126 M loop iterations against 0.75 M and 42 M, at the board's 128 KB arena (the host's 16 MB
  arena hid it). Two fixes: the largest interior run is kept out of the bins as `big_free` and carved O(1) like a second
  bump region, and a 64-bit mask of non-empty bins makes the remaining search a count-trailing-zeros. 20–34 % on every
  row that allocates per element, and every block activation allocates a Context. Any program could trip the walk; a
  compiler change tripped it by moving which object survived a collection.
- **The peephole** (`ns_compile_oevm.py`, `NSL_PEEPHOLE=1|A|B|0`). Copy propagation of a scratch `LD_REG` into its one
  consumer, and deletion of a load whose register is rewritten before any read, with liveness over the frame's
  control flow; `self`, the Context register and every register a nested block reaches through `home_ctx` are never
  touched. Image instructions −16.7 %, Hanoi's method 27 → 20. About 40 % of the emitted bytecode was register traffic
  no semantics required, and each bytecode costs ~41 cycles on the M33 (rete#296 record §9).
- **The placement** (`OEVM_HOT`). With only #403's hot set in SRAM, the peephole's new flash layout cost blockValue
  22 % through the 16 KB XIP cache. Adding the per-send callees (`lookup_method_cached`, `check_access_allowed`,
  `oevm_int_demote`, `make_closure`, `context_alloc`) and the allocation and mark paths (+3.6 KB of SRAM) removed it,
  and every row ends at or below master. The lesson for an XIP part: a change that only moves code can cost a fifth of
  a row, so the code a row touches per element belongs in RAM, not just the dispatch loop.
- **Per method activation** (Hanoi): 8.46 → 7.27 µs. MicroPython on the same silicon is 12.9 µs.
- **Gated by:** the nsl suite on msi-7d43 for each merge (aux line green), and all fourteen board scenarios on
  `c6spare` (2026-10-10/11). The sweep first found two #233 defects on master that failed every scenario built
  without the radio overlay: the OTA thread's stack call failed the link, and the runner compiled as its stub
  for want of `CONFIG_SETTINGS`, so the node booted and idled. Both are fixed under #412.

## Opt #7 — the kernel's `do:` in one dispatch per element: `OP_ITER_AT` (rete#37, 2026-10-11)

One opcode, `ITER_AT b, off` (0x1F): R(b) next index, R(b+1) limit, R(b+2) the indexable, R(b+3) ← the
element; past the limit it branches, else it loads the element and increments the index. The compiler emits it
for `a to: b do: [:i | ... (X basicAt: i) ...]` in **kernel source only**, when `i` is used only as
`basicAt:`'s argument and X is `self` or a slot the body never assigns. `Array>>do:` and
`OrderedCollection>>do:` take it, and so does everything the kernel builds on `do:` (`collect:`, `select:`,
`reject:`, `inject:into:`, `detect:ifNone:`, `includes:`); an app reaches it through those, never through a
hand-written index loop. `X` and the limit are read once at entry: legal because modifying a collection inside
its own `do:` is undefined (dialect row 33), and memory-safe because the held array is only ever replaced, never
shrunk. Pinned by `nodus/nsl/tools/test_iter_at.py` and `nodus/oevm/tests/test_op_fast.c`. Decision record:
`docs/design/2026-10-11-37-iter-at-design.md`.

WeAct RP2350B, the #296 node (`Spike296Node.nsl`), `-O2`, blob in flash, three boots identical to the ms
(`docs/results/2026-10-11-37-iter-at/logs/`). The control is yesterday's spike image, the same node over the same
master: every row the opcode does not touch reproduces it to the ms.

| row (ms) | before (spike control) | `OP_ITER_AT` | ratio | vs MicroPython (1491) |
|---|---:|---:|---:|---:|
| **ocDo** | 7668 | **5594** | **0.73** | 3.75× (was 5.1×) |
| hanoi, baseLoop, blockValue, methodSend, blockCapture, closureMake, ocAt | 1910, 2495, 6295, 7049, 6960, 3760, 11114 | the same ±1 | 1.00 | |
| richards | 8361 | 8445 | 1.01 | |

- **Per element: 7.67 → 5.59 µs.** The loop's own bytecodes (compare, branch, two loads, `AT`, add, jump) are now
  one dispatch.
- **What is left is the block call:** the spike's fused row, with the block body also inlined at the call site,
  ran 1219 ms, so about 4.4 µs of the 5.6 µs per element is the `aBlock value: e` activation. That is the next
  lever: call-site lowering of a literal `do:` block, which needs the receiver's class at the call site (#395).
- richards' +1 % is not explained by the opcode (it is not a `do:`-heavy row); read it as placement until a
  profile says otherwise.

## Opt #8 — kernel iterators inlined at the call site behind a class guard: `OP_IS_VT` (rete#37, 2026-10-11)

What Opt #7 left was the block call: about 4.4 of the 5.6 µs per element. A literal block passed to `do:`
(receiver OrderedCollection or Array) or to `collect:`/`select:`/`reject:`/`inject:into:` (OrderedCollection)
in app code now compiles into its caller. `IS_VT a, k, c` (0x20) sets R(a) to whether R(c)'s vtable is exactly
the constant k. Behind it runs that kernel method's own loop on `ITER_AT`, with the block's body inlined and its
element param bound to the element register. After the last guard comes today's send, with the block as a
closure, so a miss, a subclass or any other receiver means what it always meant. The compiler reproduces six kernel
methods by hand and reads `count`/`contents` by slot name; `nodus/nsl/tools/test_iter_inline.py` pins their
source. Two cases keep the send: a block whose nested closure captures its own param or temp (#417), and a debug
build (`OEVM_DEBUG_HOOKS=1` at compile time). Pinned by `test_iter_inline.py`, `nodus/tests/04_somns_basic/68_iter_inline.nsl` and
`nodus/oevm/tests/test_op_fast.c`. Decision record: `docs/design/2026-10-11-37-do-inline-design.md`; spike
`docs/spikes/2026-10-11-37-do-inline/`.

**STIC on the WeAct** (`nodus/tests/00_perf/performance.nsl`, RP2350B, `-O2`, blob in flash, three boots each,
identical to within 2 ms; `docs/results/2026-10-11-37-stic/`):

| test (ms) | before | after | ratio |
|---|---:|---:|---:|
| **collectionIterate** | 100755 | **15918** | **0.16** |
| dictionaryWrite | 19421 | 18066 | 0.93 |
| alloc, arrayWrite, floatMath, integerMath, collectionWrite, stringCompare | 18024, 67654, 26275, 4652, 38705, 3014 | 18119, 68474, 25916, 4804, 38586, 3071 | 0.99–1.03 (placement) |

**STIC on the plain ESP32** (the E4 board, 240 MHz; three boots each, identical to the ms;
`docs/results/2026-10-11-37-stic-esp32/`): collectionIterate 122586 → **22198 ms (0.18)**, against the published
ESP32 column's Toit 12.7 s, CircuitPython 29.2 s and uLisp 42.5 s. dictionaryWrite is +0.7 % there (−7 % on the WeAct),
unexplained; the other rows are within ±1 %.

The #296 node's rows (spike, the same lowering behind a flag): ocDo 5681 → **1087 ms (0.73× MicroPython)**,
arDo 6393 → 1155. A miss (an Interval: both guards fail, then the send) costs +2.3 %, ~1.7 µs a call. The
guard on a hit is ~0.55 µs a call, ~5 % of ocDoInline, so static receiver knowledge is not worth building on
these numbers. Size: +0 B on the bell (no site in any board node), DeltaBlue +0.7 %, `performance.nsl` +832 B.

- **Found while landing: a peephole bug, latent since Opt #6.** Copy-propagation skipped a deleted
  instruction before checking whether it carried a jump label, and so propagated a copy across a join
  point. The iterator's join was the first shape to trigger it. Fixed (labels are checked first).

## Opt #9 — fewer sends per collection operation: the send levers (rete#37, 2026-10-11)

After Opt #8, STIC's array, dictionary and collection write rows still cost about one full send per kernel
collection operation (~7 µs on the WeAct). Four changes, each a general mechanism:

1. **The kernel's collections on the fast opcodes.** OrderedCollection, HashedCollection, Set and Dictionary
   use `basicAt:`/`basicAt:put:`/`basicSize` on their own Arrays (OP_AT/OP_AT_PUT), and the one-line helpers
   (`ensureCapacity`, `indexFor:`, `capacity`, `atCapacity`, `growIfNeeded`) are folded into their callers and deleted.
2. **An Array's `at:`/`at:put:`/`size` at an app call site behind `IS_VT`.** On an exact Array the site runs
   OP_AT/OP_AT_PUT (which carry the selector, so an index out of bounds falls back to the send and its error)
   or reads slot 0; anything else sends. Off in a debug build, as Opt #8 is.
3. **`basicReplaceFrom:to:with:startingAt:`** (prim 163, one bounds-checked `memmove`) for OrderedCollection's grow.
4. **Stored hashes in Set and Dictionary** (`hashes` beside `table`, as CPython's entries keep theirs). A probe
   compares the stored hash before it sends the key's `=`; a grow or a cluster rehash re-places from it with no
   `hash` and no `=`. The comparison is `=`, not the spike's `==`: a user `hash` may answer a LargeInteger, and
   two equal ones are not the same object, so `==` lost such a key (found while landing; master was correct).
   On the host `=` measured no slower (dictionaryWrite 278 vs 281–288 ms).

Kept: the initial capacity of 8 (a 16-slot start measured −26/−36 % on the host and was rejected as tuning to
STIC's 10 elements; Toit, MicroPython and CPython all grow at least as often). Pinned by
`nodus/tests/04_somns_basic/69_stored_hash_keys.nsl`, `70_basic_replace.nsl`, `71_array_at_inline.nsl`.
Spike record, per-lever numbers and the competitors' growth policies: `docs/spikes/2026-10-11-37-send-levers/`.

**STIC on the WeAct** (RP2350B, three boots each, the spike's `==` build; seconds):

| test | Opt #8 | + levers | before Opt #8 → now |
|---|--:|--:|--:|
| **arrayWrite** | 68.5 | **22.9** | 3.0× |
| **dictionaryWrite** | 18.1 | **7.5** | 2.6× |
| **collectionWrite** | 38.6 | **17.0** | 2.3× |
| alloc | 18.1 | 16.8 | 1.1× (no lever behind it; placement until measured) |
| floatMath, integerMath, collectionIterate, stringCompare | 25.9, 4.8, 15.9, 3.1 | 25.9, 4.8, 15.8, 3.1 | 1.0× |

**STIC on c6spare** (ESP32-C6, 160 MHz, the landed `=` build, three boots each, identical to within 1 ms;
`docs/results/2026-10-11-37-stic-c6/`): arrayWrite 67.8 → **25.3 s** (2.7×), dictionaryWrite 17.1 → **7.5** (2.3×),
collectionWrite 36.6 → **16.0** (2.3×); the other five rows within 0.4 %.

Size: `performance.nsl` +680 B, DeltaBlue +696 B (.oimg, host); MCU text +295 B (re-banked 44252, `tools/footprint-mcu-baseline.txt`). Not done: making the send itself cheaper; a host
gprof puts ~19 % of a monomorphic send in `send_resolve` and ~8 % in `push_frame`, and the board's ~1,050 cycles a
send (host ~300) point at XIP latency, which needs a board-side measurement before any design.

## Opt #10 — the dispatch loop trusts its methods; a load-time verifier proves them once (rete#37, 2026-10-11)

The C6's performance counter (`docs/spikes/2026-10-11-37-c6-insn-count/`) put a simple bytecode at ~36 RV32
instructions, ~16 of them the dispatch head: the `pc < pc_end` test, the jump table's bound check, the
LD_CONST/LD_REG operand checks, an out-of-line `oevm_int_demote` on every constant. The loop now checks none of
it per instruction. `oevm_method_check` / `oevm_inst_check` (vm.c) refuse an unknown opcode, an out-of-range
LD_CONST/LD_REG operand, a jump outside the method, and a last instruction that is not OP_RET or OP_JMP — once,
at load for every method in the image (format 11's `methods_ref`/`methods_count`, `oimg_loader.c`), in the patch
primitive, and in `method:instructionAt:put:`. Model A methods are compiled into the firmware and are as trusted
as its C. LD_CONST tests `IS_SMI` inline; the demotion cannot move to image build, because an image ref-word
holds a 30-bit SMI and the VM's is 31.

The verifier's first run found `release:` in Richards: an `ifTrue:ifFalse:` whose arms both return, after which
the compiler omitted `^self` but kept the then-arm's jump to one past the end. `end_with_return` (compiler) now
emits the return whenever a jump targets the end.

**STIC on the WeAct** (three boots each, `docs/results/2026-10-11-37-lean-dispatch/`; seconds): integerMath
4.80 → **4.07**, arrayWrite 22.9 → **18.8**, collectionIterate 15.8 → 14.2, dictionaryWrite 7.39 → 6.71,
collectionWrite 17.4 → 15.9, alloc 16.8 → 15.9, stringCompare 3.13 → 2.91, floatMath 25.9 → 25.3.
Every row, 2.4–17.8 %.

Size: host text +864 B (vm.o +411, oimg_loader.o +453; re-banked 77127, `tools/footprint-baseline.txt`);
the WeAct image +812 B FLASH, the blob +8 B.

## Opt #11 — the dispatch head: operands decoded per case, SMI arithmetic on the tagged words (rete#37, 2026-10-11)

What Opt #10 left in the dispatch head. **Operands:** the loop decoded all three operand fields of every
instruction before the switch. Now each case decodes the fields it reads (`DEST`/`SRC1`/`SRC2`, macros over
`inst`). On ARM the head went from nine instructions to five: `ldr`, `uxtb`, the jump table's `cmp`/`bhi`, `tbh`.
GCC 12 keeps the bound check even with `default: __builtin_unreachable()`, and only computed goto would remove it.
**Arithmetic:** ADD, SUB and MUL have their own cases and compute on the tagged words. An SMI is the word 2v+1,
and its payload is the whole word less the tag, so the word's own overflow flag means exactly "no longer an SMI":
`a + (b-1)`, `a - (b-1)`, `(a-1) * v(b)`. Anything else goes to one out-of-line `arith_slow`: an overflow, a
LargeInteger, a Float, and every `/`. `arith_slow` tests Float with Float first.
`JMP_IF_FALSE` tests the two boolean statics before it calls `is_truthy`. `tests/06_limit_tests/08_smi_arith_edges`
crosses both SMI ranges (63-bit host, 31-bit MCU and `-m32`) in each direction.

**STIC on the WeAct** (three boots each, `docs/results/2026-10-11-37-dispatch-head/`; seconds): integerMath
4.07 → **3.10**, arrayWrite 18.8 → **17.3**, collectionIterate 14.2 → 13.1, floatMath 25.3 → 24.2,
dictionaryWrite 6.71 → 6.50, collectionWrite 15.9 → 15.6, stringCompare 2.91 → 2.86, alloc 15.9 → 15.8. Every row
improves, by 0.7–24 %.

Measured and not kept (same record):
- **The slow path inlined in the loop.** Floats paid a `long_call` into SRAM, and the first split cost floatMath
  +10.5 %. Inlining the slow path at one label gave that back, but cost every other row 2–3 %, from register
  pressure in a bigger loop function. Testing Float with Float first inside the called function recovered more.
- **Peephole (C), result forwarding** (`op t …; LD_REG v, t` → `op v …`). integerMath −2.3 %, alloc and
  dictionaryWrite +2 %. It changes compiled output, so the checked-in fixture and custos's pinned offsets would
  have to be re-pinned. Not worth 2 %.

**The verifier's boot cost** is now printed at every Model B boot: `oevm: image loaded and verified (N us)`. For
the 164 KB STIC image it is 11.2 ms on the WeAct and 7.4 ms on the C6, where `program sha256` of the same image
takes 97 ms on the same boot.

Size: MCU text +244 B (decode +156, Float-first +88) on top of Opt #10's unbanked +860; re-banked 45356
(`tools/footprint-mcu-baseline.txt`). Host text 77801, inside its 2 %.

## Opt #12 — the access mechanism deleted (rete#37 L1, 2026-10-12)

The first lever of the send/frame design (`docs/design/2026-10-12-37-send-frame-cost-design.md`). Every send to
a bytecode method called `check_access_allowed`, and the check never refused anyone, because the compiler never set a
method's `access_modifier` (dialect row 25). David: delete the whole mechanism. Deleted: the check and its two
helpers, `trigger_access_violation`, `Symbol_accessViolation` and its GC root, `CompiledMethod.access_modifier`
(28 → 24 B on 32-bit; the five board layouts were edited by hand and checked with static asserts on the SDK's RV32,
ARM and Xtensa compilers), builtin #14 `accessViolation:` (registry version 5 → 6), the patch format's `access` line,
and `test_access.c`.

**AWFY** (`docs/results/2026-10-12-37-send-frame/`, L1 ÷ base): **C6 0.936** (two chips, identical: DeltaBlue −13 %,
List −7.4 %, Richards −3.5 %), **WeAct 0.961**, host x86-64 0.963. **STIC:** C6 0.986 (alloc −6.7 %), WeAct 0.997.
The loop rows do not move. stringCompare on the C6 (+3 %) is the only row that got worse. MCU text 44,602 B (−754
against the bank of 45,356), host 76,929 (−872).

Measured and not kept (same record): **L2, a frame push that answers its method** (one `push_frame`, so a send site
skips its second `closure_entry`, with the entry prologue folded in). On the host it was 0.983 when forced inline.
Left to gcc it was 1.051 (gcc stopped inlining it at four of seven sites). On the boards both forms lose: C6
STIC +2 %, WeAct STIC +2–3.5 %, WeAct AWFY +2.4 % forced inline, C6 AWFY about level. Changing `run_interpreter`'s
body moves register allocation for the whole loop, and the boards' gcc 12 pays where host gcc 16 did not. Reverted.

## Opt #13 — frameless accessor sends (rete#37 L6, 2026-10-12)

The counting build (M, `docs/results/2026-10-12-37-M-counts/`) showed that half or more of the frames on Richards, CD,
Towers and List, and 46 % on DeltaBlue, run a method that is a single slot access. Following Toit's
`INVOKE_VIRTUAL_GET/SET`, a send to a getter, a setter or `^ self` is now answered in `send_resolve` with no frame. The
compiler states each method's shape in `CompiledMethod.shape` (16 bits taken from `num_arguments`, so no offset moves),
`oevm_method_check` proves it at load, and `frameless_send` does the slot access. `run_interpreter` is unchanged: a
test at its send sites cost 10–20 % on the host rows that have no accessor.

**AWFY** (`docs/results/2026-10-12-37-l6/`, L6 ÷ base): **WeAct 0.941** (Richards −13.4 %, DeltaBlue −12.9 %, CD
−9.7 %, List −10.6 %, Towers −6.2 %; the other rows level), **C6 0.988** (Richards −13.3 %, but Towers +11 % and the
untouched rows ±2–4 %), host 0.972. **STIC:** WeAct 0.984, C6 1.016. On the C6 the interpreter runs from flash through
the cache, so code placement moves rows that L6 does not touch. The WeAct runs its hot path from SRAM and is the
control. MCU text 45,109 B (+507), host 77,680 (+751).

## Opt #14 — the C6 hot path in SRAM (rete#37, 2026-10-12)

The C6 was the last XIP target running the interpreter from flash; `OEVM_HOT` now places the hot path in `.iram.text`
there too (default on). **C6 AWFY 0.951, STIC 0.997** (`docs/results/2026-10-12-37-c6-sram/`): Towers −15 %, CD −17 %,
DeltaBlue −12 %. Beyond the speed, it ends the placement roulette that made L6's untouched rows move 2–8 % on the C6:
every XIP target now runs its hot path from RAM, and `run.sh`'s placement report says so after each build.

## Opt #15 — `gen_tag` dropped (rete#37 L4(c), 2026-10-12)

Every frame got a fresh serial number from a global counter, copied into each Closure and Context made in it, and
nothing ever compared it: the stale-home guard it was meant for was never written (send/frame design Q3). Deleted, with
both `home_generation` fields. The frame header is five words. **AWFY C6 0.988, WeAct 0.981; STIC C6 0.995, WeAct
0.978** (`docs/results/2026-10-12-37-l4/`): DeltaBlue −2.4 %, Richards −1.2 %, integerMath −4 to −5 %. The same
session's L3-lite (frame globals read once into locals) was level on the C6 and lost 2–4 % on the WeAct: a third
loop-shape change that each target's gcc prices differently.

## Opt #16 — the `send_resolve` head (rete#37 L5, 2026-10-12)

Every send tested for the first `main:args:` (a one-shot capture of the Kernel instance for the raise prims) and
read the receiver's vtable in three places before the cache probe. The launch sites now set the Kernel instance, and
the vtable is read once. **AWFY C6 0.985, WeAct 0.963; STIC C6 0.987, WeAct 0.978**
(`docs/results/2026-10-12-37-l5/`), with the SMI arm before the lookup. With the arm at the top, `-Os` copies the
primitive block for a known vtable: **AWFY C6 0.978, WeAct 0.949; STIC C6 0.980, WeAct 0.969**, for 860 B of MCU text.
David chose the speed: the arm is at the top and the bank paid for it (`tools/footprint-mcu-baseline.txt`).

## Opt #17 — the arena header in one word (rete#37, 2026-10-12)

`struct HeapHeader` was three words (`next`, `size`, `marked`) before every arena object. The free-list link now lives
in a free block's first payload word and the mark in bit 0 of the size: 12 → 4 B per object on the boards, 24 → 8 on
the host, all inside `gc.c`. Speed level on every board (1.000 ± 0.002); it is a memory lever. **The smallest arena
that verifies, board layout: AWFY board profile 154 → 136 KB, Storage 350 → 308 KB, Json 430 → 350 KB**
(`docs/results/2026-10-12-37-alloc-hdr/`). Storage and Json still do not fit the boards' 256 KB.

## Opt #18 — nothing ahead of the probe but the prim id (rete#37 L5, 2026-10-12)

After Opt #16 every send still tested, before the method cache probe, the `Symbol =` gate, three capability
receivers, and the selector (NULL, SMI, its vtable — on an image literal a blob check and an `oimg_resolve`). The
`Symbol =` arm moved into the `=` prim's case, the capabilities to the lookup miss (their vtables are empty), and the
loader now proves every send instruction's selector constant is a Symbol (`oevm_inst_check`), so the prim test is one
load of `prim_id`; `send()` checks its C callers. **AWFY C6 0.918, WeAct 0.926; STIC C6 0.941, WeAct 0.939**
(`docs/results/2026-10-12-37-l5-rest/`); the sending rows gain 8–19 %, the rest are level. MCU text +890 B, inside the
ratchet's 2 % (40 B left). A prim selector sent to a capability is now answered by the prim instead of aborting.

## Opt #19 — a method frame's home is NULL (rete#37 L4(b), 2026-10-12)

A frame stores its closure's `home_ctx` as-is (NULL for a method) instead of encoding itself, and the mirror's
`homeOf:` — the field's only reader — answers the frame itself for NULL. One encode site fewer, MCU text −28 B. Speed
level: the geomean moved 0.992 (C6 AWFY) on placement, while the frame-heavy rows were level
(`docs/results/2026-10-12-37-l4b/`). Kept for simplicity.

## A cost, not a lever — method visibility enforced (rete#372, 2026-10-12)

Visibility is a mandatory language feature (David), built at no cost on a cache hit
(`docs/design/2026-10-12-372-method-visibility-design.md`): a `CompiledMethod.access` header field read at a
fill, `OP_SEND_SELF` and `OP_SEND_PRIVATE`, and the lookup rule ORed into the method cache's key. On c6spare
(`docs/results/2026-10-12-372-visibility-board/`, against master): **AWFY 1.018, STIC 1.010** once super and private
sends went through the cache; uncached, the private send had cost AWFY 1.034 (Permute's recursive private
`permute:`, +8.5 %). Firmware text −4 B. David: merge, and record the cost.

Deferred fronts, with what would earn each back:

- **The send path's own cost (integerMath +3.5 %, alloc +3.6 %, rows with no visibility in them).** Suspects:
  `send_resolve`'s new `rule` argument, and the OP_SEND/OP_SEND_SELF and OP_SEND_SUPER/OP_SEND_PRIVATE handlers'
  shared bodies moving the loop's layout (L2's lesson). Try: the ordinary path with no rule argument, only
  OP_SEND_SELF tagging its key. Earns its place by a board A/B that moves those two rows.
- **A private send is one more dispatched instruction than the OP_SEND it replaced** (the vtable constant; Permute
  and Towers +4 %). Try: resolve a private send's target once, at load or first fill, so the site needs no vtable
  register. Earns its place by Permute and Towers moving on the board.

## Known weaknesses / tracked gaps (snapshot 2026-06-18)

Captured after the sober-reference benchmark exercise (see
`../../ns-tests/00_perf/RESULTS.md`) so progress is trackable against a fixed
baseline. At this date: conformance **47/0/1** (complete); steady-state
throughput **A/Soup 1.84×, B/Soup 2.07×** (a no-GC upper bound); VM footprint **8
files / 3,369 LOC / 60 KB stripped** (4.6× leaner than Soup). The migration
target is now **Zephyr OS** (resident VM on nRF52840-class, ~256 KB RAM), which
reorders priorities toward memory, not speed.

Those throughput/footprint numbers are competitive **specifically because two of
the largest levers are unbuilt** (a working inline cache, unboxed floats) and
**no GC cost is paid yet**. Expect the headline to move as these land.

| # | Weakness | Evidence | Status | Zephyr priority |
|---|---|---|---|---|
| W1 | **GC never triggered → unbounded heap** | `gc_collect` complete but called only by unit tests; no threshold in `gc_allocate`. mandelbrot peak RSS 4.5 GB vs Soup 6 MB. GH #3. | **DONE** — GC trigger wired (exhaustion collect-and-retry in `gc_allocate`); conservative C-stack rooting (setjmp spill + `[lo..hi)` scan at `void*` stride, host-only); `debugger_hook`/`Symbol_debug_`/`Symbol_accessViolation` + defensive `unwind_return_value` precise global roots; method-cache flush on collect. `05_gc_stress.ns` runs to completion under a **64 KB** managed-arena cap on Path A+B (smallest passing cap for gc_stress; full conformance 47/0/1 passes at 128 KB). GH #3 gate closed. **Iterative-mark follow-up (GH #4): DONE** — `mark`/`trace_object` converted to an explicit `OEVM_MARK_STACK_SIZE` worklist (default 1024) with overflow→heap-rescan recovery in `src/gc.c`; mark-phase C-stack use is O(1) in graph depth (Zephyr-safe). Deep 500k-node chain survives on a 256 KB thread stack; overflow-recovery unit test passes at `-DOEVM_MARK_STACK_SIZE=4`; conformance 47/0/1 A+B+m32. GH #4 closed. Commits: `w1-gc-trigger` branch (trigger); `w-gh4-iterative-mark` branch (iterative mark). | **P0** (spec: `design/2026-06-18-gc-embedded-memory-design.md`) |
| W4 | **Allocator not embedded-grade** | `gc_allocate` = `malloc`/object + linked list; mark is O(n²) (`is_valid_heap_object` walks the list per pointer marked); conservative word-granular tracing. | **DONE** — fixed static arena + size-binned free-list + O(1) bitmap membership; 46 non-stress tests pass under `-DOEVM_HEAP_SIZE=262144` (256 KB), 46/1/1, hard-cap OOM path fires cleanly. NB: 256 KB = *cumulative no-GC churn* (W4 has no trigger) = an **upper bound**, NOT the steady-state ceiling — true figure awaits W1. The 1 fail is `gc_stress` (fills any fixed heap without a trigger; = W1's acceptance test). | **P0** (W1 trigger is the next step in the same workstream) |
| W2 | **Boxed floats** | a `struct Float` per `OP_ADD/…`/`asFloat`/`sqrt`; dominant memory **and** speed sink. Arena (Opt #5) cut only the `malloc` cost. | **W2a DONE** — arena reclaim: per-slab mark bitmaps + cell free-list, swept from `gc_collect`; bounded slab count under sustained churn (13 slabs at 50k-float churn, 4096 cells/slab). **W2a.1 DONE** — `float_alloc` collect-at-cap trigger: at a `-D`-tunable soft slab budget (`OEVM_FLOAT_ARENA_SLAB_BUDGET`, default 16) the arena runs `gc_collect` and reuses a reclaimed cell before growing (mirrors `gc_allocate`). Float-dominated workloads now self-bound: **mandelbrot @750 RSS 2.35 GB → 3 MB** under a 256 KB main-heap cap, output still correct (`true`); conformance 47/0/1 on A+B+m32. GH #5 / GH #3 (float side) closed. **W2b open** — float unboxing (Opt #6, deferred: measured speed ceiling sub-second; revisit only if profiling justifies it). | **P1** (W2b: de-risks W1 further; fewer allocations → less GC pressure) |
| W3 | **Inline caching half-realized** | OEOM/MIC design (`OP_SEND_IC`, `struct InlineCache` in `vm.c`) never emitted by the compiler — in-stream cache slots conflict with Model B's read-only XIP blob. Global method cache (Opt #4) is the RAM-side workaround, modest 7–20%. | partial (global cache only) | **P2** (pure perf; ~2× already in budget) |
| W6 | **Thin reflection** | only low-level frame/vtable primitives (`bind:to:`, `regAt:`, debugger hooks); no Mirror hierarchy. See `reflection-mirrors-state` memory. | open (not a blocker) | **P3** (only if OTA live-debug needs it) |
| W5 | **Dynamism blocks static float proof** | compiler lowers `+−×÷` to polymorphic `OP_*` with no type info, so unboxing can't be simple escape analysis — forces runtime-guarded fusion. | constraint, not a task | shapes W2's design |

**Zephyr priority order = W1+W4 → W2 → W3 → W6** (user's proposed 1,4,2,3,6,
refined: W1 and W4 are *one* workstream and W4 — the fixed-arena allocator with
O(1) membership — is the foundation that must land *before/with* the W1 trigger,
else triggering the O(n²) `malloc`-GC is itself too slow; W2 is correctly third
because unboxing both speeds float code and shrinks the heap GC must manage). The
gating cluster for "runs on Zephyr at all" is **memory (W1+W4+W2)**; W3/W6 are
quality-of-life after the device boots and stays up.

### Not-weaknesses, but tracked status (for completeness)

- **Fibers / cooperative scheduling (old "Phase 4").** Implemented at the VM
  level — `primitive_fiber_create`/`primitive_fiber_switch`, state-by-vtable
  (`Runnable`/`Suspended`/`Dead`), GC traces per-fiber stacks. But **no `.ns`
  test exercises them**, there is no language-level `spawn:`/actor surface in
  `ns_compile_oevm.py`, and "scheduling" is manual cooperative `fiberSwitch`, not
  a scheduler loop. Covered only by `tests/test_fibers.c`. Effectively *dark* at
  the language level — same shape as the reflection gap (W6). Relevant to Zephyr
  only if the deployment needs concurrent containers; otherwise parked.
- **Bytecode-driven NLR (old "Phase 5").** Done: `OP_NLR` (`vm.c:317`) reifies
  the context and sends `#unwindTo:value:` (`vm.c:347`) — the implementation_plan
  Phase-5 vision is realized.
