# Two liveness cadences, both declared — the #273 decision

> **Decision record.** Decided by David 2026-09-11 (by phone, framing) and 2026-09-14
> (at the desk, with `docs/design/2026-08-31-liveness-timers.html` and
> `docs/spikes/2026-08-27-liveness-3-ladder.png` open). **Decided, NOT built.**
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
| **`appCad`** | is this node's app advancing? | the node, about itself | its own feed counter | **never** |
| **`telCad`** | can this node be heard? | porta, about the node | datagram arrival time | yes |

Everything #273 called a coincidence follows from conflating them. Splitting them is a
net *subtraction* of mechanism, which is why this is the answer rather than a fourth
constant reconciling the first three.

## D1 — `appCad` is declared by the app, fed from app code, judged by the observer

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

**Why the feed must come from app code.** The current mechanism catches an nsl wedge and
a VM wedge alike, because draining a pulse requires the interpreter to advance — but the
drain is issued by *kernel* code, so it misses a third shape: an app stuck in its own
state machine (a window loop waiting on a door event that never comes) while the kernel
keeps draining the base tick. Only an app-issued feed catches that. It cannot tell an
nsl wedge from a VM wedge (the dump can) and cannot catch a livelock that keeps feeding;
both limits are inherent and accepted.

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
consecutive misses is a routine event, not evidence. The shipped `× 8` default exists for
that measured reason. The mechanism is decided; **the mailbox node's own value wants its
measured loss rate before it is set**, and until there is one, `8` is the honest default
and `2` is a claim the bench does not support.

## 2. What this deletes

- The §3.2 inference, and `OBS_WEDGE_SLACK`'s use in it.
- Ten hand-written `self cadence * 8` sites in nsl.
- The `bud`-on-the-cadence-grid snap and the `bud < cad` guard in `adoptCad:bud:`
  (structurally impossible under D2).
- The proposal to publish the node's wedge deadline to porta (#273's 2026-08-28 item 1),
  refused by D1: `appCad` is not porta's business.

Nothing is added to the wire: the record stays two `uint` fields.

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
- porta-side unit tests: the derived `bud`, the miss count in the reason, and D2.1's
  refusal. `sweep_test.go:79`'s `declare(t, st, "n1", 2_000, 16_000)` becomes
  `(telCad, misses)` and is the natural home for the refusal case.

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
- `review/tests/CLAIMS.md` — the four gates of §4, with `liveness-fastpoll`'s
  reproduce-first shape named.
- `docs/design/2026-08-31-liveness-timers.html` — §8 corrected from a stated fork to a
  decided one, and §7's derivation list rewritten. That page says of itself: *"When #273
  is decided, the decision belongs in a dated `docs/design/` record of its own, and this
  page should be corrected to match it."*
