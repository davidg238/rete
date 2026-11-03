# De-recursify Deferred Lift — Phase 2: Relativize `home_ctx` — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn the lexical link `home_ctx` from an absolute stack pointer into the discriminated `{NULL | relative-SMI | reified-Context}` encoding defined in `frame_abi.h`, so it survives a future stack `realloc` (Phase 3 growth) — with zero observable behavior change (stacks still fixed).

**Architecture:** After deferred-lift Phase 1, captured-variable *access* already lives in heap `Context` temp-vectors (`Closure.captured_env` / `OP_LD_CTX`/`OP_ST_CTX`). `home_ctx`'s only remaining roles are (a) the NLR `^`-return target and (b) lexical/vtable resolution walks (`OP_LD_LEXICAL` residual loop-var captures, `primitive_closure_home`, `get_top_level_method_closure`). Both are **same-fiber** in every existing test. This phase routes all `home_ctx` *reads* through one decode chokepoint (`home_ctx_frame`), then flips all `home_ctx` *writes* to `home_link_encode_relative`. The dynamic link `prev_frame` is already relative; this makes the whole frame growth-ready.

**Tech Stack:** C99 (`oevm` VM: `src/vm.c`, `src/dispatch.c`, `src/gc.c`, `src/frame_abi.h`), the frozen L0 `frame_abi.h` helpers, the dual-path test harness (`ns-oevm/tools/run_tests_oevm.py`).

## Global Constraints

