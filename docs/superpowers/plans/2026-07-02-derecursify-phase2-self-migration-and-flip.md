# De-recursify Phase 2 (redesigned) — `self`-migration + relativize `home_ctx` — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make enclosing-`self` reads fiber-independent (snapshot `self` onto the `Closure`), then relativize the `home_ctx` lexical link so a frame survives a future stack `realloc` — behavior-equivalent, both image paths.

**Architecture:** Two parts. **Part 1 (`self`-migration, Approach B):** every nested block currently reads its enclosing `self` by walking the absolute `home_ctx` chain (`get_self_reg` → `OP_LD_LEXICAL`), which breaks cross-fiber once `home_ctx` is relative. Instead, snapshot the receiver onto each `Closure` at creation (`captured_self`, computed O(1) in `make_closure`) and read it via a new `OP_LD_SELF` opcode. **Part 2 (the flip):** with `home_ctx` no longer carrying cross-fiber `self` reads, relativize its 4 encode sites and switch the decode chokepoint's non-SMI branch to a reified-Context fail-fast. Reuses the already-merged decode chokepoint `home_ctx_frame` (oevm `6e988af`).

**Tech Stack:** C99 (`oevm` VM: `src/vm.c`, `src/dispatch.c`, `src/oevm.h`, `src/gc.c`, `src/frame_abi.h`), the Newspeak→bytecode compiler (`ns-oevm/tools/ns_compile_oevm.py`), the dual-path harness (`ns-oevm/tools/run_tests_oevm.py`).

**Design:** `oevm/docs/superpowers/specs/2026-07-02-derecursify-phase2-self-migration-design.md`.

## Global Constraints

