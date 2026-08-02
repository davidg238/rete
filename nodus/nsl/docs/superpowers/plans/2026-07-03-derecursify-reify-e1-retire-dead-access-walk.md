# Reify-on-escape E1 — Retire the dead `home_ctx` access-walk Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Delete the unreachable `home_ctx` lexical walk in `get_top_level_method_closure` and pin its `home_ctx == NULL` invariant, dropping the last `home_ctx` frame-identity walk from the E3 reify surface — at zero runtime cost, with no new state.

**Architecture:** `get_top_level_method_closure` (`oevm/src/vm.c:1424`) is reached only from `find_defining_vtable` ← the `ACCESS_PRIVATE` branch of `check_access_allowed`. That branch only fires when `receiver == caller_self == current_frame->regs[0]`; a block frame's `regs[0]` is the closure (never a private method's receiver), so the caller here is always a **method** closure (`home_ctx == NULL`) and the walk loop never iterates. Reduce the function to `return closure`, guarded by a fail-fast if `home_ctx != NULL` (should-never-happen). Pure dead-code deletion; behaviour-identical on every reachable path.

**Tech Stack:** C99, GCC. VM in `/home/david/workspace/oevm/src/{vm,dispatch,gc}.c` + `oevm.h`. C unit tests are standalone `main()` programs compiled ad hoc with `gcc`. Conformance harness: `python3 tools/run_tests_oevm.py` in `/home/david/workspace/ns-oevm`.

## Global Constraints

