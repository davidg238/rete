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

## Addendum 2026-08-27 — a declared cadence the node cannot hold under silence (rete#269)

D2 makes the node the authority on how long its silence may last: it publishes
`obs/liveness = Liveness { cad, bud }` and the gateway judges that number and no other. This
addendum records a defect **in the node's ability to honour its own declaration**, found when
c6spare died 204 s after losing its uplink to a porta that was up the whole time and that two
other nodes were reaching normally.

**The declaration is not achievable at every cadence.** `Kernel>>awaitReply:` busy-spins
`replyBudget = 2000` ms (`kernel.nsl:1807`, `:2845`) and `checkIn` has two legs, so when
nothing answers a tick costs up to **4000 ms of spin**. A refused node pays ~0, because an
`#err` fails `isCrossed:` and returns on the first datagram. Therefore:

> **A node can hold its declared cadence under a silent uplink only if
> `cad > 2 x replyBudget`** — 4 s with today's constant.

`c6spare` declares `cad: 2000`: its tick period is shorter than a *single* leg's budget, so
under silence it saturates completely. `c6-bell` declares `cad: 30000` and rode a 10 h gateway
outage fine at a 13% duty cycle. Both facts were on the bench and the general claim drawn from
them ("absence is survivable, refusal kills") was wrong — it was a statement about cadence.

**Measured, `porta-silent` first run, 2026-08-27:** tick spacing **4.007 s from tick 1** (the
tick period simply becomes the spin cost), `misses` +2/tick, `ref 0`, `acked 0`, and
`WEDGE cond=pulse-undrained ... up=111734ms` at tick 26 — **111.7 s from a cold boot, against
#199's refused death at 157 s.** Three wedges then the designed HALT, reproducing the
unplanned death exactly. Time-to-death varies ~40% boot to boot (111.7 / 157.8 / 157.8 s)
while the saturation is present on every tick, so a green needs several runs or one long one.

**This is a THIRD failure under "gateway unreachable"**, not a recurrence of the first two: the
unbounded pending queue (fixed by the 64-row cap) and #199's rebuild churn (fixed by
`196ff1c`'s frame reuse) are both still doing their jobs. The terminal event here is #215's
detector, not an OOM — no GC error line, no fatal allocation size — and the empty poll
allocates nothing (`udp.c:83` returns before `udp_make_bytes`), so this is a **wall-clock**
failure and not an arena one. Each of the first two was gated by a refusal test or a host
test; none was ever gated by an actually-unreachable gateway. `porta-silent` is that gate.

**Not decided here:** the fix. Deriving `replyBudget` from the declared cadence
(`min(2000, cad/N)`) is the smallest candidate and uses a value the node already declares, but
`N` trades duty-cycle headroom against misses on a lossy mesh — the bell "misses roughly half
its check-ins to ordinary mesh loss" (`kernel.nsl:1689`). Per CLAUDE.md, the knob is not
invented before the data: `porta-silent` at `cad: 2000` is the saturated arm and the same
probe at a slower cadence is the control. Two runs pick `N`.

Also worth carrying, because it cost this session's forensics: the `RebootNote` lives in
`.rtc_noinit` (`zephyr/src/reboot_note.c:29`), so it survives a **reset** and NOT a power
cycle. A halted node holding a note must be reset, never power-cycled, if the note is wanted.