- **Behavior-equivalent.** No observable output change, no `.oimg` `format_version` bump, **no dialect row**. `captured_self` and `home_ctx`'s encoding are runtime-only; `OP_LD_SELF` rides the existing bytecode encoding (raw method-bytecode data the `.oimg` loader passes through verbatim — verify no serializer change is needed).
- **Both image paths.** Every gate runs `OEVM_PATH=AB` (Path A = solidified-C, Path B = `.oimg`). The single `run_interpreter` dispatch serves both paths, so one `OP_LD_SELF` case covers both. **`solidify.py` needs NO opcode change** (its `OP_*` mentions are fixture comments, not a dispatch table).
- **Master gate** (from `ns-oevm/`): `OEVM_PATH=AB python3 tools/run_tests_oevm.py` — full suite BOTH paths + all 5 gates (`grant`, `debugger-integration`, `portability-32`, `fibers-gc-stress`, `boot-gc-asan`). **Baseline to preserve: 84 pass / 0 fail / 6 skip both paths, all 5 gates PASS.**
- **Zephyr syntax gate** (from `oevm/`): `gcc -fsyntax-only -D__ZEPHYR__ -Isrc src/vm.c` — clean.
- **Free opcode is `0x1A`** (0x16–0x19 are `OP_BLOCK_INVOKE`/`OP_MK_CONTEXT`/`OP_LD_CTX`/`OP_ST_CTX`). Use `0x1A` for `OP_LD_SELF` in BOTH the C opcode enum and the Python compiler constant — they must match.
- **GC precision.** `captured_self` is a heap oop (the receiver) and must be traced. Closures are traced by the generic size-based word-walk (`trace_object`), which already covers `captured_env`; growing the struct by one word keeps it covered — but `make_closure` MUST initialize `captured_self` (gc_allocate does not zero), and any hand-rolled C-test `Closure` that GC may trace must set it too (same surface as oevm#32). The `fibers-gc-stress` + `boot-gc-asan` gates are the proof.
- **Concurrency caveat.** `run_tests_oevm.py` is NOT concurrency-safe. Run gates SERIALLY; re-run a red SOLO before believing it. The suite regenerates `solidified_image.c`/`.stmap` as a side effect — do NOT commit those.
- **Repos & branch.** Continue on branch `derecursify-deferred-phase2` (already checked out in `oevm` + `ns-oevm`; carries the chokepoint `6e988af` and the doc commits). Compiler change is in `ns-oevm`. No `ns-tests` change expected. **Do not push** (push only on David's explicit say-so).

---

## File Structure

- `oevm/src/oevm.h` — MODIFY. Add `oop captured_self;` to `struct Closure` (after `captured_env`); add `OP_LD_SELF = 0x1A` to the opcode enum.
- `oevm/src/dispatch.c` — MODIFY. `make_closure`: compute + store `captured_self` (Part 1). Later, `make_closure` `home_ctx` → relative (Part 2).
- `oevm/src/vm.c` — MODIFY. `OP_LD_SELF` dispatch case (Part 1). Later, the 3 `home_ctx` encode sites + `home_ctx_frame` reified guard + truthful comments (Part 2).
- `oevm/src/gc.c` — MODIFY (Part 2, comment-only at the two `mark(home_ctx)` sites). Verify Closure trace covers `captured_self` (no code change expected).
- `ns-oevm/tools/ns_compile_oevm.py` — MODIFY (Part 1). `OP_LD_SELF` constant; `get_self_reg` emits it for block scopes.
- Docs (Part 3): design docs + memory close-out.

---

## Task 1: `self`-migration (Approach B) — `captured_self` + `OP_LD_SELF`

Snapshot the receiver onto every `Closure` and read enclosing `self` via a new opcode instead of walking `home_ctx`. Runtime + compiler land together (the opcode is inert until the compiler emits it, and the compiler change is invalid until the opcode exists). **Behavior-equivalent** — every `self` read returns the same receiver.

**Files:**
- Modify: `oevm/src/oevm.h` (`struct Closure` field + opcode enum)
- Modify: `oevm/src/dispatch.c` (`make_closure`)
- Modify: `oevm/src/vm.c` (`OP_LD_SELF` dispatch case)
- Modify: `ns-oevm/tools/ns_compile_oevm.py` (`OP_LD_SELF` constant + `get_self_reg`)

**Interfaces:**
- Produces: `struct Closure.captured_self` (the home method's receiver, fiber-independent); `OP_LD_SELF = 0x1A` (`dest` = register to load `self` into).
- Consumes: `make_closure`'s `current_frame`; `frame_abi.h` unchanged this task.

- [ ] **Step 1: Add the `captured_self` field to `struct Closure`**

In `oevm/src/oevm.h`, after the `captured_env` field (`~line 143`):

```c
    oop captured_env;   /* enclosing Context for captured-var access (deferred-lift P1);
                            NULL at top level. Runtime-only — Closures are never
                            .oimg-serialized, so this adds no format concern. */
    oop captured_self;  /* De-recursify Phase 2 (self-migration): the home method's
                            receiver, snapshotted at make_closure. Read via OP_LD_SELF
                            so a block reaches enclosing `self` fiber-independently
                            instead of walking the (relativized) home_ctx chain.
                            Runtime-only. */
```

- [ ] **Step 2: Add the `OP_LD_SELF` opcode to the C enum**

Find the opcode enum in `oevm/src/oevm.h` (or wherever `OP_ST_CTX = 0x19` is defined — grep `OP_ST_CTX`). Add after the last entry:

```c
    OP_LD_SELF = 0x1A,   /* dest = reg; load closure->captured_self (self-migration, P2) */
```

- [ ] **Step 3: Set `captured_self` in `make_closure`**

In `oevm/src/dispatch.c`, `make_closure` (`~line 634`), after the existing field inits and before `return cl;`:

```c
    /* De-recursify Phase 2 (self-migration, Approach B): snapshot the home method's
       receiver. A block's self IS its home method's receiver (shared across a method
       and all its nested blocks), so compute it O(1): a method creating-frame's
       receiver is regs[0]; a block creating-frame propagates its own captured_self.
       Same-fiber at creation, so valid regardless of home_ctx encoding. */
    {
        struct Closure *ccl = current_frame ? (struct Closure *)current_frame->closure : NULL;
        oop self_val;
        if (ccl == NULL)                self_val = NULL;                    /* no frame (boot) */
        else if (ccl->home_ctx == NULL) self_val = current_frame->regs[0]; /* method frame */
        else                            self_val = ccl->captured_self;     /* block frame: propagate */
        ((struct Closure *)cl)->captured_self = self_val;
    }
```

(`make_closure` already `extern`s `current_frame`. Place this block so `captured_self` is set on every path out of `make_closure`, including the `PRIM_createClosure` caller at `dispatch.c:774` — since it is inside `make_closure` itself, both callers are covered.)

- [ ] **Step 4: Add the `OP_LD_SELF` dispatch case in `vm.c`**

In `oevm/src/vm.c`, in the interpreter's opcode `switch`, add a case near the other load ops (e.g. after `OP_LD_REG` or the Context ops):

```c
            case OP_LD_SELF:
                /* self-migration: enclosing self, snapshotted on the running block's
                   closure — fiber-independent (no home_ctx walk). */
                ctx->regs[dest] = ((struct Closure *)ctx->closure)->captured_self;
                break;
```

- [ ] **Step 5: Add the `OP_LD_SELF` constant to the compiler**

In `ns-oevm/tools/ns_compile_oevm.py`, next to `OP_ST_CTX = 0x19` (`~line 39`):

```python
OP_LD_SELF    = 0x1A   # dest=reg; load running block closure's captured_self (self-migration)
```

- [ ] **Step 6: Emit `OP_LD_SELF` from `get_self_reg` for block scopes**

Replace `get_self_reg` (`ns-oevm/tools/ns_compile_oevm.py:736`). Old:

```python
    def get_self_reg(self):
        if "_block" in self.name:
            outer_depth = 0
            curr = self
            while curr.outer:
                outer_depth += 1
                curr = curr.outer

            assert outer_depth < 256
            base_reg = self.alloc_reg()
            self.emit_ABC(OP_LD_LEXICAL, base_reg, outer_depth, 0)
            return base_reg
        else:
            return 0
```

New:

```python
    def get_self_reg(self):
        if "_block" in self.name:
            # De-recursify Phase 2 (self-migration, Approach B): a block's self is its
            # home method's receiver, snapshotted onto the closure at creation
            # (captured_self). Read it fiber-independently via OP_LD_SELF instead of
            # walking the home_ctx chain (OP_LD_LEXICAL) — the latter is not
            # cross-fiber safe once home_ctx is relativized (Task 2).
            base_reg = self.alloc_reg()
            self.emit_ABC(OP_LD_SELF, base_reg, 0, 0)
            return base_reg
        else:
            return 0
```

- [ ] **Step 7: Run the full gate — verify behavior-equivalence**

From `ns-oevm/`:
```bash
OEVM_PATH=AB python3 tools/run_tests_oevm.py
```
Expected: `84 pass / 0 fail / 6 skip` both paths; all 5 gates PASS — identical to baseline. Note `55_fiber_capture` still passes here (home_ctx is still absolute this task; the point is self now flows through `captured_self`, and the cross-fiber test is unaffected).

Zephyr syntax from `oevm/`:
```bash
gcc -fsyntax-only -D__ZEPHYR__ -Isrc src/vm.c
```
Expected: clean.

- [ ] **Step 8: Structural verification — self reads no longer walk home_ctx**

From `ns-oevm/`:
```bash
# get_self_reg emits OP_LD_SELF, not OP_LD_LEXICAL, for blocks:
grep -n "OP_LD_SELF\|OP_LD_LEXICAL" tools/ns_compile_oevm.py
```
Expected: `get_self_reg` (Step 6 site) emits `OP_LD_SELF`; the only remaining `OP_LD_LEXICAL` emit is the captured-name pre-rewrite path (`~line 826`), which serves residual inlined-loop-var captures (deferred — see design §5). Confirm `make_closure` sets `captured_self` on all exit paths (Step 3).

- [ ] **Step 9: Commit (both repos)**

```bash
git -C /home/david/workspace/oevm add src/oevm.h src/dispatch.c src/vm.c
git -C /home/david/workspace/oevm commit -m "feat(derecursify): snapshot self onto Closure + OP_LD_SELF (Phase 2 self-migration)

A block's enclosing self is now read fiber-independently from the closure's
captured_self (set O(1) in make_closure) via OP_LD_SELF=0x1A, instead of
walking the home_ctx chain. Behavior-equivalent (same receiver); prerequisite
for relativizing home_ctx (Task 2). 84/0/6 both paths + 5 gates.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_013RNThyg4MgNJD33fbSSv4h"
git -C /home/david/workspace/ns-oevm add tools/ns_compile_oevm.py
git -C /home/david/workspace/ns-oevm commit -m "feat(derecursify): compiler emits OP_LD_SELF for block self-reads (Phase 2 self-migration)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_013RNThyg4MgNJD33fbSSv4h"
```

---

## Task 2: Complete `self`-migration (explicit path) + the flip — relativize `home_ctx`

Task 1 migrated only `get_self_reg` (the implicit-receiver/slot-base path). Explicit `self` used as a value/receiver (e.g. `self complete` in the kernel scheduler entry block) still emits `OP_LD_LEXICAL` via `resolve_name`→`("lexical", 0, depth)` — a cross-fiber `home_ctx` read that crashes under the flip. **Step 1 completes the migration** (verified: it turns the entry block's last `OP_LD_LEXICAL 0x00010512` into `OP_LD_SELF 0x0000051A`). **Steps 2–7 are the flip** (the exact edits reverted from the first attempt, now safe with both self paths migrated). The whole task was proven end-to-end: full suite 84/0/6 both paths + `fibers-gc-stress` with Step 1 + the flip applied together.

**Files:**
- Modify: `ns-oevm/tools/ns_compile_oevm.py` — complete the `self` migration in the `varRef` "lexical" emit (Step 1).
- Modify: `oevm/src/vm.c` — 3 encode sites (`push_frame`, `run_interpreter` setup, `primitive_fiber_create`) + `home_ctx_frame` reified branch + truthful comments.
- Modify: `oevm/src/dispatch.c` — `make_closure` `home_ctx` → relative.
- Modify: `oevm/src/gc.c` — comment-only at the two `mark(frame->home_ctx)` sites.

**Interfaces:**
- Consumes: `home_ctx_frame` (chokepoint, `6e988af`), `home_link_encode_relative(base, frame)` (frame_abi.h), `captured_self` + `OP_LD_SELF` (Task 1).
- Produces: the invariant "a live `home_ctx` is `NULL` or `home_link_is_relative`" — required by Phase 3 growth.

- [ ] **Step 1: Complete `self`-migration — explicit `varRef self`** (`ns-oevm/tools/ns_compile_oevm.py`)

In `compile_expr`'s `varRef` handler, the `res[0] == "lexical"` branch (`~line 821`) currently emits `OP_LD_LEXICAL` for every cross-frame lexical, including `self`. Special-case `self`:

```python
            elif res[0] == "lexical":
                reg_idx, depth = res[1], res[2]
                assert depth < 256 and reg_idx < 256
                if name == "self":
                    # self-migration (Approach B): explicit `self` used as a value or
                    # send-receiver in a block reads the closure's captured_self
                    # (fiber-independent) via OP_LD_SELF, NOT the home_ctx chain — the
                    # same treatment get_self_reg gives implicit receivers. Not
                    # Context-homed, so no ctx-patch. (Completes the self-migration:
                    # get_self_reg was the implicit path; this is the explicit path.)
                    self.emit_ABC(OP_LD_SELF, target_reg, 0, 0)
                else:
                    idx = len(self.instructions)
                    self.emit_ABC(OP_LD_LEXICAL, target_reg, depth, reg_idx)
                    # A cross-frame read is captured (except `self`, handled above,
                    # and inlined-loop vars, which stay lexical/home_ctx-homed — a
                    # documented deferred limitation); the post-pass rewrites captured
                    # ones to OP_LD_CTX.
                    self._ctx_patch.append(
                        (idx, False, self.current_scope.defining_frame_of(name),
                         name, depth, target_reg))
```

Verify with: `python3 tools/ns_compile_oevm.py ../ns-tests/04_somns_basic/45_fibers.ns /tmp/45f.c` then `grep block16_value_insts /tmp/45f.c` — the entry block's instruction array must contain NO `0x......12` (OP_LD_LEXICAL) word; the former `0x00010512` is now `0x0000051A` (OP_LD_SELF).

- [ ] **Step 2: `make_closure` `home_ctx` → relative** (`oevm/src/dispatch.c`)

Add `extern oop *vm_stack;` beside the existing `extern struct MethodContext *current_frame;`, and change the `home_ctx` assignment:

```c
    extern struct MethodContext *current_frame;
    extern oop *vm_stack;
    extern oop gc_allocate(oop vtable, size_t size);
    oop cl = gc_allocate(BytecodeClosureVtable, sizeof(struct Closure));
    /* Capture the enclosing frame as a relative home_ctx (growth-ready). NULL when
       there is no current frame, preserving the old (oop)NULL no-home result. */
    ((struct Closure *)cl)->home_ctx =
        current_frame ? home_link_encode_relative(vm_stack, current_frame) : NULL;
```

(`dispatch.c` already `#include`s `frame_abi.h`.) Keep the `captured_self` block from Task 1 — it reads `ccl->home_ctx == NULL` which is still the correct method/block discriminator (NULL vs non-NULL is encoding-independent).

- [ ] **Step 2: Flip the three `vm.c` encode sites**

Each currently reads `ctx->home_ctx = cl->home_ctx ? cl->home_ctx : (oop)ctx;`.

`push_frame` and `run_interpreter` setup — relative to `vm_stack`:
```c
/* method closure homes to itself (encoded relative, growth-ready); a block
   closure's inherited home_ctx is already a relative SMI — pass it through. */
ctx->home_ctx = cl->home_ctx ? cl->home_ctx
                             : home_link_encode_relative(vm_stack, ctx);
```

`primitive_fiber_create` — the new frame lives on the NEW fiber's Stack, encode against THAT base:
```c
/* fiber_create: this frame lives on new_fib->stack_obj, NOT the active vm_stack.
   Encode the self-home relative to THIS fiber's base. A block closure's inherited
   home_ctx stays as-is (a relative SMI into the spawner's stack — decoded only by a
   cross-fiber ^, not exercised; see design §5). */
{
    oop *fib_base = ((struct Stack *)new_fib->stack_obj)->data;
    ctx->home_ctx = cl->home_ctx ? cl->home_ctx
                                 : home_link_encode_relative(fib_base, ctx);
}
```

- [ ] **Step 3: `home_ctx_frame` non-SMI branch → reified fail-fast** (`oevm/src/vm.c`)

Change the chokepoint's absolute-passthrough branch (Task-1 form) to:

```c
static inline struct MethodContext *home_ctx_frame(oop home) {
    if (home == NULL) return NULL;
    if (IS_SMI(home)) return home_link_decode_frame(vm_stack, home);
    /* Reified Context: reserved (frame_abi.h Option B) but NOT produced until the
       growth/escape phase. A non-SMI heap home here is a bug (stray absolute pointer
       or premature reify). Fail loudly. */
    fprintf(stderr, "VM Error: reified home_ctx not supported in Phase 2\n");
    abort();
}
```

- [ ] **Step 4: Truthful comments + GC comments**

Update the stale `/* ABSOLUTE (fixed stacks) */` comments at the 3 `vm.c` encode sites and the `push_frame` header comment to state the link is now relative (growth-ready) — one line each. At `oevm/src/gc.c`, add the confirming comment (no code change) at both `mark(frame->home_ctx)` sites:

```c
mark(frame->home_ctx);   /* relative SMI: mark() no-ops it (frame lives in the marked Stack) */
```

- [ ] **Step 5: Run the full gate — including the tests the first attempt crashed**

From `ns-oevm/`:
```bash
OEVM_PATH=AB python3 tools/run_tests_oevm.py
```
Expected: `84 pass / 0 fail / 6 skip` both paths; all 5 gates PASS. **Specifically `55_fiber_capture` prints `before10`/`mutated`/`after99`, `45_fibers` prints `ABABAB`, and `fibers-gc-stress` PASSES** — the exact cases the first attempt SIGSEGV'd.

Zephyr syntax from `oevm/`:
```bash
gcc -fsyntax-only -D__ZEPHYR__ -Isrc src/vm.c
```
Expected: clean.

- [ ] **Step 6: Structural grep proof — the flip actually happened**

From `oevm/`:
```bash
grep -n "home_link_encode_relative" src/vm.c src/dispatch.c   # expect 4 encode sites (3 vm.c + 1 dispatch.c)
grep -nE "home_ctx = .*\(oop\)ctx|home_ctx = \(oop\)current_frame" src/vm.c src/dispatch.c   # expect NOTHING
```
Confirm the reified-guard `abort()` never fired during the suite (no "reified home_ctx not supported" line in output) — proof every live home is NULL or relative.

- [ ] **Step 7: Commit (both repos)**

```bash
git -C /home/david/workspace/ns-oevm add tools/ns_compile_oevm.py
git -C /home/david/workspace/ns-oevm commit -m "feat(derecursify): complete self-migration — explicit varRef self -> OP_LD_SELF

get_self_reg (Task 1) covered the implicit self path; this covers explicit
self used as a value/receiver (resolve_name -> lexical, e.g. 'self complete').
Both paths now emit OP_LD_SELF so no self read walks home_ctx cross-fiber.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_013RNThyg4MgNJD33fbSSv4h"
git -C /home/david/workspace/oevm add src/vm.c src/dispatch.c src/gc.c
git -C /home/david/workspace/oevm commit -m "feat(derecursify): relativize home_ctx (Phase 2 the flip)

All 4 home_ctx encode sites store home_link_encode_relative; home_ctx_frame
fail-fasts on a (not-yet-produced) reified Context. Safe now that BOTH self
emit paths go through OP_LD_SELF/captured_self, not the home_ctx chain. 84/0/6
both paths, 55_fiber_capture + fibers-gc-stress green (the cases the first
attempt crashed), grep proof clean, -D__ZEPHYR__ syntax-clean. Growth-ready.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_013RNThyg4MgNJD33fbSSv4h"
```

---

## Task 3: Docs, ABI status, and phase close-out

**Files:**
- Modify: `oevm/src/frame_abi.h` (status comment: `home_ctx` now relative)
- Modify: `oevm/docs/superpowers/specs/2026-07-02-derecursify-phase2-self-migration-design.md` (mark DONE)
- Modify: `oevm/docs/superpowers/specs/2026-07-01-derecursify-deferred-lift-growable-stacks-upward-closures-design.md` (Phase 2 → DONE via redesign)
- Update: the deferred-lift memory file + MEMORY.md index (controller does this outside the plan).

- [ ] **Step 1: Update `frame_abi.h` status** — the header note that "the live engine still uses an absolute prev_frame today" / L1 switching: update the LEXICAL-link line to state `home_ctx` is now relative as of Phase 2 self-migration+flip; the reified-`Context` branch remains reserved for the growth/escape phase.

- [ ] **Step 2: Mark the redesign spec DONE** — add a `✅ DONE <date>` line to `2026-07-02-derecursify-phase2-self-migration-design.md` with final gate results (84/0/6 both paths, 5 gates, `55`/`fibers-gc-stress` green, Zephyr syntax) and the landed commits.

- [ ] **Step 3: Close out the older design doc** — in `2026-07-01-...-design.md` §4 Phase 2, replace the `⚠️ BLOCKED` note's forward-pointer with `✅ DONE (via self-migration redesign)` and the commit range. Note the deferred loop-var limitation (design §5) and its failure-mode change.

- [ ] **Step 4: Commit**

```bash
git -C /home/david/workspace/oevm add src/frame_abi.h docs/superpowers/specs/2026-07-02-derecursify-phase2-self-migration-design.md docs/superpowers/specs/2026-07-01-derecursify-deferred-lift-growable-stacks-upward-closures-design.md
git -C /home/david/workspace/oevm commit -m "docs(derecursify): Phase 2 self-migration + flip DONE + ABI status

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_013RNThyg4MgNJD33fbSSv4h"
```

- [ ] **Step 5: Whole-phase review + finish** — request a final whole-branch review (opus) across `oevm` + `ns-oevm` per `superpowers:requesting-code-review`. On clean, use `superpowers:finishing-a-development-branch` for the merge decision. **Do not push or merge without David's say-so.**

---

## Notes for the executor

- **Task 2 is the reverted first-attempt edits** — they are captured verbatim in `ns-oevm/.superpowers/sdd/task-2-report.md` (§"What was changed"). Cross-check against that report; the only reason they crashed before was the missing Task 1.
- **The `home_ctx_frame` chokepoint already exists** (oevm `6e988af`, near the top of `vm.c` ~line 198). Task 2 Step 3 only changes its non-SMI branch.
- **oevm#32 surface:** adding `captured_self` extends the "hand-rolled C-test Closures with an uninitialized runtime field" surface. If `fibers-gc-stress`/`boot-gc-asan` surfaces a garbage-`captured_self` trace, initialize the field in the offending C-test closure(s) and note it. The real allocator (`make_closure`) always sets it.
- **The residual (design §5):** inlined-loop-var captures read cross-fiber now fail-fast rather than working-by-accident. No test exercises it; do not try to fix it here.