- **Design of record:** `oevm/docs/design/2026-07-03-derecursify-e1-retire-dead-access-walk-design.md`.
- **Repos:** VM code in `/home/david/workspace/oevm`; `.ns` tests in `/home/david/workspace/ns-tests`; runner + gates in `/home/david/workspace/ns-oevm/tools/run_tests_oevm.py`. Commit each repo separately.
- **Branch:** `derecursify-reify-e1-home-method` already exists in `/home/david/workspace/oevm` (holds the design commits). Work on it.
- **Do NOT push** (David's push-only-when-asked rule).
- **No `.oimg`/dialect/GC change.** No `struct Closure` field is added. Latest dialect stays #19; registry VERSION stays 4.
- **Harness is NOT concurrency-safe** — run the suite **solo** and in the **FOREGROUND** (background runs get released mid-run). Re-run a red solo before believing it.
- **This is a refactor with no runtime-observable delta.** There is NO `.ns` red→green regression to write (the scenario is unconstructible — see design §2/§5). The red→green proof is (a) the structural grep gate and (b) `test_access.c` staying green.

---

### Task 1: Retire the dead walk and pin the invariant

**Files:**
- Modify: `/home/david/workspace/oevm/src/vm.c:1424-1432` (`get_top_level_method_closure`)
- Test (existing, must stay green): `/home/david/workspace/oevm/tests/test_access.c`

**Interfaces:**
- Consumes: nothing new.
- Produces: `get_top_level_method_closure(oop closure)` unchanged signature (`static oop`, returns the top-level method closure). Its post-condition is unchanged for all reachable inputs (method closures): returns `closure`. `find_defining_vtable` and `check_access_allowed` are untouched.

- [ ] **Step 1: Write the failing structural gate**

The gate asserts the function body no longer walks `home_ctx`. Run it against the current (unmodified) code so it FAILS first:

```bash
cd /home/david/workspace/oevm && \
awk '/^static oop get_top_level_method_closure/,/^}/' src/vm.c \
  | grep -Eq 'home_ctx_frame|while|for *\(' \
  && echo "GATE FAIL: dead walk still present" \
  || echo "GATE PASS: walk retired"
```

- [ ] **Step 2: Run the gate to verify it fails**

Run the Step-1 command.
Expected: `GATE FAIL: dead walk still present` (the current body contains `while (cl->home_ctx != NULL)` and `home_ctx_frame`).

- [ ] **Step 3: Replace the function body**

In `/home/david/workspace/oevm/src/vm.c`, replace the whole function (lines 1424-1432):

```c
static oop get_top_level_method_closure(oop closure) {
    struct Closure *cl = (struct Closure *)closure;
    while (cl->home_ctx != NULL) {
        struct MethodContext *ctx = home_ctx_frame(cl->home_ctx);
        if (ctx == NULL) break;
        cl = (struct Closure *)ctx->closure;
    }
    return (oop)cl;
}
```

with:

```c
static oop get_top_level_method_closure(oop closure) {
    /* Reify-on-escape E1: the lexical walk this function used to perform is
       unreachable through dispatch. Its only caller (find_defining_vtable, from the
       ACCESS_PRIVATE branch of check_access_allowed) is reached only when
       receiver == caller_self == current_frame->regs[0]; a private method's receiver
       is a user instance, and a block frame's regs[0] is the closure (push_frame at
       OP_BLOCK_INVOKE) — so the closure here is always a METHOD closure
       (home_ctx == NULL) and the old `while` loop never iterated. Removing it drops
       the last home_ctx frame-identity walk (shrinks the E3 reify surface) at zero
       runtime cost. The invariant is pinned: a non-NULL home_ctx here would mean a
       block reached access control, which the model does not produce — fail loudly
       (house posture) rather than silently walk a dead path. See design
       2026-07-03-derecursify-e1-retire-dead-access-walk-design.md §2. */
    if (((struct Closure *)closure)->home_ctx != NULL) {
        fprintf(stderr, "VM Error: access-control method lookup on a block closure "
            "(home_ctx != NULL) — unexpected; see reify-on-escape E1.\n");
        abort();
    }
    return closure;
}
```

- [ ] **Step 4: Run the structural gate to verify it passes**

Run the Step-1 command again.
Expected: `GATE PASS: walk retired`.

- [ ] **Step 5: Build and run `test_access.c` (the sole exerciser of the access branch)**

```bash
cd /home/david/workspace/oevm && TARGET=posix \
gcc -Wall -Wextra -std=c99 -Isrc tests/test_access.c \
  src/solidified_image.c src/dispatch.c src/vm.c src/gc.c \
  src/platform.c src/backend/posix/platform_posix.c \
  src/serial.c src/backend/posix/serial_posix.c \
  src/mirror.c src/udp.c src/backend/posix/udp_posix.c src/oimg_loader.c \
  -lm -o /tmp/test_access && /tmp/test_access; echo "exit=$?"
git -C /home/david/workspace/oevm checkout src/solidified_image.c src/solidified_image.stmap
```

Expected: all six sub-tests print `Passed!`, final line `All OEVM Access Modifier & Dynamic Capability tests PASSED successfully!`, `exit=0`. (A `-Wmissing-field-initializers` warning on `solidified_image.c:3134` for `captured_env` is the pre-existing oevm#32 surface — harmless, ignore. The trailing `git checkout` reverts the harness's touch of the generated image.)

- [ ] **Step 6: Run the full conformance suite, both paths, FOREGROUND + solo**

```bash
cd /home/david/workspace/ns-oevm && OEVM_PATH=AB python3 tools/run_tests_oevm.py
```

Expected: the merged-master baseline **86/0/6 both paths + 7 gates** (unchanged — this task is behaviour-equivalent). If any test is red, re-run solo before believing it (harness not concurrency-safe).

- [ ] **Step 7: `-D__ZEPHYR__` syntax-clean check**

```bash
cd /home/david/workspace/oevm && \
gcc -std=c99 -Isrc -D__ZEPHYR__ -fsyntax-only src/vm.c && echo "zephyr-syntax OK"
```

Expected: `zephyr-syntax OK` (a function-body simplification stays syntax-clean; the identical path compiles under Zephyr).

- [ ] **Step 8: Commit**

```bash
cd /home/david/workspace/oevm && git add src/vm.c && git commit -m "feat(derecursify): E1 — retire the dead home_ctx access-walk in get_top_level_method_closure

The lexical walk was unreachable through dispatch: its only caller path
(find_defining_vtable <- ACCESS_PRIVATE branch of check_access_allowed) is
reached only when receiver==caller_self==current_frame->regs[0], and a block
frame's regs[0] is the closure — so the closure here is always a METHOD closure
(home_ctx==NULL) and the loop never iterated. Reduce to \`return closure\`, pinned
by a home_ctx==NULL invariant fail-fast (loud-not-silent house posture). Drops
the last home_ctx frame-identity walk (shrinks the E3 reify surface). No Closure
field, no GC/.oimg/dialect change; behaviour-identical on all reachable paths.

Validated: structural grep gate (walk gone) + test_access.c green (6/6, the sole
exerciser of the access branch) + full suite 86/0/6 both paths + Zephyr syntax.

Design: docs/design/2026-07-03-derecursify-e1-retire-dead-access-walk-design.md

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01HDjWqD3GNzriGx9E6iiEc6"
```

---

## Self-Review

**Spec coverage:**
- Design §3 (delete walk + invariant fail-fast) → Task 1 Steps 3. ✅
- Design §5 validation: `test_access.c` green → Step 5; grep proof → Steps 1/2/4; house gate both-paths + Zephyr → Steps 6/7; "no `.ns` regression / unconstructible" → stated in Global Constraints. ✅
- Design §4 (no GC/.oimg/dialect change) → asserted in Global Constraints; nothing in the task touches those. ✅

**Placeholder scan:** no TBD/TODO; every step has an exact command and expected output; the full replacement code is shown verbatim. ✅

**Type consistency:** `get_top_level_method_closure(oop closure)` signature and return type are unchanged from the existing code; `struct Closure` / `home_ctx` field names match `oevm.h:139-141`. No new symbols introduced. ✅
