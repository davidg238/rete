# The liveness contract — a node declares how long its silence may last

**Design record, 2026-08-21. APPROVED by David 2026-08-21** — D7's 24 h ceiling ("think fair")
and §7's staging, which deliberately puts the board-dependent kernel heartbeat last behind three
host-testable steps ("yep"). Decisions were offered and chosen inline; the chosen ones are
recorded here as D1–D7. **Next step: the implementation plan** (`writing-plans`), queued for the
next session — nothing here is built.

Related: #215 (the wedge detector, whose principle this generalises), #217 (`gw/liveness` and
the sweep), #219, #220, #225, #152, #224.

---

## 1. One mechanism, six symptoms

Six open problems look unrelated and are not. Every one of them is the same defect: **something
in this system judges a node against a number the node never declared.**

| Symptom | The invented number |
|---|---|
| The #215 detector **cannot see a node with no repeating timer** — a purely GPIO-driven node is outside the instrument entirely | there is no declared period, so there is no deadline |
| **#219** — a node that was already dead when the gateway upgraded accrues no pull rows, so cadence inference answers `ok=false` forever and it is never judged | cadence, inferred from history a dead node cannot supply |
| **#220** — a fast node's `3 × cadence` deadline is tight enough that routine blocking work writes a spurious `#silent` | `k = 3`, and an inferred cadence |
| **#152** — a UDP sink cannot tell a dead node from a quiet one; a bell speaking once per half hour went silent for two hours and nothing looked wrong | the sink has no idea what quiet means for this node |
| **#225** — `gw/liveness` read `#live` through a ten-hour gateway outage | the store keeps asserting the last thing it heard |
| radio wakeups do not coalesce | every periodic obligation picks its own period |

#215 already solved this for the node's own reactor, and the solution was not a better estimate.
It was to **stop estimating**: the detector judges the reactor against the app's *own declared*
timer periods, `8 ×` the shortest live one, so the instrument carries no idea of how often a
node should wake. That is why it caught a wedge at `deadline=8000ms` on a 1 s probe and
`deadline=16000ms` on a 2 s bridge without a line of configuration.

**This design extends that one principle across the wire.**

> Every node declares a **silence budget** — the longest it may go without speaking — and both
> the node's own detector and the gateway's sweep judge against that one declared number.
> Nothing infers.

David's framing, and it is the whole argument: *"if a node is written to be absolutely radio
silent for 1 month, that is the best we can do for health, there is no magic."*

---

## 2. Decisions

### D1 — The kernel owns the heartbeat, and it rides the reactor

VM health becomes independent of app health: the **kernel**, not the app, declares the timer and
performs the check-in. An app that declares nothing still speaks.

**It is nsl on the reactor, not C on its own thread, and the reason is not cost.**

> **A heartbeat a wedged node can still emit is a lie.** If C sent it independently of the
> reactor, a node whose VM is dead would keep reporting "alive" — which is `port=up` exactly,
> the failure mode #152 is about and the one this program has been fooled by repeatedly. The
> heartbeat has to ride the reactor **so that its absence means something.**

The C side already contributes its half. #215's observer is a cooperative-priority thread that
judges the reactor from outside it and derives its deadline from `target_timers[]`
(`timer_zephyr.c`). A kernel-owned `timers every:` registers there like any other repeater, so:

- **one declaration feeds two consumers, and adds no C at all.** The observer gets a deadline
  that *always exists* — closing its blind spot, because a node with no app timer now has a
  kernel one — and the same timer drives the check-in.
- a wedged node does not fake a heartbeat. It is rebooted in `8 × base` and reports
  `Panic { reason: #watchdog }` on the next boot, which tells the gateway strictly **more** than
  a heartbeat would have.

