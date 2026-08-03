# Milestone D — supervisor + let-it-crash + host job (design)

**Status:** brainstormed + approved-in-dialogue with David 2026-07-07 ("good, proceed").
Three open forks decided by David the same day: (Q1) host job = a **demo-node-shaped
integration test** on top of the T-D scripted suite; (Q2) restart policy = **log + restart
+ quarantine counter**; (Q3) crash record = **first-class error value carrying text + origin**
(no frame-chain snapshot yet). Three in-text calls ratified with the approval: §5 K=3
consecutive-crash stub; §7 respell `%sys/*` → `sys/*`; §8 mock-sensor-can-go-silent as the
crash driver.

**Parents / provenance (this spec RESTATES pinned constraints, it does not re-decide them):**
- `2026-07-03-nsl-tuvm-first-step-design.md` §4 Milestone D, §5 (minimal crash boundary),
  §8 (non-goals) + `2026-07-03-first-step-design-addendum.md` A1 (segment-aligned
  confinement), A3 (no VM suspension — the deep-sleep/`resume:` argument).
- `2026-07-06-milestone-c-channels-reactor-design.md` D-C1..D-C5 (dispatch semantics), §6
  (owner-tagged registrations — the sweep provision bought for D), §5b (liveness/soft-WD),
  §9 Q1 (`%` is not a mechanism; supervisor namespacing = prefix discipline).
- `tuvm/review/lib/exceptions.md` — the five constraints (C1 errors are first-class values;
  C2 terminate-only; C3 handler runs after unwind; C4 unwind-to-a-marked-frame; C5 a crash
  never crosses a dispatch boundary).
- `tuvm/review/lib/timers.ns` (L3 — lands this milestone) and `tuvm/review/lib/README.md`
  (`timers.ns` → Milestone D; `exceptions.md` → read before D).
- `tuvm/review/demo-node.ns` (the north-star program + findings 1/4 — the FSM idiom and the
  supervisor/job restart contract) and `tuvm/review/test-suites-spec.md` T-D (D.1–D.6).
- GitHub: `davidg238/#138` (adopt exceptions.md C4/C5), `#9` (Milestone C carry-forwards:
  Protocol snapshot/restore test, owner-tag identity pin, `channel path too long` test).
- Program position: phase 1 milestone **D of 4** (final host milestone),
  `tuvm/review/program-sequence.md`. Milestone D green is the entry criterion for phase 2.

---

## 1. Scope

Host-only, all policy in nsl over a thin C substrate. Milestone C built the reactor machine
(TupleSpace, channel capabilities, three verbs, `Space>>drain`). D **wraps that machine**:

- **Crash boundary** — a C `setjmp`/`longjmp` at the reactor, built as "unwind to a marked
  frame" with exactly one marked frame. `self error:` becomes the raise primitive.
- **Supervisor** — an nsl object owning the root space: grants capabilities to jobs at setup,
  runs the reactor loop, observes crashes, applies a restart-with-quarantine policy.
- **Timers** — `timers.ns` lands against a host timer event-source seam.
- **The end-to-end proof** — the T-D scripted suite (D.1–D.6) plus a demo-node-shaped
  integration test that runs the real Protocol-FSM + timers idiom under the crash boundary.
