# GH#4 — Iterative Mark Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Convert the GC mark phase in `src/gc.c` from C recursion to an explicit fixed-size worklist (mark stack) with an overflow→heap-rescan recovery pass, so mark-phase C-stack usage is O(1) in object-graph depth (the pre-Zephyr stack-overflow gate, GH#4).

**Architecture:** Split today's recursive `mark()` into (a) a push-only `mark()` that classifies a pointer, sets its mark bit, and enqueues it, and (b) a `trace_one()` (the old per-vtable dispatch body) driven by an iterative `drain_mark_stack()`. `gc_collect` enqueues all roots exactly as today, then calls `mark_drain_with_recovery()` (drain, then rescan the arena re-tracing marked objects while an overflow flag is set) once before the sweep. The sweep, float-arena sweep, and all rooting are unchanged.

**Tech Stack:** C99, single-file change in `src/gc.c`; unit tests built directly with `gcc` (host arena); conformance via `ns-oevm/tools/run_tests_oevm.py`; `-m32` via `tests/test_portability_32.sh`. Task 1's red→green test uses POSIX threads (`-lpthread`) to model Zephyr's small fixed stacks.

## Global Constraints

- All implementation changes confined to the mark section of `src/gc.c` (~lines 351–445 + the `gc_collect` body ~520–605) plus the new `tests/test_gc_mark_iterative.c`. **No change** to `gc_collect`'s root set, the sweep, `float_sweep`, the allocator, `struct HeapHeader`, `oop`, or anything the compiler emits.
- Worklist macro: `OEVM_MARK_STACK_SIZE`, default `1024` entries, overridable via `-D`.
- The mark bit is set **before** the push-or-overflow branch (an overflowed object is marked — never swept — only its outgoing edges are deferred to recovery).
- `trace_one` runs **only** on already-marked heap objects (pushed only after `header->marked = 1`; recovery calls it only on `h->marked` objects) and does **not** repeat the classify prologue.
- Recovery reuses the existing `[heap_base, heap_top)` linear walk idiom (`cur += sizeof(struct HeapHeader) + h->size`).
- Overflow degrades to extra O(objects) rescans, **never** to incorrect sweeping.
- Unit tests build **without** `-O` (default `-O0`) so the old recursive mark is not tail-call-optimized into shallow depth.
- `src/solidified_image.c` is a generated artifact — `git restore src/solidified_image.c` before any commit; never commit it.
- Conformance must stay **47 pass / 0 fail / 1 skip** on Model A + Model B + `-m32`.
- Commit trailer: `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`

---

## File Structure

- `src/gc.c` — mark section: add `OEVM_MARK_STACK_SIZE` + worklist state; convert `mark` to push-only; extract `trace_one`; add `drain_mark_stack` + `mark_drain_with_recovery`; reset worklist state and insert one drain call in `gc_collect`.
- `tests/test_gc_mark_iterative.c` (new) — Task 1 adds the deep-chain-on-small-stack red→green test; Task 2 adds the overflow-recovery case.

### Interfaces produced

```c
/* file-static in src/gc.c — not exported */
static void mark(oop ptr);                 /* existing decl; body becomes push-only */
static void trace_one(oop ptr);            /* NEW: push the children of one marked object */
static void drain_mark_stack(void);        /* NEW: pop-and-trace until empty */
static void mark_drain_with_recovery(void);/* NEW: drain, then overflow rescan loop */
```

No new exported symbols. `mark_object(oop)` (the existing exported custom-root entry) and `gc_is_live(oop)` are unchanged and reused by the tests.

---

## Task 1: Iterative-mark conversion + deep-chain-on-small-stack test

The conversion is one cohesive change to `src/gc.c` (extracting `trace_one` without the drain would not build), proven by a test that crashes under the old recursive mark on a small thread stack and passes under the new iterative mark.

**Files:**
- Modify: `src/gc.c` (mark section ~351–445; `gc_collect` ~520–605)
- Create: `tests/test_gc_mark_iterative.c`

