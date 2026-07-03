# OEVM Roadmap

Consolidated, prioritized backlog for the OEVM virtual machine. This is the one
place to look for "what's left." Detailed rationale lives in the linked docs;
this file is the index and the priority order.

**Status (2026-06-18):** Conformance **COMPLETE — 47 pass / 0 fail / 1 skip**
(Path A+B+`-m32`). All former P1 fails closed (nested-class/super/bignum families +
deltablue). Model B (decoupled `.oimg` execute-in-place) is the default build;
Model A is `-DOEVM_APPLIANCE`. Perf lever 1 (inline caching → global method cache)
shipped. **P1 below is now historical** — kept for the record; live backlog is
P0 Phase 2 (deferred), P2 correctness, P3 perf, and GH #3 (GC never triggers).

## Prime directive

**GC / embedded memory comes first now (Zephyr gate).** The migration target is
**Zephyr OS** (resident VM on nRF52840-class, ~256 KB RAM). The 2026-06-18 sober
benchmark exercise confirmed throughput is already in budget (steady-state
A/Soup 1.84×, B/Soup 2.07× — within the 2–3× win zone) but the heap grows
**unbounded** (mandelbrot peak RSS 4.5 GB; GC never triggers, GH #3) — fatal on
device. **Memory, not speed, is the blocker.** Priority order (= weaknesses
W1+W4 → W2 → W3 → W6, see `docs/optimizations.md` snapshot 2026-06-18):
GC trigger + embedded allocator → float unboxing → inline cache → reflection.
(User decision, 2026-06-18.)

**Superseded prime directive (2026-06-17):** "unboxed floats first." Phase 1
(arena) shipped; Phase 2 (unboxing) stays deferred but is now **P1 under the GC
workstream** because it also cuts GC pressure — see GP0 below.

---

## GP0 — GC & embedded memory (THE Zephyr gate) — GH #3

**Scope:** `docs/superpowers/specs/2026-06-18-gc-embedded-memory-design.md`.
A complete mark-sweep already exists (`src/gc.c:238`) but is **never triggered**
at runtime, the float arena is **unreclaimable**, and the allocator is
`malloc`-backed with O(n²) mark — none embedded-grade. Sequencing:

1. **Allocator foundation (W4): DONE.** Fixed static arena + bump alloc + O(1)
   bitmap membership (kills the O(n²); removes `malloc` dependency). The 46
   non-stress conformance tests run to completion under `-DOEVM_HEAP_SIZE=262144`
   (256 KB), 46/1/1, and the hard-cap OOM path fires cleanly. **Caveat:** W4 has
   no runtime GC trigger, so 256 KB is the *cumulative no-GC allocation* of the
   heaviest run (live + dead churn) — an **upper bound**, not the steady-state
   live set. The true embedded ceiling can only be measured once W1 reclaims at
   runtime; it is expected to be far lower. The 1 fail is `gc_stress`, which fills
   any fixed heap without a trigger — exactly W1's acceptance test.
2. **Trigger + safe-points (W1): DONE.** Exhaustion collect-and-retry wired into
   `gc_allocate`; conservative C-stack rooting (setjmp spill + `[lo..hi)` scan at
   `void*` stride, host-only); `debugger_hook`/`Symbol_debug_`/`Symbol_accessViolation`
   + defensive `unwind_return_value` precise global roots; method-cache flush on
   collect. `05_gc_stress.ns` runs to completion under a **64 KB** managed-arena cap
   on Path A+B (full conformance 47/0/1 holds at 128 KB). GH #3 gate closed.
   Branch: `w1-gc-trigger`.
   - **Iterative-mark follow-up (GH #4): DONE** — see step 5 below.
3. **Float reclamation (W2a): DONE.** Per-slab mark bitmaps + cell free-list
   rebuilt by `float_sweep`, called from `gc_collect` after the main-heap sweep.
   `float_alloc` draws from the free-list before the bump cursor, so reclaimed
   cells are reused immediately. Bounded slab count verified under sustained churn
   (13 slabs at 50k-float/round, 4096 cells/slab — steady state = minimum capacity
   for the live set). Pristine-markbit edge case fixed (bump branch clears the bit
   before returning). `tests/test_float_gc.c` covers reclaim/reuse, mixed liveness,
   conservative survival, slab-boundary, and bounded churn. Branch: `w2a-float-reclaim`.
4. **Float-arena GC trigger (W2a.1): DONE.** W2a reclaimed cells but nothing *ran*
   the collector from the float path, so a float-dominated workload that never
   touched the main heap grew the arena without bound. `float_alloc` now collects
   at a soft slab cap: at the new-slab boundary, if `float_slab_count` has reached
   `OEVM_FLOAT_ARENA_SLAB_BUDGET` (`-D`-tunable, default 16) it runs `gc_collect`
   (which refills the free-list via `float_sweep`) and reuses a reclaimed cell
   instead of growing — falling through to `malloc` only if the live set genuinely
   needs it (mirrors `gc_allocate`'s collect-on-exhaustion; fast paths untouched).
   **mandelbrot @750 RSS 2.35 GB → 3 MB** under a 256 KB main-heap cap, output still
   correct (`true`); conformance 47/0/1 on A+B+m32; `tests/test_float_gc.c` gains a
   self-triggering-bound regression. GH #5 / GH #3 (float side) closed.
   Branch: `w2a1-float-trigger`.
5. **Iterative mark (GH #4): DONE.** Recursive `mark`/`trace_object` converted to
   an explicit `OEVM_MARK_STACK_SIZE` worklist (default 1024) with overflow→heap-rescan
   recovery in `src/gc.c`; mark-phase C-stack use is O(1) in graph depth (Zephyr-safe).
   Deep 500k-node chain survives `gc_collect` on a 256 KB thread stack (old recursive
   mark crashed); overflow path covered by unit test at `-DOEVM_MARK_STACK_SIZE=4`;
   conformance 47/0/1 on Model A+B+m32; gc_stress passes bounded under the 64 KB cap.
   GH #4 closed. Branch: `w-gh4-iterative-mark`.
6. **Float unboxing (W2b / Opt #6 — DEFERRED):** the fusion work — perf + less GC
   load. Measured speed ceiling is sub-second; revisit only if profiling justifies
   it. Was P0 below; folded here. Its own spec + plan when picked up.

With steps 1–5 done, the pre-Zephyr GC surface (trigger + safe-points, float
reclaim/trigger, iterative mark) is closed; only W2b (step 6) remains, deferred.
The no-GC steady-state numbers in `ns-tests/00_perf/RESULTS.md` predate collection
running; a small steady-state regression under collection is expected and accepted.

---

## P0 — Unboxed / tagged floats *(now folded into GP0 step 3–4)*

> **Re-prioritised 2026-06-18:** float reclamation (W2a) and unboxing (W2b) are
> now steps 3–4 of the GC workstream **GP0** above, because under Zephyr the
> float problem is primarily a *memory* problem. Detail retained below.

Staged **arena now, unboxing after** (design:
`docs/superpowers/specs/2026-06-17-unboxed-floats-design.md`).

**Phase 1 — float bump arena (Opt #5): DONE & shipped** (master `c2cbaea`). Every
float result was boxed via a full `malloc` per op; `float_alloc` (`src/gc.c`) now
bump-allocates `struct Float` cells from slabs. **nbody @250000 1.53×, mandelbrot
@750 1.89×** (Model A -O2, best of 5); 36/5/1 on Model A+B and `-m32` hold; covered
by `tests/test_float_arena.c`. NaN-boxing was rejected — it needs a 64-bit `Value`
everywhere, breaking the pointer-sized `oop`, the `-m32` gate, and the `.oimg`
serializer. See `docs/optimizations.md` Opt #5.

**Phase 2 — unboxed temporaries (Opt #6): DEFERRED** (user decision 2026-06-17,
after Phase 1 shipped). The original "compiler proves which floats don't escape"
framing **does not hold**: Newspeak is dynamically typed and the compiler lowers
`+ − × ÷` straight to the polymorphic `OP_ADD/SUB/MUL/DIV` with no type info
(`ns-oevm/tools/ns_compile_oevm.py:651`). The feasible design is **arithmetic
expression-tree fusion**: fold a pure `+−×÷` subtree into one fused op that
computes in raw C doubles and boxes only the root, with runtime leaf type-guards
and a slow-path fallback that preserves exact `/` (vm.c:389), integer overflow,
and non-number DNU. Deferred because the arena already captured the large bite and
the fusion correctness surface (fast/slow path) is sizeable; revisit if perf
demands it. Its own spec + plan when picked up.

- **Refs:** `docs/optimizations.md` (Opt #5), the design doc above,
  `../ns-tests/00_perf/RESULTS.md`.

---

## P1 — Conformance: ALL CLOSED ✅ (historical)

**As of 2026-06-18 the entire failing set below is GREEN** (suite 47/0/1, Path
A+B+`-m32`). Closed via the nested-class BoundClass model, super-send staging,
bignum (LargeInteger), and a 7-bug deltablue chain. Commits: oevm `6fad867`,
ns-oevm `1856e14` (and the earlier nested/super/bignum commits). The original
triage is preserved below for reference.

Original failing set (see `../ns-tests/benchmark_comparison.md` and the suite):

- `06_outer_sends` — outer-send resolution gap.
- `07_super_sends` — super-send resolution gap.
- `01_deep_nesting` — nested-class resolution.
- `07_deltablue` — runtime crash (`DeltaBlueBenchmark` DNU).
- `04_integer_overflow` — needs **bignum** (SMI→largeint promotion). Also why the
  Model B path can't yet serialize large SMIs (62-bit serializer overflow).

---

## P2 — Correctness: bugs + the regression net

Priority here is the *correctness safety net*, not feature count. The behavioral
gate (ns-oevm conformance suite, 47/0/1, Model A+B) runs clean; the C unit tests
are the finer-grained net — and two of them are currently dark.

- **GitHub issue #2:** `does_not_understand` segfaults on a SMI (tagged-integer)
  receiver — dereferences `receiver->vtable` without a SMI guard
  (`src/dispatch.c`, ~line 130). Latent; noted in `docs/optimizations.md`.
- **Revive the two dark behavioral unit tests** (correctness-net hole, not feature
  backlog). `tests/test_mirrors.c` (frame + vtable introspection via
  `primitive_inspect_stack`) and `tests/test_solidified.c` fail to link: they
  hard-code app symbols (`RecursiveRunnerVtable`, `CounterVtable`,
  `Symbol_runRecurse_`, …) from an old per-test `solidified_image.c` fixture the
  current generated image no longer provides. Decouple them from the live fixture
  (give them a dedicated, committed fixture) so they run in the unit-test set
  again. Until then, the introspection primitives (`sender`/`pc`/`regAt:`/
  `delegate`/`bindings`/`bind:to:`) have **no** automated behavioral coverage.

---

## P3 — Performance follow-ups (AFTER P0 floats)

- **Global method cache tuning.** The 4096-entry direct-mapped table saturates on
  richards (~97k collisions, still 1.20×). Resize (8192) or go 2-way
  set-associative; the `collisions` counter measures the win.
  → `docs/optimizations.md` Opt #4.
- **Measure Model B perf.** `bench.sh` only measures Model A (-O2, gcc-optimized
  static image — where the cache helps *least*). The cache exists for Model B
  (read-only image, unoptimized walk); its real shipping benefit is unmeasured.
- **Per-call-site inline cache ("lever B").** A RAM side-table IC keyed off the
  blob-resident `CompiledMethod`. The in-stream `OP_SEND_IC` machinery is unusable
  under Model B (writing the IC into the `PROT_READ` blob would SIGSEGV), so it
  needs the side-table redesign. Deferred.
- **GitHub issue #1:** Closure representation — replace reflective `home`/`regAt:`
  outer-variable access with a direct outer-frame pointer + indexed access (the
  remaining non-inlined-closure cost; block inlining already handles the inlined
  cases).

---

## Reflection / Mirrors / introspection

**There is no Newspeak Mirror class hierarchy (ObjectMirror/ClassMirror/etc.).**
What exists is a set of **low-level introspection primitives** in the VM that a
mirror layer *would* be built on:

- Stack-frame introspection: `sender`, `pc`, `regAt:`, `regAt:put:`, `home`,
  `outer` (frame walking).
- Vtable introspection: `delegate`, `bindings`.
- Reflective mutation: `bind:to:`, `instructionAt:put:`.
- Debugger: `singleStep:`, debugger-hook registration.

These live in the primitive table in `src/dispatch.c` and the frame/vtable
accessors. The C unit tests `tests/test_mirrors.c` (stack + vtable reflection via
a `primitive_inspect_stack`) and `tests/test_debugger.c` exercise these
primitives directly.

**Do the `.ns` test suites need a Mirror API?** Based on a scan of all `.ns`
tests: **no current suite uses mirror/reflective selectors** (`perform:`,
`respondsTo:`, `instVarAt:`, `ObjectMirror`, …). The `class` keyword in the
suites is class-*declaration* syntax, not a reflective `self class` send. So a
Mirror layer is **not** a blocker for the present suites. If a future suite (or a
fuller SOMns/Newspeak port) requires capability-based mirrors, that is net-new
work to scope.

**Behavioral coverage of these primitives is currently dark** — the C unit tests
that exercise them (`test_mirrors.c`, `test_solidified.c`) don't link. Reviving
them is a **P2 correctness item** (see above), not feature work.

---

## Deferred — nodus keeper ↔ VM integration

Make oevm the VM the nodus / nodus-st supervisor runs (replacing Berry): the
C-keeper ↔ VM seam, OTA container-image versioning, isolation. **Blocked by
decision:** the NS (Newspeak) language must be fully working and benchmarked
first. → `docs/deployment-model.md`.

---

## Where each area is documented

| Area | Document |
|------|----------|
| Perf work + deferred levers | `docs/optimizations.md`; rationale in `docs/topics.md` |
| Conformance gaps | `../ns-tests/benchmark_comparison.md`, `../ns-tests/00_perf/RESULTS.md` |
| Open bugs / structural items | GitHub issues #1, #2 (`gh issue list`) |
| Image format (Model B) | `docs/superpowers/specs/2026-06-17-clustered-image-xip-design.md` |
| Deployment / nodus | `docs/deployment-model.md` |
| Historical phased build plan | `docs/implementation_plan.md` (not a live backlog) |