- **Milestone C carry-forwards** (#9) closed: Protocol snapshot/restore behavioral test;
  owner-tag identity pinned explicitly; optional `channel path too long` end-to-end test.

**Out of scope (pinned so it is not re-litigated — §11 restates the reasons):** full
`on:do:`/`ensure:` (phase 2 §8), the soft watchdog / safe-point budget (first-step §8 —
but the crash path must stay compatible with it), frame-chain snapshots inside the error
value, real i2c / ISR-pulse seams (phase 2 work orders), Zephyr target, CBOR/porta wire.

---

## 2. The crash boundary (the core mechanism)

**Pinned by exceptions.md C4 and first-step §5:** the boundary is a C `setjmp`/`longjmp` at
the reactor, and it MUST be built as **"unwind to a marked frame"** — NOT hard-coded
"crash = reset the whole stack." In Milestone D exactly one marked frame exists (the
reactor's), so the frame walk is trivial; the point is that the *code path* is the general
one, so the phase-2 `on:do:` step generalizes it (user frames carry the marker; `ensure:`
blocks run during the walk) instead of reworking it.

**New C primitive — the protected firing.** Spelled here `platform protectedFire: aBlock
value: v` (final selector is a plan-time detail; the shape is fixed):

- installs the single marked frame and `setjmp`s;
- invokes `aBlock value: v` (one run-to-completion firing — P2/P3 guarantee nothing
  suspends or re-enters);
- **clean return** → answers the block's result;
- **caught fault** (a `self error:` unwound to this marker) → answers the **crash record**
  (§3), distinguishable from any clean result.

The reactor loop, the fan-out, and all restart policy stay in **nsl**; only the `setjmp`
lives in **C** — consistent with first-step §"the C seam carries data, not control."

**`self error:` becomes the raise primitive.** `self error: 'text'` (already used by
demo-node — `self error: 'tmp117 timeout'`) now: (1) records the error text as a pending
error on the VM, (2) walks `prev_frame` to the nearest marked frame — running `ensure:`
blocks as it passes them (none exist in D; the walk machinery is present for phase 2),
(3) resets `current_frame` to the marked frame, (4) `longjmp`s to the matching `setjmp`.
Terminate-only (C2): raise never returns to its raise point. The handler (here, the
reactor's crash-record construction) runs **after** the unwind, on the reset stack (C3).

**A crash never crosses a dispatch boundary (C5).** A firing unwinds at most to the current
firing's reactor marker. Job J's fault is caught by `protectedFire:`, converted to a value,
and *posted*; the supervisor reacts to it in a **separate later firing**. No handler in
job A ever catches job B's crash — that is the supervisor's channel, by design. This keeps
crash-isolation semantics identical before and after `on:do:` exists.

---

## 3. The crash record — a first-class error value (Q3: text + origin)

Per exceptions.md C1, an error is a **first-class value from day one**, and the Milestone-D
crash record on the crash channel carries *it*, not a bare string. Decision (Q3): the record
carries **text + origin**; the frame-chain snapshot is the deferred "ideal", not the D
minimum.

**Representation reuses the Milestone-B algebra — no new type.** The crash record is a
`Record tag: #crash` (a legal, immutable, postable value — it must be, to travel a channel
via `out:value:` under `checkValue:`):

| Field | Source | Meaning |
|---|---|---|
| `#text` | `self error:`'s argument | the error string (errors.md vocabulary — the future `on:do:` match key) |
| `#channel` | reactor knows the firing's channel | which channel's reaction crashed (path String) |
| `#job` | supervisor's owner→job map | which job crashed (identity resolved from the reaction owner tag, §6) |
| `#state` | present only if the job is a Protocol | the FSM's `currentState` at crash (optional; omitted for non-FSM jobs) |

`self error:` supplies `#text`; the reactor's `protectedFire:` catch augments it with the
origin **it already has in hand** (the channel being dispatched, the owner tag of the
reaction). `on:do:` later catches this **same value**; a `#frames` field can be added then
without breaking the shape. FROZEN-RESERVED spelling: `#crash` tag and the four field names.

---

## 4. The supervisor (new nsl class)

A new kernel-module class (sibling of `Space`/`Channel`). It is the entity first-step §4D
names: "owns the space, grants channel capabilities to a job at setup, wraps each handler
dispatch in a minimal crash boundary … logs + applies a stub restart policy."

**State — a job registry.** Per supervised job J: `{ rootCap, restartThunk,
consecutiveCrashes }`.
- `rootCap` — the job's root `Channel` capability, the SAME object handed to the job at
  construction (§6 makes this identity load-bearing).
- `restartThunk` — how to re-enter the job after a sweep: the demo-node contract
  `job resume: snapshot` (which itself calls `start`), or plain `job start` for a stateless
  job. Supplied at registration.
- `consecutiveCrashes` — the quarantine counter (§5).

**The reactor loop.** The supervisor runs the running-node reactor: dequeue → snapshot
(D-C5) → fire **each** reaction via `protectedFire:` (§2). This is the running-node
counterpart to `Space>>drain`; **`drain` stays** as the tests' "run until the queue is
empty" driver (demo-node `main:` uses it) — the supervisor loop is `drain` with the
per-firing `protectedFire:` wrap and the crash-record post on a caught fault. (Plan-time
choice, not a spec fork: whether the supervisor loop is literally `drain` re-pointed through
`protectedFire:` or a distinct method — the observable contract is identical.)

**Crash observation is itself a firing.** On a caught fault, the loop posts the §3 crash
record to the crash channel (`sys/crash`, §7). The supervisor's restart logic is a
`whenever: 'crash'` reaction on that channel — so it runs **protected like any other
firing** and in a **separate dispatch** from the crash it handles (C5). If the restart
reaction itself faults, that is a supervisor bug surfaced as its own contained crash, not a
node death.

---

## 5. Restart + quarantine policy (Q2)

The observable behavior T-D D.4 requires, as the smallest honest supervision stub. On a
crash record naming job J:

1. **Log** — emit the record (observable; the test asserts on it).
2. `J.consecutiveCrashes := J.consecutiveCrashes + 1`.
3. If `consecutiveCrashes < K`: **sweep + restart** —
   `platform spaceDropReactionsOwnedBy: J.rootCap` (§6), then evaluate `J.restartThunk`
   (`resume:`/`start`). The sweep is mandatory: demo-node's restart path is
   `resume: → start → whenever:` again, so a crashed job's surviving registrations would
   **double-register** (D-C4 is not hygiene here — it is structural, which is exactly why
   Milestone C §6 bought the owner tag).
4. If `consecutiveCrashes >= K`: **quarantine** — sweep J's reactions, do **not** restart,
   log a quarantine notice. J is inert until externally re-registered.

**Counter reset.** `consecutiveCrashes` resets to 0 on any **clean** firing of J (a firing
owned by J that returns without a fault) — so a job that recovers is not punished for old
crashes; only a *storm* trips quarantine.

**Stub constant (David-ratified 2026-07-07):** K = **3**, a single named constant. This is a
**consecutive**-crash stub, deliberately not a time-window policy — a real time-windowed
supervisor needs the monotonic clock and restart-rate accounting, which is more than a stub
and belongs with the full supervision story, not D. Recorded so it is not re-litigated.

---

## 6. Owner-tag identity — pinned explicitly (carry-forward #9.2)

Milestone C §6 records each registration's **owner** = the `Channel` capability that made it
(`whenever:` stores `owner: self`). The D sweep "drop all reactions owned by job J"
(`spaceDropReactionsOwnedBy:`) works **only under one structural precondition**, and #9 item
2 demands D pin it rather than inherit it implicitly:

- `platform space` mints a **fresh** `Space` per call, and every attenuation (`cap channel:
  'sub'`) mints a fresh `Channel`. Capability objects are therefore **not** interchangeable
  by value — identity matters.
- The sweep matches reactions by `==` against `J.rootCap` (nsl `==` is raw pointer identity
  on any receiver, per D-C4's precision note). It is correct **iff** the job registered its
  reactions through the *same* capability object the supervisor holds in the registry.
- That holds **because** the supervisor constructs each job with its capabilities as
  **constructor arguments** (A1 discipline / demo-node finding 5 — `TempNode
  i2c:bus:chan:timers:power:`) and keeps that identical `chan` object in the registry. A job
  that re-derived a capability internally, or was handed one through a message, would be
  un-sweepable — which is *also* why capabilities are not values (valueRank -1) and cannot
  travel channels.

Spec requirement: the supervisor's grant-at-setup is the **only** path by which a job obtains
its root capability, and the registry holds that exact object. Owner-tag identity is a
structural invariant of the runtime, not job hygiene.

---

## 7. Timers land + the `%sys/*` → `sys/*` naming cleanup

**Timers.** `timers.ns` (L3) lands as staged, against a **host timer event-source seam** —
the C work order already written at the top of `timers.ns`:
`platform timerAfter: ms token: tok`, `timerEvery: ms token: tok`, `timerCancel: tok`,
`monotonicMillis`. C fires "token N expired" onto one reserved internal channel; the
`Timers` nsl object owns the fan-out (`dispatch:`), the token registry, and cancel
bookkeeping (data in nsl, one dumb posting path in C — mirrors the general event-source
seam). Timer delivery is an ordinary reactor dispatch: run-to-completion (D-C/P3) applies
identically; no nsl runs in ISR context, ever. A late fire after cancel is dropped silently
(transient-signal semantics, already pinned in `timers.ns`).

**Naming cleanup (David-ratified 2026-07-07).** The staged `timers.ns` wires the reserved
channel as `%sys/timer`, and `exceptions.md` names `%sys/crash`. Milestone C §9 Q1 **killed
`%` as a mechanism** — supervisor namespacing is plain **prefix discipline**, no character
carries reserved meaning. So D **respells**:

- `%sys/timer` → `sys/timer` (timer seam delivery channel),
- `%sys/crash` → `sys/crash` (crash-record observation channel).

The supervisor owns the `sys/` root by holding it and **never granting `sys/**` to any job**
— exactly as it owns `sup/<job>/…` request channels (demo-node). This retires the `%`
placeholder debt in `timers.ns` and `exceptions.md`; `sys/` (supervisor-internal seam +
observation) and `sup/` (per-job supervisor-facing request channels) are both plain prefixes,
distinguished only by convention. No A1 grammar change — segments stay plain names.

---

## 8. The demo-node-shaped integration test (Q1)

Full `demo-node.ns` **cannot** run in D: its `main:` needs `platform mockI2cWithTemp:` and
`i2c onAlertPost:to:` — the i2c mock and the **ISR→channel pulse** are explicit **phase-2**
seam work orders (demo-node findings 2/5), not D. So D proves the north-star *idiom* with a
**reduced job** that uses only D-and-earlier surface:

- A `TempNode`-shaped job = **real Protocol FSM (fsm.ns, landed in C) + real timers.ns +
  publish-via-`out:value:` + crash-on-`#timeout`** (`self error: 'tmp117 timeout'`). No i2c,
  no `onAlertPost:`.
- The sensor is a **host mock event source** the test controls: normally it posts the
  "conversion done" event so the FSM completes a sample; on demand the test makes it **go
  silent**, so the `converting` state's timer fires `#timeout` and the handler crashes —
  driving the crash boundary → supervisor restart → (after K=3) quarantine, all observable.
- What it proves that the scripted D.1–D.6 do not: the **FSM + timer + crash + restart**
  composition — i.e. that `resume:` correctly rehydrates the machine (`#idle` default) and
  the swept-and-restarted job resumes sampling. This is the deep-sleep `resume:` contract
  (A3) exercised through the crash path.

"Mock sensor can go silent" is the crash driver (David-ratified 2026-07-07). The mock stays
**dumb** — data, not policy: it posts an event or it doesn't; the timeout/crash/restart
decisions all live in nsl.

---

## 9. GC ⋂ longjmp coherence (D.2 — the likeliest first-step bug-nest)

The single test (D.2) that proves the `setjmp` boundary and the GC agree. The design
invariants that make it pass:

- **Abandoned frames are ordinary garbage.** The language stack is heap-side and
  precisely GC-rooted (kept from Milestone A). Unwinding = resetting `current_frame` to the
  marked frame; the frames between raise and marker become unreachable and are collected by
  the normal precise mark-sweep. No frame carries an identity that must survive its own death
  (this is *why* exceptions are cheap where NLR was not — exceptions.md's structural
  argument).
- **No C-side space mutation is in flight across a firing's unwind.** Dequeue + reaction-list
  snapshot (D-C5) happen **before** the firing; `protectedFire:` wraps **only** the nsl block
  invocation. The C-side registry / FIFO queue / reaction storage are not mid-update when a
  `longjmp` fires, so a caught fault cannot leave a torn C structure. Enqueues made by the
  faulting handler *before* it crashed are already committed to the queue tail (P3) and are
  valid events — they dispatch normally later; this is intended, not a leak.
- **The marked frame must be GC-safe** across the `setjmp`/`longjmp` round trip (the reactor
  loop's roots — `ev`, snapshot, loop indices — stay rooted). Plan-time care item; D.2 under
  ASAN + full GC cycles after recovery is the proof.

Consequence for D.5 (soak/leak plateau): because a leaky job **cannot** be evicted by
restart (shared heap — the deliberate tradeoff), creep must be caught in test. D.5 is the
standing leak detector; heap usage must plateau across ≥100k events with periodic crashes.

---

## 10. Tests

**T-D contract (`test-suites-spec.md`), lands as written:**
- **D.1** crash containment baseline — N handlers on N channels, one crashes every firing;
  the other N−1 accumulate correct counts across rounds; supervisor observes each crash;
  node never exits.
- **D.2** crash mid-allocation — crashing handler allocates nested messages then faults;
  after recovery, allocate heavily + full GC; heap coherent. **ASAN.** (§9.)
- **D.3** crash at depth — fault inside deeply nested block invocations, and separately near
  the stack cap; unwind is clean from any stack shape.
- **D.4** restart + quarantine stub — repeated crashes of one handler trigger the §5
  policy's observable behavior (log + restart, then quarantine at K=3); pin whatever it does.
- **D.5** soak + leak plateau — ≥100k events, periodic deliberate crashes, sample heap every
  10k; usage plateaus, not creeps. **Slow — milestone gates only.**
- **D.6** event-source seam — host seam (timer) posting into the space, end-to-end
  `event → C seam → out: → reaction → out: → supervisor observes`, deterministic input.

**This design adds:**
- **D.7 (§8) demo-node-shaped integration** — Protocol FSM + timers + crash-on-timeout +
  restart + quarantine + `resume:` rehydration, driven by the go-silent mock sensor.
- **D.8 (carry-forward #9.1) Protocol snapshot/restore behavioral test** — a *direct* test
  that `snapshot` then `restore:` round-trips machine state (10_fsm only exercised the
  transition cycle / unhandled / validate). Deep-sleep persistence will depend on these, so
  they need a behavioral test before that dependency exists.
- **D.9 (carry-forward #9.3, optional/cheap)** — one `.ns` `expect-runtime-fail: channel
  path too long` driving the kernel→C length cap (≥ `OEVM_SPACE_MAX_PATH`, canonical path)
  end-to-end; C-side is covered by `test_space` property 8, the kernel-path route is not.

Suites run **solo and foreground** (harness is not concurrency-safe).

**CLAIMS / test-suites-spec** get updated: T-D rows move `pending D` → landed;
`td-crash/01..06` land; add `td-crash/07_demo_node_integration`, the Protocol
snapshot/restore test in the fsm suite, and (optional) the `channel path too long` file.

---

## 11. Non-goals (restated, with the reasons that keep them out)

- **Full `on:do:`/`ensure:`** (phase 2 §8). D builds the *degenerate* case of the same
  recovery path (one marker, no user handlers, no `ensure:` bodies to run). The §2 walk
  machinery is present so phase 2 generalizes, not reworks. Deferring is safe *because* of
  exceptions.md's five constraints, now all honored by D's boundary.
- **Soft watchdog / safe-point budget** (first-step §8). D's crash path must be **compatible**
  with it — a budget trip is a Tier-2 crash that unwinds to the reactor and hits the same
  supervisor policy (Milestone C §5b) — but the budget counter and the debugger safe-point
  hook wiring are phase-2 work, not D.
- **Frame-chain snapshot in the error value** (Q3). Shape is forward-compatible: add a
  `#frames` field when post-mortem needs it.
- **Real i2c + ISR→channel pulse seams** (phase-2 work orders — demo-node findings 2/5).
  D's mock sensor stands in.
- **Zephyr target, CBOR/porta wire, deep-sleep power management.** D proves the `resume:`
  *contract* on the host (§8); the power-mode seam and on-target persistence are phase 2/3.
- **Any tuple-space generalization** (wildcards, persistent tuples, ownership-transfer).
  Still the degenerate case (Milestone C §10); D adds a supervisor and a crash boundary over
  the same reactor, forecloses none of it.

---

## 12. Why Milestone D closes the first step

First-step §7 is **done** when, on the host: the balloon is deleted (A ✓), the message
algebra is airtight (B ✓), capability-scoped channels confine (C ✓), **and the end-to-end
`event → reaction → supervisor` loop runs with a crashing handler contained (let-it-crash),
other reactions unaffected** — which is exactly D. Milestone D green is the phase-2 entry
criterion (`program-sequence.md`): after it, the work leaves the host for exceptions-proper
and the Zephyr backend seam.
