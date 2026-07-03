# W1 — GC Trigger + Safe-Points / Rooting Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `gc_collect` fire at runtime so the fixed arena reclaims and the VM continues instead of aborting on exhaustion, without collecting oops held only in C locals mid-construction.

**Architecture:** A collect-and-retry loop inside `gc_allocate` (terminal OOM stays the `oevm_oom_handler` hook). A conservative scan of the live C call stack is added as an extra root source in `gc_collect`, alongside the existing precise frame/vtable scan; it is the load-bearing safety mechanism. A verification audit of the ~36 runtime alloc sites adds explicit roots only for genuine off-stack/off-frame live oops (expected few/none).

**Tech Stack:** C99, no C11. `gcc` (host x86-64 and `-m32`). Unit tests are standalone C `main()`s compiled directly. Conformance via `ns-oevm/tools/run_tests_oevm.py` (`OEVM_PATH=AB`, `OEVM_EXTRA_CFLAGS`).

## Global Constraints

- **C99 only**, no C11 (`_Alignas`, `max_align_t`, etc. are unavailable — see `gc.c` `oevm_align_cell`).
- **The managed arena is fixed and static** (`OEVM_HEAP_SIZE`, default `16*1024*1024`; `-DOEVM_HEAP_SIZE=` override for caps/embedded). Do not reintroduce `malloc` for managed objects.
- **`oevm_oom_handler` stays the terminal OOM policy** (signature `oop (*)(size_t)`, default `abort`). Do not change its signature.
- **Float arena is out of scope** (W2a) — `float_alloc` cells stay malloc-backed and unreclaimed. The W1 gate test must be object-allocating.
- **Conservative tracing is C-stack-only.** Do not widen object-payload tracing. `mark`/`is_valid_heap_object` already gate payload words via the exact-start bitmap.
- **No moving/compaction.** Non-moving mark-sweep only (reified frames are not relocated).
- **Regression bar:** conformance **47 pass / 0 fail / 1 skip** on Model A + Model B + `-m32`, unchanged.
- **Host scope only.** Zephyr thread-stack bounds are a marked `#ifdef OEVM_ZEPHYR` / TODO seam, not implemented.

---

### Task 1: Conservative C-stack scan as a GC root source

Add a conservative scan of the live C call stack to `gc_collect`, plus the
stack-base global it needs. This is the rooting correctness mechanism, tested
in isolation by forcing a manual collection while an oop is held only in a C
local.

**Files:**
- Modify: `src/gc.c` (add globals + `scan_c_stack`, call it in `gc_collect`)
- Create: `tests/test_gc_trigger.c`

**Interfaces:**
- Consumes: `gc_allocate(oop, size_t)`, `gc_collect(void)`, `gc_is_live(oop)`, `heap_init(void)`, `mark_object(oop)` (all existing in `gc.c`).
- Produces:
  - `void *vm_c_stack_base;` — global, the older end of the C stack; set once at VM entry. `NULL` ⇒ conservative scan is skipped (precise roots still run).
  - `void gc_set_stack_base(void *p);` — sets `vm_c_stack_base = p`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_gc_trigger.c`. The first test proves an oop reachable ONLY
through a C local survives a collection (it would be swept without the C-stack
scan: `vm_stack`/`current_frame`/`current_fiber` are all empty/NULL in a unit
test, and the global vtables are NULL before kernel boot, so nothing else roots
it).

```c
/* Unit test for the W1 GC trigger + conservative C-stack rooting (src/gc.c).
   Build:
     gcc -std=c99 -Isrc -DOEVM_HEAP_SIZE=65536 tests/test_gc_trigger.c \
         src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/solidified_image.c \
         -lm -o /tmp/tgct && /tmp/tgct
   Verifies: (1) an oop held only in a C local survives gc_collect via the
   conservative C-stack scan; (2) the exhaustion path collects + retries rather
   than aborting, and reclaims a bounded rolling root set within a small cap. */
#include "../src/oevm.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

extern oop  gc_allocate(oop vtable, size_t size);
extern void gc_collect(void);
extern void heap_init(void);
extern int  gc_is_live(oop);
extern void gc_set_stack_base(void *p);
extern void (*gc_additional_roots_hook)(void);

/* A minimal vtable so allocated objects look like ordinary instances to the
   GC (payload is traced word-by-word; NULL/SMI words are ignored by mark). */