- **Behavior-equivalent.** No observable output change, no `.oimg` `format_version` bump, **no dialect row**. The only "new" capability (growth-safe links) is not observable until Phase 3.
- **Both image paths.** Every gate runs `OEVM_PATH=AB` (Path A = solidified-C, Path B = `.oimg`). `Context`/`home_ctx` are runtime-only; the `.oimg` format is untouched. If any `.oimg`-visible structure appears, STOP — it means the encoding leaked into the serialized image.
- **Fixed stacks this phase.** `VM_STACK_MAX_WORDS` unchanged; `abort()` on overflow stays (growth is Phase 3). The reified-`Context` discriminator branch is **reserved but not produced** — guard it with a fail-fast, do not implement reify.
- **Rooting stays fully precise.** The conservative C-stack scan deleted in core Phase 3 does not return. `mark()` ignores SMIs, so a relative-SMI `home_ctx` is a safe no-op for GC.
- **Frozen L0 ABI.** `frame_abi.h`'s `struct Context` and `home_link_*` helpers are the L0 contract — use them, do not amend their signatures without escalating to David.
- **Master gate command** (from `ns-oevm/`): `OEVM_PATH=AB python3 tools/run_tests_oevm.py` — runs the full suite BOTH paths plus all 5 gates (`grant`, `debugger-integration`, `portability-32`, `fibers-gc-stress`, `boot-gc-asan`). **Baseline to preserve: 84 pass / 0 fail / 6 skip both paths, all 5 gates PASS.**
- **Zephyr syntax gate** (from `oevm/`): `gcc -fsyntax-only -D__ZEPHYR__ -Isrc src/vm.c` — must be clean (core Phase 3 removed all `__ZEPHYR__`-conditional fiber code; the identical path compiles).
- **Concurrency caveat.** `run_tests_oevm.py` is NOT concurrency-safe (shared `/tmp` fixtures). Run gates SERIALLY and re-run a red SOLO before believing it.
- **Repos & branch.** Work on branch `derecursify-deferred-phase2` in all touched repos (`oevm`, and `ns-oevm` for docs/ledger). No `ns-tests` change expected. **Do not push** (push only on David's explicit say-so).

---

## File Structure

- `oevm/src/vm.c` — MODIFY. The decode chokepoint `home_ctx_frame` (new static inline), the 3 in-`vm.c` encode sites (`push_frame`, `run_interpreter` setup, `primitive_fiber_create`), and the 4 walk/decode sites (NLR walk, `primitive_closure_home`, `get_top_level_method_closure`, `OP_LD_LEXICAL`/`OP_ST_LEXICAL` via `primitive_closure_home`).
- `oevm/src/dispatch.c` — MODIFY. The 4th encode site: `make_closure` (`home_ctx = (oop)current_frame`).
- `oevm/src/gc.c` — VERIFY-ONLY (+ comment). `mark(frame->home_ctx)` at two sites already no-ops on SMIs; confirm and document.
- `oevm/src/frame_abi.h` — MODIFY (Task 3, comment/status only). Update the "still uses an absolute prev_frame / home_ctx" status note.
- `oevm/tests/test_frame_abi.c` — MODIFY (Task 1). Add the base-move relativity proof for `home_link_encode_relative`/`home_link_decode_frame`.
- `oevm/docs/design/2026-07-01-derecursify-deferred-lift-growable-stacks-upward-closures-design.md` — MODIFY (Task 3). Mark Phase 2 DONE.
- `ns-oevm/.superpowers/sdd/progress.md` — REPLACE (Task 0). Fresh Phase 2 ledger.

---

## Task 0: SDD ledger + branch setup

**Files:**
- Modify: `ns-oevm/.superpowers/sdd/progress.md` (replace with the Phase 2 relativize ledger; archive the old flip ledger content if not already archived)

- [ ] **Step 1: Create the working branch in both repos**

```bash
git -C ~/workspace/oevm checkout -b derecursify-deferred-phase2
git -C ~/workspace/ns-oevm checkout -b derecursify-deferred-phase2
```

Expected: `Switched to a new branch 'derecursify-deferred-phase2'` in each.

- [ ] **Step 2: Record the baseline gate**

Run from `ns-oevm/`:
```bash
OEVM_PATH=AB python3 tools/run_tests_oevm.py
```
Expected (record verbatim in the ledger): `84 pass / 0 fail / 6 skip` both paths; `grant`, `debugger-integration`, `portability-32`, `fibers-gc-stress`, `boot-gc-asan` all PASS.

- [ ] **Step 3: Write the fresh ledger** to `ns-oevm/.superpowers/sdd/progress.md` with: GOAL (relativize `home_ctx`), the baseline numbers from Step 2, the encode/decode site map (below), BASE commits (`oevm` HEAD, `ns-oevm` HEAD), and the task list.

- [ ] **Step 4: Commit the ledger**

```bash
git -C ~/workspace/ns-oevm add .superpowers/sdd/progress.md
git -C ~/workspace/ns-oevm commit -m "chore(derecursify): Phase 2 relativize-home_ctx SDD ledger + baseline"
```

---

## Task 1: Decode chokepoint `home_ctx_frame` (behavior-preserving refactor) + relativity proof

Introduce ONE function that decodes a `home_ctx` value to the `MethodContext*` it refers to, and route every read of `home_ctx` through it. In this task `home_ctx` is STILL stored absolute, so the chokepoint's non-SMI branch returns the pointer as-is — **zero behavior change**. This isolates the "prep" from the "flip" so the reviewer can confirm the routing is a true no-op before semantics change. Also land the standalone relativity proof (independent of the VM).

**Files:**
- Modify: `oevm/src/vm.c` — add `home_ctx_frame`; route NLR walk (`OP_NLR` case), `primitive_closure_home`, `get_top_level_method_closure` through it.
- Test: `oevm/tests/test_frame_abi.c` — add base-move relativity test.

**Interfaces:**
- Produces: `static inline struct MethodContext *home_ctx_frame(oop home)` — decode chokepoint. Contract: `NULL → NULL`; in Task 1, non-`NULL` non-SMI (absolute pointer) → that pointer; SMI (not yet produced) → `home_link_decode_frame(vm_stack, home)`. Task 2 re-points the non-SMI branch to the reified guard.
- Consumes: `frame_abi.h` helpers `home_link_decode_frame`, `IS_SMI`; the global `oop *vm_stack`.

- [ ] **Step 1: Extend the standalone relativity proof (write the failing assertion first)**

In `oevm/tests/test_frame_abi.c`, add a test that a `home_ctx` encoded relative against one base still resolves to the logically-same frame after the whole stack is moved to a new base (simulating Phase 3 `realloc`). Add near the existing `prev_frame` tests:

```c
/* Phase 2: home_link relative offset survives a base move with zero fixups
   (the property that lets Phase 3 realloc the stack). */
static void test_home_link_survives_base_move(void) {
    oop base_a[64] = {0};
    /* a "frame" at word index 5 of base_a */
    struct MethodContext *frame_a = (struct MethodContext *)&base_a[5];
    oop enc = home_link_encode_relative(base_a, frame_a);
    assert(home_link_is_relative(enc));            /* it's a relative SMI */
    assert(home_link_decode_frame(base_a, enc) == frame_a);

    /* Move the stack: copy words to a fresh, differently-located buffer. */
    oop base_b[64] = {0};
    for (int i = 0; i < 64; i++) base_b[i] = base_a[i];
    struct MethodContext *frame_b_expected = (struct MethodContext *)&base_b[5];
    /* Same stored offset, new base → resolves to the moved frame, no fixup. */
    assert(home_link_decode_frame(base_b, enc) == frame_b_expected);
    printf("test_home_link_survives_base_move: PASS\n");
}
```

Register the call in `main()` of `test_frame_abi.c` alongside the existing tests.

- [ ] **Step 2: Run it — verify it passes immediately (the helpers are already correct L0 code)**

```bash
cd ~/workspace/oevm && cc -std=c99 -Isrc tests/test_frame_abi.c -o /tmp/test_frame_abi && /tmp/test_frame_abi
```
Expected: all lines PASS including `test_home_link_survives_base_move: PASS`. (This is a characterization proof of the frozen helpers, not a red→green cycle — the L0 helpers already exist; the test locks in the relativity property this phase depends on.)

- [ ] **Step 3: Add the `home_ctx_frame` chokepoint to `vm.c`**

Add near the top of `vm.c` (after the includes / `frame_abi.h` is already included), above `primitive_closure_home`:

```c
/* Decode a home_ctx discriminator value (frame_abi.h Option B) to the home
   MethodContext frame it refers to, on the CURRENT fiber's stack.
     NULL            -> NULL (no lexical home: a method's own closure)
     relative SMI    -> stack frame at vm_stack[offset] (same fiber)
     absolute ptr    -> (Task 1 only) returned as-is; Task 2 re-points this to
                        the reified-Context guard once encode is relativized.
   Single chokepoint so the Task-2 flip changes decode semantics in ONE place. */
static inline struct MethodContext *home_ctx_frame(oop home) {
    if (home == NULL) return NULL;
    if (IS_SMI(home)) return home_link_decode_frame(vm_stack, home);
    return (struct MethodContext *)home;   /* absolute (pre-flip); Task 2 guards */
}
```

- [ ] **Step 4: Route `primitive_closure_home` through the chokepoint**

Replace the body of `primitive_closure_home` (currently returns the raw `home_ctx` value) so it ALWAYS returns a decoded frame pointer — required because its callers (`OP_LD_LEXICAL`'s multi-depth walk) treat the result as a `MethodContext` to send `regAt:`:

```c
oop primitive_closure_home(oop receiver, int argc, oop *args) {
    (void)argc; (void)args;
    if (receiver == NULL || IS_SMI(receiver)) return NULL;
    oop home;
    oop vt = obj_vtable(receiver);
    if (vt != BytecodeClosureVtable && vt != PrimitiveClosureVtable) {
        home = ((struct MethodContext *)receiver)->home_ctx;   /* a frame's lexical link */
    } else {
        home = ((struct Closure *)receiver)->home_ctx;          /* a closure's lexical link */
    }
    return (oop)home_ctx_frame(home);   /* decode → frame ptr (absolute passthrough in Task 1) */
}
```

Note: pre-flip this is behavior-identical — `home_ctx_frame` returns the same absolute pointer the old code returned raw.

- [ ] **Step 5: Route the NLR walk (`OP_NLR` case) through the chokepoint**

Replace the lexical-outer chase loop and the local-return comparison in the `OP_NLR` case. The old loop leaves `target_home` as an absolute pointer to the method frame; the new loop decodes at each hop and ends with the method frame pointer in `hframe`:

```c
                struct Closure *cl = (struct Closure *)ctx->closure;
                oop rVal = (dest < method->num_registers) ? ctx->regs[dest] : NULL;
                oop target_home = cl->home_ctx;

                if (target_home == NULL) {
                    fprintf(stderr, "VM Error: Block closure has no home context\n");
                    abort();
                }

                /* A `^` returns from the enclosing *method*, not an intermediate
                   block: chase the lexical-outer (home) links until the frame
                   whose closure has no home (the method frame). Each hop decodes
                   through home_ctx_frame so it is correct for the relative
                   encoding (Task 2) and the absolute one (Task 1). */
                struct MethodContext *hframe = home_ctx_frame(target_home);
                while (hframe != NULL) {
                    struct Closure *hcl = (struct Closure *)hframe->closure;
                    if (hcl != NULL && hcl->home_ctx != NULL) {
                        hframe = home_ctx_frame(hcl->home_ctx);
                    } else {
                        break;   /* hframe is the method frame */
                    }
                }

                if (ctx == hframe) {
                    // Local return: home is this very frame — return rVal from it.
                    return_val = rVal;
                    goto do_ret_deliver;
                }
```

Then, further down in the same case, update the unwind-target argument from the old `target_home` pointer to `(oop)hframe`:

```c
                ctx->vtable = MethodContextVtable;
                ctx->saved_pc = pc;
                {
                    oop unwind_args[2] = { (oop)hframe, rVal };
```

(Leave the rest of the `OP_NLR` body — the `unwindTo:value:` send and `do_unwind` drive — unchanged.)

- [ ] **Step 6: Route `get_top_level_method_closure` through the chokepoint**

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

(Old loop condition `!IS_SMI(cl->home_ctx)` meant "it's a pointer, chase it"; a method closure has `home_ctx == NULL` and stops the loop. New form decodes each hop and stops on the NULL home — identical pre-flip.)

- [ ] **Step 7: Run the full gate — verify ZERO behavior change**

Run from `ns-oevm/`:
```bash
OEVM_PATH=AB python3 tools/run_tests_oevm.py
```
Expected: `84 pass / 0 fail / 6 skip` both paths; all 5 gates PASS — identical to baseline.

Also the Zephyr syntax gate from `oevm/`:
```bash
gcc -fsyntax-only -D__ZEPHYR__ -Isrc src/vm.c
```
Expected: clean (no output).

- [ ] **Step 8: Commit**

```bash
git -C ~/workspace/oevm add src/vm.c tests/test_frame_abi.c
git -C ~/workspace/oevm commit -m "refactor(derecursify): home_ctx decode chokepoint + relativity proof (Phase 2 Task 1)

Route every home_ctx read (NLR walk, primitive_closure_home,
get_top_level_method_closure) through one home_ctx_frame() decoder.
Behavior-identical (home_ctx still absolute). test_frame_abi.c gains the
base-move relativity proof the Task-2 flip depends on."
```

---

## Task 2: The flip — encode `home_ctx` relative + reified guard + full gate

Change all 4 `home_ctx` WRITE sites from absolute to `home_link_encode_relative`, and re-point the chokepoint's non-SMI branch from "absolute passthrough" to the reified-Context fail-fast. After this task, every live `home_ctx` is a relative SMI (or NULL, or — never yet — a reified Context). Behavior stays equivalent; the frame is now growth-ready.

**Files:**
- Modify: `oevm/src/vm.c` — 3 encode sites (`push_frame`, `run_interpreter` setup, `primitive_fiber_create`) + `home_ctx_frame` reified branch + truthful comments.
- Modify: `oevm/src/dispatch.c` — `make_closure` encode site.
- Modify: `oevm/src/gc.c` — verify + comment the two `mark(frame->home_ctx)` sites.

**Interfaces:**
- Consumes: `home_ctx_frame` (Task 1), `home_link_encode_relative(vm_stack, frame)` (frame_abi.h).
- Produces: the invariant "a live `home_ctx` is `NULL` or `home_link_is_relative`" — relied on by Phase 3 growth.

- [ ] **Step 1: Add an external declaration of `vm_stack` in `dispatch.c` for `make_closure`**

`make_closure` (in `dispatch.c`) already `extern`s `current_frame`. Add `vm_stack` next to it inside `make_closure`:

```c
oop make_closure(oop method) {
    extern struct MethodContext *current_frame;
    extern oop *vm_stack;
    extern oop gc_allocate(oop vtable, size_t size);
    oop cl = gc_allocate(BytecodeClosureVtable, sizeof(struct Closure));
    ((struct Closure *)cl)->home_ctx =
        current_frame ? home_link_encode_relative(vm_stack, current_frame) : NULL;
    ((struct Closure *)cl)->home_generation = current_frame ? current_frame->gen_tag.tag32 : 0;
    ((struct Closure *)cl)->entry_point = (void *)method;
    ((struct Closure *)cl)->captured_env = NULL;
    return cl;
}
```

(`dispatch.c` already `#include`s `frame_abi.h` at line 2, and `extern oop *vm_stack` matches the global in `vm.c:41` — same `extern` `gc.c` already uses. Note: `home_ctx == NULL` when `current_frame == NULL` preserves the old `(oop)NULL` result for the no-frame case.)

- [ ] **Step 2: Flip the three `vm.c` encode sites**

At `push_frame`, `run_interpreter` frame setup, and `primitive_fiber_create` initial frame, replace the absolute assignment. All three currently read:

```c
ctx->home_ctx = cl->home_ctx ? cl->home_ctx : (oop)ctx;
```

Replace each with (encode the self-home relative; a block's inherited `cl->home_ctx` is ALREADY a relative SMI from `make_closure`, so pass it through unchanged):

```c
/* A method closure (cl->home_ctx==NULL) homes to itself; encode that self-link
   relative so it survives stack growth. A block closure already carries a
   relative home_ctx from make_closure — pass it through unchanged. */
ctx->home_ctx = cl->home_ctx ? cl->home_ctx
                             : home_link_encode_relative(vm_stack, ctx);
```

For `primitive_fiber_create` (`vm.c:1219`), the new frame is built on the NEW fiber's Stack, not `vm_stack`. Encode the self-home against that fiber's stack base:

```c
/* fiber_create: this frame lives on new_fib->stack_obj, NOT the active vm_stack.
   Encode the self-home relative to THIS fiber's base. A block closure's inherited
   home_ctx stays as-is (a relative SMI into the spawner's stack — decoded only if
   a cross-fiber ^ occurs, which no test exercises; see the residual note below). */
{
    oop *fib_base = ((struct Stack *)new_fib->stack_obj)->data;
    ctx->home_ctx = cl->home_ctx ? cl->home_ctx
                                 : home_link_encode_relative(fib_base, ctx);
}
```

- [ ] **Step 3: Re-point the `home_ctx_frame` non-SMI branch to the reified guard**

Now that no live `home_ctx` is a raw absolute pointer, a non-SMI non-NULL value can only be a reified `Context` — which this phase does not produce. Fail fast so a stray absolute pointer or premature reify is caught immediately, not silently mis-decoded:

```c
static inline struct MethodContext *home_ctx_frame(oop home) {
    if (home == NULL) return NULL;
    if (IS_SMI(home)) return home_link_decode_frame(vm_stack, home);
    /* Reified Context: reserved (frame_abi.h Option B) but NOT produced until the
       growth/escape phase. A non-SMI heap home here is a bug (stray absolute
       pointer or premature reify). Fail loudly. */
    fprintf(stderr, "VM Error: reified home_ctx not supported in Phase 2\n");
    abort();
}
```

- [ ] **Step 4: Truthful comments at the encode sites**

Update the now-stale `/* ABSOLUTE (fixed stacks) */` comments at the three `vm.c` sites and the `push_frame` header comment ("ABSOLUTE home_ctx") to state the link is relative (growth-ready). Keep them terse — one line each.

- [ ] **Step 5: Verify + comment the GC sites**

At `gc.c` (the two `mark(frame->home_ctx)` sites), no code change is needed — `mark()` exact-start-validates and no-ops a SMI, and traces a reified `Context` normally (none exist yet). Add a one-line comment at each confirming this:

```c
mark(frame->home_ctx);   /* relative SMI: mark() no-ops it (frame lives in the marked Stack) */
```

- [ ] **Step 6: Run the full gate — verify behavior-equivalence including the cross-fiber path**

Run from `ns-oevm/`:
```bash
OEVM_PATH=AB python3 tools/run_tests_oevm.py
```
Expected: `84 pass / 0 fail / 6 skip` both paths; all 5 gates PASS. **Pay special attention to `45_fibers` (must print `ABABAB`), `55_fiber_capture` (`before10`/`mutated`/`after99`), `56_upward_closure` (`15 25 1 2 3`), `57_loop_var_capture`, and `fibers-gc-stress` PASS** — these exercise the cross-fiber / escaped-closure / loop-var-`home_ctx` paths that the relativized link touches.

Zephyr syntax gate from `oevm/`:
```bash
gcc -fsyntax-only -D__ZEPHYR__ -Isrc src/vm.c
```
Expected: clean.

- [ ] **Step 7: Structural verification — confirm the flip actually happened**

The reviewer/executor must confirm (not just that behavior is unchanged, but that the encoding changed — see "behavior-proofs ≠ intent-proofs"):

```bash
cd ~/workspace/oevm
# All 4 encode sites now use the relative helper; none assign a raw (oop)ctx/(oop)current_frame to home_ctx:
grep -n "home_link_encode_relative" src/vm.c src/dispatch.c
grep -n "home_ctx = .*(oop)ctx\|home_ctx = (oop)current_frame" src/vm.c src/dispatch.c
```
Expected: FIRST grep shows 4 encode sites (3 in vm.c + 1 in dispatch.c). SECOND grep shows NOTHING (no absolute assignment remains). The `home_ctx_frame` reified branch `abort()` never fires during the suite (proves every live home is NULL or relative).

- [ ] **Step 8: Commit**

```bash
git -C ~/workspace/oevm add src/vm.c src/dispatch.c src/gc.c
git -C ~/workspace/oevm commit -m "feat(derecursify): relativize home_ctx (Phase 2 the flip)

All 4 home_ctx encode sites (push_frame, run_interpreter setup,
fiber_create, make_closure) now store home_link_encode_relative; the
home_ctx_frame chokepoint decodes relative-SMI and fail-fasts on a
(not-yet-produced) reified Context. Behavior-equivalent, stacks still
fixed: 84/0/6 both paths, 45_fibers=ABABAB, 55/56/57 + fibers-gc-stress
green, -D__ZEPHYR__ syntax-clean. Frame is now growth-ready (Phase 3)."
```

---

## Task 3: Docs, ABI status, and phase close-out

Pure documentation + status. No code behavior change; folded here because it spans files no code task owns.

**Files:**
- Modify: `oevm/src/frame_abi.h` (status comment)
- Modify: `oevm/docs/design/2026-07-01-derecursify-deferred-lift-growable-stacks-upward-closures-design.md` (Phase 2 → DONE)
- Modify: `ns-oevm/.superpowers/sdd/progress.md` (close-out)

- [ ] **Step 1: Update the `frame_abi.h` status note**

The header comment (lines ~9–13) says "the live engine still uses an absolute prev_frame today" and that L1 switches vm.c to the helpers. Update the LEXICAL-link portion to state `home_ctx` is now relative as of deferred-lift Phase 2, and the reified-`Context` branch remains reserved for the growth/escape phase.

- [ ] **Step 2: Mark Phase 2 DONE in the design doc**

In `2026-07-01-derecursify-deferred-lift-...-design.md` §4 "Phase 2 — Relativize `home_ctx`", add a `✅ DONE <date>` status line mirroring the Phase 1 close-out style: gate results (84/0/6 both paths, all 5 gates, cross-fiber tests, Zephyr syntax), the residual note (a cross-fiber `^`/super from a fiber block would decode a relative home against the wrong base — NOT exercised by any test; the reified-`Context` branch that would fix it is deferred to the growth/escape phase), and the landed commits.

- [ ] **Step 3: Close out the SDD ledger**

Fill `ns-oevm/.superpowers/sdd/progress.md` with per-task outcomes, final gate numbers, the structural-verification result, and the "NEXT = Phase 3 (growable stacks + catchable StackOverflow)" pointer.

- [ ] **Step 4: Commit**

```bash
git -C ~/workspace/oevm add src/frame_abi.h docs/design/2026-07-01-derecursify-deferred-lift-growable-stacks-upward-closures-design.md
git -C ~/workspace/oevm commit -m "docs(derecursify): Phase 2 relativize-home_ctx DONE + ABI status"
git -C ~/workspace/ns-oevm add .superpowers/sdd/progress.md
git -C ~/workspace/ns-oevm commit -m "docs(derecursify): Phase 2 SDD close-out ledger"
```

- [ ] **Step 5: Whole-phase review + finish**

Request a final whole-branch review (opus) across `oevm` + `ns-oevm` per `superpowers:requesting-code-review`. On clean, use `superpowers:finishing-a-development-branch` to present David the merge decision. **Do not push or merge without David's say-so.**

---

## Site Map (reference for all tasks)

**Encode sites (4)** — flip to `home_link_encode_relative` in Task 2:
| Site | File:sym | Note |
|---|---|---|
| 1 | `vm.c` `push_frame` (~:187) | self-home vs inherited rel-SMI |
| 2 | `vm.c` `run_interpreter` setup (~:282) | same |
| 3 | `vm.c` `primitive_fiber_create` (~:1219) | encode against the NEW fiber's stack base |
| 4 | `dispatch.c` `make_closure` (~:638) | `(oop)current_frame` → relative; needs `extern oop *vm_stack` |

**Decode/walk sites (4)** — route through `home_ctx_frame` in Task 1:
| Site | File:sym | Semantics flip |
|---|---|---|
| A | `vm.c` `OP_NLR` walk (~:612) + local-return cmp (~:621) + unwind arg (~:637) | `!IS_SMI==pointer` → decode each hop; compare decoded frame ptrs |
| B | `vm.c` `primitive_closure_home` (~:1149) | return decoded frame ptr (feeds `OP_LD_LEXICAL`) |
| C | `vm.c` `get_top_level_method_closure` (~:1280) | stop on NULL home, decode each hop |
| D | `vm.c` `OP_LD_LEXICAL`/`OP_ST_LEXICAL` (~:408/418) | unchanged — call B, which now decodes |

**GC sites (2)** — verify no-op on SMI, comment only (Task 2):
`gc.c:516` (Stack tracer), `gc.c:584` (`scan_vm_stack`).

**The residual (documented, David-approved risk):** a fiber-entry block carries a `home_ctx` relative to its SPAWNER's stack; decoded against the RUNNING fiber's base it would be wrong. This is ONLY reachable via a cross-fiber `^`-NLR or super-resolution from inside a fiber block — **no existing test does this** (fiber blocks only `yield`/fall off the end; variable capture goes through the heap `Context`, not `home_ctx`). The correct fix is the reified-`Context` home branch, deferred to the growth/escape phase. If a NEW test surfaces this, escalate — do not paper over it.