Two scales of detection fall out of one number: **seconds** locally (dump, reboot, self-recover)
and **the budget** remotely (the gateway's verdict).

### D2 — The node declares on `obs/liveness`; the operator requests on `goal/liveness`

`obs/**` is the node's own authority, and porta writing there is a lie (2026-08-10 D7). A
liveness budget is an observation a node makes *about itself* — it is the only party that knows
whether it can keep the promise, because battery, duty cycle and radio conditions are its own.

So the operator does not *set* the budget. They **request** one by writing `goal/liveness`, and
the ordinary `goal/X` vs `obs/X` tree diff answers "did the node accept?" — which is the
convergence view this system already has. **No new mechanism.** The node always wins.

This also makes the grid self-describing without publishing it. An operator asks for 45 s; the
node snaps to its own multiple and publishes 48 s; `goal` and `obs` differ, and *that difference
is the answer*. Nothing has to know the legal values in advance.

**Deliberately not on the debug seam.** A profile window is transient and self-limiting by
design (#165 D6); a liveness budget is a standing property. And `dbg/req` is precisely where
#180 and #206 live — where a porta write with the wrong kind, or merely one row too many
correctly kinded, wedges a node silently at *every* boot and reflashing does not clear it. That
is the last seam on which to hang fleet-wide liveness.

**A node that has never heard from porta still has a budget** — its compiled-in default. That
property is what dissolves #219 (D5).

### D3 — One node-wide base tick, and every period is an integer multiple of it

The node has **one** base period. Every periodic obligation — the check-in, the liveness
heartbeat, profile-window reporting, telemetry — declares an integer multiple of it.

Two things follow, and the first is David's reason for asking:

- **The radio starts and stops once.** Obligations land on the same wakeups by construction
  rather than by an alignment mechanism with its own tuning.
- **The base is the natural floor for the detector.** It is the shortest live period, so it is
  what `8 ×` multiplies when nothing else is armed.

The cost is real and is the point: a slow obligation cannot be finer-grained than the base.

### D4 — porta stops inferring cadence

2026-08-10 **D8 is reversed.** That decision inferred cadence from the `exchanges` history, and
its stated reason was that `nodes.cadence_ms` *"was found to be a dead column — declared and
never written or read anywhere in the Go tree"*. **Inference was never argued to be better than
declaration; it was chosen because the declaration did not exist.** It does now, and it arrives
from the only party entitled to make it.

What this deletes rather than adds:

- the 10-gap inference window (a chosen constant),
- the minimum-3-exchanges-for-a-verdict rule (a chosen constant),
- `CadenceMs` and the `leg`-column bookkeeping that #217 needed to make it correct,
- **`k = 3`** — the offline window multiplier, inherited from v1. A declared budget already *is*
  "the longest this node may be silent"; multiplying it by a second invented number would be
  judging a declaration against an estimate again.

Four constants and a code path leave §8's table. #219 and #220 close with them.

### D5 — Two verdicts, because they are two different problems

`#neverSeen` and `#silent` are not the same fact and do not have the same operator action.

David's position, and it is correct: *"if you cannot bring up a just shipped node, it is broken
not discoverable."* A node that has never spoken is a **commissioning** failure — go plug it in,
go reflash it. A node that spoke and stopped is a **liveness** failure — go find out why. The
only defect in #219 was that porta gave both the same answer, and it gave it by trying to infer
a cadence from a node that never supplied one.

With D2, every node has a budget from its first breath, so a node that has never been heard from
is judgeable immediately — against its default — instead of being invisible forever.

### D6 — The gateway records its own deaf intervals

`Store.Sweep` runs inside `porta serve`, so a gateway that is down writes nothing and every
node's verdict freezes at whatever it was. On 2026-08-19 that meant `gw/liveness` read **`#live`
for ten hours and six minutes** while `c6-bell` missed every single check-in.

The store must not keep asserting the last thing it heard. porta records the intervals during
which it was **not listening**, and a verdict that falls inside one reads `#unknown` — never
`#live`, and never `#silent` either, because the gateway has no evidence in either direction.

This is the same defect as a gate that reads green while testing nothing: the record is not
missing, it is **positively wrong**, and it is unfalsifiable from the store alone.

**The node side of #225 needs nothing.** It was already true: `c6-bell` ran the entire outage,
checked in every 30 s, missed every one, and was fine. Node autonomy across gateway *absence* is
proven. (Autonomy across gateway *refusal* was not — that is #199, and it is a different
failure with an opposite outcome: absence a node survives indefinitely, refusal killed one in
2 min 37 s.)

### D7 — A declared budget may not exceed 24 hours

A managed fleet means no node may make itself unjudgeable. The kernel **clamps** rather than
rejects, and publishes what it actually adopted — so an app asking for a week shows up as a
`goal`/`obs` divergence rather than as a silent refusal.

24 h is David's number and it is a **ceiling on the declaration**, not a cadence: a node speaking
once a month is still free to try, it simply cannot promise a month of silence and be considered
healthy. This is the one invented constant in the design, and §5 tables it with what would earn
a revision.

---

## 3. The record

```
obs/liveness   Liveness { cad: int, bud: int }     (* cell, node's authority *)
goal/liveness  Liveness { cad: int, bud: int }     (* cell, operator's request *)
```

- `cad` — how often the node **intends** to speak, in milliseconds. An integer multiple of the
  node's base tick (D3).
- `bud` — the **silence budget**: the longest silence that is still healthy. `bud >= cad`, and
  the gap between them is the node's own allowance for missed datagrams.

**Two numbers and not one, because the bench already proved one is not enough.** `c6-bell` runs
at a 30 s cadence and misses roughly half its check-ins — ordinary radio loss on a mesh, not a
fault. Judging it against its cadence would call a healthy node silent constantly. Judging it
against a budget it declared with that loss rate in mind is the honest test, and it is the node —
not the gateway — that knows its own loss rate.

The gateway's rule becomes one line with no constants in it:

```
now - last_seen > bud   →   #silent
```

Adding these is **two paths and no tables, no verbs and no endpoints**, which is the extension
rule this system is built on.

---

## 4. What this does NOT solve, said out loud

- **A powered-off node is indistinguishable from a wedged-and-halted one.** Nothing in this
  design changes that, and nothing can from the gateway's side. What it buys is knowing *when to
  start worrying*, which is exactly what was missing.
- **The budget is a promise about the radio, not about the app doing useful work.** A node that
  check-ins faithfully while its sensor loop does nothing is `#live` and is wrong, and this
  design will not catch it. That is an application-level assertion — the `goal/` vs `obs/`
  convergence view is where it belongs.
- **A refused node.** #199 — porta answering `err` — is a separate failure with an opposite
  outcome and its own fix. It is out of scope here beyond the note in D6.
- **#224's uptime wrap** is adjacent but independent: `monoMs` is an SMI, so uptime wraps at
  ~12.4 days, and a node that may now legitimately be silent for a day is closer to that edge
  than anything on this bench has been. Tracked separately; it does not gate this.

---

## 5. Constants, chosen not derived

Recorded so the deferral is falsifiable rather than vague — the discipline 2026-08-10 §8 set.

| Constant | Value | Standing |
|---|---|---|
| maximum declarable budget | 24 h | **Chosen and APPROVED** (David, 2026-08-21: "think fair"). A managed fleet means no node makes itself unjudgeable, and a day is the coarsest granularity at which a fleet is still a fleet. Earns revision from the first real deployment whose duty cycle cannot afford a daily radio wake — a battery budget, not an opinion |
| wedge slack | 8 × (unchanged) | **Inherited from #215**, where it is measured and 5/5 on hardware. This design gives it a period to multiply on a node that had none; it does not touch the number |

**Deleted rather than added:** `k = 3`, the 10-gap inference window, and the minimum-3-exchanges
rule all leave 2026-08-10 §8 with D4.

---

## 6. How each step is falsified

| Step | Falsified by |
|---|---|
| A node with **no app timer** is judged at all | A GPIO-only workload with no declared repeating timer, wedged deliberately. Before: the detector has no deadline and never fires. After: the kernel's own base tick is the deadline, and the dump names it |
| The heartbeat's **absence means something** | Wedge a node and confirm NO heartbeat is emitted between the wedge and the reboot. A heartbeat appearing here would prove the C-side design was chosen after all, and would be the `port=up` lie |
| The operator's request **converges, or visibly does not** | Write `goal/liveness` with a value off the node's grid; `obs/liveness` must show the snapped value and the two must differ in `porta get` |
| porta judges on the **declaration**, not on history | A node that has never been heard from must get a verdict from its default budget — the #219 case, which today is invisible forever |
| A gateway outage is **recorded as an outage** | Kill `porta serve` for longer than a node's budget, restart, and confirm the verdict across that window reads `#unknown` rather than `#live`. This is #225's exact scenario and it has already happened once by accident |
| Radio wakeups **coalesce** | Two obligations at different multiples of one base; count radio wakes over a window against the sum of independent periods |

The first, second and last need a board. The third, fourth and fifth are host-testable —
`porta_sync_host.sh` drives both ends over loopback.

---

## 7. Staging

Ordered so each step is falsifiable before the next depends on it.

| Step | What lands | Why here |
|---|---|---|
| 1 | The record + `obs/liveness` published from a compiled-in default | Nothing judges on it yet. Proves the path, the shape and the size before anything depends on the number |
| 2 | porta judges on `bud`; cadence inference and `k` deleted | #219 and #220 close. Host-testable end to end |
| 3 | `goal/liveness`, the snap, the convergence view | The operator half. Only now is it settable, and only after the judging side is proven |
| 4 | The kernel-owned base tick and heartbeat (D1, D3) | The node half, and the one that needs a board. Closes the detector's blind spot |
| 5 | D6 — porta's deaf intervals | Independent of 1–4; can land any time after 2 |

**Step 1 before step 2 for the same reason the gauge landed before the fix** (2026-08-10 §7): a
number nothing reads is a number nobody has checked.

---

## 8. Promotion — what absorbs this when it closes

| Knowledge | Mainline document |
|---|---|
| The `Liveness` record and its size | `protocol/PROTOCOL-NSL-RECORDS.md` §4.1/§4.2 |
| `obs/liveness` / `goal/liveness` as pinned paths; the declaration replacing inference | `protocol/PROTOCOL-NSL.md` §4.2 |
| D4's reversal of 2026-08-10 D8, and the four constants deleted | `docs/design/2026-08-10-node-memory-and-liveness-design.md` §8, as a dated correction |
| Silence detection, the two verdicts, the deaf intervals | `porta/docs/ARCHITECTURE.md` |
| The kernel-owned heartbeat, and why it is nsl rather than C | `review/lib/` VM doc + the rung in `review/program-sequence.md` |
