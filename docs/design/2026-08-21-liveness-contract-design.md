# The liveness contract — a node declares how long its silence may last

**Design record, 2026-08-21. APPROVED by David 2026-08-21** — D7's 24 h ceiling ("think fair")
and §7's staging, which deliberately puts the board-dependent kernel heartbeat last behind three
host-testable steps ("yep"). Decisions were offered and chosen inline; the chosen ones are
recorded here as D1–D7. **The implementation plan landed 2026-08-21**:
`docs/superpowers/plans/2026-08-21-liveness-contract.md` (`5ab67a3`), ten tasks in §7's order,
and §9 below records the three decisions writing it forced. **Nothing here is built yet.**

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

---

## 9. Addendum, 2026-08-21 — three decisions the plan had to make

The implementation plan is `docs/superpowers/plans/2026-08-21-liveness-contract.md` (`5ab67a3`),
and writing it forced three choices this record had left open. They are recorded here rather than
only in the plan, because a plan is execution scaffolding and goes historical when the work
closes, while these are decisions. **All three are open to reversal by David; none has been
built.**

### A. Two compiled-in defaults, added to §5's table

D2 requires that *"a node that has never heard from porta still has a budget — its compiled-in
default"*, which is the property that dissolves the never-seen case. §5 tabled the ceiling and
the slack but not the default itself.

| Constant | Value | Standing |
|---|---|---|
| default `cad` for an app that declares nothing | **30 000 ms** | **Chosen by the plan.** The slowest real cadence on this bench (`c6-bell`). Declaring a *slower* cadence than the node keeps is always safe — porta judges `bud`, and speaking more often than declared violates nothing — so the conservative default cannot manufacture a `#silent`. Earns revision the moment a node exists whose real cadence is slower than 30 s and which declares nothing, because then the default is wrong in the dangerous direction |
| default `bud` | **`8 × cad`** | **Chosen by the plan**, reusing #215's measured slack rather than inventing a second multiplier |

**Why `8 ×` here is not the `k = 3` that D4 deletes**, since it looks like it and is not: `k` was
the **gateway** multiplying someone else's declaration by a number the gateway picked. This is
the **node** multiplying its own, with its own loss rate in hand, and what arrives at porta is
one number with no multiplication left in it. Both constants leave the story when the kernel base
tick lands and `cad` becomes the tick itself.

### B. `goal/liveness`'s cadence half is accepted but not honoured until D1 lands

§7 orders the operator half (step 3) **before** the kernel-owned tick (step 4), so for the
duration of that gap the app still owns the timer. A requested `cad` is therefore adopted through
the same grid and ceiling as any other, but the node re-asserts the cadence it actually keeps and
publishes that — because publishing a cadence it is not honouring is `port=up` with better
manners, which is the lie D1 exists to refuse.

Nothing in §6 is lost: the falsification *"the operator's request converges, or visibly does
not"* runs on `bud`, which converges fully and carries the snap. Ask 45 s of a 2 s node, get
46 s, and the divergence is the answer — the design's own example, unchanged.

### C. D6's deaf intervals need a table, and it is argued rather than assumed

`gw_runs`: one row per `porta serve` run, holding when it started and the last moment the sweep
stamped it alive. This is the one exception to the rewrite's D2 (*"a new feature adds rows, not
tables"*), and the exception is narrow: **D2 governs per-node data, whose unit of extension is a
path.** The gateway's record of its own presence is not about a node and has no path.

The two path-shaped alternatives are worse in kind, which is what makes this a decision rather
than a lapse. A reserved node id would put a fake node in `nodes` — the table `httpsrv`'s
`NodeCounter` reports the fleet size from. A per-tick row in `events` needs a `node_id` and would
write 2 880 rows a day into the log that exists to stay readable.

**One consequence, stated now rather than discovered later:** a store that predates the table
answers "deaf" for every window before its first run. That is exactly true of it, no backfill is
possible, and it means verdicts about pre-upgrade windows read `#unknown` rather than `#silent`.
The failure direction is the safe one — it can only turn a confident verdict into an honest
refusal, never the reverse.

---

## Addendum, 2026-08-22 — D1 is board-proven, and the build nearly disproved it by accident

**D1 held on hardware.** `liveness-heartbeat` on `c6spare` (first run of that scenario ever):
`TimerlessNodeC6` declares no app timer, so the kernel's 2000 ms base tick was the only timer in
the image. It wedged deliberately at tick 15, and the observer fired

```
tuvm: WEDGE cond=pulse-undrained overdue=16ms deadline=16000ms up=50606ms pending=1 …
```

Between `entering the wedge` and the reset there were **18.03 s and zero console lines of any
kind** — verified by grep over the window, not read off the TAP. The heartbeat did not outlive
the wedge, which is the entire reason D1 chose nsl-on-the-reactor over a C thread. Boot 2
reported `pass=2 fail=0` and ran clean through the 180 s observation window. `bridge-health`
(3/3) and `wedge-quiet` (1/1) showed no regression from moving the tick into the kernel. porta
holds the other end: `c6-timerless` published `#Liveness{#bud: 16000, #cad: 2000}` over the radio
and reached `#silent` when it stopped.

