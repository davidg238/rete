# Two liveness cadences, both declared — the #273 decision

> **Decision record.** Decided by David 2026-09-11 (by phone, framing) and 2026-09-14
> (at the desk, with `docs/design/2026-08-31-liveness-timers.html` and
> `docs/spikes/2026-08-27-liveness-3-ladder.png` open — D1/D2 in the first round, then
> **`misses: 8`** and **D3 `#stalled`** in a second round the same day). **D1 and D2 BUILT 2026-09-17** (`1315e1e` observer rule, `00c1470` prim 158, `365f8f9` kernel, `47633bb` node sites, `9b6332a` manifest — sha256 `844ba659…` — `3b4331c` porta, `888500f` board: five gates green on `c6spare`, `liveness-fastpoll` RED on the pre-fix VM sources first; plan `docs/superpowers/plans/2026-09-17-liveness-two-cadences.md`). **Not yet landed on the gateway** — Task 10 is with David. **D3 (`#stalled`) is decided and NOT built** and has its own plan after the TLA `DoSweep` correction.
> Closes the fork #273 states; unblocks #272's age rule in `verdictFor`.
>
> Supersedes the proposal in #273's 2026-08-28 comment and the addendum §604 of
> `docs/design/2026-08-21-liveness-contract-design.md`, which proposed publishing the
> node's *wedge deadline* to porta. That is now refused on principle: see D1.

## 1. The defect, restated in the terms that resolve it

#273 named four numbers that agree by arithmetic coincidence. The walkthrough on
2026-09-14 found the coincidence has a single cause, and it is not the two eights:

**`cad` is doing two unrelated jobs.** It is the telemetry cadence *and*, through #231
D1's kernel base tick (`kernel.nsl:2579`), the sole source of the app-liveness deadline
that `observer_core.h:131` infers. On the bench they coincide because `cad` happens to be
2 s. They are answers to two different questions, asked of two different parties:

| | the question | judged by | the evidence | on the wire? |
|---|---|---|---|---|
| **`appCad`** | are nsl and the VM alive on this node? | the node, about itself | its own feed counter | **never** |
| **`telCad`** | can this node be heard? | porta, about the node | datagram arrival time | yes |

Everything #273 called a coincidence follows from conflating them. Splitting them is a
net *subtraction* of mechanism, which is why this is the answer rather than a fourth
constant reconciling the first three.

## D1 — `appCad` is declared by the app, fed from the kernel's event dispatch, judged by the observer

`platform liveness app: ms` declares it; `platform liveness feed` bumps a counter the
observer reads beside `pulse_drained()`. `observer_core.h` §3.2 becomes *feed age >
appCad*, and `OBS_WEDGE_SLACK` **leaves §3.2 entirely** — it stays in §3.1, where it
multiplies a declared timer period and means what it says.

**Why declared, not inferred.** The inference is wrong in both directions, and neither
direction is a tuning error:

- **Too short.** §3.2 judges the *shortest* declared period × 8. A 100 ms sensor beside
  a 1.5 s e-paper refresh yields an 800 ms deadline and reboots a healthy node. The
  correct bound is the app's longest legitimate blocking reaction, about which the
  shortest period says nothing. (§3.1 is unaffected: `since_ms` is `now − last_fire_ms`,
  ISR age, `timer_zephyr.c:132` — a slow reaction never trips it.)
- **Absent.** A run-once deep-sleep node registers no repeating timer, `timer_target_snapshot`
  excludes one-shots by design (`timer_zephyr.c:117-119`), so `s->n == 0` and
  `observer_core.h:108` returns `OBS_COND_NONE`. Nothing is judged; a wedge inside the
  wake window burns the battery down. This is `MagTagDeepSleepNode.nsl` today, and the
  mailbox node tomorrow. The 30 s task watchdog (`observer.c:76`) guards the observer
  *thread*, not the VM, and is 3× the wake window besides.

