# GH#4 — Iterative Mark (worklist) Design

**Issue:** GH#4 — *GC mark/trace_object recurse with no depth bound — convert to iterative worklist before Zephyr bring-up.*

**Status:** design — supersedes the W1 follow-up note in `docs/optimizations.md` (W1 row) and `docs/superpowers/notes/2026-06-18-w1-rooting-audit.md`.

## Problem

The GC mark phase in `src/gc.c` traverses the live object graph by **C recursion**: `mark()` → (per-vtable body) → `mark()` / `trace_object()` → `mark()`, one stack frame per level of graph depth, with **no depth bound**. Benign on the host (large stack), but since W1 (GH#3) `gc_collect` now runs **at runtime under memory pressure**, so a deep reachable chain can drive arbitrarily deep recursion mid-collection. On Zephyr threads run on small fixed stacks (a few KB) — unbounded mark recursion is a stack-overflow risk that fires precisely when the heap is most pressured. This is a **pre-Zephyr gate**, not a host blocker.

## Goal

Convert the mark phase to an **iterative mark** driven by an explicit fixed-size worklist (mark stack), so C-stack usage during a collection is **O(1) in graph depth**. Keep the existing linear-walk sweep, the float-arena sweep, all rooting (precise globals + `scan_vm_stack` + conservative `scan_c_stack`), and Model A/B parity unchanged.

## Non-goals

- No change to `gc_collect`'s root set, sweep, float-arena mechanics, the allocator, or anything the compiler emits.
- Not pursuing pointer-reversal (Deutsch–Schorr–Waite): zero extra memory but irregular and error-prone here (`trace_object` scans payload word-by-word; the Vtable/Fiber cases traverse non-uniformly). Rejected for risk.
- Not sizing the stack to the arena's max object count (~128 KB on a 256 KB arena — half the arena). Rejected for waste on embedded.

## Architecture

Today `mark()` does two jobs in one recursive function: **(a) classify a pointer + set its mark bit**, and **(b) trace a freshly-marked object's children**. The conversion separates them: (a) stays in `mark()` and becomes *push-only*; (b) moves into a new `trace_one()` driven by an explicit worklist.

### Worklist state (file-static in `src/gc.c`)

```c
#ifndef OEVM_MARK_STACK_SIZE
#define OEVM_MARK_STACK_SIZE 1024   /* entries; -D lower for embedded. 8 KB/64-bit, 4 KB/32-bit */
#endif
static oop    mark_stack[OEVM_MARK_STACK_SIZE];
static size_t mark_sp;            /* next free slot; 0 = empty */
static int    mark_overflowed;    /* a push was dropped this cycle → recovery rescan needed */
```

### `mark(ptr)` — classify + set bit + push (no recursion)

Identical prologue to today: return on `IS_SMI`, `NULL`, `is_in_blob`, `is_read_only`; `is_in_float_arena` → `float_mark` and return (leaf, never pushed); `!is_valid_heap_object` → return; `header->marked` → return (already visited). Then, where today it would recurse:

```c
    header->marked = 1;
    if (mark_sp < OEVM_MARK_STACK_SIZE) {
        mark_stack[mark_sp++] = ptr;     /* enqueue for tracing */
    } else {
        mark_overflowed = 1;             /* dropped: object IS marked (won't be swept);
                                            its children are recovered by the rescan pass */
    }
```

The mark bit is set **before** the push-or-overflow branch, so an overflowed object is never swept and is never re-pushed redundantly — only its outgoing edges are deferred to recovery.

### `trace_one(ptr)` — trace a marked object's children

The current per-vtable dispatch body of `mark()` (the `if (ptr->vtable == VtableVtable) … else if (Fiber…) … else trace_object(…)` block) moves verbatim into `trace_one(oop ptr)`. Every child reference is reached via `mark()` (a push), never a recursive descent. The Fiber case keeps its existing `while (frame != NULL)` frame-walk (already iterative). `trace_object()` is unchanged; it calls `mark()` per field as today.

`trace_one` assumes its argument is an already-marked heap object (it only ever runs on popped/relisted entries), so it does **not** repeat the classify prologue.

### `drain_mark_stack()` — pop until empty

```c
static void drain_mark_stack(void) {
    while (mark_sp > 0) {
        oop ptr = mark_stack[--mark_sp];
        trace_one(ptr);            /* pushes children via mark() */
    }
}
```

### `mark_drain_with_recovery()` — drain + overflow rescan

```c
static void mark_drain_with_recovery(void) {
    drain_mark_stack();
    while (mark_overflowed) {
        mark_overflowed = 0;
        /* Linear walk of the arena (same idiom as the mark-reset and sweep loops):
           re-trace every marked object so its still-unmarked children get pushed.
           Children already marked → mark() early-returns (no push, no work). */
        uint8_t *cur = heap_base;
        while (cur < heap_top) {
            struct HeapHeader *h = (struct HeapHeader *)cur;
            if (h->marked) trace_one((oop)(h + 1));
            cur += sizeof(struct HeapHeader) + h->size;
        }
        drain_mark_stack();        /* may set mark_overflowed again */
    }
}
```

**Termination:** each recovery round pushes at least one previously-unmarked object (else no `trace_one` call would have overflowed), so the marked set grows monotonically; bounded by the total object count. Under no overflow the `while` body never runs — one `drain_mark_stack()`, zero rescans.

### `gc_collect()` integration (minimal change)

`mark_sp` and `mark_overflowed` are reset to 0 at the top of `gc_collect` (alongside the existing mark-bit reset walk). The entire root-marking section is **unchanged** — every `mark(root)` now enqueues instead of recursing. A single `mark_drain_with_recovery()` call is inserted **after `scan_c_stack()` and before the sweep** (current step 4). `mark_object()` (exported custom-root entry) and the `gc_additional_roots_hook` path are unchanged — they enqueue, and the post-roots drain traces them.

Roots may overflow the stack *during* enqueue (e.g. a large conservative `scan_c_stack`); that is safe — the flag is set and recovery handles it. Normal workloads never overflow at a 1024 default.

## Components & boundaries

| Unit | Purpose | Depends on |
|------|---------|-----------|
| `mark(oop)` | classify a pointer, set its bit, enqueue (or flag overflow) | `mark_stack`, classification predicates (`is_in_blob`/`is_read_only`/`is_in_float_arena`/`is_valid_heap_object`), `float_mark` |
| `trace_one(oop)` | push the children of one already-marked object | `mark`, `trace_object`, Vtable/Fiber layout |
| `drain_mark_stack()` | pop-and-trace until the worklist empties | `mark_stack`, `trace_one` |
| `mark_drain_with_recovery()` | drain, then rescan-the-heap recovery while overflow flagged | `drain_mark_stack`, `trace_one`, the `[heap_base,heap_top)` walk |

`trace_one` can be understood and changed without touching `mark`'s classification, and vice versa; the worklist is the only shared state.

## Testing

New cases in a unit test compiled directly with `gcc` (host arena), following the existing `tests/test_gc_*.c` pattern (`vm_c_stack_base = NULL` to keep dropped objects deterministically unreachable where needed):

1. **Deep-chain survival.** Build an N-deep reachable linked list (N ≫ any comfortable old recursion depth, e.g. 100k), rooted from a scanned location; `gc_collect()`; assert every node still `gc_is_live`. Proves correctness with no graph-depth recursion (this test would blow the C stack under the old recursive mark on a small `-D` stack ulimit).
2. **Overflow-path recovery** (built `-DOEVM_MARK_STACK_SIZE=4`). A reachable graph with ≫4 objects plus some unreachable objects; `gc_collect()`; assert **all reachable survive** (recovery rescan re-found the dropped children) **and unreachable are swept** (not falsely retained). This is the issue's required mark-stack-overflow unit test.
3. **Regression gates:** `05_gc_stress.ns` still bounded under the 64 KB managed-arena cap on Path A+B; full conformance **47 / 0 / 1** on Model A + B; `-m32` portability (`tests/test_portability_32.sh`). The change adds only a static `oop` array + `size_t`/`int` and a pointer-walk already exercised by the sweep, so it is pointer-width-neutral.

## Acceptance criteria

1. No C recursion proportional to object-graph depth in the mark phase — `mark`→push, `drain` iterative, `trace_one`→`mark` is push-and-return (verified by the deep-chain test surviving under a small stack ulimit).
2. Mark-stack overflow path exercised by a passing unit test (case 2).
3. `05_gc_stress.ns` bounded under the 64 KB cap; conformance 47/0/1 on Model A + B; `-m32` passes.
4. `OEVM_MARK_STACK_SIZE` is `-D`-tunable; overflow degrades to extra rescans, never to incorrect sweeping.

## Risks

- **Recovery cost under sustained overflow:** O(objects) per overflow round. Acceptable graceful degradation; the default 1024 makes overflow unreachable on normal graphs, and embedded builds tune the stack to their arena.
- **`trace_one` correctness on relisted objects:** it must run only on already-marked heap objects (guaranteed: pushed only after `header->marked = 1`; recovery only calls it on `h->marked` objects). It must not repeat the classify prologue — by construction it doesn't.
- **Generated `src/solidified_image.c`:** `git restore` before any commit; never commit it.

## Files

- `src/gc.c` — add worklist state + `OEVM_MARK_STACK_SIZE`; convert `mark` to push-only; extract `trace_one`; add `drain_mark_stack` + `mark_drain_with_recovery`; reset worklist state and insert the single drain call in `gc_collect`.
- `tests/test_gc_mark_iterative.c` (new) — deep-chain + overflow-recovery cases.

## Commit trailer

`Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`