**Interfaces:**
- Consumes (already in `src/gc.c`): `is_in_blob`, `is_read_only`, `is_in_float_arena`, `float_mark`, `is_valid_heap_object`, `struct HeapHeader`, `heap_base`/`heap_top`, the `VtableVtable`/`Fiber*Vtable` externs, `trace_object`, `current_fiber`, `gc_allocate`, `gc_collect`, `gc_is_live`, `mark_object`, `gc_additional_roots_hook`, `vm_c_stack_base`, `struct Object`/`struct ObjectInstance`. (The worklist `mark_stack`/`mark_sp`/`mark_overflowed` are introduced within this task — Step 3.)
- Produces: `trace_one`, `drain_mark_stack`, `mark_drain_with_recovery`, `OEVM_MARK_STACK_SIZE`; a push-only `mark`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_gc_mark_iterative.c`:

```c
/* GH#4: the mark phase must NOT recurse proportionally to object-graph depth.
   This test builds a deep reachable chain, then runs gc_collect() on a pthread
   with a small (256 KB) stack — modeling Zephyr's small fixed thread stacks.
   Under the OLD recursive mark, marking the chain recurses ~DEPTH frames deep
   and overflows the 256 KB stack → the process crashes (SIGSEGV). Under the
   iterative mark, C-stack use is O(1) and the whole chain survives.

   Build (NO -O so the old recursion is not tail-call-optimized away):
     gcc -std=c99 -Isrc -DOEVM_HEAP_SIZE=33554432 tests/test_gc_mark_iterative.c \
         src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/solidified_image.c \
         -lm -lpthread -o /tmp/tgmi && /tmp/tgmi */
#include "../src/oevm.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <pthread.h>

extern oop  gc_allocate(oop vtable, size_t size);
extern void gc_collect(void);
extern void heap_init(void);
extern int  gc_is_live(oop);
extern void mark_object(oop);
extern void (*gc_additional_roots_hook)(void);
extern void *vm_c_stack_base;

/* Plain instance vtable: the GC traces the payload word-by-word; the vtable
   word points at this static (not a heap object) so mark() skips it, and each
   node's slots[0] is the only traced child. */
static struct Vtable PlainVtable;

#define DEPTH 500000           /* 500k * 32 B/node = 16 MB < 32 MB arena */

static oop chain_head;         /* newest node; reaches the whole chain */
static oop chain_tail;         /* first/deepest node; survives only via the chain */

/* Precise root: marks just the chain head (transitively reaches all DEPTH nodes). */
static void root_chain(void) { mark_object(chain_head); }

static volatile int head_live;
static volatile int tail_live;

/* Runs on a 256 KB pthread stack. Old recursive mark overflows it here. */
static void *collect_on_small_stack(void *arg) {
    (void)arg;
    gc_collect();
    head_live = gc_is_live(chain_head);
    tail_live = gc_is_live(chain_tail);
    return NULL;
}

static void test_deep_chain_no_recursion(void) {
    heap_init();
    vm_c_stack_base = NULL;              /* precise roots only; deterministic */
    chain_head = NULL;
    for (size_t i = 0; i < DEPTH; i++) {
        oop n = gc_allocate((oop)&PlainVtable, sizeof(struct Object) + sizeof(oop));
        ((struct ObjectInstance *)n)->slots[0] = chain_head;
        chain_head = n;
        if (i == 0) chain_tail = n;      /* deepest node */
    }
    gc_additional_roots_hook = root_chain;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    pthread_t th;
    int rc = pthread_create(&th, &attr, collect_on_small_stack, NULL);
    assert(rc == 0);
    pthread_join(th, NULL);
    pthread_attr_destroy(&attr);

    gc_additional_roots_hook = NULL;
    assert(head_live);                   /* whole chain reachable from the root */
    assert(tail_live);                   /* deepest node traced too */
    printf("test_gc_mark_iterative: deep chain (%d) survived on 256 KB stack OK\n", DEPTH);
}