**What the feed proves, and where it is issued — amended 2026-09-17.** The feed is
issued by the *kernel*, from its event dispatch, and it proves exactly one sentence: *the
VM executed this node's nsl event dispatch within `appCad`.* That is "nsl and the VM are
alive", and nothing more. The first draft of this section had the feed issued from app
code, on the claim that it would also catch "an app stuck in its own state machine while
the kernel keeps draining". That claim is only true when the app puts the feed inside its
real work path, so it is the app's claim and not the mechanism's, and it was the kind of
exaggerated expectation this design is meant to shed (David, 2026-09-17: *"is the node
alive and can we talk to it … simple hopefully equals robust, without exaggerated
expectations or claims"*). Whether an application is making progress is out of scope
here and stays out. One feed, one caller, no app-side call. *In the build the feed IS
`pulse_drained()` — no new counter was added; the prim carries only the declaration, and
the observer sees a number, not whether the app chose it or the bridge defaulted it (only
the bridge knows; a deliberate simplification of D1.1).* What `appCad` still declares
is the app's longest legitimate blocking reaction, because a slow reaction blocks the
dispatch that feeds. The mechanism cannot tell an nsl wedge from a VM wedge (the dump
can) and cannot catch a livelock that keeps dispatching; both limits are inherent and
accepted.

**Why the observer and not a bare hardware watchdog.** The observer *already* sits on the
hardware watchdog (`g_hw_wdt`, `observer.c:78`). "Arm a raw hw wdt at boot" is not a
mechanism we would add — it is this mechanism with the observer deleted, and what it
deletes is exactly the dump, the `RebootNote` and `#watchdog` on the next push. Those are
the only things that have ever told us *which* condition fired.

**`appCad` never crosses the wire and must not.** It is the node's own business, and a
gateway holding a second opinion about it would be a number to reconcile guarding
nothing — the same finding §6 of the timers page records about custos.

### D1.1 — Two bounds, because a declared number can be declared wrong quietly

An inferred deadline was at least tied to real timers. A declared one can be typo'd to
100000 and disable judgement with nothing to catch it — `cad` already carries the
analogous ceiling (`livenessCeilingMs`, 24 h, `kernel.nsl:2503`). So:

1. **`appCad` is clamped**, on `adoptCad:bud:`'s precedent: clamped and published, never
   rejected, so an app asking for an hour appears as a value rather than a silent refusal.
2. **The default must print in the dump as a default, not as a declaration.** `appCad`
   defaults to `cad × 8` so that every existing node and both liveness gates behave
   exactly as they do today (`deadline=16000ms`) — but a dump that cannot distinguish
   "the app chose 16 s" from "nobody chose anything" invites reading an inference as a
   human decision. One flag, printed.

## D2 — `telCad` is declared with a tolerated-miss count, and porta derives the budget

The node publishes `Liveness { misses: uint, telCad: uint }`. porta computes
`bud = telCad × misses`. `verdictFor` stays constant-free — every number in the verdict
still came from the node (#231 D4 preserved).

**Why `misses` and not `bud`** (the three options put to David 2026-09-14; **B chosen**):

- **A.** `{telCad, bud}` — today's record, porta starts reading `telCad`. Permits a node
  to declare a `bud` shorter than its own `telCad`: incoherent, and nothing would catch it.
- **B.** `{telCad, misses}` — same wire cost, the multiplier is the node's, an incoherent
  declaration is *unrepresentable*, and #231 D3 ("`bud` is an integer multiple of `cad`")
  becomes **structural instead of conventional** — the snap arithmetic
  `((bud + cad - 1) // cad) * cad` in `adoptCad:bud:` disappears, along with the ten
  hand-written `self cadence * 8` sites (`PortaBridgeNodeC6.nsl:100` and nine others).
- **C.** `telCad` alone, porta multiplying by its own constant. **Rejected: that is H2** —
  a gateway-side constant deciding the deadline, which is the defect #273 names.

**H1 is therefore the answer to #273's fork.** The node's declaration is the source of
truth; porta contributes no number to the verdict. `SweepTick` is porta's own sample rate
and is not a deadline — §7 of the timers page already says it derives from nothing, and
after this it still derives from nothing, because it is not that kind of number.

### D2.1 — porta refuses to promise what its sweep cannot deliver

Where `telCad × misses < SweepTick` (30 s, `sweep.go:20`), porta writes `#unknown` with a
reason naming all three numbers, instead of quietly enforcing 30 s. A silent disagreement
becomes a visible one, and it produces the data that would earn per-node sweep scheduling
later. (#273's 2026-08-28 item 5, carried forward unchanged.)

Consequence, stated so it is not a surprise: the bench node at `telCad = 2000` reads
`#unknown` at porta until it declares `misses ≥ 15`, or raises `telCad`. **Resolve it by
raising the bench declaration, not by lowering `SweepTick`** — the 2 s is for bench
turnaround and was never a claim about the radio.

### D2.2 — The miss ladder is a reason string, not a new verdict

porta can now count misses (`since / telCad`), which it could not before: it held the
tolerance and not the expectation. That count goes in the verdict's `reason`
(`"silent: 3.2 missed check-ins of 3600000 ms"`). **No `#missed` verdict is added.** Per
the standing posture, ship the degenerate case whose cost can be measured and let the
measurement earn the knob; a rung nothing consumes is a mechanism with no reader. What
would earn it: evidence that an operator acted differently on the first miss than on the
fifth.

### D2.3 — The measured input for `misses`, and a caution about 2

The RECORDS catalogue already holds the empirical reason the gap between `cad` and `bud`
exists, and it is exactly this number: **`c6-bell` runs a 30 s cadence and misses roughly
half its check-ins to ordinary mesh loss** (`protocol/PROTOCOL-NSL-RECORDS.md`, the
`obs/liveness` row). So `misses` is not a taste parameter — it is the node's allowance for
its own measured loss rate, and the node is the only party that knows it.

**Therefore the phone framing's "budget 2–3 h on a 1 h `telCad`" is `misses: 2` or `3`,
which on a mesh like `c6-bell`'s would false-trip.** At ~50 % per-exchange loss, two
consecutive misses is a routine event, not evidence.

**DECIDED 2026-09-14 (second round): one default, `misses: 8`, at every period.** A split
by period was considered (`× 4` long / `× 8` short) and **rejected**, for three reasons in
descending order of force:

1. **The multiplier is set by loss probability, not by period.** The false-trip rate is
   roughly `p^misses`; the period cancels out. At the one rate measured — `c6-bell`, ~50 %
   per check-in — `misses: 4` false-trips about **6 %** of windows against **0.4 %** for
   `misses: 8`. Bursty loss makes both worse and preserves the ranking.
2. **If a split existed it would go the other way.** A deep-sleep node re-attaches the
   radio on every wake, so its dominant loss mode is not a dropped datagram but an *attach
   failure*, which is **correlated** — a mesh that is down stays down for minutes. `4ec0`'s
   `thread: attach timed out` (#281) is that mode on the bench now. Correlated misses break
   the `p^k` independence in the direction that needs *more* slack at long periods, not less.
3. **Two multipliers plus a threshold for "long" is three invented constants where one
   default does**, and nothing yet acts on the verdict (#290), so the detection latency the
   split would buy currently costs nothing while a false `#silent` costs the instrument's
   credibility.

**The lever for faster detection at long periods is `p`, not `misses`.** A ~10 s wake
window against a ~4 s two-leg reply budget affords **two check-in attempts per wake**,
possibly three. That drops per-*wake* loss to `p²`–`p³`, at which point `misses: 2` gives a
*better* false-trip rate than `misses: 8` does today, with a 2 h budget instead of 8 h. That
is how the mailbox node earns a short budget honestly: measure `p`, drive it down inside the
wake, then declare against the measurement. Recorded here so the deferral is falsifiable —
**what would earn a `misses` below 8 is a measured per-wake loss rate, not a preference.**

## D3 — `#stalled` is a fifth verdict, and it means "behind the declared cadence"

Decided 2026-09-14 in the same sitting; the issue is #272, and this **absorbs #271**.

**The correction that makes it real.** #272 proposes judging `wak` (the bridge's reactor-wake
count) as "`pulse_drained()`'s equivalent at the app layer". On this node that does not hold:
the check-in runs from the app's *own* tick reaction — `tickChan whenever: '' do: [ :m | self
tick ]` (`PortaBridgeNodeC6.nsl:156`) and `tick` calls `bridge checkIn` (`:186`) — so **an
arrival implies a tick, which implies `wak` advanced.** `wak` is a re-encoding of arrival, not
independent evidence of progress, which is precisely the proxy trap #269 was about. A verdict
asking "did `wak` move" is **vacuous**. It is also actively wrong per-exchange: two exchanges
ride one check-in (pull + push, same tick), so a per-exchange comparison sees a frozen `wak`
on **every push leg** and would false-`#stalled` a healthy node.

**What `wak` does answer, and it is two of #272's four labels:**

- **H3, the reboot loop.** `ticks` resets to 0 on boot, so `wak` moving *backwards* is direct
  evidence of a reboot loop — and better than the `boot_id` join #272 explicitly rules out.
- **H8, the node's own deafness.** Not *did* it move but **did it move at the declared rate**:
  expected `Δwak ≈ elapsed / telCad`. A node that ticked once across eight windows moved its
  counter and is still broken. An age, not a delta — #269's lesson again.

**So `#stalled` = arriving, but behind the cadence it declared.** Reachable and already
**measured**: 4.007 s delivered against a declared 2 s under a silent uplink (§4 of the timers
page; #271's table). That is why it is a verdict and the miss ladder (D2.2) is only a reason —
**this corrects a false green**, where a node not meeting its own declaration reads `#live`
today, and a wrong statement is worth fixing before it has a reader; a gradient is not.

**Its gate must prove reachability before it proves health** (the vacuity rule): a probe whose
reactor ticks at half its declared rate, showing `#stalled` *fires*, before any run is trusted
to show that it does not.

### D3.1 — This refutes #271's preferred direction

#271 direction 1 is "publish the **delivered** cadence — makes the declaration true by
construction." Under D3 that is **mutually exclusive with the verdict**: a node that
re-publishes its delivered cadence renames its failure as its intention, and destroys the only
evidence porta has that anything is wrong. Direction 3 (accept it, document the ceiling) plus
this verdict is the resolution: **the declaration stays the node's promise, and the gateway
reports the divergence.** #271's directions 2 (stop `awaitReply` burning the full budget) stays
live as a *cause* fix — D3 absorbs #271's **detection**, not its cause, and the spin still
halves the check-in rate under silence.

### D3.2 — Model-checked, and refined, 2026-10-03

The rate rule now runs in `docs/spikes/2026-08-27-liveness-model/` as `Rule = "rateAge"`.
`sound-rate.cfg` completes with no stale `#live`, no behind `#live` and no false `#stalled`,
and both verdicts are proven reachable. The detection bound is **`bud + SweepTick + telCad`**;
TLC found it, nobody chose it. TLC also refuted two readings of D3, so the rule is narrower
than the text above:

- **"Between porta's last two samples" means since the MARK** (the sample judged at the last
  sweep), and **a sweep with no new sample keeps both the verdict and the mark.** Re-marking
  on an empty window let a stalled node flip back to `#live`, unbounded.
- **One sample is `#unknown`**, not `#live`. No new symbol: `#unknown` already exists.
- ~~Added by the plan, outside the model: a node that is arriving while `obs/health` has
  stopped landing reads `#unknown`.~~ **Deleted by D3.3** — the guard existed only because
  the sample rode a frame that could stop landing.

Plan: `porta/docs/superpowers/plans/2026-10-03-272-stalled-verdict.md` (Tasks 1–3 executed
2026-10-03; superseded from its Task 4 by D3.3's plan).

### D3.3 — The count rides the pull, 2026-10-03

**What was wrong.** D3.2's rule was checked on the assumption that every check-in that lands
hands porta the node's *current* count. It does not: `wak` rode `obs/health` in the push
frame, and `pushFrame` re-sends the same encoded frame until an ack is heard (rete#199). The
whole-branch review of the D3.2 code found the consequence (its F2) and the model now
reproduces it in both directions (`Frame = "cached"`; model README, "What the cached frame
found"): a healthy node that loses one push reads `#stalled`, and a node at half rate reads
`#live` past the bound. A deaf node — #271, the case D3 exists for — delivers no sample at all.

**Decided (David, 2026-10-03): the rule stands; its sample moves to the pull.**

- **The pull request carries `#wak`**, a count the kernel bridge increments once per
  `checkIn`. The pull is built fresh on every tick (`pullFrame`), so the assumption the model
  was checked on becomes true by construction. `#wak` is **required**: a pull without it is
  refused like any other mis-versioned request.
- **porta's sample is (that count, the pull's arrival time)** and it is the *only* thing the
  verdict reads. Silence is judged from the same sample — `now − wak_ms > telCad × misses` —
  so "arriving, but the count has stopped" cannot be represented, and the guard D3.2 added
  for it is deleted rather than tuned (the review's F4).
- **A verdict past the budget forgets the mark** (the review's F5; model `ClearMark`). A node
  that returns is judged from its first new sample, never across the silence: a replug reads
  `#silent → #unknown → #live`, not `#silent → #stalled → #live`.
- **A window that spans an interval porta was not listening re-marks and does not judge**
  (the review's F3; `WasDeaf`, which already exists). A 2 s node ticks at 4 s while porta is
  down (#271), and porta's own absence is not evidence about the node.
- **A node whose pulls carry no count** — an image built before this — reads `#unknown`
  with a reason saying so, until it is reflashed. No arrival-only fallback: that would be a
  second rule beside the first.
- **`obs/health.wak` is untouched** and is no longer read by porta. It stays what the
  catalogue says it is: the app's own tick count, for a person reading the record.

**"No wire change" is withdrawn.** #272's body said the fix "adds no field, no wire change
and no new mechanism" because the count was believed to arrive on every check-in already.
It never did. One field on a request both ends rebuild together is the cheapest true thing.

**Model evidence:** `fresh-clear.cfg` (29 141 987 states) and `fresh-lossy.cfg` (13 387 396)
hold `NeverStaleLive`, `NeverBehindLive` and `NoFalseStall`; `cached-lossy.cfg`,
`cached-rate.cfg` and `cached-rate-guard.cfg` are the three refutations of the D3.2 transport.
The bound is unchanged: `bud + SweepTick + telCad`.

**Left to the fleet, on purpose** (each is a number or a wording that only real nodes on the
home network can price — recorded so the deferral is falsifiable):

- **The one period of slack.** One lost reply on a 2 s node delays that tick to 4 s, which is
  exactly one period short: such a node will read `#stalled` for one sweep whenever a reply is
  lost in the window. What earns a different slack: the count of `#stalled` rows per node per
  day against the node's own `pto`.
- **What a restart is called.** A reboot inside the budget reads `#stalled`, named
  "backwards" only when the new count is below the mark's.
- **A stale reason.** A verdict whose cause changes without the symbol changing writes no row.

Plan: `porta/docs/superpowers/plans/2026-10-03-272-count-on-the-pull.md`.

**Addendum, 2026-10-03 (David, after the final review).** A node whose pulls carry no count
stays `#unknown` even past its budget — never `#silent` from arrival. So a dead old-image node,
and every pre-#272 row in the gateway store, reads `#unknown` until reflashed; the reason
carries the age of its last exchange so it never claims the node "checks in" (`07f3750`).
The deaf re-mark is test-pinned but not in the TLA model (#327).

## 2. What this deletes

- The §3.2 inference, and `OBS_WEDGE_SLACK`'s use in it.
- Ten hand-written `self cadence * 8` sites in nsl.
- The `bud`-on-the-cadence-grid snap and the `bud < cad` guard in `adoptCad:bud:`
  (structurally impossible under D2).
- The proposal to publish the node's wedge deadline to porta (#273's 2026-08-28 item 1),
  refused by D1: `appCad` is not porta's business.
- #271's direction 1 (publish the *delivered* cadence), refused by D3.1: it would make the
  declaration true by construction and destroy the evidence the verdict rests on.

Nothing is added to the wire: the record stays two `uint` fields, and D3 adds no field
either — `wak` is already published on every check-in and already stored in `cells`; what
changes is which stored value the verdict tests, and against what rate.

## 3. Scope, and the three gaps left open deliberately

David, 2026-09-14: *"comfortable with gaps not covered."* Named so they are not
rediscovered as findings:

1. **A node that advances but is wrong.** #224 is the live example — `monotonicMillis`
   went negative at 12.4 days while the app kept running, and would have kept feeding. The
   observer's own clock is C-side `uint32` (`k_uptime_get_32`) and was never affected.
   Liveness is not correctness; a livelock that keeps feeding is the same shape.
2. **A node whose VM never runs.** #201 — a failed flash parks the chip in the ROM
   downloader with the striker energized. No software mechanism reaches it; #202's series
   capacitor is the answer and stays it.
3. **Nothing acts on `#silent`** — filed as **#290** (P3, `found-during #273`). Verified
   2026-09-14: the only readers of `gw/liveness` are porta's own tests and `porta get`. The
   verdict is *recorded*, not *actioned*. Filed rather than folded in — it is a separate
   decision about what porta *does*, and it wants the miss data this design produces first.
   So "guards against quiet death" today means "makes quiet death visible to someone who
   looks", which is a real property and not the same one.

## 4. How each claim is falsified

Gates, not arguments. The first two must stay green **unchanged** — this design's job is
to hold today's behaviour while deleting the inference:

- `liveness-heartbeat` — `deadline=16000ms` from the default, 18.03 s of silence.
- `liveness-noradio` — #274's gate: a deadline exists when the bind fails.
- **NEW `liveness-fastpoll`** — a node with a 100 ms timer beside a ~1.5 s blocking
  reaction. **It must false-reboot on today's build and not on the new one**, which makes
  it the one gate here that starts by reproducing the defect rather than asserting health.
- **NEW `liveness-runonce`** — a node with no repeating timer at all is judged. Today's
  build scores nothing; vacancy must not read as agreement, so the gate asserts a
  *deadline was formed*, not merely that no reboot happened.
- **NEW `liveness-stalled`** — D3's reachability gate: a probe whose reactor ticks at half
  its declared rate. It must show `#stalled` **firing** before any run is trusted to show it
  not firing, because a passing invariant is vacuous until the interesting state is proven
  reachable.
- porta-side unit tests: the derived `bud`, the miss count in the reason, D2.1's refusal, and
  D3's rate rule — including the push-leg case, which must **not** read `#stalled` (two
  exchanges, one tick, one `wak`). `sweep_test.go:79`'s `declare(t, st, "n1", 2_000, 16_000)`
  becomes `(telCad, misses)` and is the natural home for the refusal case.

## 5. Sequencing — this moves the manifest hash

`protocol/MANIFEST.cddl:190-193` is `liveness = #6.27([sym-Liveness, {sym-bud => uint,
sym-cad => uint}])`, and both `goal/liveness` and `obs/liveness` ride it. D2 renames both
keys: `{sym-misses => uint, sym-telCad => uint}` — and **key order is wire order**, so
`misses` precedes `telCad` (byte-compare: `m` < `t`, equal length). Editing the manifest
moves its SHA-256, which is the `obs/build` pin and `check_provision.py`'s served-hash
side. **This lands with #285's one-line fix and a gateway deploy, not alone** — the same
sequencing #285 is already being held for. Both ends reflash together; there is no fleet
and no compatibility window (design posture).

## 6. Promotion — what absorbs this when it closes

- `protocol/PROTOCOL-NSL-RECORDS.md` — the `obs/liveness` row: the new shape, the
  measured-loss-rate reading of `misses`, and a re-measured byte count.
- `protocol/MANIFEST.cddl` — the machine-normative half, per §5's sequencing.
- `docs/design/2026-08-21-liveness-contract-design.md` — D2/D3/D7 restated; §604's
  proposal marked superseded by this record.
- `porta/docs/ARCHITECTURE.md` — the verdict rule and D2.1's refusal.
- `review/tests/CLAIMS.md` — the five gates of §4, with `liveness-fastpoll`'s and
  `liveness-stalled`'s reproduce-first shape named.
- `protocol/PROTOCOL-NSL.md` — the fifth verdict and the rate rule, alongside the four
  `verdictFor` already answers.
- `docs/design/2026-08-31-liveness-timers.html` — §8 corrected from a stated fork to a
  decided one, and §7's derivation list rewritten. That page says of itself: *"When #273
  is decided, the decision belongs in a dated `docs/design/` record of its own, and this
  page should be corrected to match it."*

## 7. Close-out — D1 and D2 landed on the running system, 2026-09-17/18

The plan (`docs/superpowers/plans/2026-09-17-liveness-two-cadences.md`) ran Tasks 1–9 on
2026-09-17 and Task 10, the gateway landing, across 2026-09-17 23:01 → 2026-09-18 18:19
local. §4's falsification plan is discharged on `c6spare` (the five gates) and now on the
running system:

- **Gateway**: porta `2488c2e`, manifest `844ba659…` persisted to the store; ~35 s outage
  by the three-call restart (kill by PID). **Not predicted here or in the plan**: the first
  sweep after the restart moved every un-reflashed node (`c6bench`, `nrf4ec0`) from its old
  verdict to `#unknown` — *"declared no cadence"* — because the old `{bud, cad}` row is no
  declaration under the new shape. That is D2.1's refusal doing its job on a trailing node;
  a reflash clears it. It is the visible cost of §5's "both ends reflash together".
- **First real node**: `c6bench` reflashed from the gateway with `PortaBridgeNodeC6` +
  the Thread FTD overlay (`g2488c2e`). `obs/liveness` = `{misses: 16, telCad: 2000}`,
  `obs/build.man` = `0x844ba659b3a9a3b0`, `gw/liveness` `#live` within 30 s of boot
  (`live: 269 ms since the last exchange, 0.1 missed check-ins of 2000 ms, 16 tolerated`).
  Overnight: 17 k ticks, 132 cumulative misses (~0.8 % loss), `dropped 0`.
- **The physical falsification**: USB pulled 08:42:20 → `#silent` at 08:43:01, reason
  `silent: 42121 ms since the last exchange — 21.1 missed check-ins of 2000 ms, 16
  tolerated` — 41 s after the pull, inside the predicted 32–62 s window (`bud` 32 s + a
  sweep tick), the reason counting ≥16. Replugged 18:19 → boot 4, `#live` at the next
  sweep (24 s). No constant on the gateway took part in either verdict: the numbers in
  both reasons are the node's own declaration.

**Absorbed by** (§6, done in the promotion commit `1aa8eec`): RECORDS §4.2, PROTOCOL-NSL
§4/§8, ARCHITECTURE §3.5, CLAIMS (two rows). D3 (`#stalled`) is NOT landed — it waits on the
TLA `DoSweep` correction (#272) and is a plan of its own. Found-during: #300 (custos is
location-free except for the `live` binding).
