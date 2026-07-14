# De-recursify reify-on-escape — E1: retire the dead `home_ctx` walk in `get_top_level_method_closure` (design)

**Status:** approved (David, 2026-07-03), after two scope corrections during design
(see §2). First of the reify-on-escape sub-phases (E1→E2→E3→E4) that close the
frame-identity gaps left after the de-recursify deferred lift (Phases 1–3). This spec
covers **E1 only**.

> **Scope note — E1 changed shape during design.** It began as "snapshot the home
> method onto the closure (a `captured_self`-style field) so `get_top_level_method_closure`
> reads it O(1) instead of walking `home_ctx`." Investigation then showed the walk is
> not merely cross-fiber-unsafe but **fully unreachable through dispatch** (§2). So a
> snapshot field would be a *dead* field carrying a hot-path cost. E1 is therefore the
> simpler, strictly-better change: **delete the dead walk** and pin its invariant.

Parent context: [[derecursify-deferred-lift]] · queue item #1. The four sites that
still ride the fiber-local, stack-relative `home_ctx` link for *frame identity*:

| # | Site | Sub-phase | Status found |
|---|------|-----------|--------------|
| oevm#34 | NLR home frame flows through kernel `unwindTo:value:`; stranded across a grow | E3 | reachable (`-D` small-init) |
| (1) | inlined loop-var captures read cross-fiber | E2 | reachable (fibers) |
| (2) | non-local `^` from a block homed on another fiber's stack | E4 | reachable (fibers) |
| **(3)** | **`get_top_level_method_closure` walks `home_ctx` for access-control method identity** | **E1 (this doc)** | **dead code (see §2)** |

## 1. The site

`get_top_level_method_closure` (`oevm/src/vm.c:1424`) resolves the *defining method
closure* of the closure making a send, so `find_defining_vtable` / `check_access_allowed`
can gate a **private** method. It does so by walking the lexical chain:

```c
static oop get_top_level_method_closure(oop closure) {
    struct Closure *cl = (struct Closure *)closure;
    while (cl->home_ctx != NULL) {                       /* <-- dead loop body (§2) */
        struct MethodContext *ctx = home_ctx_frame(cl->home_ctx);
        if (ctx == NULL) break;
        cl = (struct Closure *)ctx->closure;
    }
    return (oop)cl;
}
```

Since Phase 2, `home_ctx` is a fiber-local relative SMI decoded by `home_ctx_frame`
against the *current* fiber's `vm_stack`, so the walk was flagged as a deferred
cross-fiber `home_ctx` reader. Deeper analysis (§2) shows the loop body never runs at
all.

## 2. Why the walk is dead (the two corrections)

**Call graph (verified):** `get_top_level_method_closure` ← `find_defining_vtable`
(only caller) ← the `ACCESS_PRIVATE` branch of `check_access_allowed` (only caller,
`vm.c:1472`) ← `dispatch.c:1194`. The `ACCESS_PROTECTED` branch — the one that *is*
live via protected slot accessors — does **not** call it.

**Correction 1 — send-visibility is enforced *nowhere* from `.ns`, deliberately.**
The grammar parses method/slot `visibility` (`grammar/ns.ohm:30`; `ns-parse.js` records
`m["visibility"]`), but the compiler uses it **only to gate which accessors are
synthesised** (public/protected slots get accessors; private slots get none) — it
**never maps visibility to `access_modifier`**: every `solidify.add_method(...)` call
(real methods *and* synthesised accessors) omits the parameter, which defaults to
`0 = ACCESS_PUBLIC` (`solidify.py:76`, `oimg_serialize.py:512`). So **every
compiler-generated method is PUBLIC**, and *both* the `ACCESS_PRIVATE` and
`ACCESS_PROTECTED` branches of `check_access_allowed` are unreachable from any `.ns`
image.

This is **intentional and load-bearing**, not an oversight:
- The compiler states it outright: *"The VM does not enforce send-visibility"*
  (`ns_compile_oevm.py:1804`).
- It is **regression-pinned**: `23_protected_slot_accessor.ns` expects a `protected`
  slot's accessor to work from any context (`expect: 5`) and its own comment says *"the
  VM does not enforce send-visibility."* Wiring enforcement would break that shipped test.
