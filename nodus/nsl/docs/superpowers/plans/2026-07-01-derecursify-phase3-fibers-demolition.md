# De-recursify Phase 3 — Fibers + Demolition Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make fibers a swap-`Stack` operation on the flat (Phase-2) interpreter — one C activation for all fibers — and demolish the now-redundant machinery (ucontext/`swapcontext`, `fiber_trampoline`, `g_fiber_registry`, the `#ifdef __ZEPHYR__` reload gates, the conservative C-stack scan, and the dead `cleanup_cl`/`ensure:` path). Prove the historically-broken `ABBBAA` multi-fiber interleave now yields `ABABAB`.

**Architecture:** Phase 2 turned `run_interpreter` into one flat loop per fiber, with all execution state living in first-class heap `Stack` objects. On Zephyr, a fiber switch already works by (1) swapping the VM global pointers to the target's `Stack` and (2) the four in-loop reload blocks re-syncing `pc`/`ctx`/`method` from the now-current frame and delivering the switch value via the `saved_pc-1` src2 decode. Phase 3 makes **that single-C-stack mechanism universal**: the POSIX `swapcontext` path is deleted, the `#ifdef __ZEPHYR__` gates are removed so reload is always compiled, and per-fiber C stacks vanish. Precise GC roots (`scan_vm_stack` for the active `Stack` + the `StackVtable` tracer for suspended `Stack`s) fully cover fiber execution state, so the conservative C-stack scan is deleted.

**Tech Stack:** C99 interpreter (`oevm/src/*.c`); Newspeak facade unchanged (`ns-oevm/lib/kernel.ns`); Python test harness (`oevm/tools/run_tests_oevm.py`); `.ns` conformance suite (`ns-tests/`); host `-D__ZEPHYR__` repro build of `oevm/oevm_image_main.c`.

## Global Constraints

