# OEVM interpreter optimization log

Tracks performance work on the OEVM interpreter core (`src/dispatch.c`, `src/vm.c`),
with before/after measurements and the commit that landed each change so
improvements are traceable.

**Design rationale** for the decisions referenced here lives in `nodus/oevm/docs/topics.md`:
[selector-dispatch-caching](topics.md#selector-dispatch-caching) (Opt #1/#2),
[reflective-frame-access](topics.md#reflective-frame-access) (the post-#2
bottleneck), [block-inlining](topics.md#block-inlining) and
[scope-vs-frame](topics.md#scope-vs-frame) (the structural fix in progress).

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
- **Gated by:** the nsl suite on msi-7d43 for each merge. The fourteen C6 scenarios were not run: c6spare was not on
  msi's USB that night.

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