- It matches the **dialect's privacy model** (`NEWSPEAK_DIALECT.md:146`): privacy comes
  from **lexical nesting / module scoping** ("a private helper class is genuinely
  private"), the Newspeak way — not from send-time access checks.

Consequently the **entire C access-check subsystem** (`check_access_allowed`,
`ACCESS_PRIVATE/PROTECTED`, `find_defining_vtable`, `get_top_level_method_closure`) is
**vestigial** — aspirational infra reachable only from the hand-built C unit test
`oevm/tests/test_access.c` (which sets `access_modifier` directly). Reader #3 is one
walk inside that dead subsystem. Its broader fate (wire visibility→`access_modifier`,
or delete the subsystem + `test_access.c`) is a separate cleanup, tracked in oevm#36;
E1 only trims the one `home_ctx` walk and leaves the rest for that decision.

**Correction 2 — the loop body is unreachable even in that C test.** For the
`while (cl->home_ctx != NULL)` body to execute once, the closure passed
(`current_frame->closure`) must be a **block** closure (`home_ctx != NULL`). But
`check_access_allowed` reaches the walk only after `receiver == caller_self`, where
`caller_self = current_frame->regs[0]` — and a **block** frame's `regs[0]` is the
**closure itself** (`push_frame(target, target, …)` at `OP_BLOCK_INVOKE`,
`vm.c:659`), never a receiver a private method is sent to. So a block caller is denied
at `receiver != caller_self` *before* the walk. Every path that actually reaches
`get_top_level_method_closure` passes a **method** closure (`home_ctx == NULL`) →
immediate return, loop never iterates. `test_access.c` TEST 5 confirms this: it passes
`cl_sub_run_priv` whose `home_ctx == NULL`.

**Conclusion:** the loop is dead. It is the only remaining `home_ctx_frame` caller that
walks for *method identity*, and removing it shrinks the surface E3 must reason about —
at zero runtime cost, with no new state.

## 3. Change

Reduce the function to the only behaviour that ever executes, and pin the invariant
so a future regression (e.g. if private-from-block enforcement is ever built) is caught
loudly rather than silently walking a dead path:

```c
static oop get_top_level_method_closure(oop closure) {
    /* Reify-on-escape E1: the lexical walk this function used to perform is
       unreachable through dispatch. Its only caller (find_defining_vtable, from the
       ACCESS_PRIVATE branch of check_access_allowed) is reached only when
       receiver == caller_self == current_frame->regs[0]; a private method's receiver
       is a user instance, and a block frame's regs[0] is the closure — so the caller
       here is always a METHOD closure (home_ctx == NULL). The walk therefore always
       returned `closure` unchanged. Removing it drops the last home_ctx frame-identity
       walk (shrinks the E3 reify surface). The invariant is pinned: a non-NULL home_ctx
       here would mean a block reached access control, which the model does not produce. */
    if (((struct Closure *)closure)->home_ctx != NULL) {
        fprintf(stderr, "VM Error: access-control method lookup on a block closure "
            "(home_ctx != NULL) — unexpected; see reify-on-escape E1.\n");
        abort();
    }
    return closure;
}
```

No `struct Closure` field is added. No `make_closure` change. `home_ctx_frame` loses
one caller. `find_defining_vtable` is otherwise unchanged (it still walks the receiver
vtable hierarchy for the binding whose method equals this method closure).

## 4. Cross-cutting

- **GC / .oimg / dialect:** none — no struct, allocation, or serialized-shape change.
  Latest dialect stays #19; both image paths structurally identical.
- **`-D__ZEPHYR__`:** a function-body simplification stays syntax-clean.
- **Behaviour:** identical on every reachable path. `test_access.c` (the sole exerciser
  of the access branch, all method-frame callers) is unchanged in outcome.

## 5. Validation

- **`oevm/tests/test_access.c` green** — the only test that drives
  `check_access_allowed`'s PRIVATE/PROTECTED branches; all six sub-tests pass
  unchanged, proving the reachable path (method-closure immediate return) is preserved.
- **Grep proof (structural gate):** `get_top_level_method_closure`'s body no longer
  references `home_ctx_frame` or performs a `while`/`for` walk — a scoped grep asserting
  the dead walk is gone (behavior-proof ≠ intent-proof: this is the "the refactor
  actually happened" check).
- **The invariant fail-fast is asserted, not exercised at runtime** — there is no
  dispatch path that produces a block closure here (that is the whole point). Do **not**
  attempt a `.ns` red→green regression; the scenario is unconstructible today. If a
  future phase enforces private-from-block, that phase owns the test.
- **House gate (deferred-lift standard):** full suite **both paths** (`OEVM_PATH=AB`) +
  `fibers-gc-stress` + `45_fibers` interleave + `portability-32` + `boot-gc-asan` +
  ASan/`OEVM_GC_STRESS=1` clean + `-D__ZEPHYR__ -fsyntax-only` clean. Run the suite
  **solo** (harness not concurrency-safe) and in the **foreground**.

## 6. Risk

Minimal. A dead-code deletion plus an invariant guard; no new state, no format/dialect
change, behaviour-identical on all reachable paths. The only way to "break" it is for a
future change to route a block closure into access control, which the guard converts
from silent-wrong into a loud abort.

## 7. Out of scope (later sub-phases)

- **E2:** inlined loop-var captures (reader #1) — compiler Context-homing.
- **E3:** reified-`Context` home producer + `home_generation` + NLR identity (oevm#34);
  the gen-tag-vs-reify fork is decided there.
- **E4:** cross-fiber `^` (reader #2) — reified cross-fiber unwind vs `cannotReturn`.
- **Enforcing method privacy** (compiler emits `access_modifier` from `visibility`, plus
  the `caller_self = regs[0]`-for-blocks fix it would surface) is a **separate feature**
  outside the reify lane — explicitly not undertaken here (David's call, 2026-07-03).
- Catchable `StackOverflow` / image exception facility (rides the exception phase).
