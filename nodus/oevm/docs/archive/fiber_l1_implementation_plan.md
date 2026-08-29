> **HISTORICAL — archived 2026-08-29.** Fibers, NLR and growable stacks were DELETED in Milestone A (2026-07-04, `nodus/oevm/docs/MILESTONE_A.md` §1); the VM is run-to-completion. What this document built and which SURVIVED is the flat non-recursive interpreter and the heap `Context` layer (`MILESTONE_A.md` §2). Read it for how the interpreter got flat, not for what the VM does. Current architecture: `nodus/oevm/docs/vm.md`.

# L1 — First-class Stack + relative dynamic links + precise GC: Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Make the per-fiber call stack a first-class GC-managed heap object and switch the dynamic frame link (`prev_frame`) to the L0 base-relative encoding — without changing observable behavior.

**Architecture:** Three moves, each leaving the suite green: (1) relativize `prev_frame` on the existing stack arrays; (2) introduce a `Stack` heap object; (3) route the main thread (as "fiber 0") and per-fiber stacks through `Stack` objects. The lexical link (`home_ctx`), `Context`/temp-vectors, NLR rework, stack *growth*, and flat dispatch are explicitly **out of scope** (later layers — see Scope).

**Tech Stack:** C99, the oevm VM (`src/vm.c`, `src/gc.c`, `src/dispatch.c`), the L0 encoding `src/frame_abi.h`, the cross-repo test runner `../ns-oevm/tools/run_tests_oevm.py`.

## Global Constraints

- **Stay green every task, both image paths.** L1 is behavior-equivalent by design (relative links on non-moving stacks == absolute). The full suite (`python3 ../ns-oevm/tools/run_tests_oevm.py`) + `tests/test_frame_abi.c` + `fibers-gc-stress` are the acceptance oracle.
- **`home_ctx` stays ABSOLUTE in L1.** It is read cross-fiber (R7 entry-block `OP_LD_LEXICAL` home-walk, `vm.c:496–510`/`:1156`); relativizing it would break fibers. Do **not** touch the lexical link.
- **No stack growth in L1.** Stacks stay fixed-capacity; overflow still aborts. Growth is the final layer (needs flat dispatch + heap lexical links).
- **Relative links via `frame_abi.h` only** — never hand-roll the offset math; use `frame_link_encode`/`frame_link_decode`.
- C99, `-std=c99 -Wall -Wextra`; portable to `-m32` (the `portability-32` gate must stay green).
- Commit after each task. Branch: `derecursify-fibers`.