static struct Vtable PlainVtable;

/* Held only in a C local across the collection. Marked noinline so the local
   genuinely lives in this frame (not folded into main). Used after the collect
   so the compiler must preserve it across the opaque gc_collect() call. */
__attribute__((noinline))
static int local_only_survives(void) {
    oop obj = gc_allocate((oop)&PlainVtable, sizeof(struct ObjectInstance));
    gc_collect();                 /* obj is rooted ONLY by this C local */
    return gc_is_live(obj);       /* use obj after the collect */
}

int main(void) {
    int base_anchor;
    gc_set_stack_base(&base_anchor);
    heap_init();
    PlainVtable.base.vtable = NULL;   /* VtableVtable is NULL pre-boot; fine */
    PlainVtable.size = 0;
    PlainVtable.delegate = NULL;

    assert(local_only_survives() == 1);
    printf("test_gc_trigger: conservative-scan survival OK\n");

    printf("ALL TESTS PASSED\n");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run:
```bash
cd /home/david/workspace/oevm
gcc -std=c99 -Isrc -DOEVM_HEAP_SIZE=65536 tests/test_gc_trigger.c \
    src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/solidified_image.c \
    -lm -o /tmp/tgct
```
Expected: **link error** — `undefined reference to gc_set_stack_base`. (The
symbol does not exist yet.) This confirms the test drives the new interface.

- [ ] **Step 3: Add the stack-base global, setter, and conservative scan to `gc.c`**

Near the top of `src/gc.c`, after the `heap_*` globals (around line 27), add:

```c
/* ---- Conservative C-stack root scan (W1) ----------------------------- */
/* Older end of the live C call stack, captured once at VM entry. NULL means
   "not captured" — the conservative scan is skipped and only precise roots run
   (still safe, just no coverage for oops living solely in C locals). */
void *vm_c_stack_base = NULL;
void gc_set_stack_base(void *p) { vm_c_stack_base = p; }
```

Add `#include <setjmp.h>` to the includes at the top of `gc.c`.

Add the scan function just above `gc_collect` (before line 335):

```c
/* Conservatively scan the live C call stack as a root source. Any word that,
   read as a pointer, is a live arena payload start (validated by mark via the
   exact-start bitmap) is retained. setjmp spills callee-saved registers into
   the jmp_buf so register-resident oops are covered; caller-saved oops live
   across the call into gc_allocate are already compiler-spilled to the stack.
   Host-only (grow-down or grow-up handled by the min/max span). */
static void scan_c_stack(void) {
    if (vm_c_stack_base == NULL) return;   /* not captured: skip (precise roots still run) */

    jmp_buf regs;
    setjmp(regs);                          /* flush callee-saved regs to `regs` */

    /* Scan the jmp_buf itself (register-resident oops). */
    {
        void **w   = (void **)&regs;
        void **end = (void **)((char *)&regs + sizeof regs);
        for (; w < end; w++) mark_object((oop)*w);
    }

    /* Scan [younger .. older) of the stack at pointer stride. */
    int anchor;
    char *top  = (char *)&anchor;
    char *base = (char *)vm_c_stack_base;
    char *lo = top < base ? top : base;
    char *hi = top < base ? base : top;
    /* Align lo up to a pointer boundary; stack-stored pointers are aligned. */
    lo = (char *)(((uintptr_t)lo + sizeof(void *) - 1) & ~(uintptr_t)(sizeof(void *) - 1));
    for (void **w = (void **)lo; (char *)w + sizeof(void *) <= hi; w++) {
        mark_object((oop)*w);
    }
}
```

In `gc_collect`, after the additional-roots hook call (currently `src/gc.c`
around line 386–388, just before the sweep phase comment "4. Sweep phase"),
add the conservative scan so it participates as a root source:

```c
    /* Conservative C-stack scan: roots oops held only in C locals (W1). */
    scan_c_stack();
```

- [ ] **Step 4: Run test to verify it passes**

Run:
```bash
cd /home/david/workspace/oevm
gcc -std=c99 -Isrc -DOEVM_HEAP_SIZE=65536 tests/test_gc_trigger.c \
    src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/solidified_image.c \
    -lm -o /tmp/tgct && /tmp/tgct
```
Expected:
```
test_gc_trigger: conservative-scan survival OK
ALL TESTS PASSED
```

- [ ] **Step 5: Commit**

```bash
cd /home/david/workspace/oevm
git add src/gc.c tests/test_gc_trigger.c
git commit -m "feat(gc): conservative C-stack scan as a GC root source (W1 step 1)"
```

---

### Task 2: Exhaustion trigger — collect-and-retry in `gc_allocate`

Wire the trigger: on arena exhaustion, `gc_allocate` runs one full collection
and retries once before invoking the terminal `oevm_oom_handler`. A
re-entrancy guard prevents recursive collection.

**Files:**
- Modify: `src/gc.c` (refactor `gc_allocate`; add `gc_in_progress` + `arena_try_alloc`)
- Modify: `tests/test_gc_trigger.c` (add the rolling-root reclamation test)

**Interfaces:**
- Consumes: `vm_c_stack_base` / `scan_c_stack` (Task 1), existing `gc_collect`, `gc_additional_roots_hook`, `gc_is_live`.
- Produces: no new public symbol; `gc_allocate` now self-triggers GC on exhaustion. Behavior contract: returns a valid object whenever a collection can free enough room; only reaches `oevm_oom_handler` when the live set itself exceeds the arena.

- [ ] **Step 1: Write the failing test**

Append a second test to `tests/test_gc_trigger.c`. It keeps a bounded rolling
root set (8 live objects) via the additional-roots hook and allocates far more
objects than the 64 KB cap holds — so it can only succeed if exhaustion
triggers a collection that reclaims the dropped objects. Without the trigger,
`gc_allocate` hits `oevm_oom_handler` (abort) and the process dies.

Add above `main`:

```c
/* Rolling root set: only these survive each collection. */
#define ROLL 8
static oop roll[ROLL];
static void roll_roots(void) { for (int i = 0; i < ROLL; i++) mark_object(roll[i]); }

__attribute__((noinline))
static void exhaustion_reclaims(void) {
    gc_additional_roots_hook = roll_roots;
    for (int i = 0; i < ROLL; i++) roll[i] = NULL;

    /* 64 KB cap holds far fewer than 20000 of these live at once; success
       requires the exhaustion path to collect + reclaim the dropped objects. */
    for (int i = 0; i < 20000; i++) {
        oop o = gc_allocate((oop)&PlainVtable, sizeof(struct ObjectInstance) + 4 * sizeof(oop));
        roll[i % ROLL] = o;            /* drop the object ROLL allocations ago */
    }
    /* All current roots must still be live after all that churn. */
    for (int i = 0; i < ROLL; i++) assert(gc_is_live(roll[i]));
    gc_additional_roots_hook = NULL;
}
```

Call it from `main` after the first assertion:

```c
    exhaustion_reclaims();
    printf("test_gc_trigger: exhaustion collect+retry reclaims OK\n");
```

- [ ] **Step 2: Run test to verify it fails**

Run:
```bash
cd /home/david/workspace/oevm
gcc -std=c99 -Isrc -DOEVM_HEAP_SIZE=65536 tests/test_gc_trigger.c \
    src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/solidified_image.c \
    -lm -o /tmp/tgct && /tmp/tgct
```
Expected: **abort** — `VM GC Error: Out of Memory (...)` then the process
aborts (non-zero exit), because `gc_allocate` still calls `oevm_oom_handler`
on the first exhaustion without collecting.

- [ ] **Step 3: Refactor `gc_allocate` to collect-and-retry**

In `src/gc.c`, add a re-entrancy guard near the other file-scope statics
(after the `oevm_oom_handler` definition, ~line 84):

```c
/* Set while gc_collect runs, so an allocation during collection (none today)
   cannot recursively trigger another collection. */
static int gc_in_progress = 0;
```

Extract the W4 fast path (free-list search, then bump) into a helper that
returns `NULL` when neither succeeds. Replace the body of `gc_allocate`
(`src/gc.c:117`) so it reads:

```c
/* One allocation attempt against the arena: exact-bin free-list first, then
   bump. Returns NULL if neither has room (caller decides whether to collect). */
static oop arena_try_alloc(oop vtable, size_t rounded, size_t need) {
    size_t bi = bin_index(rounded);
    struct HeapHeader **pp = &free_bins[bi];
    while (*pp != NULL) {
        if ((*pp)->size >= rounded) {
            struct HeapHeader *h = *pp;
            *pp = h->next;
            h->next = NULL;
            h->marked = 0;
            oop obj = (oop)(h + 1);
            bm_set(bm_granule(obj));
            obj->vtable = vtable;
            return obj;
        }
        pp = &(*pp)->next;
    }
    if (heap_top + need > heap_end) return NULL;
    struct HeapHeader *header = (struct HeapHeader *)heap_top;
    heap_top += need;
    header->next = NULL;
    header->size = rounded;
    header->marked = 0;
    oop obj = (oop)(header + 1);
    bm_set(bm_granule(obj));
    obj->vtable = vtable;
    return obj;
}

oop gc_allocate(oop vtable, size_t size) {
    if (heap_base == NULL) heap_init();

    size_t rounded = (size + (OEVM_GRANULE - 1)) & ~(size_t)(OEVM_GRANULE - 1);
    if (rounded == 0) rounded = OEVM_GRANULE;   /* I2: zero-size guard */
    size_t need = sizeof(struct HeapHeader) + rounded;

    oop obj = arena_try_alloc(vtable, rounded, need);
    if (obj != NULL) return obj;

    /* Exhausted: collect once and retry, unless already collecting. */
    if (!gc_in_progress) {
        gc_in_progress = 1;
        gc_collect();
        gc_in_progress = 0;
        obj = arena_try_alloc(vtable, rounded, need);
        if (obj != NULL) return obj;
    }

    /* Still no room after a full collection: terminal OOM policy. */
    return oevm_oom_handler(size);
}
```

(Remove the original inline free-list/bump block now living in
`arena_try_alloc`; preserve the `I1`/`I2` comments by moving the `I1`
"exact-bin miss falls through to bump" note above `arena_try_alloc`.)

- [ ] **Step 4: Run test to verify it passes**

Run:
```bash
cd /home/david/workspace/oevm
gcc -std=c99 -Isrc -DOEVM_HEAP_SIZE=65536 tests/test_gc_trigger.c \
    src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/solidified_image.c \
    -lm -o /tmp/tgct && /tmp/tgct
```
Expected:
```
test_gc_trigger: conservative-scan survival OK
test_gc_trigger: exhaustion collect+retry reclaims OK
ALL TESTS PASSED
```

- [ ] **Step 5: Verify the W4 arena unit test still passes (no regression)**

Run:
```bash
cd /home/david/workspace/oevm
gcc -std=c99 -Isrc tests/test_arena.c \
    src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/solidified_image.c \
    -lm -o /tmp/ta && /tmp/ta
```
Expected: the existing `test_arena` success output, unchanged (the exhaustion
test there installs a longjmp OOM handler; collect-then-handler must still
reach it when the live set truly exceeds the arena).

- [ ] **Step 6: Commit**

```bash
cd /home/david/workspace/oevm
git add src/gc.c tests/test_gc_trigger.c
git commit -m "feat(gc): exhaustion-triggered collect-and-retry in gc_allocate (W1 step 2)"
```

---

### Task 3: Capture the C stack base at VM entry (both models)

Set `vm_c_stack_base` once at the top of each production `main`, so the
conservative scan has coverage in real runs (not just unit tests).

**Files:**
- Modify: `src/oevm_main.c` (Model A, `-DOEVM_APPLIANCE`, `main` at line 30)
- Modify: `oevm_image_main.c` (Model B default, `main` at line 18)

**Interfaces:**
- Consumes: `gc_set_stack_base(void *)` (Task 1).

- [ ] **Step 1: Declare and call the setter in Model A `main`**

In `src/oevm_main.c`, add the extern near the other gc externs (by line 23):

```c
extern void gc_set_stack_base(void *p);
```

As the FIRST statement inside `main` (line 30), capture the base:

```c
    int oevm_stack_anchor;
    gc_set_stack_base(&oevm_stack_anchor);
```

- [ ] **Step 2: Declare and call the setter in Model B `main`**

In `oevm_image_main.c`, add near the existing extern (line 15):

```c
extern void gc_set_stack_base(void *p);
```

As the FIRST statement inside `main` (line 18), before the `argc` check:

```c
    int oevm_stack_anchor;
    gc_set_stack_base(&oevm_stack_anchor);
```

- [ ] **Step 3: Build both production binaries to verify they compile**

Run:
```bash
cd /home/david/workspace/oevm
# Model A
gcc -Wall -Wextra -std=c99 -DOEVM_APPLIANCE \
    src/vm.c src/gc.c src/dispatch.c src/solidified_image.c src/oevm_main.c \
    -lm -o /tmp/oevm_runner_a
# Model B
gcc -Wall -std=c99 \
    src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/kernel_boot.c \
    oevm_image_main.c -lm -o /tmp/oevm_img_b
```
Expected: both compile with no errors (warnings about the unused
`oevm_stack_anchor` value are not expected — its address is taken).

- [ ] **Step 4: Commit**

```bash
cd /home/david/workspace/oevm
git add src/oevm_main.c oevm_image_main.c
git commit -m "feat(gc): capture C stack base at VM entry for conservative scan (W1 step 3)"
```

---

### Task 4: Verification audit of runtime allocation sites

Review the ~36 runtime `gc_allocate` sites and confirm every oop held across an
allocation is reachable from {C stack, reified VM frame, global root}. Add an
explicit root ONLY for a genuine off-stack/off-frame case. Deliverable is a
documented audit table; code changes are expected to be zero or near-zero
because the conservative C-stack scan already covers stack-resident locals.

**Files:**
- Create: `docs/superpowers/notes/2026-06-18-w1-rooting-audit.md` (the table)
- Modify (only if a gap is found): `src/dispatch.c` / `src/vm.c` (add a temp-root or `gc_additional_roots_hook` entry)

**Interfaces:**
- Consumes: `gc_additional_roots_hook` (existing), `scan_c_stack` (Task 1).

- [ ] **Step 1: Enumerate the runtime alloc sites**

Run:
```bash
cd /home/david/workspace/oevm
grep -n "gc_allocate\|float_alloc" src/dispatch.c src/vm.c
```
Expected: ~24 sites in `dispatch.c`, ~12 in `vm.c`. (The `oimg_loader.c` /
`solidified_image.c` sites run at image load, before user code executes; note
them but they do not need roots.)

- [ ] **Step 2: Classify each site and write the audit table**

For each site, answer: between this allocation and the point where its result
(and any sibling oops being assembled) becomes frame/global-reachable, is any
live oop held ONLY in a C local? If yes → covered by the conservative C-stack
scan (Task 1). The only sites needing an explicit root are those holding a live
oop in memory the scan cannot see — an oop stored solely inside a malloc'd C
struct that is not on the call stack and not a reified VM frame.

Write `docs/superpowers/notes/2026-06-18-w1-rooting-audit.md` with one row per
site: `file:line | what is allocated | live oops in C locals across it |
covered by (stack-scan / frame / global) | action (none / root added)`.

Reference example to classify (`src/dispatch.c:268`, `make_message`): `selector`
is a C-local argument held across `gc_allocate`; it is covered by the C-stack
scan, so **no action**. Use the same reasoning for the rest.

- [ ] **Step 3: Add roots only where a genuine gap exists**

If (and only if) a site holds a live oop the conservative scan cannot reach,
add it to a roots callback. Pattern (only if needed):

```c
/* in the relevant translation unit */
static oop w1_extra_root = NULL;
static void w1_root_cb(void) { mark_object(w1_extra_root); }
/* set w1_root_cb via gc_additional_roots_hook chaining, set/clear
   w1_extra_root around the unsafe span */
```

If no gap is found (the expected outcome), record "no roots required — C-stack
scan + existing precise roots cover all runtime sites" in the audit doc and add
no code.

- [ ] **Step 4: Build to confirm no breakage (only if code changed)**

Run (Model A + B builds from Task 3 Step 3). Expected: compile clean. If no
code changed, skip.

- [ ] **Step 5: Commit**

```bash
cd /home/david/workspace/oevm
git add docs/superpowers/notes/2026-06-18-w1-rooting-audit.md
# add src files only if a root was required:
# git add src/dispatch.c src/vm.c
git commit -m "docs(gc): W1 rooting audit of runtime alloc sites (W1 step 4)"
```

---

### Task 5: Acceptance — gc_stress under a capped heap + full conformance

Prove the GH #3 gate (`05_gc_stress.ns` runs to completion under a small heap
cap with no OOM) and that conformance is unchanged on all three configs. Record
the capped-heap figure as the start of the nRF52840 RAM-ceiling number.