- **No `.ns` API change.** The fiber facade (`Concurrency`, `Fiber`, `fiberCreate:`, `fiberSwitch:value:`, `currentFiber`) is untouched. This is a C-mechanism swap only.
- **No image-format bump.** No new bytecode (`0x17` stays free), no new opcode, no facade change ⇒ `.oimg` `format_version` and the solidified image are unchanged. Removing the runtime `cleanup_cl` frame word does **not** touch the image: live frames are never serialized (the image holds `CompiledMethod`s/closures, not activations).
- **Both paths, every task.** Every task's gate runs the full suite on Path A (solidified) **and** Path B (`.oimg`) — `OEVM_PATH=AB` — plus `-D__ZEPHYR__` syntax/host build. Per-phase gate matrix (from design doc §6): full suite both paths, `tests/test_fibers.c`, `ns-tests/04_somns_basic/45_fibers.ns` interleave, `tests/test_portability_32.sh`, boot-gc-asan, `OEVM_GC_STRESS=1`, ASan-clean.
- **Scope discipline (design doc §1):** growth (movable stacks) and heap-closure/temp-vector (`Context`) rework are **DEFERRED, out of this lift**. `home_ctx` stays an **ABSOLUTE** pointer (valid because stacks are fixed). Do not implement `ensure:`/`ifCurtailed:` — the dead path is deleted, not wired (David's call 2026-07-01; oevm#25 stays open for the later heap-closure phase).
- **Test harness is NOT concurrency-safe** ([[test-harness-not-concurrency-safe]]): run the suite and gates **serially**; re-run a red solo before believing it.
- **Structural intent gates required** ([[behavior-proofs-vs-intent-proofs]]): behavior-equivalence does not prove a construct was removed. Each demolition task ends with a `grep`-clean assertion scoped to the **live** `src/` artifacts.
- **Locate edit sites by quoted code, not line numbers** ([[subagents-match-quoted-code]]) — line numbers below are orientation only.

---

## File Structure

- `oevm/src/vm.c` — the flat loop, the 4 reload blocks, `primitive_fiber_create`, `primitive_fiber_switch`, `fiber_trampoline`, `push_frame`, `run_interpreter` prologue. **Most changes live here.**
- `oevm/src/gc.c` — `scan_c_stack` (deleted), `g_fiber_registry` walk + unregister (deleted), `StackVtable`/`scan_vm_stack` precise tracers (kept), `cleanup_cl` marks (deleted), `vm_c_stack_base`/`gc_set_stack_base` (deleted).
- `oevm/src/oevm.h` — `struct Fiber` (drop ucontext fields), `struct MethodContext` (drop `cleanup_cl`), the `#include <ucontext.h>` block + `OEVM_FIBER_CSTACK_SIZE`.
- `oevm/src/oevm_main.c` — drop the `gc_set_stack_base(&oevm_stack_anchor)` call.
- `oevm/src/dispatch.c` — `primitive_method_context_execute_cleanup` (deleted) + its registration.
- `oevm/tests/test_fibers.c` — C-level fiber unit test; extend with the `ABBBAA→ABABAB` assertion if not already present.
- `ns-tests/04_somns_basic/45_fibers.ns` — `.ns` interleave conformance test; strengthen ordering assertion.
- `oevm/docs/fiber_derecursify_core_design.md` — mark Phase 3 DONE (§5).
- `ns-oevm/docs/NEWSPEAK_DIALECT.md` — no new dialect row expected (no language change); confirm and note.

---

### Task 1: Universal single-stack fiber switch (the core mechanism swap)

Make the Zephyr single-C-stack reload mechanism universal: reload blocks always compiled, `primitive_fiber_switch` returns the value instead of `swapcontext`ing, and `primitive_fiber_create` builds the initial activation frame on the fiber's `Stack` for all targets. After this task there is one C activation for all fibers on every platform. Dead ucontext/trampoline/registry code is left in place (removed in Task 2) to keep this task focused and the build green.

**Files:**
- Modify: `oevm/src/vm.c` — 4 reload blocks, `primitive_fiber_switch`, `primitive_fiber_create`
- Test: `oevm/tests/test_fibers.c` (existing), `ns-tests/04_somns_basic/45_fibers.ns` (existing)

**Interfaces:**
- Consumes: `push_frame` semantics, the `saved_pc-1` src2 decode (`(*(saved_pc-1) >> 24) & 0xFF`), `frame_link_encode/decode` (frame_abi.h), global VM state (`current_fiber`, `g_active_stack`, `vm_stack`, `vm_stack_ptr`, `current_frame`).
- Produces: `primitive_fiber_switch(oop receiver, int argc, oop *args)` — swaps globals and **returns `return_val`** on all platforms (no `swapcontext`); the caller's flat loop reload block re-syncs. `primitive_fiber_create(oop receiver, int argc, oop *args)` — builds the initial `MethodContext` on `new_fib->stack_obj->data[0]` with `saved_pc == NULL`, `regs[0] = closure`, for all platforms.

- [ ] **Step 1: Make the 4 reload blocks always-compiled.** In `oevm/src/vm.c`, each of the four fiber-resume blocks is wrapped in `#ifdef __ZEPHYR__` / `#endif`. Find each by its guard body:

```c
            if (ctx != current_frame) {
                ctx = current_frame;
                method = (struct CompiledMethod *)closure_entry((struct Closure *)ctx->closure);
                if (ctx->saved_pc != NULL) {
                    pc = ctx->saved_pc;
                    uint32_t *inst_ptr = (uint32_t *)(ctx->saved_pc - 1);
                    uint8_t suspended_src2 = (*inst_ptr >> 24) & 0xFF;
                    ctx->regs[suspended_src2] = result;
                } else {
                    pc = cm_instructions(method);
                }
                pc_end = cm_instructions(method) + method->instruction_count;
                break;
            }
```

Remove the `#ifdef __ZEPHYR__` line above each of the four occurrences (OP_SEND, OP_BLOCK_INVOKE, OP_SEND_IMPLICIT, OP_SEND_SUPER) and the matching `#endif` below, so the block is unconditional. **Leave the block bodies unchanged.** This is safe even while POSIX still `swapcontext`s: on a POSIX resume the fiber returns inside `primitive_fiber_switch` with its own C locals intact and `ctx == current_frame`, so the guard is false and the block is skipped. Keep the leading comment but drop the "POSIX never reaches here" clause.

- [ ] **Step 2: Collapse `primitive_fiber_switch` to return-the-value.** Find the tail of `primitive_fiber_switch`:

```c
#ifndef __ZEPHYR__
    /* POSIX: switch C call stacks via swapcontext.
       Store the transfer value before the switch; on resume the other side
       will have stored its own transfer value in g_fiber_switch_value for us. */
    g_fiber_switch_value = return_val;
    /* Record where this fiber is parking its C stack ... */
    {
        volatile char sp_anchor;
        curr_fib->c_stack_sp = (char *)&sp_anchor;
    }
    swapcontext(&curr_fib->uctx, &target_fib->uctx);
    /* Execution resumes here when another fiber switches back to us.
       Return the value that was transferred TO us by that switch. */
    return g_fiber_switch_value;
#else
    return return_val;
#endif
```

Replace the whole `#ifndef __ZEPHYR__ … #else … #endif` with a single line and comment:

```c
    /* Single-stack switch: the globals were swapped above; the caller's flat-loop
       reload block re-syncs pc/ctx/method and delivers return_val into the
       suspended dest register. No per-fiber C stack, no swapcontext. */
    return return_val;
```

- [ ] **Step 3: Build the initial frame on the Stack for all platforms in `primitive_fiber_create`.** Find the `#ifndef __ZEPHYR__ … #else … #endif` split (the POSIX branch does ucontext/malloc/registry; the Zephyr branch builds the frame). Delete the `#ifndef __ZEPHYR__` POSIX branch **and its `#else`/`#endif` guards**, keeping the frame-building body unconditional:

```c
    /* Build the initial activation record on the fiber's Newspeak stack.
       primitive_fiber_switch updates current_frame to this frame and the flat
       loop's reload block picks it up (saved_pc == NULL ⇒ start at method entry). */
    {
        struct Closure *cl = (struct Closure *)closure;
        struct CompiledMethod *method = (struct CompiledMethod *)closure_entry(cl);
        size_t frame_words = sizeof(struct MethodContext) / sizeof(oop) + method->num_registers;
        struct MethodContext *ctx = (struct MethodContext *)&((struct Stack *)new_fib->stack_obj)->data[0];
        new_fib->stack_ptr = frame_words;
        new_fib->current_context = ctx;
        { struct Stack *zs = (struct Stack *)new_fib->stack_obj; zs->sp = (uint32_t)frame_words; zs->current_off = 0; }

        ctx->vtable = NULL;
        ctx->prev_frame = NULL;  /* bottom sentinel; relative encoding: NULL == no caller */
        ctx->saved_pc = NULL;
        ctx->closure = closure;
        ctx->home_ctx = cl->home_ctx ? cl->home_ctx : (oop)ctx;
        ctx->gen_tag.tag32 = ++current_generation_tag;
        ctx->cleanup_cl = NULL;

        ctx->regs[0] = closure;
        for (int i = 1; i < method->num_registers; i++) {
            ctx->regs[i] = NULL;
        }
    }
```

Also delete the now-orphaned POSIX field seeds earlier in the function (`new_fib->entry_closure = NULL; new_fib->entry_receiver = NULL;` under `#ifndef __ZEPHYR__`) — leave them for Task 2 **only if** they still compile; if the struct fields are still present (they are until Task 2), leaving those `= NULL` seeds is harmless. Prefer to delete the `#ifndef __ZEPHYR__ … #endif` seed block here now since the fields are going away in Task 2.

- [ ] **Step 4: Build the runner (Path A).** Run:

```bash
cd /home/david/workspace/oevm && rm -f oevm_runner && \
gcc -Wall -Wextra -std=c99 src/vm.c src/gc.c src/dispatch.c src/platform.c src/solidified_image.c src/oevm_main.c -o oevm_runner
```
Expected: compiles clean (warnings about now-unused `fiber_trampoline`/ucontext fields are acceptable this task; they are removed in Task 2).

- [ ] **Step 5: Run the fiber unit test + suite both paths.** Run serially:

```bash
cd /home/david/workspace/oevm && python3 tools/run_tests_oevm.py
```
Then the Path-B / `OEVM_PATH=AB` suite per the established gate recipe (design doc §6), and the C fiber unit test:
```bash
cd /home/david/workspace/oevm && gcc -Wall -Wextra -std=c99 -DOEVM_UNIT_TEST tests/test_fibers.c src/vm.c src/gc.c src/dispatch.c src/platform.c src/solidified_image.c -o /tmp/test_fibers 2>/dev/null && /tmp/test_fibers; echo "exit=$?"
```
Expected: suite green both paths (baseline 81/0/6); `45_fibers.ns` PASS; `test_fibers` PASS. If `test_fibers.c`'s exact build recipe differs, use the one embedded in its header comment.

- [ ] **Step 6: `-D__ZEPHYR__` host syntax build.** The Zephyr path is now the universal path, so this must stay clean:

```bash
cd /home/david/workspace/oevm && gcc -D__ZEPHYR__ -Wall -Wextra -std=c99 -fsyntax-only src/vm.c src/gc.c src/dispatch.c && echo "ZEPHYR-SYNTAX-OK"
```
Expected: `ZEPHYR-SYNTAX-OK` (unused-`fiber_trampoline` warnings acceptable; `-fsyntax-only` won't error on them).

- [ ] **Step 7: Commit.**

```bash
cd /home/david/workspace/oevm && git add -A && git commit -m "$(cat <<'EOF'
feat(derecursify): universal single-stack fiber switch (Phase 3 core)

Make the flat-loop reload mechanism universal: reload blocks always
compiled, primitive_fiber_switch returns the transfer value (no
swapcontext), primitive_fiber_create builds the initial frame on the
fiber Stack for all platforms. One C activation for all fibers.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_014BtRk5kkx9rVwbPB8N3VnK
EOF
)"
```

---

### Task 2: Demolish ucontext / trampoline / registry

Delete the machinery that Task 1 made dead: `fiber_trampoline`, the `<ucontext.h>` include + `OEVM_FIBER_CSTACK_SIZE`, the ucontext/C-stack fields of `struct Fiber`, `g_fiber_registry` + its unregister code, and the suspended-fiber registry walk inside `scan_c_stack` (the active-stack conservative scan itself is removed in Task 3). One demolition, gated by a structural grep.

**Files:**
- Modify: `oevm/src/vm.c` (delete `fiber_trampoline`, `g_fiber_registry` global), `oevm/src/oevm.h` (`#include <ucontext.h>` block, `struct Fiber` fields), `oevm/src/gc.c` (registry walk in `scan_c_stack`, unregister loop, `entry_closure`/`entry_receiver` marks, `fiber_c_stack_top` helper)

**Interfaces:**
- Consumes: nothing new.
- Produces: `struct Fiber` reduced to `{ base, stack_obj, current_context, stack_ptr, status, single_step, hook_step_request }` — no `uctx`, `c_stack`, `entry_closure`, `entry_receiver`, `c_stack_sp`, `reg_next`.

- [ ] **Step 1: Delete `fiber_trampoline`.** In `oevm/src/vm.c`, remove the whole function:

```c
static void fiber_trampoline(uint32_t hi, uint32_t lo) {
    ...
    fprintf(stderr, "VM Error: fiber trampoline returned unexpectedly\n");
    abort();
}
```

- [ ] **Step 2: Delete the ucontext include + C-stack-size macro.** In `oevm/src/oevm.h`, remove:

```c
#ifndef __ZEPHYR__
#include <ucontext.h>
#define OEVM_FIBER_CSTACK_SIZE (256 * 1024)   /* 256 KB per fiber C stack */
#endif
```

- [ ] **Step 3: Trim `struct Fiber`.** In `oevm/src/oevm.h`, delete the entire `#ifndef __ZEPHYR__ … #endif` block of POSIX fields:

```c
#ifndef __ZEPHYR__
    ucontext_t uctx;
    char      *c_stack;
    oop        entry_closure;
    oop        entry_receiver;
    char      *c_stack_sp;
    struct Fiber *reg_next;
#endif
```
Leaving `struct Fiber { struct Object base; oop stack_obj; struct MethodContext *current_context; size_t stack_ptr; oop status; int single_step; int hook_step_request; };`.

- [ ] **Step 4: Delete `g_fiber_registry` and its uses in vm.c.** Remove the global `struct Fiber *g_fiber_registry = NULL;` and any `new_fib->reg_next = g_fiber_registry; g_fiber_registry = new_fib;` registration (removed with the POSIX branch in Task 1 — verify none remains). Remove the comment block at vm.c:66-68 referencing per-fiber malloc'd C stacks / `scan_c_stack`.

- [ ] **Step 5: Delete the registry walk + ucontext scan in `scan_c_stack` (gc.c).** Remove the `#ifndef __ZEPHYR__` suspended-fiber loop:

```c
#ifndef __ZEPHYR__
    for (struct Fiber *fib = g_fiber_registry; fib != NULL; fib = fib->reg_next) {
        if ((oop)fib == current_fiber) continue;
        if (fib->c_stack_sp == NULL) continue;
        scan_c_range(fib->c_stack_sp, fiber_c_stack_top(fib));
        scan_c_range((char *)&fib->uctx, (char *)&fib->uctx + sizeof fib->uctx);
    }
#endif
```
and the active-fiber `fiber_c_stack_top` branch:
```c
#ifndef __ZEPHYR__
    if (current_fiber != NULL) {
        top = fiber_c_stack_top((struct Fiber *)current_fiber);
    }
#endif
```
(leaving `char *top = (char *)vm_c_stack_base;`), plus the `fiber_c_stack_top` helper definition and the fiber-unregister loop in the sweep (gc.c ~760):
```c
{
    extern struct Fiber *g_fiber_registry;
    struct Fiber **link = &g_fiber_registry;
    while (*link != NULL) { if (*link == fib) { *link = fib->reg_next; break; } link = &(*link)->reg_next; }
    fib->reg_next = NULL;
}
```

- [ ] **Step 6: Delete the `entry_closure`/`entry_receiver` GC marks (gc.c).** Remove:

```c
#ifndef __ZEPHYR__
        mark(fib->entry_closure);
        mark(fib->entry_receiver);
#endif
```
The entry closure is now reachable via `mark(fib->stack_obj)` (it lives in `regs[0]` of the frame built at create) — that `mark(fib->stack_obj);` stays.

- [ ] **Step 7: Build both paths + fiber tests + `-D__ZEPHYR__`.** Repeat Task 1 Steps 4–6 (Path A build, suite both paths, `test_fibers`, `-D__ZEPHYR__` syntax). Expected: clean, no unused-function/field warnings now; suite green both paths; `45_fibers.ns` PASS.

- [ ] **Step 8: Structural intent gate — grep-clean the live artifacts.**

```bash
cd /home/david/workspace/oevm && \
! grep -rnE 'ucontext|swapcontext|makecontext|getcontext|fiber_trampoline|g_fiber_registry|reg_next|c_stack_sp|OEVM_FIBER_CSTACK_SIZE|fiber_c_stack_top|entry_closure|entry_receiver' src/ \
&& echo "DEMOLITION-CLEAN"
```
Expected: `DEMOLITION-CLEAN` (no matches in `src/`).

- [ ] **Step 9: Commit.**

```bash
cd /home/david/workspace/oevm && git add -A && git commit -m "$(cat <<'EOF'
refactor(derecursify): demolish ucontext/trampoline/fiber-registry (Phase 3)

Delete the per-fiber C-stack machinery made dead by the universal
single-stack switch: fiber_trampoline, <ucontext.h>, struct Fiber
POSIX fields, g_fiber_registry + unregister, the suspended-fiber
registry walk in scan_c_stack. grep-clean gate green.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_014BtRk5kkx9rVwbPB8N3VnK
EOF
)"
```

---

### Task 3: Delete the conservative C-stack scan + handlerize bare-local roots

Precise roots now cover all fiber execution state (`scan_vm_stack` for the active `Stack`; the `StackVtable` tracer for suspended `Stack`s). Delete `scan_c_stack`, `vm_c_stack_base`/`gc_set_stack_base`, the `setjmp`/`jmp_buf` register flush, and the call site. Then handlerize the one known oop that was live **only** in a C local across a GC-triggering allocation — `new_fib_oop` in `primitive_fiber_create` — using the existing `gc_temp_root_push`/`gc_temp_roots_clear` API, and hunt for any others via `OEVM_GC_STRESS=1` + ASan. This is the highest-risk task; its gate is exhaustive.

**Files:**
- Modify: `oevm/src/gc.c` (delete `scan_c_stack`, `vm_c_stack_base`, `gc_set_stack_base`, the `scan_c_stack()` call in `gc_collect`), `oevm/src/oevm_main.c` (drop `gc_set_stack_base` call), `oevm/src/vm.c` (`primitive_fiber_create` temp-root; comment fixups)

**Interfaces:**
- Consumes: `gc_temp_root_push(oop)` / `gc_temp_roots_clear(void)` (gc.c:633/644) — marked as roots in `gc_collect` (gc.c:876).
- Produces: GC rooting is fully precise (stack-slot + temp-roots + globals); no conservative C-stack scan on any platform.

- [ ] **Step 1: Protect `new_fib_oop` in `primitive_fiber_create`.** Immediately after `struct Fiber *new_fib = (struct Fiber *)new_fib_oop;`, before the `stack_alloc` call that can trigger GC, add a temp root; clear it before returning. Find:

```c
    oop new_fib_oop = gc_allocate(SuspendedFiberVtable ? SuspendedFiberVtable : FiberVtable, sizeof(struct Fiber));
    struct Fiber *new_fib = (struct Fiber *)new_fib_oop;
```
Add below it:
```c
    /* new_fib_oop lives only in a C local until it is returned and stored by the
       .ns facade. With the conservative C-stack scan gone, a GC triggered by
       stack_alloc() below would otherwise reclaim it — pin it as a temp root. */
    extern void gc_temp_root_push(oop o);
    extern void gc_temp_roots_clear(void);
    gc_temp_root_push(new_fib_oop);
```
And immediately before `return new_fib_oop;` at the end of the function:
```c
    gc_temp_roots_clear();
    return new_fib_oop;
```
Update the now-stale comment that says new_fib is "reachable … via the conservative C-stack scan" to reference the temp root instead.

- [ ] **Step 2: Delete `scan_c_stack` and its call.** In `oevm/src/gc.c`, remove the entire `static void scan_c_stack(void) { … }` function and its call `scan_c_stack();` in `gc_collect` (leaving `scan_vm_stack();` and the temp-roots/globals marking). Remove the `scan_c_range` helper **only if** it has no other caller (grep first; it is used by `scan_c_stack` — if that is its sole user, delete it too).

- [ ] **Step 3: Delete the stack-base capture apparatus.** Remove from `oevm/src/gc.c`:

```c
void *vm_c_stack_base = NULL;
void gc_set_stack_base(void *p) { vm_c_stack_base = p; }
```
and from `oevm/src/oevm_main.c`:
```c
extern void gc_set_stack_base(void *p);
...
gc_set_stack_base(&oevm_stack_anchor);
```
(remove the `oevm_stack_anchor` local if now unused). Fix any remaining `vm_c_stack_base` references in comments (vm.c:259, vm.c:67 already gone from Task 2).

- [ ] **Step 4: Build Path A + run suite both paths.** Repeat Task 1 Steps 4–5. Expected: green both paths.

- [ ] **Step 5: GC-stress + ASan gate (the risk gate).** Build with GC-on-every-allocation and ASan, run the gc-stress test and the fiber tests, both paths:

```bash
cd /home/david/workspace/oevm && \
gcc -DOEVM_GC_STRESS=1 -fsanitize=address -g -Wall -Wextra -std=c99 \
  src/vm.c src/gc.c src/dispatch.c src/platform.c src/solidified_image.c src/oevm_main.c -o /tmp/oevm_gcstress_asan && \
OEVM_GC_STRESS=1 /tmp/oevm_gcstress_asan; echo "runner exit=$?"
```
Then run `ns-tests/06_limit_tests/05_gc_stress.ns` and `ns-tests/04_somns_basic/45_fibers.ns` through the harness under the GC-stress/ASan build (use the established `fibers-gc-stress` and `boot-gc-asan` gate recipes named in design doc §6). Expected: **no ASan report, no use-after-free, suite green.** If ASan flags a freed oop, that is a newly-exposed bare-C-local root — handlerize it with `gc_temp_root_push` at the allocation site (same pattern as Step 1), re-run until clean. Record every site fixed in the commit message.

- [ ] **Step 6: `-D__ZEPHYR__` host build.** `cd /home/david/workspace/oevm && gcc -D__ZEPHYR__ -Wall -Wextra -std=c99 -fsyntax-only src/vm.c src/gc.c src/dispatch.c && echo ZEPHYR-SYNTAX-OK`. Expected: `ZEPHYR-SYNTAX-OK`.

- [ ] **Step 7: Structural intent gate.**

```bash
cd /home/david/workspace/oevm && \
! grep -rnE 'scan_c_stack|vm_c_stack_base|gc_set_stack_base|setjmp|jmp_buf' src/ \
&& echo "CONSERVATIVE-SCAN-GONE"
```
Expected: `CONSERVATIVE-SCAN-GONE`. (If `scan_c_range` was kept for another caller, confirm that caller is legitimate.)

- [ ] **Step 8: Commit.**

```bash
cd /home/david/workspace/oevm && git add -A && git commit -m "$(cat <<'EOF'
refactor(derecursify): delete conservative C-stack scan; precise roots only (Phase 3)

Flat dispatch + first-class Stacks make every fiber activation a precise
root (scan_vm_stack for active, StackVtable tracer for suspended). Delete
scan_c_stack + setjmp register flush + vm_c_stack_base capture. Handlerize
new_fib_oop (and any GC-stress-exposed bare locals) via gc_temp_root_push.
OEVM_GC_STRESS=1 + ASan clean both paths.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_014BtRk5kkx9rVwbPB8N3VnK
EOF
)"
```

---

### Task 4: Delete the dead `cleanup_cl` / `ensure:`/`ifCurtailed:` path

`cleanup_cl` is only ever assigned `NULL`, has no kernel/compiler user, and — per David's decision (2026-07-01) — is deleted rather than implemented (oevm#25 stays open for the later heap-closure phase). Remove the frame word, its seeds, its GC marks, and the orphaned `primitive_method_context_execute_cleanup` handler.

**Files:**
- Modify: `oevm/src/oevm.h` (`struct MethodContext` — remove `cleanup_cl`), `oevm/src/vm.c` (`ctx->cleanup_cl = NULL` seeds in `push_frame`, `run_interpreter` prologue; the OP_NLR comment; `primitive_method_context_execute_cleanup` if defined here), `oevm/src/dispatch.c` (`primitive_method_context_execute_cleanup` + its registration if there), `oevm/src/gc.c` (`mark(frame->cleanup_cl)` in `scan_vm_stack` and the `StackVtable` tracer)

**Interfaces:**
- Consumes: nothing.
- Produces: `struct MethodContext` without the `cleanup_cl` field. No runtime frame layout is serialized ⇒ no image change (Global Constraints).

- [ ] **Step 1: Confirm no live user.** Run:
```bash
cd /home/david/workspace && grep -rn "cleanup_cl\|execute_cleanup\|executeCleanup\|ifCurtailed\|ensure:" oevm/src ns-oevm/lib ns-oevm/tools ns-tests | grep -v "\.md:" 
```
Expected: matches only in `oevm/src` (the dead C path) — **no** `.ns` (kernel/compiler) or test user. If a `.ns` user appears, STOP and escalate ([[escalate-design-reversals]]) — the delete premise is wrong.

- [ ] **Step 2: Remove the field.** In `oevm/src/oevm.h`, delete `cleanup_cl` from `struct MethodContext`. Check `oevm/src/frame_abi.h` for any hardcoded frame-word offset or field-index constant that names or counts `cleanup_cl`; update the frame layout comment/offsets accordingly (the header enumerates the frame words). Also check `ns-oevm/tools/solidify.py` for any `cleanup_cl`/frame-offset reference (frames are runtime, so none expected — confirm).

- [ ] **Step 3: Remove the seeds.** Delete every `ctx->cleanup_cl = NULL;` in `push_frame`, the `run_interpreter` prologue, and `primitive_fiber_create` (Task 1's frame builder — remove that line too).

- [ ] **Step 4: Remove the GC marks.** In `oevm/src/gc.c`, delete `mark(frame->cleanup_cl);` in both `scan_vm_stack` and the `StackVtable` tracer.

- [ ] **Step 5: Remove the orphaned handler.** Delete `primitive_method_context_execute_cleanup` (the `if (ctx->cleanup_cl != NULL) send(ctx->cleanup_cl, Symbol_value, 0, NULL);` function) and any registration/`Symbol_` wiring for it. Fix the OP_NLR comment that mentions "running each frame's cleanup_cl".

- [ ] **Step 6: Build both paths + suite + fibers + `-D__ZEPHYR__`.** Repeat Task 1 Steps 4–6. Expected: green both paths; `45_fibers.ns` PASS.

- [ ] **Step 7: Structural intent gate.**
```bash
cd /home/david/workspace/oevm && ! grep -rn "cleanup_cl\|execute_cleanup" src/ && echo "CLEANUP-DEAD-PATH-GONE"
```
Expected: `CLEANUP-DEAD-PATH-GONE`.

- [ ] **Step 8: Commit.**
```bash
cd /home/david/workspace/oevm && git add -A && git commit -m "$(cat <<'EOF'
refactor(derecursify): delete dead cleanup_cl / ensure: path (Phase 3, oevm#25)

cleanup_cl was only ever NULL with no kernel/compiler user. Delete the
frame word, seeds, GC marks, and the orphaned execute_cleanup handler.
Real ensure:/ifCurtailed: deferred to the heap-closure phase (oevm#25
stays open). No image change (frames are not serialized).

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_014BtRk5kkx9rVwbPB8N3VnK
EOF
)"
```

---

### Task 5: `ABBBAA → ABABAB` correctness proof + `home_ctx`-across-stacks validation

The design names the fiber-correctness proof: the interleave that the old Zephyr hijack got wrong (`ABBBAA`) must now yield `ABABAB` on the flat engine, verified on both paths **and** under the `-D__ZEPHYR__` host build (which exercises the exact mechanism the hardware runs). Also validate the §1 caveat: a fiber entry block reading a captured lexical (`home_ctx`) across `Stack`s while the spawner frame is live.

**Files:**
- Create/Modify: `ns-tests/04_somns_basic/45_fibers.ns` (strengthen to assert strict `ABABAB` ordering) or a new `ns-tests/04_somns_basic/4X_fiber_interleave.ns`
- Create/Modify: `oevm/tests/test_fibers.c` (add a C-level interleave assertion if the `.ns` test can't express strict ordering)

**Interfaces:**
- Consumes: `Concurrency spawn:` / `yield` / `run` (kernel.ns facade).
- Produces: a committed regression test whose expected output is exactly the `ABABAB` interleave.

- [ ] **Step 1: Write the failing-if-broken interleave test.** Add a `.ns` test that spawns two fibers A and B, each printing its label twice with a `yield` between prints, driven by the cooperative scheduler, e.g. expected output `A B A B A B` (or the precise interleave the `Concurrency` scheduler produces — determine it by reading `kernel.ns` `Concurrency>>run`). Encode the exact expected lines in the test's `(* expect: … *)` metadata block. The point: a mechanism that reuses activations (the old `ABBBAA` bug) produces a different, wrong order.

Example shape (adjust labels/counts to the scheduler's actual semantics):
```
(* expect:
A1
B1
A2
B2
*)
| c |
c:: Concurrency new: kernel.
c spawn: [ Transcript showCr: 'A1'. c yield. Transcript showCr: 'A2' ].
c spawn: [ Transcript showCr: 'B1'. c yield. Transcript showCr: 'B2' ].
c run.
```

- [ ] **Step 2: Run it both paths — verify it passes (proving correct interleave).**
```bash
cd /home/david/workspace/oevm && python3 tools/run_tests_oevm.py 2>&1 | grep -i fiber
```
Then the Path-B run. Expected: the interleave test PASSes both paths with the `ABABAB`-style order. If it produces `ABBBAA`-style output, the mechanism is wrong — STOP and debug ([[superpowers:systematic-debugging]]) before proceeding.

- [ ] **Step 3: Prove it on the `-D__ZEPHYR__` host build.** Build the host `.oimg` image runner with the Zephyr flag and run the fiber image (per `oevm/docs/fiber_implementation.md:30`, `gcc -D__ZEPHYR__ … oevm_image_main.c`):
```bash
cd /home/david/workspace/oevm && \
gcc -D__ZEPHYR__ -Wall -Wextra -std=c99 src/vm.c src/gc.c src/dispatch.c src/platform.c oevm_image_main.c -o /tmp/oevm_zephyr_host 2>&1 | tail -5
```
Run the fiber interleave `.oimg` under `/tmp/oevm_zephyr_host` (use the image the harness builds for `45_fibers.ns`, or the `test_fibers.c` `-D__ZEPHYR__` build). Expected: `ABABAB`-style order — the historically-broken case is now correct on the exact mechanism the hardware executes.

- [ ] **Step 4: Validate the `home_ctx`-across-stacks caveat.** Add a test where a spawned fiber's entry block reads an outer temp/`self` captured lexically (so it dereferences `home_ctx` into the spawner's `Stack`), then `yield`s and reads it again after resuming. Expected: the captured value survives across the switch (absolute `home_ctx` into a fixed `Stack` stays valid while the spawner frame is live). Encode expected output in the test.

- [ ] **Step 5: Run full fiber tests both paths + `test_fibers` C unit.** Expected: all PASS.

- [ ] **Step 6: Commit.**
```bash
cd /home/david/workspace && git -C oevm add -A && git -C ns-tests add -A && \
git -C ns-tests commit -m "$(cat <<'EOF'
test(derecursify): ABBBAA->ABABAB fiber interleave proof + home_ctx-across-stacks (Phase 3)

Regression test asserting the correct fiber interleave the old Zephyr
hijack got wrong; validated both paths and on the -D__ZEPHYR__ host
build. Plus a lexical-capture-across-Stacks test for the fixed-stack
absolute home_ctx invariant.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_014BtRk5kkx9rVwbPB8N3VnK
EOF
)" && git -C oevm commit -m "test(derecursify): -D__ZEPHYR__ host fiber interleave proof (Phase 3)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_014BtRk5kkx9rVwbPB8N3VnK"
```

---

### Task 6: Full gate matrix + docs + issue/memory updates (finish)

Run the complete per-phase gate matrix, refresh the design doc and roadmap, confirm no dialect change, update oevm#25, and record the phase in memory. Deliverable: a green matrix and accurate docs.

**Files:**
- Modify: `oevm/docs/fiber_derecursify_core_design.md` (§5 mark Phase 3 DONE; move Phase-3-inherited items to resolved), `ns-oevm/docs/superpowers/plans/2026-06-25-ns-zephyr-capability-node-roadmap.md` (Phase 3 status), `ns-oevm/docs/NEWSPEAK_DIALECT.md` (confirm/annotate no new row)

- [ ] **Step 1: Run the full gate matrix serially (both paths).** Per design doc §6:
  - Full suite `OEVM_PATH=AB` (expect ≥ 81/0/6; the 2 new fiber tests raise the pass count).
  - `oevm/tests/test_fibers.c` unit + `-D__ZEPHYR__` fiber unit.
  - `ns-tests/04_somns_basic/45_fibers.ns` interleave + the new interleave/capture tests.
  - `oevm/tests/test_portability_32.sh` (32-bit).
  - `boot-gc-asan` + `OEVM_GC_STRESS=1` + `-fsanitize=address` clean.
  - `fibers-gc-stress` gate.
  Record the exact pass/fail/skip line. Expected: all green.

- [ ] **Step 2: Perf gate (optional but recommended).** Run Perfstex (`ns-zephyr`) or `oevm/bench.sh` and confirm no regression vs the Phase-2 baseline (flat dispatch + no swapcontext should be neutral-to-better on fiber-heavy loops). Record numbers.

- [ ] **Step 3: Update the design doc.** In `oevm/docs/fiber_derecursify_core_design.md` §5, mark **Phase 3 DONE**; note the resolved inheritances: (a) the 4 reload blocks are now universal (not `#ifdef __ZEPHYR__`); (b) `cleanup_cl`/`ensure:` dead path **deleted** (oevm#25 remains for real impl); (c) conservative scan **deleted**; (d) `valueWithArguments:` still an ordinary send (unchanged, still deferred). State the `ABBBAA→ABABAB` proof result.

- [ ] **Step 4: Confirm no dialect change.** No language/bytecode/facade change ⇒ no new `NEWSPEAK_DIALECT.md` row. Add a one-line note in the dialect doc's changelog that Phase 3 was a C-mechanism swap with no dialect impact (so the absence is intentional, not an omission).

- [ ] **Step 5: Update issues + roadmap.** Comment on oevm#25 (dead `cleanup_cl` path deleted this phase; real `ensure:`/`ifCurtailed:` still open, tied to heap-closure work). Update the roadmap doc's Phase 3 line to DONE. File any newly-surfaced deferred items ([[file-issues-for-deferred-work]]).

- [ ] **Step 6: Commit docs.**
```bash
cd /home/david/workspace && git -C oevm add -A && git -C ns-oevm add -A && \
git -C oevm commit -m "docs(derecursify): mark Phase 3 (fibers + demolition) DONE

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_014BtRk5kkx9rVwbPB8N3VnK" && \
git -C ns-oevm commit -m "docs(derecursify): Phase 3 roadmap + dialect no-change note

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_014BtRk5kkx9rVwbPB8N3VnK"
```

- [ ] **Step 7: Final whole-branch review + memory.** Request a final review ([[superpowers:requesting-code-review]] / opus whole-branch) before any merge/push. Do **not** push until David says so ([[push-only-when-asked]]). Update MEMORY with Phase 3 outcome (both paths green, `ABBBAA→ABABAB` proven, demolition complete, oevm#25 open).

---

## Self-Review

**Spec coverage (design doc §5 Phase 3 items):**
- Swap-Stack `fiberSwitch:value:` → Task 1 (mechanism swap; the `.ns` `fiberSwitch:value:` is unchanged, the C `primitive_fiber_switch` becomes single-stack). ✓
- Delete ucontext / trampoline / registry → Task 2. ✓
- Delete conservative C-stack scan + Task-2b apparatus → Task 3. ✓
- `#ifdef __ZEPHYR__` hijack + POSIX twin both vanish, ONE mechanism → Task 1 (gates removed) + Task 2 (twin deleted). ✓
- Validate `ABBBAA → ABABAB` (host `-D__ZEPHYR__`) → Task 5. ✓
- Inherited (b) dead `ensure:`/`ifCurtailed:` implement-or-delete → Task 4 (delete, per David). ✓
- `home_ctx` stays ABSOLUTE + §1 caveat validation → Global Constraints + Task 5 Step 4. ✓
- Deferred (growth, heap-closure) explicitly OUT → Global Constraints. ✓

**Placeholder scan:** every code step quotes the actual code to find/replace and the exact build/gate command. No TBD/"handle edge cases"/"similar to Task N". ✓

**Type consistency:** `primitive_fiber_switch`/`primitive_fiber_create` signatures unchanged across tasks; `struct Fiber` reduction (Task 2) and `struct MethodContext` reduction (Task 4) are each single-task and self-contained; `gc_temp_root_push`/`gc_temp_roots_clear` names match gc.c. ✓

**Risk note:** Task 3 (conservative-scan delete) is the one with unknown scope — new bare-local roots may surface under GC-stress+ASan. Its gate is designed to find and fix them iteratively; if the set is large, the executor should pause and report before forcing it.
