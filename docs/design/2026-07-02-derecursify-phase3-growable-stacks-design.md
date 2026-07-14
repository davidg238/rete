# De-recursify deferred lift — Phase 3: growable stacks + overflow policy (design)

**Status:** DONE 2026-07-02 (scope **A**). Shipped on branch
`derecursify-deferred-phase3` (4 SDD tasks, each reviewed clean). Predecessor phases
(temp-vectors,
self-migration + relativize `home_ctx`) DONE + pushed; both frame links are now
relative SMIs, so the substrate for growth is in place. Parent design:
`2026-07-01-derecursify-deferred-lift-growable-stacks-upward-closures-design.md` §4
Phase 3 + §7.

**One-line:** replace the fixed-capacity per-fiber stack + `abort()`-on-overflow with
a `realloc`-style grow-on-demand stack (footprint win: start small, grow only as
needed), and adopt a robust hard-cap policy that keeps the node alive.

---

## 0. What shipped (2026-07-02)

Behaviour-equivalent below the cap; suite **86/0/6 BOTH paths** + **7 gates**
(added `stack-overflow-cap` + `growable-stack-small-init`), no `.oimg` format change
(oevm branch diff = `src/vm.c` only; registry `VERSION` stays 4; Path B green), no new
dialect row (growth is behaviour-equivalent; latest row stays #19).

- **Growth mechanism** (`stack_ensure`): a push that would overflow the active fiber
  Stack's real capacity grows it (`stack_alloc` a larger Stack, `memcpy` the live
  prefix, repoint `vm_stack`/`g_active_stack`/`current_frame`/the Fiber's `stack_obj`);
  relative `prev_frame`/`home_ctx` survive with zero fixups. `receiver` is temp-rooted
  across the one new GC point. Reload surface: `args`/`prev_ctx` rebased in `push_frame`;
  `run_interpreter`'s `entry_ctx`→`entry_off` (offset) and `prev_ctx`→base-relative link
  so a nested grow can't dangle them.
- **Sizing knobs** (`-D`-tunable): `VM_STACK_INIT_WORDS` (16384, == legacy host default →
  zero host behaviour/perf change), `VM_STACK_CAP_WORDS` (262144), grow factor 2×;
  `VM_STACK_MAX_WORDS` retired. Footprint win = `-D` small init on embedded.
- **Hard-cap policy A**: at the cap, a clear diagnostic (fiber, needed words, cap) +
  `abort()`. Catchable-in-image `StackOverflow` / worker-fiber survival deferred (needs
  the exception facility; see §4).
- **Known limitation filed oevm#34**: a `^` whose unwind straddles a grow strands its
  (absolute, kernel-flowed) target frame — host-unreachable (needs NLR > init depth);
  guarded by a fail-fast in `do_unwind`; proper fix = the deferred reify-on-escape phase.
- **Commits** (branch `derecursify-deferred-phase3`): oevm `aad4df0` (mechanism),
  `45410f1` (oevm#34 guard), `426cff0` (hard-cap A); ns-tests `b874315`
  (`58_growable_stack`), `16c249f` (`59_grow_under_fibers`); ns-oevm `9be0fdb`
  (`stack-overflow-cap` gate), `181ac35` (`growable-stack-small-init` gate).

---

## 1. What is already true (grounded in the current engine)

- **Both frame links are relative SMIs.** `prev_frame` (dynamic) since L1;
  `home_ctx` (lexical) since Phase 2 (the flip). `frame_abi.h`: relative offsets
  "survive a stack realloc with ZERO fixups: only `stack_base` changes; every stored
  offset stays valid." So the *stored* links need no repair on grow.
- **`Stack` is a first-class heap object** (`struct Stack` in `oevm.h`) already
  carrying `capacity`, `sp`, `current_off`, `oop data[]`. Allocated by `stack_alloc`
  (`dispatch.c`), GC-traced by `StackVtable`. `vm_stack == ((struct Stack*)g_active_stack)->data`;
  `vm_stack_ptr` is a word-index cursor (an offset, not a pointer).
- **Overflow today** (`vm.c:177`, `:287`, `push_frame` + `run_interpreter` inline
  setup): tests `vm_stack_ptr + frame_words > VM_STACK_MAX_WORDS` (a *compile-time
  constant*, not the Stack's real `capacity`) → `fprintf` + `abort()`.
- **`VM_STACK_MAX_WORDS`** (`vm.c:33`, default 16384) is used for **both** the
  initial per-fiber allocation (`vm.c:255` bootstrap; `vm.c:1231` `primitive_fiber_create`)
  **and** the overflow ceiling. Growth must split these two roles.
- **Non-moving GC.** The conservative C-stack scan was deleted in core Phase 3;
  rooting is precise (`scan_vm_stack` over `g_active_stack` + `StackVtable` tracer for
  suspended fibers + precise roots). Heap oops do **not** move — only the stack `data[]`
  region is copied by us, explicitly, during a grow.

## 2. The reload surface (the one thing that actually needs care)

A grow allocates a larger `Stack` and copies `data[]`, so `vm_stack` (the base
pointer) changes. Every **stored** frame link is a relative offset → survives
untouched. The hazard is **live C-local raw pointers into the old stack** held across
the grow. The complete set (audited):

| Holder | Kind | Handling on grow |
|---|---|---|
| `vm_stack` | global base ptr | `stack_grow` repoints it to new `data[]` |
| `g_active_stack` | global Stack oop | `stack_grow` repoints to the new Stack |
| `current_frame` | global frame ptr | `stack_grow` rebases by the base delta |
| `((struct Fiber*)current_fiber)->stack_obj` | per-fiber Stack oop | `stack_grow` repoints |
| `vm_stack_ptr` | global **offset** | unchanged (it is already relative) |
| `ctx` | flat-loop frame ptr | already reloaded from `current_frame` after every push |
| `prev_ctx` | `push_frame` local | reload `= current_frame` after the grow |
| `args` / `send_args` | `push_frame` arg ptr | if it points into the **old** stack range, rebase by the base delta *before* reading |
| **`entry_ctx`** | flat-loop bottom-out marker | **convert pointer → offset** (`entry_off`); compare via offset, not identity |

**`entry_ctx` is the subtle one.** It is a C-local pointer captured once at loop entry
(`vm.c:325`) and compared by identity at the bottom-out (`vm.c:994`
`ctx == entry_ctx`) and the unwind boundary (`vm.c:1019` `uf == entry_ctx`). A *nested*
push that grows would leave `entry_ctx` dangling into the freed old stack → the
comparison never matches → the loop fails to bottom out (and reads freed memory). Fix:
store it as an **offset** `entry_off = (oop*)ctx - vm_stack` at entry and compare
`(oop*)ctx - vm_stack == entry_off`. This is the same relative-link philosophy already
applied to `prev_frame`/`home_ctx`, now extended to the last absolute C-local held
across a grow. (`saved_stack_ptr` in the F4 re-entrant path is *already* an offset —
no change.)

**`args` rebasing.** On the hot path `send_args = &ctx->regs[...]` points into the
stack, and is passed into `push_frame`, which reads it *after* the capacity check.
`stack_grow` must therefore run *before* `push_frame` reads `args`, and `push_frame`
must rebase `args` when it fell inside the old stack: `old_base <= args < old_base +
old_capacity` → `args = new_base + (args - old_base)`. Args that point at C-locals
(the cold F4 callers — debugger hook `&dbg_arg`, DNU handler, init) are outside that
range and left untouched.

## 3. Mechanism

Add one helper, used at both frame-push sites — `push_frame` (`vm.c:172`) and the
`run_interpreter` inline entry setup (`vm.c:287`); there is no `push_frame_reg` (a
stale reference in the parent design):

```
/* Ensure the active fiber stack can hold `extra_words` more above vm_stack_ptr,
   growing (realloc-copy-to-larger) if needed. Returns the base delta in words
   (new_base - old_base), 0 if no grow happened. Updates vm_stack, g_active_stack,
   current_frame, the active Fiber's stack_obj, and the new Stack's capacity/sp.
   Raises the hard-cap policy (§4) when the grown size would exceed VM_STACK_CAP_WORDS. */
static ptrdiff_t stack_ensure(size_t extra_words);
```

- **Grow condition:** `vm_stack_ptr + extra_words > ((struct Stack*)g_active_stack)->capacity`
  (the Stack's *real* capacity, not the old constant).
- **New capacity:** `max(old_capacity * VM_STACK_GROW_NUM / VM_STACK_GROW_DEN,
  vm_stack_ptr + extra_words)`, clamped at `VM_STACK_CAP_WORDS`. Default factor 2×.
- **Copy:** `stack_alloc(new_capacity)` (GC-managed; may trigger a collection — the
  **old** Stack is still `g_active_stack`, fully rooted, so this is safe), then
  `memcpy(new->data, old->data, vm_stack_ptr * sizeof(oop))`, set `new->sp = old->sp`,
  `new->current_off = old->current_off`.
- **Repoint globals + rebase `current_frame`** by `delta = new->data - old->data`.
  The old Stack is now unreferenced → collectable next GC (no leak, no free() — it is
  GC-managed).
- **GC-safety window:** the only allocation is `stack_alloc`, done while the old stack
  is still active/rooted; nothing half-built is reachable at the collection point.

### Sizing knobs (all `-D`-tunable; host-first)

| Knob | Role | Host default |
|---|---|---|
| `VM_STACK_INIT_WORDS` | initial per-fiber allocation | **16384** (== today; zero host behaviour/perf change — host almost never grows) |
| `VM_STACK_GROW_NUM`/`_DEN` | growth factor | 2 / 1 |
| `VM_STACK_CAP_WORDS` | hard cap (overflow ceiling) | 262144 (1 MB on 32-bit) |

`VM_STACK_MAX_WORDS` is **retired** as a name (it conflated init + ceiling); the
bootstrap and `primitive_fiber_create` allocate `VM_STACK_INIT_WORDS`. Keeping the host
init at 16384 means host runs behave and perform exactly as today (the grow path is
cold); the **footprint win is realised on embedded** by `-DVM_STACK_INIT_WORDS=<small>`
— each fiber starts small and grows only on demand, instead of pre-committing the max.
Embedded-specific sizing is deferred to a later on-silicon footprint pass (design §3
"host-first").

## 4. Overflow / hard-cap policy — THE decision for review

At `VM_STACK_CAP_WORDS` a fiber genuinely cannot proceed. The parent design's aspiration
was a **catchable `StackOverflow`** the image recovers from. **Grounded finding: the
image has no exception facility at all** — no `on:do:`, no `signal`, no `Exception`/
`Error` hierarchy. The only unwinding (`unwindTo:value:`/`resumeWith:`) serves non-local
`^`-returns exclusively; there is no handler concept to unwind *to*. The parent design
(§4 Phase 3) pre-authorised: *"If it needs more than a small seam, split it to its own
mini-phase rather than bloating Phase 3."* It does. So three scopings:

- **(A) Grow + controlled diagnostic termination — RECOMMENDED & DECIDED.** Build
  growable stacks. At the hard cap, print a clear diagnostic (fiber id, current depth,
  cap in words) and controlled-terminate the process for **all** fibers.
  **Scope-refinement finding (2026-07-02):** the originally-drafted "worker fiber dies +
  node survives" refinement is **not** achievable with existing machinery — the
  scheduler's identity lives in the image (`Concurrency`'s `schedulerFiber` slot, set at
  `run`) and it only resumes a worker via image-level signals (`doneMark`/`yieldMark`);
  the C engine cannot name the scheduler nor synthesize a signal it understands. So
  worker-fiber-survival needs the **same** signalable-error infra we are deferring, and
  moves **with** it to the deferred catchable-`StackOverflow` mini-phase. Growth itself
  already makes overflow far rarer (stacks grow to the cap before it can trigger), so the
  backstop is genuinely a backstop. This keeps Phase 3 purely the footprint win —
  smallest, lowest-risk, cleanly buildable now, **no new language feature**.
- **(B) Grow + kill-fiber-only always.** Same as A's worker path, applied uniformly;
  main-fiber overflow still ends the process (nothing to return to). Marginal delta over
  A.
- **(C) Grow + full catchable exception.** Also build a minimal image-level exception
  facility (a `StackOverflow` signal + `on:do:`-style handler) so the image recovers
  in-language. Largest scope — bundles a **new language feature** into Phase 3; exactly
  what the parent design said to split out.

**DECIDED: (A)** (David, 2026-07-02). Ship the footprint win + node-survival now; the
catchable `StackOverflow` (and the general exception facility it needs) is deferred to
its own future mini-phase / issue. Rationale: (1) the parent design explicitly
authorised the split; (2) "catchable" requires a whole exception subsystem that doesn't
exist; (3) A already satisfies the real motivation ("node survives" for a many-fiber
node) using machinery we have; (4) a runaway on the main fiber is unrecoverable
regardless of policy, so C's extra complexity buys little for the common single-fiber
overflow.

## 5. Gate / validation (per parent design §6)

Full suite **both paths** (`OEVM_PATH=AB`) + `fibers-gc-stress` + `45_fibers` interleave
+ `portability-32` + `boot-gc-asan` + ASan/`OEVM_GC_STRESS=1` clean +
`-D__ZEPHYR__ -fsyntax-only` clean. Plus **new tests**:

- **deep-recursion-that-grows:** a `sum:`/factorial depth that forces ≥1 realloc; result
  correct. Cheap-and-forcing variant: build with `-DVM_STACK_INIT_WORDS=<small>` (the
  `portability-32` pattern — a dedicated gate build) so a modest depth triggers **multiple**
  reallocs, exercising the copy + rebase repeatedly.
- **grow-under-fibers:** a fiber whose stack grows while other fibers hold live frames on
  their own (unchanged) stacks — proves per-fiber growth isolation + GC over a mix of
  grown and un-grown stacks (run under `OEVM_GC_STRESS=1`).
- **hard-cap policy (§4 A):** a runaway (unbounded self-recursion) built with a small
  `-DVM_STACK_CAP_WORDS` hits the cap; assert the controlled diagnostic termination
  (non-zero exit + the diagnostic line on stderr), exercised via a subprocess/exit-code
  check rather than an in-suite assertion (the process is expected to terminate).

Perf: growth is a cold path on host (init stays 16384); the hot push path gains one
extra predicted-not-taken compare (capacity vs constant → capacity vs field). Perfstex /
`mandelbrot` as the guard against hot-path regression.

## 6. Risks & mitigations

- **A missed live stack pointer across a grow** → use-after-free / wrong bottom-out. The
  §2 table is the exhaustive audit; the `entry_ctx→entry_off` conversion is the one
  structural change, the rest are reload-from-`current_frame`/rebase-by-delta. Gate:
  small-init multi-realloc test under ASan + GC stress makes a missed pointer
  observable (freed old Stack reused by the allocator).
- **`args` read after a grow in `push_frame`** → §2 rebase, before the arg-copy loop.
- **Fiber-switch interaction:** `primitive_fiber_switch` saves/restores per-fiber
  `stack_obj`/`stack_ptr`/`current_context`; since `stack_ensure` updates `stack_obj`,
  a later switch-out saves the grown Stack and switch-in restores it correctly. Covered
  by grow-under-fibers.
- **Scope creep into exception infra** — bounded out by choosing (A); (C) is a separate
  spec.

## 7. Deliverable shape

A small, well-bounded SDD (est. 3–4 tasks): (1) `stack_ensure` + sizing knobs + retire
`VM_STACK_MAX_WORDS`, wired at both push sites with the `entry_off`/`args` rebasing;
(2) hard-cap policy A — controlled diagnostic termination at `VM_STACK_CAP_WORDS` (clear
stderr line + non-zero exit, all fibers); (3) the new gate tests incl. the small-init
multi-realloc build + the cap-termination subprocess check; (4) docs + dialect check
(growth is behaviour-equivalent below the cap → likely **no** dialect row; the cap
diagnostic is a new observable but not a language-semantics change — confirm during
implementation). Both image paths: `Stack`/growth are runtime-only, no `.oimg` format
change (verify serializer/loader untouched).