## Scope (what L1 does NOT do — deferred, with the layer it moves to)
- Lexical link `home_ctx` relativization, `Context`/indirect temp-vectors, `OP_LD_LEXICAL` Context-chain switch, NLR cross-fiber/Stack-identity checks → **L4** (with the compiler captured-variable emission; can't go live without it).
- Non-recursive/flat dispatch → **L2**.
- Stack **growth** (realloc-on-overflow) → **final layer** (needs L2 flat dispatch *and* L4 heap lexical links; until then a Stack move would dangle C-cached frame pointers and interior `home_ctx` pointers).
- Deleting ucontext / conservative C-stack scan → **L5** (while dispatch recurses, fibers still need separate C stacks).

## File Structure
- `src/oevm.h` — add `struct Stack`, `StackVtable` extern, `stack_alloc` decl.
- `src/vm.c` — relativize `prev_frame` at frame alloc + readers; main-Stack-as-fiber-0; per-fiber Stack objects.
- `src/gc.c` — `Stack` tracer; relative-link frame walks; active-Stack bootstrap root; interior-pointer safety.
- `src/kernel_boot.c` (and/or `src/oevm_main.c`) — allocate the main `Stack` before first GC.
- `tests/test_stack.c` — new standalone unit test (Stack alloc + relative frame walk + GC survival).
- `tests/test_frame_abi.c` — unchanged; must stay green.

---

### Task 1: Relativize the dynamic link (`prev_frame`) on the existing arrays

Switch `prev_frame` to the L0 base-relative SMI encoding at the one writer and every reader. No `Stack` object yet — base is the current array (`vm_stack` for the main stack, `fib->stack` for fibers). Behavior-equivalent on fixed bases.

**Files:**
- Modify: `src/vm.c` (`run_interpreter` frame alloc `:217`, frame-link read `:940`, fiber-frame init `:1076`)
- Modify: `src/gc.c` (`trace_one` per-fiber walk `:512,:528`; `scan_vm_stack` `:572,:592`)
- Test: full suite + `tests/test_frame_abi.c`

**Interfaces:**
- Consumes: `frame_link_encode(const oop *base, const struct MethodContext *frame)`, `frame_link_decode(oop *base, oop link)` (from `src/frame_abi.h`).
- Produces: `prev_frame` now holds `NULL` (bottom) or `TAG_SMI(offset)`; all readers decode via `frame_link_decode`.

- [ ] **Step 1: Add the include.** In `src/vm.c` and `src/gc.c`, ensure `#include "frame_abi.h"` is present (after `#include "oevm.h"`).

- [ ] **Step 2: Encode at frame alloc.** `src/vm.c:217`, replace:
```c
    ctx->prev_frame = (oop)prev_ctx;
```
with:
```c
    ctx->prev_frame = frame_link_encode(vm_stack, prev_ctx);  /* L0 relative dynamic link */
```

- [ ] **Step 3: Decode at the `exit_frame` read.** `src/vm.c:940`, replace:
```c
    oop prev = ctx->prev_frame;
```
with:
```c
    oop prev = (oop)frame_link_decode(vm_stack, ctx->prev_frame);
```
(Confirm the surrounding code uses `prev` as a `MethodContext*`/`oop` consistently; cast as the existing code expects.)

- [ ] **Step 4: Fiber-frame init keeps NULL.** `src/vm.c:1076` (`ctx->prev_frame = NULL;`) is already the bottom sentinel — `frame_link_encode(base, NULL)` is `NULL`. Leave as `NULL`. Add a comment: `/* bottom sentinel; relative encoding leaves NULL == no caller */`.

- [ ] **Step 5: GC per-fiber walk (decode + drop SMI mark).** `src/gc.c:512` remove `mark(frame->prev_frame);` (a relative SMI is not a heap root; `mark` would no-op but the call is misleading). `src/gc.c:528`, replace:
```c
                frame = (struct MethodContext *)frame->prev_frame;
```
with:
```c
                frame = frame_link_decode(fib->stack, frame->prev_frame);
```

- [ ] **Step 6: GC main-stack walk (decode + drop SMI mark).** `src/gc.c:572` remove `mark(frame->prev_frame);`. `src/gc.c:592`, replace:
```c
        frame = (struct MethodContext *)frame->prev_frame;
```
with:
```c
        frame = frame_link_decode(vm_stack, frame->prev_frame);
```

- [ ] **Step 7: Build + run the full suite.**
Run: `cd ../ns-oevm && python3 tools/run_tests_oevm.py`
Expected: all pass, both image paths; `portability-32: PASS`; `fibers-gc-stress: PASS`.

- [ ] **Step 8: Run the L0 unit test.**
Run: `cd ../oevm && cc -std=c99 -Wall -Wextra -Isrc tests/test_frame_abi.c -o /tmp/test_frame_abi && /tmp/test_frame_abi`
Expected: `L0 frame-ABI unit test: PASSED`.

- [ ] **Step 9: Commit.**
```bash
git add src/vm.c src/gc.c
git commit -m "feat(L1): relativize prev_frame dynamic link via frame_abi (behavior-equivalent)"
```

---

### Task 2: Introduce the `Stack` heap object (dormant)

Define and allocate a first-class `Stack` heap object with a vtable and a GC tracer — not yet wired into the running engine. Proven by a standalone unit test.

**Files:**
- Modify: `src/oevm.h` (add `struct Stack`, `extern oop StackVtable;`, `oop stack_alloc(uint32_t capacity);`)
- Modify: `src/gc.c` (register `StackVtable`; add `Stack` case in `trace_one`)
- Modify: `src/dispatch.c` or wherever vtables are initialized (define `StackVtable`, mirroring an existing simple vtable like `FiberVtable`)
- Test: `tests/test_stack.c` (new, standalone)

**Interfaces:**
- Produces: `struct Stack { struct Object base; uint32_t capacity; uint32_t sp; uint32_t current_off; oop data[]; };`, `oop stack_alloc(uint32_t capacity)` (returns a zeroed `Stack` with `vtable=StackVtable`, `capacity` set, `sp=0`, `current_off=0`), `StackVtable`.

- [ ] **Step 1: Write the failing unit test.** Create `tests/test_stack.c`:
```c
/* L1 Stack-object unit test: allocation + GC survival.
 * cc -std=c99 -Isrc tests/test_stack.c src/gc.c src/dispatch.c src/vm.c \
 *   src/platform.c src/serial.c src/udp.c src/mirror.c src/backend/posix/*.c \
 *   src/oimg_loader.c src/solidified_image.c -o /tmp/test_stack -lm */
#include <stdio.h>
#include <assert.h>
#include "../src/oevm.h"

int main(void) {
    oevm_boot_minimal();            /* whatever the other C tests use to init heap+vtables */
    oop s = stack_alloc(64);
    assert(s != NULL);
    assert(obj_vtable(s) == StackVtable);
    struct Stack *st = (struct Stack *)s;
    assert(st->capacity == 64 && st->sp == 0);
    printf("test_stack: PASSED\n");
    return 0;
}
```
(Use the same boot/init helper the existing C unit tests use — check `tests/test_fibers.c` for the exact init call and link set; mirror it.)

- [ ] **Step 2: Run to verify it fails** (no `struct Stack`/`stack_alloc`/`StackVtable`).
Run: build per the header comment. Expected: compile error (undeclared `stack_alloc`/`StackVtable`).

- [ ] **Step 3: Declare the struct + API.** In `src/oevm.h`, near `struct Fiber`:
```c
struct Stack {
    struct Object base;      /* vtable = StackVtable; GC-managed heap object */
    uint32_t capacity;       /* slots in data[] */
    uint32_t sp;             /* bump cursor (top), word index */
    uint32_t current_off;    /* offset of the live frame within data[] */
    oop      data[];         /* frame region: MethodContext headers + regs */
};
extern oop StackVtable;
oop stack_alloc(uint32_t capacity);
```

- [ ] **Step 4: Define `StackVtable` + `stack_alloc`.** Mirror an existing simple vtable definition/registration (grep `FiberVtable =`); in the same file:
```c
oop StackVtable = NULL;
oop stack_alloc(uint32_t capacity) {
    size_t words = sizeof(struct Stack) / sizeof(oop) + capacity;
    oop s = gc_allocate(words * sizeof(oop));   /* match the allocator the other heap objs use */
    obj_set_vtable(s, StackVtable);
    struct Stack *st = (struct Stack *)s;
    st->capacity = capacity; st->sp = 0; st->current_off = 0;
    return s;
}
```
(Use the project's actual allocator + vtable-set idiom — copy from `fiber` allocation in `vm.c:1017` area.)

- [ ] **Step 5: GC tracer for `Stack`.** In `src/gc.c` `trace_one`, add a branch (before the generic object trace) that, for a `Stack`, marks the live slots `data[0..sp)` and skips `prev_frame` SMIs / `saved_pc`. For Task 2 (dormant Stack with no frames), `sp==0` so nothing to mark — but write the precise walk now (it will be exercised in Tasks 3–4):
```c
    if (obj_vtable(ptr) == StackVtable) {
        struct Stack *st = (struct Stack *)ptr;
        struct MethodContext *frame = st->sp ? (struct MethodContext *)(st->data + st->current_off) : NULL;
        struct MethodContext *higher = NULL;
        while (frame != NULL) {
            mark(frame->vtable); mark(frame->home_ctx); mark(frame->cleanup_cl);
            oop *regs = frame->regs;
            oop *end = higher ? (oop *)higher : (st->data + st->sp);
            for (oop *r = regs; r < end; r++) mark(*r);
            higher = frame;
            frame = frame_link_decode(st->data, frame->prev_frame);
        }
        return;
    }
```
(Mark `StackVtable` itself as a root alongside the other vtables — grep where `mark(FiberVtable)` / vtable roots are listed, add `mark(StackVtable);`.)

- [ ] **Step 6: Register `StackVtable` init.** Wherever `FiberVtable` is created/installed (grep `FiberVtable =` in the boot path), create `StackVtable` the same way so `obj_vtable(s) == StackVtable` holds after boot.

- [ ] **Step 7: Run the unit test to pass.**
Run: build per Step 1 header; `/tmp/test_stack`. Expected: `test_stack: PASSED`.

- [ ] **Step 8: Full suite still green** (dormant object must not perturb anything).
Run: `cd ../ns-oevm && python3 tools/run_tests_oevm.py`. Expected: all pass.

- [ ] **Step 9: Commit.**
```bash
git add src/oevm.h src/gc.c src/vm.c tests/test_stack.c
git commit -m "feat(L1): first-class Stack heap object + precise GC tracer (dormant)"
```

---

### Task 3: Main thread becomes "fiber 0" (main `Stack` object)

Allocate the main `Stack` at boot and route `vm_stack`/`vm_stack_ptr`/`current_frame` through it; retire the static `boot_stack`. Establish the bootstrap root and interior-pointer safety.

**Files:**
- Modify: `src/vm.c` (`boot_stack`/`vm_stack`/`vm_stack_ptr` `:31–36`; frame alloc base; `scan_vm_stack` callers)
- Modify: `src/gc.c` (`scan_vm_stack` walks the active `Stack`; add the active `Stack` as a root)
- Modify: `src/kernel_boot.c` and/or `src/oevm_main.c` (allocate main `Stack` before first allocation that can GC)

**Interfaces:**
- Consumes: `stack_alloc`, the `Stack` tracer (Task 2), `frame_link_*`.
- Produces: a global `oop g_main_stack` (or reuse an existing fiber-0 slot); `vm_stack == ((struct Stack*)g_active_stack)->data`, `vm_stack_ptr` mirrors `sp`. Active-`Stack` is a GC root.

- [ ] **Step 1: Add the active-Stack globals.** In `src/vm.c`, replace the static array:
```c
oop boot_stack[VM_STACK_MAX_WORDS];
oop *vm_stack = boot_stack;
size_t vm_stack_ptr = 0;
```
with an active-Stack-backed view:
```c
oop  g_active_stack = NULL;             /* the running fiber's Stack object (GC root) */
oop *vm_stack = NULL;                    /* == ((struct Stack*)g_active_stack)->data */
size_t vm_stack_ptr = 0;                 /* mirrors ((struct Stack*)g_active_stack)->sp */
```

- [ ] **Step 2: Bootstrap the main Stack early.** In the boot sequence (find where the heap is initialized and the first fiber/`current_frame` is set up — grep `vm_stack_ptr = 0` / boot entry), after heap init and `StackVtable` install, before any method runs:
```c
    g_active_stack = stack_alloc(VM_STACK_MAX_WORDS);
    vm_stack = ((struct Stack *)g_active_stack)->data;
    vm_stack_ptr = 0;
```
(The main `Stack` must exist before the first `gc_allocate` that could collect — place this immediately after vtables are installed.)

- [ ] **Step 3: Keep `sp` in sync.** Anywhere `vm_stack_ptr` is updated in the hot path (`run_interpreter` `:214`, `exit_frame` `:881`), the canonical store is still `vm_stack_ptr`; mirror it into the Stack at fiber-switch boundaries only (the active Stack's `sp = vm_stack_ptr` on save). Add a comment documenting that `vm_stack_ptr` is the live cursor and `((struct Stack*)g_active_stack)->sp` is synced on switch. (No per-instruction sync — keep the hot path unchanged.)

- [ ] **Step 4: GC root + scan via the active Stack.** In `src/gc.c`: add `mark(g_active_stack);` to the roots; change `scan_vm_stack` to walk the active Stack's frames (it can simply `trace_one(g_active_stack)` reuse, or keep the dedicated walk using `vm_stack`/`vm_stack_ptr` which now point into the Stack payload — either is fine; prefer reusing the Task 2 tracer for one code path).

- [ ] **Step 5: Interior-pointer safety.** Confirm `home_ctx` (absolute, now pointing *into* a `Stack` payload) is never passed to `mark` as an object start. In the frame walks, do **not** `mark(home_ctx)` as a heap object if it points inside a `Stack` (the home frame is kept alive transitively by its `Stack` being a root). Simplest: drop `mark(frame->home_ctx)` in the Stack tracer and rely on Stack reachability (every fiber's Stack is a root via the fiber registry — verified in Task 4). Add a comment explaining why.

- [ ] **Step 6: Remove `VM_STACK_MAX_WORDS` static array refs.** Ensure nothing else references `boot_stack`. Grep: `grep -rn boot_stack src/`. Expected: no remaining uses.

- [ ] **Step 7: Full suite green, both paths.**
Run: `cd ../ns-oevm && python3 tools/run_tests_oevm.py`. Expected: all pass; `portability-32: PASS`; `fibers-gc-stress: PASS`.

- [ ] **Step 8: Commit.**
```bash
git add src/vm.c src/gc.c src/kernel_boot.c
git commit -m "feat(L1): main thread as fiber-0 Stack object (retire static boot_stack)"
```

---

### Task 4: Per-fiber stacks become `Stack` objects

Make `fiberCreate:` allocate a `Stack` object instead of a raw malloc'd array, and make fiber switching swap the active `Stack`. Unifies the GC path (per-fiber walk → the Task 2 tracer).

**Files:**
- Modify: `src/vm.c` (fiber alloc `:1017` area; fiber switch `:1115–1128`; `struct Fiber.stack` usage)
- Modify: `src/oevm.h` (`struct Fiber`: `oop *stack` → `oop stack_obj` *or* keep `stack`/`stack_capacity` pointing at a `Stack`'s `data` with a back-pointer — choose the minimal change; see Step 1)
- Modify: `src/gc.c` (per-fiber walk → trace each fiber's `Stack` object; drop the bespoke array walk)

**Interfaces:**
- Consumes: `stack_alloc`, the `Stack` tracer, `g_active_stack`/`vm_stack`/`vm_stack_ptr`.
- Produces: each `Fiber` owns a `Stack` object; switching sets `g_active_stack`/`vm_stack`/`vm_stack_ptr` from it and syncs `sp` on save.

- [ ] **Step 1: Fiber holds a Stack object.** In `src/oevm.h` `struct Fiber`, replace the raw `oop *stack; size_t stack_capacity;` with `oop stack_obj;` (the `Stack`). Keep `current_context`/`stack_ptr` or fold them into the `Stack` (`current_off`/`sp`) — prefer folding to avoid duplicate state; update readers accordingly.

- [ ] **Step 2: Allocate a Stack at fiber create.** `src/vm.c:1017` area, replace the malloc'd array with `fib_stack = stack_alloc(cap);` and store it in `fib->stack_obj`. Mark it reachable from the fiber so GC keeps it (the fiber is already a root via `g_fiber_registry`).

- [ ] **Step 3: Switch swaps the active Stack.** `src/vm.c:1115–1128` (the switch): on save, `((struct Stack*)g_active_stack)->sp = vm_stack_ptr;` then set `g_active_stack = target->stack_obj; vm_stack = ((struct Stack*)g_active_stack)->data; vm_stack_ptr = ((struct Stack*)g_active_stack)->sp;`. (Replaces the raw `vm_stack = target_fib->stack;` lines.)

- [ ] **Step 4: GC per-fiber walk → Stack tracer.** In `src/gc.c` `trace_one` for a `Fiber`: replace the inline per-fiber frame walk (`:507–529`) with `mark(fib->stack_obj);` (the Stack tracer from Task 2 does the precise frame/reg walk). Keep the `entry_closure`/`entry_receiver` marks (C1).

- [ ] **Step 5: Full suite + fiber gates green.**
Run: `cd ../ns-oevm && python3 tools/run_tests_oevm.py`
Expected: all pass; `fibers-gc-stress: PASS`; `45_fibers` interleave `ABABAB` still correct (POSIX ucontext path unchanged); `portability-32: PASS`.

- [ ] **Step 6: ASan + GC stress sanity.**
Run: `cd ../oevm && OEVM_GC_STRESS=1` build of the fibers appliance (mirror `run_fibers_gc_stress_gate` in the runner) under ASan; expect clean.

- [ ] **Step 7: Commit.**
```bash
git add src/oevm.h src/vm.c src/gc.c
git commit -m "feat(L1): per-fiber stacks as Stack objects; unified precise GC walk"
```

---

## Self-Review notes
- **Spec coverage:** §2.1 Stack object (T2–T4); §2.4 relative `prev_frame` (T1); §6 precise GC tracer + bootstrap + interior safety (T2 S5, T3 S2/S4/S5, T4 S4); §7 acceptance "suite green / test_frame_abi green / fibers-gc-stress" (every task's run steps). Deferred-by-design items (Context/temp-vectors, lexical switch, NLR cross-fiber, growth, flat dispatch, ucontext deletion) are listed under Scope with their target layer.
- **Boundary refinement folded in:** `home_ctx` stays absolute; lexical/`Context`/NLR moved to L4; growth moved to the final layer. Record this in `fiber_implementation.md §7` (layer order) as a companion edit.
- **Highest-risk steps:** T3 bootstrap ordering (main `Stack` before first GC) and T3/T4 interior-pointer GC safety. If a step's exact line/idiom differs from the quoted code, locate by the quoted snippet, not the line number.