### Correction to §5 — two constants that look like one

`deadline=16000ms` equals the node's declared budget, and **that is a coincidence of constants,
not a derivation.** The observer computes `shortest_period × WEDGE_SLACK` from
`timer_target_snapshot()`, with `WEDGE_SLACK = 8` in `observer.c`; the published budget is
`cadence × 8`, the app's own multiplier from §9 A. Neither reads the other. Change one and the
other will not follow, and the agreement that makes this run legible will silently disappear.

### What the failed first run added to D1

The scenario was authored with no `CONF_EXTRA`, so the image had `NETWORKING/IPV6/UDP/SOCKETS`
but no L2. `net udpBindPort:` answered nil, no `PortaBridge` was constructed, and
`bridge == nil ifFalse: [ bridge startTicking: … ]` — the only timer registration in the image —
never ran. The node printed READY and nothing else for 300 s.

**It had been returned to exactly the `n == 0` state D1 exists to abolish, by way of the build
rather than the mechanism.** `observer_verdict()` opens `if (n == 0) return COND_NONE`, so it was
not judged unhealthy or healthy — it was unjudgeable, and `pulse_pump`'s `K_FOREVER` makes that
indistinguishable from healthy idle.

**The rule this earns, and it belongs with D1 rather than with the scenario:** *whatever registers
the base tick must not be downstream of anything that can fail at boot.* D1 put the tick behind
the bridge because the bridge owns the cadence; that also put it behind the radio. The current
shape is acceptable — a node with no network has no one to report to — but the coupling is now
deliberate rather than accidental, and if a future node must be judgeable without a gateway, this
is the line that has to move.

Promoted to `review/lib/timers.ns`, `review/program-sequence.md` and `review/tests/CLAIMS.md`
(commit `abc6c4f`).

---

## Addendum 2026-08-27 — the wedge detector was rebooting a healthy node (rete#269)

**This addendum replaces an earlier one written the same day, whose mechanism was wrong.**
That version said a node "cannot hold its declared cadence under silence" and stated a law,
`cad > 2 x replyBudget`, with the fix left as `replyBudget = min(2000, cad/N)` pending a
control run. The control ran, and it refuted the premise rather than picking `N`. It is
corrected here rather than left standing beside the correction, because a record grows
wrong exactly by accretion.

**The node was never wedged. The instrument was.**

`observer.c`'s `pulse-undrained` condition tested the mailbox's **depth at a sampling
instant** — `pulse_pending() == 0` resets `undrained_since_ms`, anything else accumulates.
That is a point sample of a transient, and it aliases against the workload.

At `cad: 2000` under a silent uplink the tick reaction costs ~4 s (`awaitReply` busy-spins
`replyBudget = 2000` ms and `checkIn` has two legs), so the VM consumes one coalesced pulse
per 4.007 s while the ISR posts every 2 s. The mailbox therefore **does** empty — for ~1 s
inside every tick, right after `pumpEvents` drains it. But the observer samples every
1001 ms, and four samples span 4004 ms against a 4007 ms cycle. That 3 ms beat slowly walks
the single sample that catches the empty window straight out of it. Once none lands inside,
`held` runs free and fires at exactly `8 x cad`.

**Measured, c6spare 2026-08-27, from the trace ring this work added to the dump:**

```
up_ms   pending drained undrained_since
140778     0      35       137775     last sample to catch the empty window
141779     1      35            0     reset
144782     1      35       141779     zeros stop being caught
145783     1      36       141779     drained CLIMBING throughout
157795     1      39       141779     fires: overdue=16ms deadline=16000ms
```

`drained` went **35 -> 39** and the console printed **ticks 34-38** during the very 16 s the
detector called undrained. Contrast a real wedge the same session (`wedge-detect`): `drained`
**frozen at 30** for the full 8 s. The `pending` column reads `1` in both — it was never the
column carrying the signal, which is why no dump ever taken can tell the two apart.

This also explains the ~40% boot-to-boot variance in time-to-death, which no allocation
story ever accounted for: it depends on where the beat happens to start.

### The fix — progress, not depth

`pulse_drained()`, a monotonic count of items the pump delivered, added to the seam and both
backends. The observer resets when the mailbox is empty **or** that counter moved since the
last sample. A counter integrates across the interval instead of sampling an instant, so it
cannot alias; a VM genuinely stuck in the interpreter never moves it. **No new tunable** —
`WEDGE_SLACK` remains this design's single invented constant, and nothing is derived from
the declared cadence.

Board-verified in both directions: `porta-silent` **PASS 2/2** (357 s alive under the exact
condition that killed it at 111.7 s and 157.8 s, zero WEDGE lines) and `wedge-detect`
**PASS 2/2**, still firing on a real wedge with its historical signature
(`overdue=8ms deadline=8000ms pending=1 pco=8`). Host: nsl 278/0 all aux green, `go test`
green, `PORTA SYNC OK` 9/9.

