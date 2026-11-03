# De-recursify deferred lift — Phase 3: growable stacks Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the fixed-capacity per-fiber VM stack + `abort()`-on-overflow with a `realloc`-style grow-on-demand stack (the footprint win: start small, grow only as needed), and a controlled diagnostic-termination hard cap.

**Architecture:** Both frame links (`prev_frame` dynamic + `home_ctx` lexical) are already relative SMIs, so a copy-to-larger `Stack` survives with zero fixups to stored links. The only care needed is **live C-local raw pointers into the old stack** held across a grow: `current_frame` (global, rebased by the grow helper), `args`/`prev_ctx` (rebased in `push_frame`), and — the one structural change — `run_interpreter`'s `entry_ctx` and `prev_ctx`, which are held across the whole invocation and become **offsets/links** instead of pointers. One new helper `stack_ensure` centralises the grow.

**Tech Stack:** C99, the oevm VM (`oevm/src/vm.c`, `dispatch.c`, `gc.c`), the `.ns`→bytecode + both image paths (Path A solidified C, Path B `.oimg`); tests are `.ns` files in `ns-tests/04_somns_basic/` run by `ns-oevm/tools/run_tests_oevm.py`.

## Global Constraints

- **Design of record:** `oevm/docs/design/2026-07-02-derecursify-phase3-growable-stacks-design.md` (scope **A**, DECIDED). Read it before starting.
- **Behaviour-equivalent below the cap.** Same observable output on every existing test, BOTH paths. Growth is invisible to the language; the cap diagnostic is a new observable but not a language-semantics change → **no dialect row expected** (confirm in Task 4).
- **Both image paths.** All changes are in `vm.c` runtime (compiled by both Path A and Path B); **no `.oimg` format change** — `Stack`/growth are runtime-only. No `builtins.def` change, no new vtable, no `solidify.py` change. Task 4 verifies the serializer/loader are untouched.
- **Repos:** VM code in `~/workspace/oevm`; tests in `~/workspace/ns-tests`; runner + gates in `~/workspace/ns-oevm/tools/run_tests_oevm.py`. Commit each repo separately. **Do not push** (David's push-only-when-asked rule).
- **Gate (every task that touches the VM):** full suite BOTH paths + all 5 existing gates must stay green:
  ```
  cd ~/workspace/ns-oevm && OEVM_PATH=AB python3 tools/run_tests_oevm.py
  ```
  Plus, when the VM changed: ASan/`OEVM_GC_STRESS=1` (the boot-gc-asan + fibers-gc-stress gates cover this) and `-D__ZEPHYR__ -fsyntax-only` syntax-clean:
  ```
  cd ~/workspace/oevm && gcc -std=c99 -Isrc -D__ZEPHYR__ -DOEVM_APPLIANCE -fsyntax-only src/vm.c
  ```
- **Runner is NOT concurrency-safe** (writes fixed `/tmp` fixtures). Run the suite **solo/serial**; re-run a red once solo before believing it.
- **Locate edit sites by the quoted anchor code, not line numbers** (line numbers drift as you edit).

---

## File Structure

- `oevm/src/vm.c` — all growth code: `stack_ensure` helper (new) + growth knobs; edits to `push_frame`, the `run_interpreter` inline frame-setup + its `entry_ctx`/`prev_ctx`/exit; `primitive_fiber_create` + bootstrap init size; the cap diagnostic.
- `ns-tests/04_somns_basic/58_growable_stack.ns` — new; deep recursion that forces ≥1 grow (Task 1).
- `ns-tests/04_somns_basic/59_grow_under_fibers.ns` — new; a fiber grows while another holds live frames (Task 3).
- `ns-oevm/tools/run_tests_oevm.py` — two new gates: `stack-overflow-cap` (Task 2) and `growable-stack-small-init` (Task 3).
- Docs + memory + `progress.md` ledger (Task 4).

---

### Task 1: Growth mechanism + sizing knobs + reload-surface

**Files:**
- Modify: `oevm/src/vm.c` (growth knobs; `stack_ensure`; `push_frame`; `run_interpreter` inline setup + `entry_ctx`/`prev_ctx`/exit; `primitive_fiber_create`; bootstrap)
- Test: `ns-tests/04_somns_basic/58_growable_stack.ns` (new)

**Interfaces:**
- Produces: `static ptrdiff_t stack_ensure(size_t extra_words, oop receiver)` — grows the active fiber stack if `vm_stack_ptr + extra_words` exceeds the Stack's capacity; returns base delta in words (0 if no grow); repoints `vm_stack`/`g_active_stack`/`current_frame`/active-fiber `stack_obj`; terminates (Task 2 fills the diagnostic) at the cap. Callers rebase live C-local stack pointers by the returned delta.
- Produces: growth knobs `VM_STACK_INIT_WORDS` (16384), `VM_STACK_CAP_WORDS` (262144), `VM_STACK_GROW_NUM`/`_DEN` (2/1). `VM_STACK_MAX_WORDS` is **retired**.

- [ ] **Step 1: Write the failing test** — `ns-tests/04_somns_basic/58_growable_stack.ns`

```
(*
Growable stack (de-recursify deferred lift, Phase 3).

`sum: n` recurses n frames deep. At the default init capacity (16384 words,
~17 words/frame -> overflows ~950 frames on the FIXED stack), depth 2000 needs
~34000 words: it forces the stack to GROW (16384 -> 32768 -> 65536, i.e. TWO
reallocs) and still compute the correct result on the way back up. Before Phase 3
this aborted with "Stack Overflow" around depth ~950; growth makes it succeed.
Relative prev_frame/home_ctx links survive the copy-to-larger with zero fixups.
sum 1..2000 == 2001000.

expect:
2001000
skip-berry: true
*)
class GrowableStackTest packageUsing: manifest = () (
sum: n = (
    n = 0 ifTrue: [ ^ 0 ].
    ^ n + (self sum: n - 1)
)
public main: platform args: args = (
    (self sum: 2000) println
)
) : (
)
```

- [ ] **Step 2: Run it to verify it FAILS (current engine aborts)**

Run:
```bash
cd ~/workspace/ns-oevm && OEVM_PATH=A python3 tools/run_tests_oevm.py 58_growable_stack
```
Expected: FAIL — runtime crash "VM Error: Stack Overflow" (the fixed stack overflows near depth ~950). (If the runner filters by name differently, run the full suite and confirm `58_growable_stack` is the only new failure.)

- [ ] **Step 3: Add the growth knobs.** In `oevm/src/vm.c`, replace the block anchored by `#ifndef VM_STACK_MAX_WORDS`:

```c
#ifndef VM_STACK_MAX_WORDS
#define VM_STACK_MAX_WORDS 16384   /* host default; -D lower for embedded */
#endif
```

with:

```c
/* De-recursify Phase 3: grow-on-demand stacks. INIT is the initial per-fiber
   capacity (host default == the legacy fixed size, so host runs never grow and
   behave/perform exactly as before; -D lower on embedded to start small and grow
   on demand — the footprint win). CAP is the hard ceiling before controlled
   termination (StackOverflow is not yet catchable in-image; see the Phase 3
   design). GROW_NUM/_DEN is the growth factor (default 2x). */
#ifndef VM_STACK_INIT_WORDS
#define VM_STACK_INIT_WORDS 16384
#endif
#ifndef VM_STACK_CAP_WORDS
#define VM_STACK_CAP_WORDS 262144   /* 1 MB on 32-bit */
#endif
#ifndef VM_STACK_GROW_NUM
#define VM_STACK_GROW_NUM 2
#endif
#ifndef VM_STACK_GROW_DEN
#define VM_STACK_GROW_DEN 1
#endif
```

- [ ] **Step 4: Add `stack_ensure`** immediately ABOVE `push_frame` (find the anchor comment `/* Push a new bytecode-method/block activation onto the active fiber stack`). Insert:

```c
/* De-recursify Phase 3: ensure the active fiber stack can hold `extra_words`
   above vm_stack_ptr, growing (realloc-copy-to-larger) if needed. Relative
   prev_frame/home_ctx links survive the copy with ZERO fixups (frame_abi.h) — only
   the base moves. Returns the base delta in words (new_base - old_base), 0 when no
   grow happened, so callers can rebase any live C-local raw pointers into the old
   stack. Repoints vm_stack / g_active_stack / current_frame / the active Fiber's
   stack_obj. `receiver` is a not-yet-stored C local at the push site — temp-root it
   across the grow's allocation (the only new GC point; the old stack stays
   g_active_stack and fully rooted during stack_alloc). At the hard cap, prints a
   diagnostic and controlled-terminates (policy A, Task 2). */
extern int gc_temp_roots_count;
extern void gc_temp_root_push(oop o);
static ptrdiff_t stack_ensure(size_t extra_words, oop receiver) {
    struct Stack *old = (struct Stack *)g_active_stack;
    if (vm_stack_ptr + extra_words <= old->capacity) return 0;   /* fast path: fits */

    size_t need = vm_stack_ptr + extra_words;
    if (need > (size_t)VM_STACK_CAP_WORDS) {
        /* Task 2 replaces this abort() with the full diagnostic. */
        fprintf(stderr, "VM Error: Stack Overflow\n");
        abort();
    }
    size_t newcap = (size_t)old->capacity * VM_STACK_GROW_NUM / VM_STACK_GROW_DEN;
    if (newcap < need) newcap = need;
    if (newcap > (size_t)VM_STACK_CAP_WORDS) newcap = (size_t)VM_STACK_CAP_WORDS;

    int saved_roots = gc_temp_roots_count;
    gc_temp_root_push(receiver);
    oop new_stack_oop = stack_alloc((uint32_t)newcap);
    gc_temp_roots_count = saved_roots;

    struct Stack *neu = (struct Stack *)new_stack_oop;
    memcpy(neu->data, old->data, vm_stack_ptr * sizeof(oop));
    neu->sp = old->sp;
    neu->current_off = old->current_off;

    ptrdiff_t delta = neu->data - old->data;
    if (current_frame != NULL)
        current_frame = (struct MethodContext *)((oop *)current_frame + delta);
    g_active_stack = new_stack_oop;
    vm_stack = neu->data;
    ((struct Fiber *)current_fiber)->stack_obj = new_stack_oop;
    return delta;
}
```

- [ ] **Step 5: Wire `stack_ensure` into `push_frame`.** Find the anchor in `push_frame`:

```c
    size_t frame_words = sizeof(struct MethodContext) / sizeof(oop) + m->num_registers;
    if (vm_stack_ptr + frame_words > VM_STACK_MAX_WORDS) {
        fprintf(stderr, "VM Error: Stack Overflow\n");
        abort();
    }
    struct MethodContext *ctx = (struct MethodContext *)&vm_stack[vm_stack_ptr];
```

Replace it with (note: `prev_ctx` was previously read ABOVE `frame_words`; delete that earlier `struct MethodContext *prev_ctx = current_frame;` line at the top of `push_frame` and re-read it AFTER the grow):

```c
    size_t frame_words = sizeof(struct MethodContext) / sizeof(oop) + m->num_registers;
    /* Grow-on-demand (Phase 3). Capture the OLD base+capacity so we can rebase an
       in-stack `args` pointer after a grow; prev_ctx is re-read from current_frame
       (which stack_ensure rebased). */
    oop *old_base = vm_stack;
    uint32_t old_cap = ((struct Stack *)g_active_stack)->capacity;
    ptrdiff_t delta = stack_ensure(frame_words, receiver);
    if (delta != 0 && args != NULL && args >= old_base && args < old_base + old_cap)
        args = args + delta;
    struct MethodContext *prev_ctx = current_frame;
    struct MethodContext *ctx = (struct MethodContext *)&vm_stack[vm_stack_ptr];
```

Then at the TOP of `push_frame`, delete the now-duplicate line:

```c
    struct MethodContext *prev_ctx = current_frame;
```
(Keep the `struct Closure *cl = ...` and `struct CompiledMethod *m = ...` lines; only the `prev_ctx` line moves down.)

- [ ] **Step 6: Wire `stack_ensure` into the `run_interpreter` inline frame-setup.** Find the anchor:

```c
    struct MethodContext *prev_ctx = current_frame;

    // Allocate stack frame sequentially (Fast Path: vtable starts as NULL)
    size_t frame_words = sizeof(struct MethodContext) / sizeof(oop) + method->num_registers;
    if (vm_stack_ptr + frame_words > VM_STACK_MAX_WORDS) {
        fprintf(stderr, "VM Error: Stack Overflow\n");
        abort();
    }

    struct MethodContext *ctx = (struct MethodContext *)&vm_stack[vm_stack_ptr];
    size_t saved_stack_ptr = vm_stack_ptr;
```

Replace with (grow BEFORE capturing `prev_ctx`; save the caller link as an OFFSET so exit_frame restores it correctly even if the stack grew during this invocation):

```c
    // Allocate stack frame sequentially (Fast Path: vtable starts as NULL)
    size_t frame_words = sizeof(struct MethodContext) / sizeof(oop) + method->num_registers;
    /* Grow-on-demand (Phase 3): rebase an in-stack `args` after a grow. */
    {
        oop *old_base = vm_stack;
        uint32_t old_cap = ((struct Stack *)g_active_stack)->capacity;
        ptrdiff_t delta = stack_ensure(frame_words, receiver);
        if (delta != 0 && args != NULL && args >= old_base && args < old_base + old_cap)
            args = args + delta;
    }
    struct MethodContext *prev_ctx = current_frame;
    /* prev_ctx (the caller frame) is restored to current_frame at exit_frame; a
       NESTED push during this invocation may grow the stack and move it, so hold it
       as a base-relative link (survives the copy) rather than a raw pointer. */
    oop entry_prev_link = frame_link_encode(vm_stack, prev_ctx);
    struct MethodContext *ctx = (struct MethodContext *)&vm_stack[vm_stack_ptr];
    size_t saved_stack_ptr = vm_stack_ptr;
```

Delete the ORIGINAL `struct MethodContext *prev_ctx = current_frame;` that sat ABOVE the comment (the one anchored just before `// Allocate stack frame sequentially`); the replacement re-declares it below. (There must be exactly one `prev_ctx` declaration in `run_interpreter` after this edit.)

- [ ] **Step 7: Convert `entry_ctx` to an offset.** Find:

```c
    struct MethodContext *entry_ctx = ctx;
```
Replace with:
```c
    /* Bottom-out marker held for the whole invocation. A nested push may grow the
       stack and move frames, so track it as a base-relative OFFSET, not a pointer. */
    size_t entry_off = (size_t)((oop *)ctx - vm_stack);
```

Find the bottom-out comparison:
```c
            if (ctx == entry_ctx || caller == NULL) {
                goto exit_frame;
            }
```
Replace with:
```c
            if ((size_t)((oop *)ctx - vm_stack) == entry_off || caller == NULL) {
                goto exit_frame;
            }
```

Find the unwind-boundary comparison:
```c
            int at_boundary = (uf == entry_ctx || ucaller == NULL);
```
Replace with:
```c
            int at_boundary = ((size_t)((oop *)uf - vm_stack) == entry_off || ucaller == NULL);
```

- [ ] **Step 8: Fix the exit_frame restore.** Find:
```c
    vm_stack_ptr = saved_stack_ptr;
    current_frame = prev_ctx;
```
Replace with:
```c
    vm_stack_ptr = saved_stack_ptr;
    current_frame = frame_link_decode(vm_stack, entry_prev_link);
```

- [ ] **Step 9: Init size at bootstrap + `primitive_fiber_create`.** Find the bootstrap:
```c
        g_active_stack = stack_alloc(VM_STACK_MAX_WORDS);
```
Replace with:
```c
        g_active_stack = stack_alloc(VM_STACK_INIT_WORDS);
```
Find in `primitive_fiber_create`:
```c
    size_t cap = VM_STACK_MAX_WORDS;
```
Replace with:
```c
    size_t cap = VM_STACK_INIT_WORDS;
```

- [ ] **Step 10: Confirm `VM_STACK_MAX_WORDS` is fully retired.**

Run:
```bash
cd ~/workspace/oevm && grep -rn "VM_STACK_MAX_WORDS" src/ | grep -v "VM_STACK_INIT_WORDS"
```
Expected: no matches. (If any remain outside the retired `#define`, replace with the appropriate `VM_STACK_INIT_WORDS`/capacity-based logic.)

- [ ] **Step 11: Run the new test — verify it PASSES both paths.**
```bash
cd ~/workspace/ns-oevm && OEVM_PATH=AB python3 tools/run_tests_oevm.py 58_growable_stack
```
Expected: PASS on Path A and Path B, output `2001000`.

- [ ] **Step 12: Run the FULL suite both paths + all 5 gates.**
```bash
cd ~/workspace/ns-oevm && OEVM_PATH=AB python3 tools/run_tests_oevm.py
```
Expected: all tests pass BOTH paths (was 84/0/6, now 85/0/6 with `58_growable_stack`); `grant`, `debugger-integration`, `portability-32`, `fibers-gc-stress`, `boot-gc-asan` all PASS. Then syntax-check Zephyr:
```bash
cd ~/workspace/oevm && gcc -std=c99 -Isrc -D__ZEPHYR__ -DOEVM_APPLIANCE -fsyntax-only src/vm.c && echo ZEPHYR-SYNTAX-OK
```
Expected: `ZEPHYR-SYNTAX-OK`.

- [ ] **Step 13: Commit (two repos).**
```bash
cd ~/workspace/oevm && git add src/vm.c && git commit -m "feat(derecursify): growable per-fiber stacks (Phase 3 mechanism)

stack_ensure grows the active fiber stack (realloc-copy-to-larger) when a push
would overflow; relative prev_frame/home_ctx links survive the copy with zero
fixups. Reload surface handled: current_frame/g_active_stack/vm_stack/fiber
stack_obj repointed in the helper; args/prev_ctx rebased in push_frame; the
run_interpreter entry_ctx -> offset and prev_ctx -> base-relative link so a
nested grow can't dangle them. Split VM_STACK_INIT_WORDS (initial, == legacy
16384 host default) from VM_STACK_CAP_WORDS (hard cap); retire VM_STACK_MAX_WORDS.
New fibers + bootstrap start at INIT and grow on demand (embedded footprint win
via -D small init). receiver temp-rooted across the grow's alloc.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
cd ~/workspace/ns-tests && git add 04_somns_basic/58_growable_stack.ns && git commit -m "test(derecursify): 58_growable_stack — deep recursion forces stack grow (Phase 3)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 2: Hard-cap policy A — controlled diagnostic termination + gate

**Files:**
- Modify: `oevm/src/vm.c` (the cap branch inside `stack_ensure`)
- Modify: `ns-oevm/tools/run_tests_oevm.py` (new `stack-overflow-cap` gate)
- Test fixture: `ns-tests/04_somns_basic/` — a bounded-but-deep recursion driven past a **tiny** `-DVM_STACK_CAP_WORDS` build (reuse `58_growable_stack.ns`; no new `.ns` needed)

**Interfaces:**
- Consumes: `stack_ensure` (Task 1).
- Produces: at the cap, a clear stderr diagnostic (fiber pointer, needed words, cap) + `abort()` (non-zero exit) for all fibers. Node-survival on worker overflow is deferred with the exception infra (design §4 A).

- [ ] **Step 1: Write the failing gate.** In `ns-oevm/tools/run_tests_oevm.py`, add a gate function near the other gates (after `run_boot_gc_asan_gate`). It builds the suite runner for `58_growable_stack.ns` with a tiny cap so depth-2000 recursion blows past it, and asserts controlled termination:

```python
def run_stack_overflow_cap_gate() -> bool:
    """Phase 3 hard-cap policy A: at VM_STACK_CAP_WORDS the VM prints a clear
    diagnostic and controlled-terminates (non-zero exit), rather than running
    unbounded. Compile 58_growable_stack (recurses to depth 2000) to Path-A
    solidified_image.c, then build with a tiny cap its ~34000-word depth cannot
    fit -> the grow hits the cap. Mirrors run_test_file's Path-A compile + the
    fibers-gc-stress gate's build shape."""
    import tempfile
    oevm_dir = OEVM_DIR
    test_ns = Path(__file__).parent.parent.parent / "ns-tests" / "04_somns_basic" / "58_growable_stack.ns"
    compiler = Path(__file__).parent / "ns_compile_oevm.py"
    target_c = oevm_dir / "src" / "solidified_image.c"   # in-place (serial gate; clobbers shared file)
    comp = subprocess.run([sys.executable, str(compiler), str(test_ns), str(target_c)],
                          capture_output=True, text=True)
    if comp.returncode != 0:
        print(f"{COLOR_RED}stack-overflow-cap: FAIL{COLOR_RESET} (compile .ns)")
        print("  " + (comp.stderr or comp.stdout).strip()[-500:])
        return False
    with tempfile.TemporaryDirectory() as td:
        runner = Path(td) / "cap_runner"
        build = subprocess.run(
            ["gcc", "-Wall", "-std=c99", "-Isrc", "-DOEVM_APPLIANCE",
             "-DVM_STACK_CAP_WORDS=2048", "-DVM_STACK_INIT_WORDS=1024",
             "src/vm.c", "src/gc.c", "src/dispatch.c", "src/platform.c",
             "src/serial.c", "src/udp.c", "src/mirror.c", *OEVM_BACKEND,
             "src/solidified_image.c", "src/oevm_main.c", "-o", str(runner), "-lm"],
            cwd=str(oevm_dir), capture_output=True, text=True)
        if build.returncode != 0:
            print(f"{COLOR_RED}stack-overflow-cap: FAIL{COLOR_RESET} (build)")
            print("  " + build.stderr.strip()[-500:])
            return False
        run = subprocess.run([str(runner)], cwd=str(oevm_dir),
                             capture_output=True, text=True, timeout=30)
        # Controlled termination: non-zero exit AND the diagnostic on stderr.
        if run.returncode == 0 or "Stack Overflow" not in run.stderr:
            print(f"{COLOR_RED}stack-overflow-cap: FAIL{COLOR_RESET} "
                  f"(exit={run.returncode}, stderr tail={run.stderr.strip()[-200:]!r})")
            return False
    print(f"{COLOR_GREEN}stack-overflow-cap: PASSED{COLOR_RESET}")
    return True
```

> **Note for implementer:** this gate compiles `58_growable_stack.ns` **in-place** to `oevm/src/solidified_image.c` (the same target `run_test_file` uses), so it must run in the **serial** gate section, not concurrently with the suite (the shared file is the documented concurrency footgun). `sys` and `tempfile` are already imported at the top of the runner (confirm; add if missing). Register the gate in `main()` alongside the others (call it in the serial-gate block + add its PASS/FAIL to the summary line). The `58_growable_stack.ns` Path-A compile that the suite runs afterward will re-clobber `solidified_image.c` — fine, each build recompiles it.

- [ ] **Step 2: Run the gate — verify it FAILS** (Task 1 left a bare `fprintf("Stack Overflow")` + abort in the cap branch, so it may already partially pass; the point of this step is to confirm the gate detects the behaviour). Run the runner from `main()` or invoke the suite:
```bash
cd ~/workspace/ns-oevm && OEVM_PATH=A python3 tools/run_tests_oevm.py 2>&1 | grep stack-overflow-cap
```
Expected: the gate runs and reports (PASS if the Task-1 stub abort already fires; if the diagnostic string differs, FAIL — Step 3 sets the final text).

- [ ] **Step 3: Finalise the cap diagnostic** in `stack_ensure`. Replace the Task-1 stub:
```c
    if (need > (size_t)VM_STACK_CAP_WORDS) {
        /* Task 2 replaces this abort() with the full diagnostic. */
        fprintf(stderr, "VM Error: Stack Overflow\n");
        abort();
    }
```
with:
```c
    if (need > (size_t)VM_STACK_CAP_WORDS) {
        /* Phase 3 hard-cap policy A: controlled diagnostic termination. Catchable
           in-image StackOverflow (kill just the fiber, node survives) is deferred
           with the general exception facility — see the Phase 3 design §4. */
        fprintf(stderr,
            "VM Error: Stack Overflow — fiber %p exceeded the stack cap "
            "(needed %zu words, cap %d). Unbounded recursion? StackOverflow is not "
            "yet catchable in-image.\n",
            (void *)current_fiber, need, (int)VM_STACK_CAP_WORDS);
        abort();
    }
```

- [ ] **Step 4: Run the gate — verify it PASSES.**
```bash
cd ~/workspace/ns-oevm && OEVM_PATH=A python3 tools/run_tests_oevm.py 2>&1 | grep stack-overflow-cap
```
Expected: `stack-overflow-cap: PASSED`.

- [ ] **Step 5: Full suite both paths + all gates still green** (the tiny-cap build is gate-only; the default-cap suite is unaffected).
```bash
cd ~/workspace/ns-oevm && OEVM_PATH=AB python3 tools/run_tests_oevm.py
```
Expected: 85/0/6 both paths; all now-6 gates PASS.

- [ ] **Step 6: Commit (two repos).**
```bash
cd ~/workspace/oevm && git add src/vm.c && git commit -m "feat(derecursify): Phase 3 hard-cap policy A — controlled diagnostic termination

At VM_STACK_CAP_WORDS stack_ensure prints a clear diagnostic (fiber, needed
words, cap) and aborts, instead of the terse pre-Phase-3 message. Catchable
in-image StackOverflow (worker fiber dies + node survives) is deferred with the
exception facility it needs (design §4 A).

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
cd ~/workspace/ns-oevm && git add tools/run_tests_oevm.py && git commit -m "test(derecursify): stack-overflow-cap gate — tiny-cap build must terminate cleanly

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 3: Grow-under-fibers + multi-realloc stress gate

**Files:**
- Test: `ns-tests/04_somns_basic/59_grow_under_fibers.ns` (new)
- Modify: `ns-oevm/tools/run_tests_oevm.py` (new `growable-stack-small-init` gate — many reallocs incl. during boot, under ASan)

**Interfaces:**
- Consumes: `stack_ensure` (Task 1); the fiber facade (`kernel concurrency`, `spawn:`, `run`) as used by `45_fibers.ns`.

- [ ] **Step 1: Write `59_grow_under_fibers.ns`.** A spawned fiber recurses deep enough to grow its own stack while the scheduler (main fiber) holds live frames on its own (unchanged) stack — proving per-fiber growth isolation + GC over a mix of grown and un-grown stacks. Model the fiber shape on `45_fibers.ns`.

```
(*
Grow-under-fibers (de-recursify deferred lift, Phase 3).

A spawned worker fiber recurses ~2000 deep (forcing its OWN stack to grow while
the scheduler fiber holds live frames on its separate, un-grown stack). Proves
per-fiber growth isolation: growing one fiber's Stack (realloc-copy + repoint)
does not disturb another fiber's frames, and GC traces a mix of grown and
un-grown Stacks. Worker computes sum 1..2000 == 2001000; main prints it after the
scheduler drains.

expect:
2001000
done
skip-berry: true
*)
class GrowUnderFibersTest packageUsing: manifest = () (
sum: n = (
    n = 0 ifTrue: [ ^ 0 ].
    ^ n + (self sum: n - 1)
)
public main: platform args: args = ( | c result |
    c:: kernel concurrency.
    c spawn: [ result:: self sum: 2000 ].
    c run.
    result println.
    'done' println
)
) : (
)
```

> **Note for implementer:** honour the `45_fibers` constraint documented in `kernel.ns` (call `spawn:`/`run` at the same stack depth; don't spawn from within a running fiber). Adjust the exact facade calls to match how `45_fibers.ns` drives `concurrency`/`spawn:`/`run` (read it first). If capturing `result` across the fiber boundary needs the `55_fiber_capture` pattern, mirror that instead. The invariant under test is "one fiber grows, another's frames stay valid," not the specific accumulation idiom.

- [ ] **Step 2: Run it — verify it PASSES both paths** (this is a positive test of Task 1's mechanism, not a pre-written failure):
```bash
cd ~/workspace/ns-oevm && OEVM_PATH=AB python3 tools/run_tests_oevm.py 59_grow_under_fibers
```
Expected: PASS both paths, output `2001000` / `done`. If it fails, that's a real growth-isolation bug in Task 1 — debug via systematic-debugging before proceeding.

- [ ] **Step 3: Run it under GC stress** (grown + un-grown stacks traced under collection pressure):
```bash
cd ~/workspace/oevm && OEVM_GC_STRESS=1 <build+run 59 via the runner or a direct Path-A build>
```
Concretely, confirm the fibers-gc-stress-style path: the existing `fibers-gc-stress` gate only runs `45_fibers`; extend coverage by running the full suite once with stress on:
```bash
cd ~/workspace/ns-oevm && OEVM_GC_STRESS=1 OEVM_PATH=A python3 tools/run_tests_oevm.py 59_grow_under_fibers
```
Expected: PASS (correct output, no crash/leak).

- [ ] **Step 4: Add the `growable-stack-small-init` gate** to `run_tests_oevm.py` — builds the suite runner for `58_growable_stack.ns` with a SMALL init (`-DVM_STACK_INIT_WORDS=1024`) under ASan, so growth reallocs many times (including during boot) and any missed live-pointer rebase surfaces as a use-after-free:

```python
def run_growable_small_init_gate() -> bool:
    """Phase 3: build with a small init capacity so the stack reallocs MANY times
    (during boot and deep recursion). Under ASan a missed live-pointer rebase across
    a grow shows up as a use-after-free on the swept old Stack. Compiles + runs
    58_growable_stack and checks the correct result + clean ASan. Same in-place
    Path-A compile as the cap gate (serial-only)."""
    import tempfile
    oevm_dir = OEVM_DIR
    test_ns = Path(__file__).parent.parent.parent / "ns-tests" / "04_somns_basic" / "58_growable_stack.ns"
    compiler = Path(__file__).parent / "ns_compile_oevm.py"
    target_c = oevm_dir / "src" / "solidified_image.c"
    comp = subprocess.run([sys.executable, str(compiler), str(test_ns), str(target_c)],
                          capture_output=True, text=True)
    if comp.returncode != 0:
        print(f"{COLOR_RED}growable-stack-small-init: FAIL{COLOR_RESET} (compile .ns)")
        print("  " + (comp.stderr or comp.stdout).strip()[-500:])
        return False
    with tempfile.TemporaryDirectory() as td:
        runner = Path(td) / "small_init_runner"
        build = subprocess.run(
            ["gcc", "-g", "-fsanitize=address", "-Wall", "-std=c99", "-Isrc",
             "-DOEVM_APPLIANCE", "-DVM_STACK_INIT_WORDS=1024",
             "src/vm.c", "src/gc.c", "src/dispatch.c", "src/platform.c",
             "src/serial.c", "src/udp.c", "src/mirror.c", *OEVM_BACKEND,
             "src/solidified_image.c", "src/oevm_main.c", "-o", str(runner), "-lm"],
            cwd=str(oevm_dir), capture_output=True, text=True)
        if build.returncode != 0:
            if "fsanitize" in (build.stderr or ""):
                print(f"{COLOR_YELLOW}growable-stack-small-init: SKIP{COLOR_RESET} (no ASan)")
                return True
            print(f"{COLOR_RED}growable-stack-small-init: FAIL{COLOR_RESET} (build)")
            print("  " + build.stderr.strip()[-500:])
            return False
        run = subprocess.run([str(runner)], cwd=str(oevm_dir),
                             capture_output=True, text=True, timeout=60)
        out = [l.strip() for l in run.stdout.strip().splitlines() if l.strip()]
        if run.returncode != 0 or out != ["2001000"]:
            print(f"{COLOR_RED}growable-stack-small-init: FAIL{COLOR_RESET} "
                  f"(exit={run.returncode}, out={out}, stderr={run.stderr.strip()[-200:]!r})")
            return False
    print(f"{COLOR_GREEN}growable-stack-small-init: PASSED{COLOR_RESET}")
    return True
```

Register it in `main()` (call + summary line), same as the other gates.

- [ ] **Step 5: Run the new gate — verify it PASSES.**
```bash
cd ~/workspace/ns-oevm && OEVM_PATH=A python3 tools/run_tests_oevm.py 2>&1 | grep growable-stack-small-init
```
Expected: `growable-stack-small-init: PASSED` (or `SKIP` on a host without ASan).

- [ ] **Step 6: Full suite both paths + all gates.**
```bash
cd ~/workspace/ns-oevm && OEVM_PATH=AB python3 tools/run_tests_oevm.py
```
Expected: 86/0/6 both paths (adds `59_grow_under_fibers`); all now-7 gates PASS.

- [ ] **Step 7: Commit (two repos).**
```bash
cd ~/workspace/ns-tests && git add 04_somns_basic/59_grow_under_fibers.ns && git commit -m "test(derecursify): 59_grow_under_fibers — per-fiber growth isolation (Phase 3)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
cd ~/workspace/ns-oevm && git add tools/run_tests_oevm.py && git commit -m "test(derecursify): growable-stack-small-init gate — many-realloc + boot-grow under ASan

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 4: Docs, dialect check, ledger + memory

**Files:**
- Modify: `oevm/docs/design/2026-07-02-derecursify-phase3-growable-stacks-design.md` (mark DONE)
- Modify: `oevm/src/frame_abi.h` (comment: growth now live) — **only** if a comment is now stale; no code change
- Check: `oevm/docs/NEWSPEAK_DIALECT.md` (dialect row?) + `.oimg` format unchanged
- Modify: `ns-oevm/.superpowers/sdd/progress.md` (ledger)
- Modify: memory files (David's auto-memory) — deferred to the human/session, note in report

- [ ] **Step 1: Verify no `.oimg` format change.**
```bash
cd ~/workspace/oevm && grep -rn "format_version\|VERSION" tools/oimg_serialize.py 2>/dev/null | head
cd ~/workspace/ns-oevm && grep -rn "format_version\|VERSION" tools/*.py 2>/dev/null | grep -i version | head
```
Expected: no version bump was needed (growth is runtime-only). Confirm the `.oimg` Path-B suite already passed in Tasks 1–3 (it did → loader/serializer untouched). Record this in the report.

- [ ] **Step 2: Dialect check.** Growth is behaviour-equivalent below the cap; the cap diagnostic is a new observable but not a language-semantics change. Confirm no existing test's expected output changed (the suite is green with only ADDED tests). Decision: **no dialect row**. If David disagrees on review, add one. Grep to confirm no dialect-guarded behaviour shifted:
```bash
cd ~/workspace/oevm && grep -n "Dialect\|dialect #" docs/NEWSPEAK_DIALECT.md | tail -5
```
Note the current dialect number in the report; do not add a row unless review requires it.

- [ ] **Step 3: Mark the design DONE.** In the spec, change the status line to `DONE 2026-07-02` and add a one-paragraph "What shipped" note (the mechanism, the knobs, the `entry_off`/`prev_link` conversion, the two gates, suite count 86/0/6, deferred worker-survival). Commit.

- [ ] **Step 4: Update `frame_abi.h` comment** if stale — the header says the reified-`Context` branch "remains RESERVED (not produced) — the growth/escape phase builds it." Growth (this phase) does NOT build reify (escape does). Confirm the comment is still accurate (it is: growth needs only relative links, which exist). No change unless inaccurate.

- [ ] **Step 5: Update the SDD ledger** `ns-oevm/.superpowers/sdd/progress.md` with the Phase 3 task outcomes.

- [ ] **Step 6: Commit docs.**
```bash
cd ~/workspace/oevm && git add docs/ && git commit -m "docs(derecursify): Phase 3 growable stacks DONE

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
cd ~/workspace/ns-oevm && git add .superpowers/sdd/progress.md && git commit -m "docs(derecursify): Phase 3 ledger

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

- [ ] **Step 7: Report for review** — summarise for the final whole-branch opus review + David's push decision: files touched, suite/gate results both paths, the deferred worker-survival + catchable-StackOverflow (file/keep the issue), and the memory-file update needed (`derecursify-deferred-lift.md`: Phase 3 DONE, next = the 3 deferred cross-fiber `home_ctx` readers + reify-on-escape).

---

## Self-Review

**Spec coverage:** §1 (grounded facts) → Task 1 knobs/mechanism. §2 (reload surface table) → Task 1 Steps 4–8 (every row: `stack_ensure` handles globals; `push_frame`/inline handle `args`/`prev_ctx`; `entry_off`/`entry_prev_link` handle the two invocation-lifetime pointers; `vm_stack_ptr`/`saved_stack_ptr` already offsets). §3 (mechanism + knobs) → Task 1. §4 (policy A) → Task 2. §5 (gates + 3 new tests: deep-grow=58, grow-under-fibers=59, hard-cap=gate, small-init multi-realloc=gate) → Tasks 1–3. §6 (risks: ASan+GC-stress small-init makes a missed pointer observable) → Task 3 gate. §7 (deliverable shape 3–4 tasks) → this plan (4 tasks). No `.oimg` change → Task 4 Step 1.

**Placeholder scan:** the two gates have an explicit implementer note to confirm/refactor the single-test solidified-compile helper (`_compile_solidified`) — this is a real "verify the exact name" instruction, not a code placeholder; the gate bodies are complete. `59`'s facade calls carry a "match `45_fibers` shape" note because the exact spawn/capture idiom must mirror the working fiber test — the test's *invariant* and expected output are fully specified.

**Type consistency:** `stack_ensure(size_t, oop) -> ptrdiff_t` used identically in `push_frame` and the inline setup. `entry_off`/`saved_stack_ptr` are `size_t` offsets; `entry_prev_link` is an `oop` link via `frame_link_encode`/`frame_link_decode` (existing `frame_abi.h` helpers). Knob names (`VM_STACK_INIT_WORDS`/`_CAP_WORDS`/`_GROW_NUM`/`_DEN`) consistent across defines, `stack_ensure`, bootstrap, `primitive_fiber_create`, and both gates.
