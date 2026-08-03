# Milestone E — full `on:do:` / `ensure:` (design)

**Status:** brainstormed + approved-in-dialogue with David 2026-07-07 ("write"). First
milestone of **phase 2** (`program-sequence.md` phase 2 rung 1 — host-only, no board).
Four forks decided by David in the same dialogue: (Q1) catch model = **predicate-filtered**
`on:[:e|…]do:` (selective propagation via a tag predicate, not an exception class hierarchy —
he pushed on "is Java's hierarchy poor design?", and we settled that the *valuable* property
is selective propagation, which a tag predicate delivers at far lower cost on a 256 KB node);
(Q2) the in-flight error value **is the crash `Record`** (`#tag`/`#text`), one representation
shared with the supervisor; (Q3) ship **`ensure:` only**, its frame marker carrying a
policy flag so a later `ifCurtailed:` is a one-line add, `ifCurtailed:` deferred; (Q4)
**no `retry`** in E (the plain `whileTrue:` retry-count idiom covers it). (Q5, decided after
scoping) `on:do:`/`ensure:` are **ordinary primitive sends**, NOT compiler-inlined — chosen
for minimal VM surface (reuse D's `run_interpreter` re-entry) and dialect uniformity; the
accepted cost is that `^` is forbidden inside handler/cleanup blocks (capture-and-return
instead). The one strike against prim-sends — the escaping-block-own-temp `OP_LD_SLOT` crash
— was **investigated and found already fixed** on HEAD (verified against the exact staged
FSM shape; §4), so E adds only a regression test to lock it, no fix.

**Parents / provenance (this spec RESTATES pinned constraints, it does not re-decide them):**
- `tuvm/review/lib/exceptions.md` — the five constraints, all load-bearing here:
  **C1** errors are first-class values; **C2** terminate-only (no `resume:`); **C3** the
  handler runs *after* unwinding (intervening frames dead first); **C4** the boundary is
  "unwind to a marked frame"; **C5** an error never crosses a dispatch boundary. Also its
  "what was deliberately NOT designed" list — the §8-time items this milestone now decides
  (predicate vs. class filtering) or explicitly defers (`ifCurtailed:`, retry).
- `2026-07-07-milestone-d-supervisor-crash-boundary-design.md` §2 (the `setjmp`/`longjmp`
  crash boundary built as unwind-to-a-marked-frame — E generalizes it), §3 (the crash
  `Record` `#crash`/`#text`/`#channel` — E makes it the in-flight value too), §9 (GC ⋂
  longjmp coherence — E's frame walk inherits the same rooting discipline).
- `2026-07-03-nsl-tuvm-first-step-design.md` §8 (the `on:do:`/`ensure:` deferral this
  milestone retires) + §5 (minimal crash boundary), §6 (additive-on-the-same-VM-shape
  guarantee).
- GitHub: `davidg238/#138` (adopt exceptions.md C4/C5 — the D half closed the
  boundary; E closes the `on:do:` generalization half).
- `tuvm/review/tests/td-crash/04` — the `policy TBD` placeholder this milestone realizes into
  a real test (`program-sequence.md` phase 2 rung 1: "Retires the td-crash/04 placeholder").
- `tuvm/review/demo-node.ns` (errors.md Tier-2 "expected + handled locally" cases — the
  driver-facing motivation for local `on:do:` alongside supervision).
- Program position: **phase 2, rung 1 of 3** (`program-sequence.md`). Rung 2 (Zephyr seam)
  and rung 3 (CBOR + Int64 wire) follow; E is host-only and needs no hardware.

---

## 1. Scope

Host-only, layered **on top of** Milestone D's crash boundary — no new concurrency, no new
capability, no target work. D installed a `setjmp`/`longjmp` boundary at the reactor frame and
proved "unwind to a marked frame" with exactly one marker. E **generalizes that same code
path** so user frames can carry markers too. `on:do:`/`ensure:` are built as **ordinary
primitive sends** (§4, Q5), not compiler-inlined — reusing D's `run_interpreter` re-entry:

- **Task 0 (prerequisite)** — the own-temp escaping-block **regression test** (§9.0). Prim-send
  handler/cleanup bodies are real closures that declare own temps and touch captured
  locals/ivars; the path is already sound on HEAD (investigated 2026-07-07), so this locks it,
  it is not a fix. Lands first.
- **`on:do:`** — a predicate-filtered handler. `[ prot ] on: [:e| pred ] do: [:e| body ]`
  runs `body` iff, when an error unwinds through this frame, `pred` answers true for the error
  value; otherwise the unwind continues past it.
- **`ensure:`** — `[ prot ] ensure: [ cleanup ]` runs `cleanup` on *every* exit path (normal
  completion or an unwind passing through).
- **`error:tag:`** — `self error: 'text' tag: #timeout` raises a tagged error;
  `self error: 'text'` keeps working (tag defaults to `#error`).
- **`raise` (re-raise)** — a handler that catches and then decides to give up re-raises the
  same value with `e raise`, resuming the walk at the enclosing frame.
- **The end-to-end proof** — a `nsl-tests/12_exceptions` black-box suite, plus realizing the
  `td-crash/04` placeholder, plus an integration check that an *uncaught* error still becomes
  the exact `sys/crash` Record the Milestone-D supervisor reads.

**Out of scope (pinned so it is not re-litigated — §10 restates the reasons):**
`ifCurtailed:` (the marker's policy flag reserves room; no caller yet), `retry` (plain-loop
idiom suffices), resumable exceptions / `resume:` (C2), an exception **class hierarchy**
(predicate filtering replaces it), frame-chain snapshots in the error value (still just
`#tag`/`#text`), and anything phase-2 rung-2+ (Zephyr seam, CBOR wire).

---

## 2. The handler-frame walk (the core mechanism)

**Pinned by exceptions.md C4** and realized minimally in Milestone D §2: the raise path is a
walk **up the live frame chain**, running cleanup blocks and looking for a matching handler.
D installed exactly one marked frame (the reactor's) and hard-stopped there; E lets user
`on:do:`/`ensure:` activations carry markers on the *same* `prev_frame`-linked heap stack,
so the walk is now general.

Why this is emphatically **not** the deleted NLR balloon (exceptions.md, restated): NLR had to
find a closure's *lexical home frame* — an identity that must survive the closure escaping,
being stored, outliving its activation (the whole `home_ctx` / reification / generation-guard
cascade). The handler walk finds the nearest *dynamically-enclosing* marked frame on the
**one live stack** at raise time. Nothing escapes; nothing outlives anything; no frame needs
an identity that survives its own death. It is a `prev_frame` traversal, not a value-return
path.

**Raise (`vm_raise`, generalizing D):**

1. Materialize the error value (a crash `Record` — §3) and root it (`g_raised_condition`,
   already rooted in `gc_collect` per D §9).
2. Walk `current_frame` → `prev_frame` → … From each frame, read its marker (if any):
   - **cleanup marker** (`ensure:`): run its block now (E: policy = `always`, so it always
     runs during an unwind). See §5 for ordering and re-entrancy.
   - **handler marker** (`on:do:`): evaluate its **predicate** block against the error value
     (§6). If it answers **true** → this frame catches: set `current_frame` to it, discard the
     frames above (they are now dead — C3), and run the handler **body** with the error value
     as argument. Stop.  If **false** → the handler is transparent; keep walking.
3. If the walk reaches the **reactor frame** (D's marker) with nothing having caught, we are
   in Milestone D's exact path: the crash `Record` is posted to `sys/crash` and the supervisor
   applies policy (§7). This is the degenerate case, unchanged.

**No interpreter surgery** — as in D, the handler body and `ensure:` blocks execute by
ordinary `run_interpreter` re-entry (the DNU-handler / `vm_protected_fire` precedent), not by
a new interpreter mode. The mechanism is: mark frames, unwind by resetting `vm_stack_ptr` /
`current_frame` / `gc_temp_roots_count` to the caught frame's saved values, then re-enter to
run the handler.

---

## 3. The error value — one representation (Q2)

**Pinned by exceptions.md C1** and Milestone D §3: the error is a first-class value, and it is
**the same `Record` the supervisor already observes**. E makes that Record the *in-flight*
value too (D carried a bare string in flight and manufactured the Record only at the
boundary):

- `self error: 'text'` → raises `Record tag: #error with: #text -> 'text'`.
- `self error: 'text' tag: #timeout` → raises `Record tag: #timeout with: #text -> 'text'`.
- A handler predicate/body reads `e tag` and `e text` — ordinary `Record` accessors (rank 9,
  Milestone B).

**`#channel` attaches at the reactor boundary, not at the raise.** The raiser does not know the
firing channel; the reactor does. Because a `Record` is an **immutable value** (it cannot be
mutated to add a field), the uncaught path *constructs a new* crash Record from the caught
value's `#tag`/`#text` plus `#channel` — exactly D's `postCrash:channel:`, now fed a Record
instead of a string. A handler that catches *before* the reactor sees `#tag`/`#text` with no
`#channel`, which is correct (it is inside the reaction; it already knows its own channel).

**Implementation latitude (fixed at plan time, not here):** whether `PRIM_error` builds the
full `Record` eagerly, or raises a tiny C-side condition carrying the `tag`/`text` oops that is
materialized into a `Record` at first observation (first predicate evaluation or the boundary),
is a plan-time performance choice. **The contract is fixed regardless:** what a handler
predicate/body and the supervisor observe is a `Record` answering `tag` and `text`. Eager
construction is the default unless the plan finds it costs a GC-rooting hazard in `PRIM_error`.

---

## 4. Language surface & compilation

Driver-facing surface (the whole of it):

```smalltalk
"raise"
self error: 'tmp117 timeout' tag: #timeout.
self error: 'generic failure'.                 "tag defaults to #error"

"selective catch — the predicate owns what this handler catches"
[ node readTemp ]
  on:  [ :e | e tag = #timeout ]
  do:  [ :e | e text log. node useLastGood ].   "a non-#timeout error keeps unwinding"

"always-run cleanup"
[ bus claim. node write: cmd ]
  ensure: [ bus release ].

"catch, then give up — re-raise the SAME value to the next enclosing handler"
[ node commit ]
  on: [ :e | e tag = #io ]
  do: [ :e | node log: e. e raise ].
```

- `on:do:` and `ensure:` are **ordinary primitive sends** — real keyword messages whose block
  arguments are genuine closures, exactly like `whenever:do:`, `detect:ifNone:`, and the FSM's
  `state:on:do:then:` already are (decided with David 2026-07-07; see the Status fork). A
  primitive installs the frame marker referencing the protected/predicate/handler/cleanup
  closures, invokes the protected closure, and removes the marker on normal exit; when the walk
  (§2) reaches the marker it fires the stored closure via the **same `run_interpreter` re-entry
  Milestone D already uses** for `vm_protected_fire`. This was chosen over compiler-inlining
  for a **minimal VM/compiler surface** (no new inlining pass, no marker bytecode — reuses D's
  proven re-entry) and **uniformity** with every other block-taking construct in the dialect.
  *(Open sub-decision carried to the plan: on which class the selectors live — Block vs. a
  kernel facade — and the exact marker record shape. Contract is fixed; placement is plan-time.)*
- **Consequence of prim-sends (the accepted tradeoff):** the protected/handler/cleanup blocks
  are real closures, so — under the no-NLR model — a `^` inside them is **forbidden** (the
  `07_no_nlr` prohibition, same as every reaction/FSM-action block today). Authors capture the
  `on:do:` result and `^` it from the method body instead: `r:: [ … ] on: […] do: […]. ^ r`.
  This is consistent with the whole dialect (no block passed to a method carries `^`); the
  errors.md guidance states it, and a `12_exceptions` test asserts `^`-in-handler is a
  compile-error (reusing the frozen `07_no_nlr` diagnostic).
- **Prerequisite regression test (own-temp escaping blocks).** Because handler/cleanup bodies
  are now real closures that will routinely declare their own temps *and* touch captured
  locals/ivars, E depends on the escaping-block-own-temp path being sound. That path was a
  Milestone-C-era VM/compiler limitation (`OP_LD_SLOT`/`Invalid receiver`) the reviewer
  *avoided* by hoisting; investigation 2026-07-07 found it **no longer reproducible on HEAD**
  (fixed incidentally in B/C/D compiler/`vm.c` work — verified against the exact staged
  `states at: s ifAbsent: [ | t | t:: plat Dictionary new. states at: s put: t. t ]` shape,
  which now runs correctly). E therefore adds **no fix**, only a **regression test** locking it
  (Task 0 / §9), since it is currently only implicitly exercised and could silently regress.
- `error:tag:` is a primitive raise (companion to today's `error:` at `dispatch.c`).
- `e raise` re-raises the receiver error value, resuming the walk (§2) at the frame enclosing
  the current handler. `e` is the crash `Record`; `raise` is its re-raise selector.

---

## 5. `ensure:` semantics + the policy-flag guard (Q3)

- **Always runs.** `[ prot ] ensure: [ cl ]` runs `cl` on the normal-completion path *and*
  when an error unwinds through the frame (§2 step 2). This is the real embedded need:
  release the bus / cancel the timer no matter what.
- **Ordering: innermost-first.** When the walk passes nested `ensure:` frames, their cleanup
  blocks run in innermost-to-outermost order (the order the walk reaches them). A test pins
  this (§9).
- **The policy-flag guard (the cheap future-proofing).** The frame's cleanup marker is
  represented as **`{ cleanup block, policy }`** where `policy ∈ { always, curtailed-only }`.
  E only ever sets `always`, and the normal-exit path runs `always` markers unconditionally.
  A future `ifCurtailed:` (§10, deferred) is then a one-line add: a new selector that installs
  the same marker with `policy = curtailed-only`, and the normal-exit path skips
  `curtailed-only` markers. No marker-format change, no change to `ensure:` or `on:do:`, no
  caller breaks — exactly the exceptions.md-C4 "build the general shape once" discipline
  applied one level down.
- **Errors inside a cleanup block** propagate to the next enclosing handler (they are raised
  during the walk; the walk simply continues from the enclosing frame with the new error).
  A test pins that an `ensure:` that itself crashes does not wedge the unwind.

---

## 6. Predicate-filtered catch semantics (Q1)

- A handler **catches iff its predicate block answers `true`** for the error value. The
  predicate is an ordinary one-arg block `[:e| … ]` evaluated during the walk (§2), receiving
  the crash `Record`; it typically tests `e tag` (and may test `e text`).
- **Non-matching handlers are transparent** — the walk continues past them, giving *selective
  propagation*: `[ readTemp ] on: [:e| e tag = #timeout ] do: […]` catches a `#timeout` and
  lets a `#sensorFault` keep unwinding to an outer handler or the supervisor. This is the
  property that makes a class hierarchy valuable, delivered by the tag predicate.
- **A matched handler's body runs after the intervening frames are dead** (C3). The body may:
  complete normally (the whole `on:do:` expression's value is the body's value), or re-raise
  (`e raise`, propagating to the enclosing frame), or raise a *new* error (propagates likewise).
- **Predicate hygiene (documented, not enforced):** a predicate block should be a pure test —
  no side effects, no `out:`/`error:` — since it runs mid-unwind. A predicate that itself
  raises is treated as non-matching-and-propagating the new error (spec pins the behavior; a
  test covers it) but the errors.md guidance will say "don't."

---

## 7. Uncaught path, supervisor integration, and no-cross-dispatch (C5)

- **Uncaught → supervisor, unchanged from D.** If the walk reaches the reactor frame with no
  match, the crash `Record` (now carrying `#channel`, §3) is posted to `sys/crash` and the
  Milestone-D supervisor applies restart/quarantine. `on:do:` changes *nothing* about that
  path; it only intercepts earlier. An integration test asserts the *same* Record shape
  reaches `sys/crash` whether or not any `on:do:` frames were present-but-non-matching.
- **An error never crosses a dispatch boundary (C5).** The walk stops at the current firing's
  reactor frame at the latest. A handler in job A cannot catch job B's crash — cross-job
  failure remains the supervisor's channel, by design, and is identical before and after E.
  A test posts two events whose reactions are independently protected and confirms one
  reaction's crash cannot be caught by the other's handler.
- **`on:do:` composes with the crash boundary, does not replace it.** Local `on:do:` is the
  errors.md **Tier-2** tool (expected, recoverable-in-place — a sensor timeout the driver
  retries/degrades); let-it-crash + supervision remains the default and the backstop for
  **Tier-1** (programmer bug) and anything a handler re-raises.

---

## 8. GC ⋂ unwind coherence

E inherits Milestone D §9's discipline verbatim, extended to the now-multi-marker walk:

- `g_raised_condition` (the error `Record`) is rooted across the walk and across any
  `gc_allocate` a cleanup block or handler body triggers — the D rooting stays, and the
  materialize-Record step (§3) must root its intermediates via the
  `gc_temp_roots_count` SAVE/RESTORE idiom, never the flat clear (boot roots live there).
- Unwinding restores `vm_stack_ptr` / `current_frame` / `gc_temp_roots_count` to the **caught
  frame's** saved values (D restored them to the reactor frame's; E restores to whichever
  marked frame caught). Cleanup blocks run *before* the restore for the frames they belong to,
  in the walk order.
- Reviewers of the walk diff hunt the D bug-class explicitly: any C code holding heap oops
  across an allocation during predicate evaluation, cleanup, or Record materialization must
  root them (space_dequeue precedent).

---

## 9. Tests (`nsl-tests/12_exceptions`, black-box, both-path)

Every claim a runnable file with an `expect:` block, verified on Path A (solidified) and
Path B (`.oimg`); the controller owns the full-suite gate (implementers run single-file both-
path only — the Milestone-D concurrency lesson).

0. **own-temp escaping-block regression (Task 0, prerequisite — lands FIRST, before any E
   surface).** Lock the escaping-block-own-temp path the prim-send handlers depend on:
   `03_blocks/…` (or `12_exceptions/00_own_temp_escaping.ns`) exercising an escaping,
   non-inlined block that declares its own temp AND captures an enclosing arg/ivar and
   allocates — the exact staged FSM shape
   `states at: s ifAbsent: [ | t | t:: plat Dictionary new. states at: s put: t. t ]`.
   Expected: correct get-or-create, no `OP_LD_SLOT` abort, A==B. (Already passes on HEAD —
   this pins it against silent regression; it is the only "own-temp" work in E, no VM fix.)
1. **catch match** — a `#timeout` raise is caught by `on:[:e|e tag = #timeout]do:`; the body
   runs, the protected block's tail after the raise does not (unwind proof).
2. **selective propagation** — a `#sensorFault` raised under an `on:[:e|e tag = #timeout]do:`
   is *not* caught there; it reaches an outer handler (or the supervisor).
3. **ensure: normal path** — cleanup runs when the protected block completes without error.
4. **ensure: unwind path** — cleanup runs when an error unwinds through it, *and* the error
   still propagates past it.
5. **ensure: ordering** — nested `ensure:` blocks run innermost-first (break the palindrome:
   distinct cleanup markers, per the D order-pin lesson).
6. **handler-runs-after-unwind (C3)** — a side effect placed *after* the raise in a dead
   intervening frame does not occur; one placed in the handler body does.
7. **re-raise** — a handler that catches then `e raise`s reaches the next enclosing handler
   with the same `tag`/`text`.
8. **nested / handler-in-handler** — an error raised inside a handler body propagates to the
   next enclosing handler; an error inside an `ensure:` block does too, without wedging.
9. **uncaught → sys/crash (D integration)** — an uncaught error becomes the *same* crash
   Record (`#tag`/`#text`/`#channel`) the supervisor reads; present-but-non-matching `on:do:`
   frames do not alter it.
10. **no-cross-dispatch (C5)** — one reaction's crash cannot be caught by another reaction's
    `on:do:`.
11. **td-crash/04 realized** — the `policy TBD` placeholder becomes a concrete driver-shaped
    test (a `demo-node`-style Tier-2 recover-in-place cycle using `on:do:` + `ensure:`).
12. **`^`-in-handler rejected** — a `do: [ :e | ^ x ]` (or `ensure: [ ^ x ]`) is a compile
    error, reusing the frozen `07_no_nlr` diagnostic (expect-compile-fail convention). Pins the
    accepted prim-send tradeoff (§4): handler/cleanup blocks are real closures, so `^` is out.

Gates: the Milestone-D 155-baseline plus the new suite green both paths; ASAN/UBSan clean over
the walk; footprint re-banked (text delta expected from `vm_raise` generalization + the raise
prim). `error:tag:` and `raise` added to the errors.md / CLAIMS vocabulary.

---

## 10. Non-goals (restated, with the reasons that keep them out)

- **`ifCurtailed:`** — deferred; the §5 policy flag makes it a one-line additive add when a
  workload wants "only on failure." No current caller (YAGNI).
- **`retry`** — deferred; the plain `[ ok not and: [n < k] ] whileTrue: [ [...] on:…do:[ n:: n+1 ] ]`
  idiom covers transient-fault retry with no re-execution primitive (which is mild
  continuation-shaped machinery the model avoids).
- **Resumable exceptions / `resume:`** — pinned out permanently by exceptions.md C2
  (continuation-shaped; a second balloon). Raise always unwinds.
- **Exception class hierarchy** — replaced by tag predicates (Q1). No `Error`/`TimeoutError`
  tree: a second error representation to reconcile with the crash Record, and class/vtable
  weight on a 256 KB target, for a selectivity we get from `e tag`.
- **Frame-chain snapshot in the error value** — still just `#tag`/`#text`; a captured
  backtrace is a later, additive enrichment of the same Record (exceptions.md C1 "ideally a
  frame-chain snapshot" — not now).
- **Anything phase-2 rung 2+** — the Zephyr seam (ISR→channel pulse, real timers,
  `FOOTPRINT_CONFIG=mcu`), CBOR/Int64 wire. E is host-only.

---

## 11. Why Milestone E advances phase 2

E retires the single largest first-step deferral (`on:do:`/`ensure:`) on the host, where it is
cheap to get right and fully black-box provable, *before* the Zephyr seam adds hardware
variables. It does so **additively on the Milestone-D shape** — the crash boundary generalizes
from one marked frame to many; the crash Record becomes the in-flight value it was always
destined to be (C1); the supervisor path is untouched. After E, the reactive core is
feature-complete on the host: channels + reactor (C), supervision + timers (D), and local
structured error handling (E). Rung 2 (Zephyr) then ports a *finished* model to the
nRF52840-DK rather than co-developing the model and the port.
