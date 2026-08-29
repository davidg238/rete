# Unboxed Floats (P0) — Design

**Date:** 2026-06-17
**Status:** Approved (design); staged implementation.
**Roadmap item:** P0 — Unboxed / tagged floats (`docs/archive/ROADMAP.md`).
**Related:** GitHub issue #3 (GC never triggered at runtime).

## Problem

Every float operation heap-allocates a `struct Float` (`src/oevm.h:147`) via
`gc_allocate` (`src/gc.c:49`), which does a **full `malloc` + linked-list insert
per call**. Allocation sites:

- `src/vm.c:392` — `OP_DIV`, inexact SMI/SMI result.
- `src/vm.c:417` — `OP_ADD/SUB/MUL/DIV`, any float operand.
- `src/dispatch.c:569` — `PRIM_asFloat`.
- `src/dispatch.c:580` — `PRIM_sqrt`.

nbody and mandelbrot do millions of these. Per the steady-state comparison
(`../ns-tests/00_perf/RESULTS.md`), nbody is **2.64×** and mandelbrot **2.24×**
slower than PrimordialSoup — the float allocation is the dominant term and the
bulk of the ~1.9× geomean gap.

## Constraints (what bounds the design)

1. **32-bit gate.** `tests/test_portability_32.sh` builds with `-m32`; `oop` is
   pointer-sized. A full IEEE double cannot live in a tagged immediate on a
   32-bit pointer, and tag bits steal precision a double needs even on 64-bit.
   → NaN-boxing / immediate-double schemes are **rejected**: they require a
   64-bit `Value` type everywhere, breaking the pointer-sized `oop` model, the
   `-m32` gate, and the `.oimg` oop serializer.
2. **`.oimg` serializes `oop`s.** Any change to *what an `oop` is* ripples into
   the image writer and the portability test. Both phases below keep `oop`
   pointer-sized and `struct Float` layout-compatible, so the image format is
   untouched.
3. **GC never runs at runtime** (issue #3). `gc_collect` is only called from C
   unit tests. So nothing is reclaimed today regardless — the memory profile is
   "grow until exit." This means a *GC-swept* free-list would never be refilled
   and is pointless here. It also means Phase 1 does not regress the memory
   profile: it is already unbounded.

## Approach — staged

### Phase 1 — Float bump arena (Opt #5)

Replace the four `gc_allocate(FloatVtable, sizeof(struct Float))` sites with
`float_alloc()`: a slab allocator that bump-pointers `struct Float` cells.

- Slabs are large `malloc`'d blocks (e.g. 4096 `Float` cells each); a cursor is
  bumped per allocation; a full slab chains a new one.
- Cells keep the normal `struct Object` vtable header, so dispatch, comparisons
  (`PRIM_lt/gt/le/ge/eq`), printing, and `asString` are **unchanged** — an
  arena `Float` is indistinguishable from a `gc_allocate`d one to every consumer.
- **Not GC-managed.** Same "never reclaimed" semantics as today; a
  `// TODO(#3): arena is unreclaimed pending GC trigger` comment ties it to the
  issue. This is the honest scope: Phase 1 removes `malloc` cost, not the leak.

**GC-test interaction:** arena floats are not on `heap_list`, so the unit-test
`gc_collect` will not sweep them (correct — they are not on the malloc list to
free). `mark()` already treats a `FloatVtable` object as a leaf (`gc.c:113`) and
`is_valid_heap_object` returns 0 for non-`heap_list` pointers, so a marked-through
arena float is simply skipped. Verify the existing unit tests still pass; if any
test asserts a float is *collected*, adjust the test, not the arena.

**Risk:** low. No `oop` representation change, no 32-bit impact, no `.oimg`
change, no dispatch change.

### Phase 2 — Unboxed temporaries (Opt #6)

The compiler-level elimination of intermediate boxing — the principled version
of "flush floats on method return." The ns compiler
(`../ns-oevm/tools/ns_compile_oevm.py`) proves which float results are pure
intermediates (consumed by a following arithmetic op, never stored to an object
slot, returned, or captured) and keeps them in raw C-double registers; boxing
happens only at escape boundaries.

This requires new raw-double opcodes (e.g. `OP_FADD/FSUB/FMUL/FDIV` operating on
a float-register file, plus `OP_FBOX`/`OP_FUNBOX` at boundaries) and compiler
dataflow/escape tracking. **Scoped separately** after Phase 1 lands and is
measured — its own spec + plan. Phase 1's arena makes the *escaping* floats
cheap; Phase 2 removes the *non-escaping* ones entirely.

## Measurement protocol (run after EACH phase)

- Build: Model A, `-O2 -DOEVM_APPLIANCE` (what `bench.sh` uses).
- Workloads (self-verifying, real float work):
  - nbody at `innerIterations = 250000` (energy check at `05_nbody.ns:27`).
  - mandelbrot at `size = 750` (checksum at `04_mandelbrot.ns:24`).
  - Regression sanity: fib(34), richards (non-float, must not regress).
- Best-of-5 wall time (`bench.sh` convention).
- **Record** before/after in `docs/optimizations.md` as Opt #5 (arena) and Opt
  #6 (unboxing): timings, ratio to Soup, and an honest read of the gain.
- **Gate:** conformance must hold **36 pass / 5 fail / 1 skip** on Model A *and*
  Model B, plus the `-m32` portability test, after each phase.

## Out of scope

- Fixing issue #3 (GC trigger / O(n²) mark). Tracked separately; Phase 1
  explicitly does not address it.
- NaN-boxing / tagged immediate floats (rejected — see Constraints).
