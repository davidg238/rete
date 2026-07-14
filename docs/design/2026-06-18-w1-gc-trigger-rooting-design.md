# W1 — GC Trigger + Safe-Points / Rooting — Design

**Date:** 2026-06-18
**Status:** APPROVED design (author a build plan via writing-plans next)
**Driver:** GH #3 / GP0. Zephyr OS migration (resident OEVM on nRF52840-class,
~256 KB RAM). W1 is the trigger half of the GC story; the allocator foundation
(**W4**) is already merged (oevm master `22a40ab`).
**Parent scope:** `2026-06-18-gc-embedded-memory-design.md`.

## Problem

`gc_collect()` (`src/gc.c:335`) is a complete, correct mark-and-sweep, but is
**never called from the running VM** (only `tests/test_*.c`). After W4 the
managed heap is a fixed static arena (`OEVM_HEAP_SIZE`, default 16 MB host;
`-D` override for embedded), so RSS is *already* hard-bounded — but on
exhaustion `gc_allocate` calls `oevm_oom_handler`, whose default **aborts**.

So W1's job is **not** "bound memory" (the arena does that). It is: **when the
arena fills, collect and continue instead of aborting** — and do so without
collecting an oop that is live but reachable only from a C local mid-construction.

Out of scope (sequenced elsewhere): float-arena reclamation (W2a — floats stay
malloc-unbounded), float unboxing (W2b), proactive watermark trigger (W1.2),
Zephyr thread-stack bounds (seam + TODO only).

## Decisions (settled in brainstorming)

1. **Trigger — phased.** W1.1 = exhaustion-only collect→retry→OOM. W1.2 =
   proactive byte-watermark on top (separate, smaller change; not in this spec).
2. **Rooting — conservative C-stack scan is the load-bearing correctness
   mechanism**, added as an extra root source alongside today's precise
   frame/vtable scan. A **verification audit** of alloc sites backs it up; it
   adds explicit roots *only* for genuine off-stack/off-frame live oops.
3. **Trigger lives in `gc_allocate`, not in a swapped handler.** The hook
   signature is `oop (*)(size_t)` — only `size`, no `vtable` — so a handler
   cannot re-drive allocation. `oevm_oom_handler` stays the **terminal** OOM
   policy (default `abort`; embedded may swap for panic/reset).

## Part 1 — Trigger & exhaustion path

Restructure the exhaustion path of `gc_allocate` (`src/gc.c:117`) to collect and
retry once before invoking the terminal handler. The W4 fast path (size-binned
free-list, then bump) is unchanged.

```
gc_allocate(vtable, size):
    ... W4 free-list + bump fast path ...
    if heap_top + need > heap_end and !gc_in_progress:
        gc_collect()                 // reclaim into free-list bins
        ... retry free-list + bump ...    // single retry
    if still no room:
        return oevm_oom_handler(size)     // terminal: default abort
```

- **`gc_in_progress` guard** (`static int`): set around `gc_collect`, prevents
  re-entrant collection and an infinite collect-loop if the retry still fails.
  `gc_collect` does not allocate (mark/sweep + `free()` of fiber stacks only), so
  the guard is a backstop, not a hot dependency.
- **Single retry**, not a loop: a fixed arena either has room after one full
  collection or it does not. A second collection would reclaim nothing new.
- The retry re-runs the *same* free-list + bump logic the fast path uses; factor
  it into a small internal helper if that avoids duplication, but a literal
  second attempt is acceptable.
- `oevm_oom_handler` keeps its W4 meaning and default (`abort` with the
  used/size diagnostic). No call-site contract change beyond the wrap.

## Part 2 — Conservative root augmentation

Add a conservative scan of the live C call stack as an **additional root
source** inside `gc_collect`, before/after the existing precise scans (order
irrelevant — `mark` is idempotent).

- **Stack base:** capture `void *vm_c_stack_base` once at VM entry (address of a
  local in `main` / the top-level `vm_run` entry) into a global.
- **Stack top + registers:** in `gc_collect`, `setjmp(jb)` to spill
  callee-saved registers into `jb`; take `void *top = (void*)&top;`.
