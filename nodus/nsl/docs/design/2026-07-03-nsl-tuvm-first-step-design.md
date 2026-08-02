# nsl-tuvm — reactive, capability-secure, no-NLR node runtime (first-step design)

**Status:** brainstormed + approved-in-dialogue with David 2026-07-03. This spec covers
the **limited first step only** — it deliberately does *not* design the whole runtime.
It stands up two seeded repos, deletes the complexity balloon, and proves the core
execution model on the host. Everything past that is fenced in §8.

**Seeds (fresh `git init`, provenance recorded, originals frozen):**
- `tuvm`      ← seeded from `oevm@aea0af1`      (the substrate VM)
- `nsl-tuvm`  ← seeded from `ns-oevm@188426e`   (the language frontend)

The originals (`~/workspace/oevm`, `~/workspace/ns-oevm`) remain untouched as the full
historical record (fibers, de-recursify, reify-on-escape E1, everything). Nothing is
lost; they are simply no longer the active line. The seeded repos owe them **zero**
backward-compatibility.

---

## 1. Why (the pivot)

Two days of work on fibers + non-local return (NLR) + reify-on-escape + growable stacks
showed the cost of preserving *full* Smalltalk/Newspeak semantics on a cooperative,
heap-stack, growable-stack VM: a cascade of interacting complexity (E1–E4, cross-fiber
closure escape, stack-grow straddle) with the VM ballooning far past its simple intent.

The strategic bet is **not** to beat Toit on type-safety or raw performance — Toit wins
both. It is:

1. **Fit the 256 KB-RAM class (nRF52840)**, which Toit cannot.
2. **Expose Zephyr's large driver/subsystem ecosystem — OpenThread, displays, sensors —
   as confined, ergonomic OO capabilities.** "Reuse a Zephyr display driver, not write
   my own" *is* the product.

So nsl-tuvm = a tiny (~24 KB engine) capability-secure OO scripting layer that sits on
top of Zephyr, surfaces Zephyr drivers as confined nsl capabilities, and coordinates a
handful of supervised user jobs through a reactive, capability-scoped tuple substrate —
**consistent with Toit's proven message model** (closed immutable value algebra, portable
wire encoding, supervised jobs, jag/porta-style deployment) even while giving up Toit's
static types and performance.

The enabling simplification: **give up NLR.** NLR is the connective tissue that forced
the reify/unwind/cross-fiber cascade; dropping it *deletes* (not defers) that balloon.

## 2. The model (validated in dialogue; recap)

- **No NLR** (st-berry's proven playbook, `st-berry/docs/bytecode-compiler-references.md`):
  - `^` inside an **inlined control-flow block** (`ifTrue:`/`whileTrue:`/`do:`/`on:do:`/
    `ensure:`) = an ordinary local method return (zero VM machinery).
  - `^` inside an **escaping closure** (stored/passed) = a **compile error**.
  - The **collection combinators** (`detect:ifNone:`, `any:`, `all:`, `inject:into:`,
    `select:`/`collect:`/`reject:`) are the sanctioned replacement for loop-and-break;
    most "early exit" is a search, which combinators express without break.
- **Exceptions on a separate unwind channel**, independent of value-return. This is also
  the supervisor's let-it-crash boundary. (First step ships only the *minimal* boundary —
  see §4/§5; full user-facing `on:do:`/`ensure:` is §8.)
- **Reactor + supervisor live in nsl; the C seam carries data, not control.** Drivers,
  blocking I/O, the VM, GC, and the message codec stay in C; *all policy* (dispatch,
  reconcile, lifecycle, capability granting, restart) is nsl. This finishes nodus-st's
  unmet goal of lifting the supervisor out of C.