int main(void) {
    test_deep_chain_no_recursion();
    printf("ALL TESTS PASSED\n");
    return 0;
}
```

- [ ] **Step 2: Run the test against the current (recursive) code to verify it crashes**

Run:
```bash
cd /home/david/workspace/oevm
git restore src/solidified_image.c 2>/dev/null
gcc -std=c99 -Isrc -DOEVM_HEAP_SIZE=33554432 tests/test_gc_mark_iterative.c \
    src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/solidified_image.c \
    -lm -lpthread -o /tmp/tgmi && /tmp/tgmi; echo "exit=$?"
```
Expected: the program **crashes** — no `ALL TESTS PASSED`; the shell reports a segfault and `exit=139` (128+SIGSEGV) or similar non-zero. (The recursive mark overflows the 256 KB thread stack tracing the 500k-deep chain.)

- [ ] **Step 3: Add the worklist state + macro in `src/gc.c`**

Just **above** the `static void mark(oop ptr);` forward declaration (~line 351), insert:

```c
/* GH#4: explicit mark worklist replaces recursive marking so the mark phase
   uses O(1) C stack regardless of object-graph depth (Zephyr small stacks). */
#ifndef OEVM_MARK_STACK_SIZE
#define OEVM_MARK_STACK_SIZE 1024   /* entries; -D lower for embedded. ~8 KB/64-bit */
#endif
static oop    mark_stack[OEVM_MARK_STACK_SIZE];
static size_t mark_sp;            /* next free slot; 0 = empty */
static int    mark_overflowed;    /* a push was dropped this cycle → recovery rescan */

static void trace_one(oop ptr);   /* forward: traces one already-marked object */
```

- [ ] **Step 4: Convert `mark` to push-only**

Replace the **entire** current `mark` function (the prologue through the per-vtable dispatch block, ~lines 364–445) with:

```c
/* Classify a pointer, set its mark bit, and enqueue it for tracing. No
   recursion: children are reached when this object is later popped and traced. */
static void mark(oop ptr) {
    if (IS_SMI(ptr)) return;
    if (ptr == NULL) return;
    if (is_in_blob(ptr)) return; /* blob objects are immutable ROM: never marked, never traced */
    if (is_read_only(ptr)) return;

    if (is_in_float_arena(ptr)) {
        float_mark(ptr);        /* leaf object: set bit, no tracing, never enqueued */
        return;
    }

    if (!is_valid_heap_object(ptr)) {
        return; // Skip non-heap pointers (e.g. raw pointers or unaligned values)
    }

    struct HeapHeader *header = ((struct HeapHeader *)ptr) - 1;
    if (header->marked) {
        return; // Already visited
    }

    header->marked = 1;          /* set BEFORE the push: an overflowed object is
                                    still marked (never swept); only its edges defer */
    if (mark_sp < OEVM_MARK_STACK_SIZE) {
        mark_stack[mark_sp++] = ptr;
    } else {
        mark_overflowed = 1;     /* recovered by the heap-rescan pass */
    }
}
```

- [ ] **Step 5: Add `trace_one` (the extracted per-vtable dispatch) + `drain_mark_stack` + `mark_drain_with_recovery`**

Immediately **after** the new `mark` function, insert:

```c
/* Trace one already-marked heap object: enqueue each of its child references.
   This is the per-vtable dispatch that used to live inside the recursive mark;
   every child is reached via mark() (an enqueue), never a recursive descent. */