**Files:**
- Modify: `docs/optimizations.md` (flip the W1 row; record the cap figure)
- Modify: `docs/ROADMAP.md` (mark W1 done; W2a next)

**Interfaces:** none (verification + docs).

- [ ] **Step 1: Confirm `05_gc_stress.ns` aborts today without a trigger (baseline)**

This is informational — it captures the "before". With the trigger already
implemented (Tasks 1–2), instead demonstrate the gate directly in Step 2.

- [ ] **Step 2: Run `gc_stress` under a capped heap on both paths**

Run (cap the managed arena well below the no-GC churn ceiling so collection
MUST trigger; 1 MB is comfortably above the live set but far below 100k arrays'
cumulative size):
```bash
cd /home/david/workspace/ns-oevm
OEVM_PATH=AB OEVM_EXTRA_CFLAGS="-DOEVM_HEAP_SIZE=1048576" \
  python3 tools/run_tests_oevm.py 06_limit_tests/05_gc_stress.ns
```
Expected: **PASS** on both Path A and Path B (`expect: true` printed; no
`Out of Memory` abort). If it still OOMs, the live set exceeds the cap — raise
the cap and record the smallest cap that passes.

- [ ] **Step 3: Run the full conformance suite on A + B**

Run:
```bash
cd /home/david/workspace/ns-oevm
OEVM_PATH=AB python3 tools/run_tests_oevm.py
```
Expected: **47 pass / 0 fail / 1 skip** on both Model A and Model B, unchanged.