- **Coordination = the disciplined tuple subset** (capability-scoped reactive channels):
  - Channels are **URL-style paths**; a **capability is a channel rooted at a prefix**,
    attenuated by extending the prefix (matches porta's `resource?id=` addressing).
  - Three non-blocking verbs only: `out:value:`, `whenever:do:` (standing reaction, runs
    a handler to completion on match), `take:` (remove-if-present-now, else nil).
  - **Forbidden** (kept out on purpose): blocking `in`/`rd`, rich associative/wildcard
    matching, and persistent tuples. The first step does **exact-channel** dispatch with
    **transient** signals only. This degenerate case *is* a plain reactor (§6).
- **Messages = immutable values over a closed algebra** (Toit's prior art, verified):
  `null, bool, int, float, string, bytes, symbol, list, map, record`, recursively closed,
  depth-bounded, acyclic. Immutability is **structural** (a mutable object is
  unrepresentable as a message), which is what makes crash-isolation and by-reference
  sharing safe. On the single shared heap, inter-job messages pass **by reference (zero
  copy)**; only the boundary (porta wire, flash, C driver events) serializes — a portable
  encoding (TISON-analog). *We beat Toit's per-message copy here by choosing shared-heap +
  immutability.* (Ownership-transfer/neutering of large buffers is an optimization — §8.)
- **Robustness (design; first-step subset in §4):** soft watchdog (a bytecode/safe-point
  budget reusing oevm's existing debugger safe-point hook) + hard Zephyr WDT; OTP-style
  restart/quarantine with restart-intensity limiting.

## 3. Repo layout & seeding

```
~/workspace/oevm       (FROZEN — historical record)
~/workspace/ns-oevm    (FROZEN — historical record)
~/workspace/tuvm       (NEW — seeded from oevm@aea0af1; the substrate VM)
~/workspace/nsl-tuvm   (NEW — seeded from ns-oevm@188426e; compiler + dialect + stdlib)
~/workspace/nsl-tests  (NEW — seeded/pruned from ns-tests; conformance for the new dialect)
```

Each new repo: `cp -r` the working tree **without `.git`**, then `git init`, then one seed
commit whose message records the exact source SHA. Node/Zephyr integration repos
(`ns-posix`/`ns-zephyr`/`nodus`) are **not** seeded in the first step — first step is
host-only.

## 4. Scope of the limited first step

Four milestones, host-only, each independently verifiable. The bar is: **prove the model
runs end-to-end on the desktop with the balloon gone**, nothing more.

**Milestone A — Seed + delete (the verified-simplification milestone).**
- Seed `tuvm`, `nsl-tuvm`, `nsl-tests` per §3.
- **Delete from `tuvm`:** cooperative fibers (R7 scheduler + fiber prims), NLR unwind
  machinery, reify-on-escape (the E-series: `Context`-as-`home_ctx`, `home_ctx` frame
  identity, `home_ctx_frame`, reified-`Context` reservation), growable stacks
  (`stack_ensure`/grow), `ucontext`/hijack remnants, and any `.oimg` format fields that
  existed only for the above.
- **Keep in `tuvm`:** the register interpreter core **including the flat/heap-language-stack
  loop** (retained for its O(1) C-stack robustness on tiny MCU C-stacks — but stripped of
  the fiber/NLR/reify layers, which is the bulk of its complexity), precise GC, `.oimg`
  loader, capability/confinement model, the confined native-capability seam
  (`backend/<target>/`), and the in-image debugger (its safe-point hook is reused by the
  watchdog later — keeping it is near-free).
- **Delete from `nsl-tuvm`:** NLR codegen, the concurrency/fiber dialect + combinator
  lowerings tied to fibers.
- **Prove:** the reduced VM boots and a **pared conformance subset** (`nsl-tests`, the
  non-fiber non-NLR tests) is green on the host; **bank the footprint delta** (engine size
  should drop below the fiber-carrying baseline).

**Milestone B — Immutable message algebra.**
- Implement the closed value algebra as tuvm values: reuse existing `int/float/string/
  bytes/symbol`; add **immutable `list`/`map`** and a **tagged `record`** (named message
  type). Enforce deep-immutability + depth bound + acyclicity by construction.
- **Prove:** unit tests constructing nested messages; a mutation attempt is a compile/
  runtime rejection; a message shared between two references cannot be mutated.

**Milestone C — Capability-scoped channels + reactor.**
- Implement the `TupleSpace` (exact-channel registry), the rooted-prefix `Channel`
  capability (`channel:`, attenuation), the three verbs (`out:value:`/`whenever:do:`/
  `take:`), and the supervisor's reactor loop (idle-wait on the space; dispatch one firing
  to completion). Degenerate tuple space only (§6).
- **Prove:** posting to a channel fires a registered reaction to completion; a job holding
  a rooted-prefix capability cannot address outside its prefix (confinement test).

**Milestone D — Supervisor + let-it-crash + a host job (the end-to-end proof).**
- Supervisor: owns the space, grants channel capabilities to a job at setup, wraps each
  handler dispatch in a **minimal crash boundary** (a C-level `setjmp`/`longjmp` at the
  reactor loop — sufficient because dispatch is single-stack run-to-completion; full
  language-level `on:do:`/`ensure:` is §8). A faulting handler unwinds to the reactor,
  the supervisor logs + applies a stub restart policy.
- A host event source (e.g. a timer tick or stdin line, injected by a host seam) posts a
  channel message; a user **job** reacts and posts a result the supervisor observes.
- **Prove:** the end-to-end loop runs on the host (`event → C seam → out: → reaction →
  out: → supervisor observes`); a deliberately-crashing handler is contained and does not
  take down the node; other reactions keep running.

## 5. Key design decisions (leans; open items flagged for the plan)

- **Flat loop kept, stripped** (not reverted to recursive `run_interpreter`): retains
  O(1) C-stack robustness (matters with MCU C-stacks) while shedding the fiber/NLR/reify
  layers that made it complex. *Open for the plan:* confirm the stripped flat loop is
  meaningfully simpler than a recursive rewrite; if not, reconsider.
- **Minimal crash boundary via C `setjmp`/`longjmp`** at the reactor for the first step;
  it is the smallest thing that delivers let-it-crash. Full `on:do:`/`ensure:` (the
  separate-channel exception model) is built properly in a later step and must be
  compatible with this boundary.
- **Immutable list/map/record are new** (oevm arrays are mutable); the algebra needs a
  genuinely immutable value family. *Open for the plan:* frozen-by-construction types vs.
  a freeze operation — pick frozen-by-construction (no mutable phase).
- **`.oimg` kept** (Model B remains primary per the established direction) — the delete
  must leave the loader working; verify no format field is orphaned by the balloon delete.
- **Channel/space core in C, policy in nsl:** the space storage + exact-match dispatch is
  a C primitive (speed); `out:`/`whenever:`/`take:`/reactor/supervisor are nsl over it.

## 6. Why this does not preclude the "modern" TupleSpace

The first-step coordination layer is the **degenerate case** of a full tuple space:
exact-channel match = a one-field template; transient signals = no persistence; no
wildcards. Every later generalization is **additive** on the same VM shape:
associative/wildcard matching, persistent state-tuples (with a lifecycle/TTL policy),
and large-buffer ownership-transfer are added *over* this substrate without reworking the
reactor, the message algebra, or the capability model. We build the reactor first and
generalize into the tuple space only when real multi-job workloads justify it.

## 7. Validation / success criteria

First step is **done** when, on the host:
- `tuvm`/`nsl-tuvm`/`nsl-tests` exist as seeded repos with recorded provenance;
- the balloon is deleted and the pared conformance subset is green;
- the footprint delta is measured and recorded (expected: engine shrinks);
- the message algebra rejects mutation/cycles and shares immutable values by reference;
- capability-scoped channels enforce prefix confinement;
- the end-to-end `event → reaction → supervisor` loop runs, and a crashing handler is
  contained (let-it-crash), with other reactions unaffected.

Run the suite **solo** and in the **foreground** (harness not concurrency-safe).

## 8. Non-goals (explicitly deferred)

- porta wire integration + the portable (TISON-analog) encoding across the C/network seam.
- Zephyr target, real drivers (display/OpenThread/sensors), the native-capability bring-up.
- Soft watchdog (safe-point budget) + hard Zephyr WDT; full OTP restart policy.
- Full user-facing exceptions (`on:do:`/`ensure:` on the separate channel).
- Large-buffer ownership-transfer/neutering (the zero-copy optimization).
- The full TupleSpace generalizations (wildcards, persistent state-tuples, rich matching).
- Multiple hardware event sources; power-mode/deep-sleep; triggers.
- Any node/keeper integration repo (`ns-posix`/`ns-zephyr`/`nodus` successors).