### What is refuted, and what survives

Refuted: "the mailbox never empties" (it empties every 4 s, all the way to the reboot);
`cad > 2 x replyBudget` as a survival law; and `replyBudget = min(2000, cad/N)` as the fix —
it was aimed at a mechanism that does not exist, and `N` never needed choosing.

Survives, and is smaller: **a node under silence does not deliver its declared cadence.**
`c6spare` declares `cad: 2000` and ticks at 4.007 s, so `obs/liveness` misreports it to
porta's `gw/liveness` verdict. That is an honesty defect against D2, not a fatal one, and it
is tracked separately as rete#271 rather than riding this fix.

Also measured and worth keeping: the control arm at `cad: 30000` held **mean 30.000 s,
sd 0.089, n=34** over 1050 s against the same silent injector — when the spin fits inside the
period the declaration is kept exactly, which is why `c6-bell` rode a 10 h outage. That is a
fact about cadence; it was simply never the reason the fast nodes died.

### The population this left open — ANSWERED 2026-08-27 (rete#270, closed)

The `pending` column being uninformative meant no archived dump could settle its own case,
so every reboot attributed to a node wedge since #215 was unverified: 58 `deadline=16000ms`
events, zero in a log that also carries an OOM.

**Swept, and the false trips are confined to the one session that found them.** Settled from
the console *around* each dump rather than the dump itself — a wedged node goes silent, a
healthy one keeps printing:

- **49 real wedges** (08-19/20). Every one identical: boot, ticks 1-6 at a 2.04 s cadence,
  then 18.2 s of silence against a 2 s cadence, which is nine missed ticks. **35 of them
  also carry the #222 forensics probe reading `reactor=NOT-ADVANCING`** with `vm_sp` and
  `frame` frozen — evidence that does not depend on mailbox depth at all. They are the
  selector prim-cache spin (#222/#192/#213, fixed in `2461d42`), whose own commit already
  named the tick-6 signature as "where each app crosses 256", and whose cure is the proof:
  the same bridge went from wedging on eight consecutive boots to `PASS 3/3` at tick 99.
- **2 deliberate** — `liveness-heartbeat`'s synthetic wedges (D1 above).
- **7 false trips**, all 2026-08-27, all on the 4.007 s delivered tick.

**No attribution was overturned; before 2026-08-27 the detector was right every time.**
Not affected: #199 (a real arena death, `VM GC Error ... requested 4104`, in logs with no
wedge at all) and #232's refusing path (a refused node returns on the first datagram, never
saturates, and cannot alias).

### What the sweep left standing, and what a review of the whole contract then found

The sharpest observation in #270 survives its own answer: **`pending` reads 1 for a wedge
and for a false trip alike, so the dump could not distinguish them.** What settled it was
the console around the dump. The trace ring added by #269 is what makes the next such
question answerable from the dump alone.

The contract was then reviewed end to end and modelled in TLA+
(`docs/spikes/2026-08-27-liveness-model/`, diagrams at `docs/spikes/2026-08-27-liveness-*.png`).
Eight holes were labelled H1-H8; they are **three defects**, and each label resolves to an
issue in that README's mapping table:

1. **porta judges ARRIVAL where the node judges PROGRESS** (H3/H5/H7/H8, rete#272) — this
   design's own #269 defect one layer up. `verdictFor` tests `now - last_seen`, a proxy for
   health in exactly the way mailbox depth was a proxy for progress. The progress counter is
   **already on the wire**: `obs/health.wak` is the bridge's `ticks`, published every
   check-in and stored in `cells`, alongside `bid`. The fix reads a value porta already has.
2. **Four deadlines chosen independently that nothing derives from each other** (H1/H2/H4,
   rete#271 + rete#273): `cad`, the wedge deadline (`WEDGE_SLACK` in C), `bud` (`cadence x 8`
   in nsl) and `SweepTick` (a constant). D4's "porta infers nothing" is silently capped by
   the 30 s sweep for every node on this bench, and the two eights are not the same eight.
3. **A failed socket bind leaves a node with no detector** (H6, rete#274), because
   `startTicking:` is downstream of the bind.

One correction to the mechanism in this document, forced by the model and settled by the
board: a check-in that overruns its own period does **not** push the next tick out. The base
tick is periodic and its posts coalesce, so the delivered period is **`max(cad, work)`, not
`cad + work`** — which predicts both of rete#271's measured rows (4.007 s at cad 2000,
30.000 s at cad 30000) and gives the exact condition under which a node misdescribes itself:
**`2 x replyBudget > cad`**, i.e. `cad < 4000 ms` at today's budget.

Also worth carrying, because it cost this session's forensics: the `RebootNote` lives in
`.rtc_noinit` (`zephyr/src/reboot_note.c:29`), so it survives a **reset** and NOT a power
cycle. A halted node holding a note must be reset, never power-cycled, if the note is wanted.