- [ ] **Step 4: Run the `-m32` portability config**

Run:
```bash
cd /home/david/workspace/oevm
bash tests/test_portability_32.sh
```
Expected: passes as before (the conservative scan uses `sizeof(void*)` stride,
so it is pointer-width-correct under `-m32`).

- [ ] **Step 5: Run both unit tests once more (final regression)**

Run:
```bash
cd /home/david/workspace/oevm
gcc -std=c99 -Isrc -DOEVM_HEAP_SIZE=65536 tests/test_gc_trigger.c \
    src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/solidified_image.c \
    -lm -o /tmp/tgct && /tmp/tgct
gcc -std=c99 -Isrc tests/test_arena.c \
    src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/solidified_image.c \
    -lm -o /tmp/ta && /tmp/ta
```
Expected: both print `ALL TESTS PASSED` / their success lines.

- [ ] **Step 6: Update docs with the result**

In `docs/optimizations.md`, flip the W1 weakness row to DONE and record: "GC
trigger wired (exhaustion collect-and-retry); conservative C-stack rooting;
`05_gc_stress.ns` runs to completion under a `<cap>` managed-arena cap". In
`docs/ROADMAP.md`, mark W1 complete and W2a (float-arena reclaim) as next.
Use the smallest passing cap from Step 2 as `<cap>`.