- **Scan:** walk `[min(top,base) … max(top,base))` at `sizeof(void*)` stride and
  `mark_object(*word)` each slot; also scan the `jmp_buf` words. `mark_object`
  is already safe — `is_valid_heap_object` range-checks the arena, requires
  `sizeof(oop)` alignment, and confirms the **exact-start** alloc bitmap, so a
  non-pointer word can only cause retention if it *exactly* equals a live
  object's payload address.
- **Caller-saved registers** holding an oop across the call into `gc_allocate`
  are already compiler-spilled to a caller stack frame, so stack + `jmp_buf`
  cover them (standard conservative-GC reasoning).
- **False retention is transient / self-healing:** a bogus pointer-looking word
  lives in a stack slot that is overwritten when its C frame returns, so any
  falsely-retained object is freed at a later cycle. Not a leak. W1 does **not**
  widen object-*payload* tracing (already word-scanned + bitmap-gated today); the
  added conservatism is C-stack-only.
- **Portability:** host scope only (x86-64 + `-m32`). Grow-down assumption
  handled by the `min/max` span. Zephyr thread-stack extents (`k_thread`) are a
  clearly-marked `#ifdef OEVM_ZEPHYR` / TODO seam, not implemented here.

## Part 3 — Verification audit

A one-pass review of the ~36 runtime allocation sites (`src/dispatch.c` 24,
`src/vm.c` 12; the `oimg_loader.c` / `solidified_image.c` sites run at load time,
before the VM executes, so a trigger there only competes with image build-out —
include them in the review but expect no roots needed).

**Deliverable:** a short table in the plan asserting that every live oop held
across an allocation is reachable from one of {C stack, reified VM frame, global
root}. Add an explicit root — a temp-root push/pop or `gc_additional_roots_hook`
entry — **only** for a genuine off-stack/off-frame case (e.g. an oop living
solely inside a malloc'd C struct, not on the call stack and not a VM frame).
Expectation: few or none, since nearly all VM state is global, frame-reachable,
or stack-resident. The audit *verifies* this rather than assuming it.

## Part 4 — Testing & acceptance

- **Unit (`tests/test_gc_trigger.c`, new):**
  - Build with a small `-DOEVM_HEAP_SIZE` cap. Allocate in a loop while holding a
    bounded rolling root set via `gc_additional_roots_hook`; assert no abort,
    bounded `heap_top` reuse (freed blocks re-served), `gc_is_live` true for
    roots / false for dropped objects.
  - **Conservative-scan test:** hold an oop **only** in a C local, drive the heap
    to exhaustion to force a collect, assert the local's object survives
    (`gc_is_live`) and is unchanged.
  - Re-entrancy: assert `gc_collect` invoked from within does not recurse
    (guard).
- **Gate (GH #3 acceptance):** `06_limit_tests/05_gc_stress.ns` (object-heavy:
  rolling 100-entry buffer, 100k arrays allocated) runs to completion under a
  capped heap via `OEVM_EXTRA_CFLAGS=-DOEVM_HEAP_SIZE=…` with **no OOM** and
  bounded RSS. Today it grows to exhaustion → abort; W1 makes it reclaim-and-run.
- **Regression:** conformance **47 / 0 / 1 on Model A + Model B + `-m32`**
  unchanged (`OEVM_PATH=AB`). Optional AddressSanitizer pass over the unit +
  stress runs to surface a missed-root use-after-free.

## Acceptance criteria

- `05_gc_stress.ns` completes under a capped heap with bounded RSS (no abort).
- New `tests/test_gc_trigger.c` passes, including the conservative-scan survival
  case.
- Conformance 47/0/1 unchanged on A + B + `-m32`.
- A documented capped-heap figure under which `gc_stress` runs to completion
  (the start of a real nRF52840 RAM-ceiling number; refined by W2a).

## Open items deferred (not W1.1)

- **W1.2** proactive byte-watermark trigger (smoother, less fragmentation).
- **W2a** float-arena mark-bitmap + sweep (the dominant allocator; required
  before a *float*-heavy heap is bounded).
- **Zephyr** thread-stack-bounds implementation behind the `OEVM_ZEPHYR` seam.
- Carry-forward from W4 review **m1**: sweep re-runs the fiber-vtable check on a
  stale freed payload each cycle — benign, revisit if it ever bites.