static void trace_one(oop ptr) {
    extern oop VtableVtable;
    extern oop FloatVtable;
    extern oop FiberVtable;
    extern oop RunnableFiberVtable;
    extern oop SuspendedFiberVtable;
    extern oop DeadFiberVtable;

    if (ptr->vtable == VtableVtable) {
        struct Vtable *vt = (struct Vtable *)ptr;
        mark(vt->delegate);
        if (vt->bindings != NULL) {
            for (size_t i = 0; i < vt->size; i++) {
                mark(vt->bindings[i].symbol);
                mark(vt->bindings[i].method);
            }
        }
    } else if (FloatVtable != NULL && ptr->vtable == FloatVtable) {
        mark(ptr->vtable);
    } else if ((FiberVtable != NULL && ptr->vtable == FiberVtable) ||
               (RunnableFiberVtable != NULL && ptr->vtable == RunnableFiberVtable) ||
               (SuspendedFiberVtable != NULL && ptr->vtable == SuspendedFiberVtable) ||
               (DeadFiberVtable != NULL && ptr->vtable == DeadFiberVtable)) {
        mark(ptr->vtable);
        struct Fiber *fib = (struct Fiber *)ptr;
        mark(fib->status);

        // Scan the active MethodContexts on this fiber's private stack.
        // Skip the active current_fiber (its stack is scanned by scan_vm_stack()).
        if (ptr != current_fiber) {
            struct MethodContext *frame = fib->current_context;
            struct MethodContext *prev_higher_frame = NULL;
            while (frame != NULL) {
                mark(frame->vtable);
                mark(frame->prev_frame);
                mark(frame->home_ctx);
                mark(frame->cleanup_cl);

                size_t reg_count = 0;
                if (prev_higher_frame != NULL) {
                    reg_count = (oop *)prev_higher_frame - frame->regs;
                } else {
                    reg_count = &fib->stack[fib->stack_ptr] - frame->regs;
                }

                for (size_t i = 0; i < reg_count; i++) {
                    mark(frame->regs[i]);
                }

                prev_higher_frame = frame;
                frame = (struct MethodContext *)frame->prev_frame;
            }
        }
    } else {
        // Trace the object's fields word-by-word (enqueues each pointer).
        struct HeapHeader *header = ((struct HeapHeader *)ptr) - 1;
        trace_object(ptr, header->size);
    }
}

/* Pop-and-trace until the worklist drains. */
static void drain_mark_stack(void) {
    while (mark_sp > 0) {
        oop ptr = mark_stack[--mark_sp];
        trace_one(ptr);            /* may enqueue more via mark() */
    }
}

/* Drain the worklist, then — if any push was dropped — re-trace every marked
   object via a linear arena walk so dropped children get re-enqueued; repeat
   until a full pass overflows nothing. Each round marks >=1 new object, so it
   terminates (bounded by the object count). No overflow → one drain, no rescan. */
static void mark_drain_with_recovery(void) {
    drain_mark_stack();
    while (mark_overflowed) {
        mark_overflowed = 0;
        uint8_t *cur = heap_base;
        while (cur < heap_top) {
            struct HeapHeader *h = (struct HeapHeader *)cur;
            if (h->marked) trace_one((oop)(h + 1));  /* re-enqueue unmarked children */
            cur += sizeof(struct HeapHeader) + h->size;
        }
        drain_mark_stack();        /* may set mark_overflowed again */
    }
}
```

(Note: `trace_object` itself is unchanged — it still loops `mark(fields[i])`. The forward declaration `static void mark(oop ptr);` already present above `trace_object` remains valid.)

- [ ] **Step 6: Reset worklist state + insert the single drain in `gc_collect`**

In `gc_collect`, in the mark-reset block (~lines 530–537), reset the worklist state alongside the mark-bit reset. Change:

```c
    {
        uint8_t *cur = heap_base;
        while (cur < heap_top) {
            struct HeapHeader *h = (struct HeapHeader *)cur;
            h->marked = 0;
            cur += sizeof(struct HeapHeader) + h->size;
        }
    }
```
to:
```c
    mark_sp = 0;
    mark_overflowed = 0;
    {
        uint8_t *cur = heap_base;
        while (cur < heap_top) {
            struct HeapHeader *h = (struct HeapHeader *)cur;
            h->marked = 0;
            cur += sizeof(struct HeapHeader) + h->size;
        }
    }
```

Then, immediately **after** the conservative `scan_c_stack();` call (~line 605) and **before** the sweep comment `// 4. Sweep phase` (~line 607), insert:

```c
    /* GH#4: all roots are now enqueued — trace the live graph iteratively
       (with overflow recovery) instead of via the old recursive mark. */
    mark_drain_with_recovery();
```

- [ ] **Step 7: Run the test to verify it now passes**

Run:
```bash
cd /home/david/workspace/oevm
git restore src/solidified_image.c 2>/dev/null
gcc -std=c99 -Isrc -DOEVM_HEAP_SIZE=33554432 tests/test_gc_mark_iterative.c \
    src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/solidified_image.c \
    -lm -lpthread -o /tmp/tgmi && /tmp/tgmi; echo "exit=$?"
```
Expected:
```
test_gc_mark_iterative: deep chain (500000) survived on 256 KB stack OK
ALL TESTS PASSED
exit=0
```

- [ ] **Step 8: Verify existing GC unit tests still pass (no regression)**

Run:
```bash
cd /home/david/workspace/oevm
gcc -std=c99 -Isrc -DOEVM_HEAP_SIZE=65536 tests/test_gc_trigger.c \
    src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/solidified_image.c \
    -lm -o /tmp/tgct && /tmp/tgct | tail -1
gcc -std=c99 -Isrc tests/test_float_gc.c \
    src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/solidified_image.c \
    -lm -o /tmp/tfgc && /tmp/tfgc | tail -1
```
Expected: `ALL TESTS PASSED` for both (the float GC suite's last line is `ALL TESTS PASSED`).

- [ ] **Step 9: Commit**

```bash
cd /home/david/workspace/oevm
git restore src/solidified_image.c 2>/dev/null
git add src/gc.c tests/test_gc_mark_iterative.c
git commit -m "feat(gc): iterative mark via explicit worklist + overflow recovery (GH#4)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: Overflow-recovery test + regression gates + docs

Proves the overflow path (the issue's required unit test) and runs the conformance/portability gates, then records completion and closes the issue.

**Files:**
- Modify: `tests/test_gc_mark_iterative.c` (add the overflow-recovery case)
- Modify: `nodus/oevm/docs/optimizations.md` (W1 row GH#4 note), `nodus/oevm/docs/archive/ROADMAP.md` (GP0 step 5)

**Interfaces:**
- Consumes: the iterative mark + `OEVM_MARK_STACK_SIZE` from Task 1; `gc_allocate`, `gc_collect`, `gc_is_live`, `mark_object`, `gc_additional_roots_hook`, `vm_c_stack_base`.

- [ ] **Step 1: Add the overflow-recovery test case**

In `tests/test_gc_mark_iterative.c`, add this function above `main` and call it in `main` before `printf("ALL TESTS PASSED\n")`:

```c
/* GH#4: when the worklist overflows, the heap-rescan recovery must still mark
   every reachable object (no false sweep) AND unreachable objects must still be
   reclaimed. Built with -DOEVM_MARK_STACK_SIZE=4 so a >4-object reachable graph
   forces overflow. */
#define REACH 64
static oop reach_nodes[REACH];     /* roots of a reachable graph (chain) */
static oop dead_node;              /* allocated, never rooted */

static void root_reach(void) { mark_object(reach_nodes[0]); }  /* head reaches all REACH */

static void test_overflow_recovery(void) {
    heap_init();
    vm_c_stack_base = NULL;
    /* Build a REACH-long reachable chain: reach_nodes[i].slots[0] = reach_nodes[i+1]. */
    for (size_t i = 0; i < REACH; i++) {
        reach_nodes[i] = gc_allocate((oop)&PlainVtable, sizeof(struct Object) + sizeof(oop));
    }
    for (size_t i = 0; i + 1 < REACH; i++) {
        ((struct ObjectInstance *)reach_nodes[i])->slots[0] = reach_nodes[i + 1];
    }
    ((struct ObjectInstance *)reach_nodes[REACH - 1])->slots[0] = NULL;
    /* An unreachable object (rooted by nothing). */
    dead_node = gc_allocate((oop)&PlainVtable, sizeof(struct Object) + sizeof(oop));
    ((struct ObjectInstance *)dead_node)->slots[0] = NULL;

    gc_additional_roots_hook = root_reach;
    gc_collect();                  /* worklist (size 4) overflows; recovery rescans */
    gc_additional_roots_hook = NULL;

    for (size_t i = 0; i < REACH; i++) {
        assert(gc_is_live(reach_nodes[i]));   /* recovery re-found every reachable node */
    }
    assert(!gc_is_live(dead_node));           /* unreachable still swept (not falsely kept) */
    printf("test_gc_mark_iterative: overflow recovery kept %d reachable, swept dead OK\n", REACH);
}
```

Add `test_overflow_recovery();` in `main` before the final `printf`.

- [ ] **Step 2: Build with a tiny mark stack and verify the overflow path passes**

Run:
```bash
cd /home/david/workspace/oevm
git restore src/solidified_image.c 2>/dev/null
gcc -std=c99 -Isrc -DOEVM_HEAP_SIZE=33554432 -DOEVM_MARK_STACK_SIZE=4 \
    tests/test_gc_mark_iterative.c \
    src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/solidified_image.c \
    -lm -lpthread -o /tmp/tgmi4 && /tmp/tgmi4; echo "exit=$?"
```
Expected (the deep-chain test also passes — recovery just rescans more):
```
test_gc_mark_iterative: deep chain (500000) survived on 256 KB stack OK
test_gc_mark_iterative: overflow recovery kept 64 reachable, swept dead OK
ALL TESTS PASSED
exit=0
```

- [ ] **Step 3: Verify both cases also pass at the default stack size**

Run:
```bash
cd /home/david/workspace/oevm
gcc -std=c99 -Isrc -DOEVM_HEAP_SIZE=33554432 tests/test_gc_mark_iterative.c \
    src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/solidified_image.c \
    -lm -lpthread -o /tmp/tgmi && /tmp/tgmi | tail -1
```
Expected: `ALL TESTS PASSED` (at the 1024 default the overflow branch is simply not taken; both cases still pass).

- [ ] **Step 4: Run the `-m32` portability gate**

Run:
```bash
cd /home/david/workspace/oevm
bash tests/test_portability_32.sh 2>&1 | tail -3
```
Expected: `32-bit portability OK` (the change adds only a static `oop` array + `size_t`/`int` and a pointer-walk already exercised by the sweep — pointer-width-neutral). If 32-bit multilib is not installed, record that as an environment limitation; `test_portability_32.sh` is the authoritative gate.

- [ ] **Step 5: Run `05_gc_stress.ns` bounded under the 64 KB cap**

Run (the W1 acceptance workload — must still complete under the small managed-arena cap with the iterative mark):
```bash
cd /home/david/workspace/ns-oevm
OEVM_EXTRA_CFLAGS="-DOEVM_HEAP_SIZE=65536" OEVM_PATH=AB \
    python3 tools/run_tests_oevm.py 05_gc_stress 2>&1 | grep -E "gc_stress|Passed|Failed" | head
```
Expected: `05_gc_stress` passes on Path A+B (runs to completion, bounded — no OOM abort, no crash). If the harness has no single-test filter, run the full suite (Step 6) at the 64 KB cap instead and confirm `05_gc_stress` is among the passes.

- [ ] **Step 6: Run full conformance on Model A + B**

Run:
```bash
cd /home/david/workspace/ns-oevm
OEVM_PATH=AB python3 tools/run_tests_oevm.py 2>&1 | grep -E "Passed|Failed|Skipped"
```
Expected: `Passed: 47`, `Failed: 0`, `Skipped: 1`.

- [ ] **Step 7: Update `nodus/oevm/docs/optimizations.md`**

In the weakness table, update the **W1** row: the recursive-mark follow-up it flagged is now resolved — note that the mark phase is iterative (explicit `OEVM_MARK_STACK_SIZE` worklist in `src/gc.c` with overflow→heap-rescan recovery), so mark-phase C-stack use is O(1) in graph depth (Zephyr-safe), GH#4 closed. Remove/curtail the "Known follow-up (pre-Zephyr): convert to iterative mark" wording so the row no longer lists it as open.

- [ ] **Step 8: Update `nodus/oevm/docs/archive/ROADMAP.md`**

Mark GP0 **step 5 (Iterative mark, GH#4) DONE** with the result (deep 500k-chain survives a 256 KB thread stack; overflow-recovery unit test passes at `-DOEVM_MARK_STACK_SIZE=4`; conformance 47/0/1 A+B+m32; gc_stress bounded @64 KB). Set the next GP0 step to **W2b (float unboxing) — deferred** (sub-second speed ceiling; revisit only if profiling justifies), i.e. the remaining pre-Zephyr GC follow-ups are closed.

- [ ] **Step 9: Commit and close the issue**

```bash
cd /home/david/workspace/oevm
git restore src/solidified_image.c 2>/dev/null
git add tests/test_gc_mark_iterative.c nodus/oevm/docs/optimizations.md nodus/oevm/docs/archive/ROADMAP.md
git commit -m "test+docs(gp0): GH#4 overflow-recovery test + iterative-mark done

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
gh issue close 4 -c "Done: mark phase converted to an explicit OEVM_MARK_STACK_SIZE worklist with overflow->heap-rescan recovery (src/gc.c). Deep 500k-node chain now survives gc_collect on a 256 KB thread stack (old recursive mark crashed); overflow path covered by a unit test at -DOEVM_MARK_STACK_SIZE=4; conformance 47/0/1 on Model A+B+m32; gc_stress bounded under the 64 KB cap."
```

---

## Self-Review

**Spec coverage:**
- §Architecture (push-only `mark`, `trace_one`, `drain_mark_stack`, `mark_drain_with_recovery`, worklist state + macro) → Task 1 Steps 3–6. ✓
- §`gc_collect` integration (reset state at top; one drain after `scan_c_stack`, before sweep) → Task 1 Step 6. ✓
- §Testing 1 (deep-chain survival, no depth recursion) → Task 1 Steps 1/2/7 (red→green on a 256 KB thread stack). ✓
- §Testing 2 (overflow-path recovery at `-DOEVM_MARK_STACK_SIZE=4`) → Task 2 Steps 1–3. ✓
- §Testing 3 (gc_stress @64 KB, conformance 47/0/1, m32) → Task 2 Steps 4–6. ✓
- §Acceptance 1 (no depth-proportional recursion) → Task 1 (thread-stack crash→pass). §Acceptance 2 (overflow unit test) → Task 2 Step 1. §Acceptance 3 (gc_stress/conformance/m32) → Task 2 Steps 4–6. §Acceptance 4 (`-D` tunable; overflow never mis-sweeps) → Task 2 Steps 2/1. ✓
- §Risks (recovery cost; `trace_one` precondition; generated `solidified_image.c`) → Global Constraints + `git restore` in every commit step. ✓

**Placeholder scan:** No "TBD"/"implement later"/"add error handling"/"write tests for the above" placeholders. Every code step shows complete code; every run step shows exact commands + expected output. ✓

**Type consistency:** `mark(oop)`, `trace_one(oop)`, `drain_mark_stack(void)`, `mark_drain_with_recovery(void)` used identically across Tasks 1–2; worklist `mark_stack`/`mark_sp`/`mark_overflowed` with `OEVM_MARK_STACK_SIZE`; node access `((struct ObjectInstance *)n)->slots[0]` and allocation size `sizeof(struct Object) + sizeof(oop)` consistent in both test cases; gate `mark_sp < OEVM_MARK_STACK_SIZE`. All consistent with existing `src/gc.c` idioms (`gc_is_live`, `mark_object`, `gc_additional_roots_hook`, the `[heap_base,heap_top)` walk). ✓