- [ ] **Step 7: Commit**

```bash
cd /home/david/workspace/oevm
git add docs/optimizations.md docs/ROADMAP.md
git commit -m "docs(gp0): W1 GC trigger + rooting done; gc_stress bounded under capped heap"
```

---

## Self-Review

**Spec coverage:**
- Part 1 (trigger & exhaustion path) → Task 2. ✓ (loop-in-`gc_allocate`, `gc_in_progress` guard, single retry, terminal hook unchanged)
- Part 2 (conservative root augmentation) → Task 1 (scan) + Task 3 (stack-base capture in both mains). ✓ (`setjmp` spill, `[lo..hi)` span, `void*` stride, host-only)
- Part 3 (verification audit) → Task 4. ✓ (audit table deliverable; roots only on a real gap)
- Part 4 (testing & acceptance) → Task 1/Task 2 unit tests, Task 5 gate + conformance + `-m32`. ✓
- Part 5 / scope boundaries → encoded in Global Constraints (floats out, watermark out, Zephyr seam). ✓
- Acceptance criteria (gc_stress capped, new unit test incl. conservative survival, 47/0/1 on A+B+m32, documented cap) → Task 5 Steps 2–6. ✓

**Placeholder scan:** No TBD/TODO-as-work; the only "if a gap is found" branch in Task 4 is a genuine conditional whose both outcomes are specified (add a root with the shown pattern, or record "no roots required"). ✓

**Type consistency:** `gc_set_stack_base(void*)`, `vm_c_stack_base` (`void*`), `gc_in_progress` (`int`), `arena_try_alloc(oop, size_t, size_t) -> oop`, `scan_c_stack(void)`, `mark_object(oop)`, `gc_is_live(oop)` used identically across Tasks 1–3. `gc_allocate(oop, size_t)` signature unchanged. ✓
