# Next steps — overview

Written 2026-07-31 at the end of the #100 session. **Last edited 2026-08-05.** A living
document, not a dated decision record: re-read it at the start of a session and edit it,
don't append to it.

Two audiences in one file. **Part 1 is the bench** — the physical work waiting on the C6
shipsBells rig. **Parts 2-4 are the thinking** — what actually gates a public release, and
which new targets are worth timing a bring-up on.

---

## Part 1 — C6 shipsBells: what to do at the bench

Status: the wall-clock seam (#100) is built, tested and running. The DS3231 answers on
i2c0 and its oscillator is ticking.

**Updated 2026-08-02 — §1.1 and §1.2 are done.** David reports the trickle-charge resistor
removed and SQW wired to GPIO10 at the bench (reported, not re-measured here). **The pin was
GPIO11** — measured 2026-08-04 when the alarm never arrived; see §1.2. They are kept
below as the record of what was done and why.

**Updated 2026-08-05 — §1.3 and §1.4 are done, and the bell is ringing on the RTC alarm.**
`ShipsBellAlarmNodeC6.nsl` is built, flashed and soaking; `CONFIG_RTC_ALARM=y` lives in the
opt-in overlay `prj_esp32c6_rtcalarm.conf`, **not** flipped in the base conf as §1.3 below
instructs — see the correction there. The alarm arrives on GPIO11, which also retires the
last doubt from §1.2. **What remains at the bench is §1.5 (the +20 s seed) and §1.6 (measure
the part's real ppm), and neither has been started.**

**#105 is CLOSED**, and the strike path is now a **3.3 V piezo**, not the 12 V solenoid:
GPIO18 → 2N3904 → piezo, with **R2, a 47 kΩ base pull-down** added 2026-08-05 so the
transistor's base is held down through the seconds *before* `wire_gpio_outputs()` runs, when
a Zephyr output is still a floating input. Drawing:
`nodus/tuvm/zephyr/boards/shipsBells/2026-08-03-shipsbells-3v3-piezo-driver.svg`. R2 does
**not** mitigate #151 — it holds the base down only while the pin is undriven, and a solid
tone is the pin being *driven*.

**What is soaking now, and what it is worth watching for.** The bell runs a `PortaBridge`
and reports `obs/build` + `sys/metrics` to porta every 30 s (#56 D8), carrying `pulseDrops`
and `pulseCoalesced`. Three live stakes on that, all filed: **#153** (watch
`largestFreeBlock` now the bridge is on the bell — the measurement is available today and
the window will not be cleaner later), **#150** (push trim starves `sys/metrics`, which is
the very instrument aimed at #151's residue), and **#152** (a UDP console sink reports
`port=up` forever and cannot tell a dead node from a quiet one — the reason #151's overnight
event went unexplained for most of a day).

### 1.1 Check the trickle-charger BEFORE trusting the coin cell — DONE

**Done at the bench: the resistor was removed.** Kept for the record.

**Do this first, and do it with the board unpowered.**

The module is the common ZS-042 layout. The photo in
`nodus/tuvm/zephyr/boards/shipsBells/board.png` shows a small red glass diode and a `201` (200 Ω)
resistor beside the SCL pad — that board's well-documented **LIR2032 trickle-charge path
from VCC to VBAT**. The cell fitted is a **CR2032, which is not rechargeable**.

`nodus/tuvm/zephyr/boards/shipsBells/rtc-schematic.png` shows no such circuit and a plain
"Battery1 +3V", so **it cannot be used to rule this out** — it is not a schematic of this
module.

The test, one minute:

1. Remove the coin cell.
2. Power the module at 3.3 V.
3. Measure the holder's **+** terminal against GND.
4. Anything near VCC (~2.6–3.2 V) means the charging path is present.

If present: remove the `201` resistor or the diode, or fit a LIR2032 instead. Do not leave
a CR2032 on a charger.

### 1.2 Wire INT/SQW → GPIO11 — DONE (the pin below is wrong; corrected 2026-08-04)

**Done at the bench.** The devicetree half was already in place and is verified here:
`nodus/tuvm/zephyr/boards/esp32c6_devkitc_esp32c6_hpcore.overlay:78` carries
`isw-gpios = <&gpio0 10 (GPIO_ACTIVE_LOW | GPIO_PULL_UP)>`. Kept for the record.

The pin is labelled **`SQW`** on the module's 6-pin header (`32K, SQW, SCL, SDA, VCC, GND`).
It is the DS3231's INT/SQW output, and `rtc-schematic.png` confirms **R21 pulls it up 10 kΩ
to +VCC3.3** — open-drain with the pull-up already on the board, so nothing is needed
MCU-side.

**CORRECTION, 2026-08-04 (#102): the bench wire runs SQW → GPIO11, not GPIO10.** The overlay
now declares 11. This was invisible for as long as nobody looked at both ends at once: the
driver armed the alarm, the DS3231 matched on schedule and set A1F with INTCN=1 and A1IE=1 —
and GPIO10, with nothing attached, read inactive through its own pull-up. An unconnected input
with a pull-up is indistinguishable from an idle open-drain interrupt. Only reading the part's
FLAG and the PIN in the same breath separates them. "Reported, not re-measured" is exactly the
class of claim that hides this.

Target is **GPIO11**, declared in the overlay. Checked free on this part: not
strapping (4/5/8/9/15), not USB (12/13), not the UART0 console (16/17), not SPI flash
(24–29; `ngpios` is 30).

Already wired and working, for reference: **SDA → GPIO6, SCL → GPIO7**, module VCC → **3V3**.

Two standing rules for this module:

- **Power it at 3.3 V, never 5 V.** R22/R23 pull SDA/SCL up to whatever VCC is fed, straight
  onto the C6's pins.
- **Do not add pull-ups.** The module already carries 10 kΩ on SDA, SCL and INT/SQW
  (R21/R22/R23).

### 1.3 Then, and only then, turn the alarm on — DONE, but NOT the way this said

**Done 2026-08-05, and the instruction below was superseded rather than followed.** The
base conf stays `CONFIG_RTC_ALARM=n`; `y` lives in an opt-in overlay,
`nodus/tuvm/zephyr/prj_esp32c6_rtcalarm.conf`, selected per build:

```
west build … --conf-extra "prj_esp32c6_thread.conf;prj_esp32c6_rtcalarm.conf"
```

That is the better shape and worth keeping: the caveat below is real — `CONFIG_RTC_ALARM`
takes the *entire RTC device* down if the ISW pin is not ready — so every C6 build that does
not need the alarm should not pay that risk. The cost is that **forgetting the overlay is
silent**: `rtcAlarm at:` simply answers false and the node runs on its watchdog alone, which
is correct behaviour and the wrong node. `ShipsBellAlarmNodeC6.nsl`'s header documents this.

Kept below as the record of the constraint, which still binds.

**Not before the wire is physically in place.** Either `CONFIG_RTC_ALARM` or
`CONFIG_RTC_UPDATE` makes `rtc_ds3231_init()` call `rtc_ds3231_init_isw()`, which configures
the pin and returns `-ENODEV` if it is not ready — taking the *entire RTC device* down with
it. That presents as "the driver is broken", not "the pin is floating", and will cost an
hour if you hit it without expecting it.

(`isw-gpios` is already in the overlay regardless, because the driver's device macro reads
it with `GPIO_DT_SPEC_INST_GET` — not the `_OR` form it uses for `freq-32khz-gpios` — so
omitting the property is a *compile* error, not graceful degradation.)

### 1.4 Build the alarm seam — DONE (`ShipsBellAlarmNodeC6.nsl`)

**Built 2026-08-05, and the design below survived contact.** Alarm2 with a minutes-match
mask is what shipped. Two things the design did not anticipate, both now in the source:

- **A watchdog rides alongside the alarm, and it is the only diagnostic.** It fires at
  boundary + 2 s. When it finds the boundary unrung, that is *positive evidence the alarm
  failed* — a poll could never report that, because it would be ringing everything and the
  alarm's failure would be invisible. `lastSlot` is stamped when the ring **starts**, not
  when it finishes, or the watchdog's own tick lands inside the pattern and re-fires.
- **The seam was where #151 lived.** One reaction serves three sources (alarm, countdown,
  watchdog), and registering the alarm + watchdog *reaped* the 10 ms stroke one-shot still
  queued in the pulse mailbox — a timer event destroyed by the next, unrelated registration.
  Fixed by carrying the token by value (#151, closed 2026-08-05); target gate
  `nodus/tuvm/tests/target/run.sh stale-handle`.

Kept below as the design record:

**Alarm2, not a 1 Hz square wave.** Alarm2 with a minutes-match mask fires on a boundary you
set, so after each strike you write the next half-hour into it and the C6 does *nothing at
all* until the boundary — no polling, no counting edges, no accumulated error, and the wake
instant is the RTC's own rather than yours. A 1 Hz SQW is a poll loop wearing a hardware
hat, and a missed edge is silent.

Mechanics: set `INTCN=1` so the pin is the alarm output rather than a square wave. Alarm2
supports `HOUR|MINUTE` masks but not `SECOND` (the driver gives `SECOND` to Alarm1 only),
which is exactly right for half-hour boundaries — alternate the minutes field between 0 and
30 after each strike, one I²C write per strike.

**The thing that will bite:** INT stays asserted until you clear `A2F` in the status
register over I²C. The flag clear is what deasserts the line — *not* the read of the time.

Shape it on `sensorArmEventPost:on:threshold:` — the established
`pulse_post_isr(chan_id, handle_idx)` pattern, with both resolved in thread context by the
caller. An ISR must never touch the VM.

### 1.5 Correct the clock

It currently reads **+20 s against host**. That is the stale seed constant in
`nodus/tuvm/zephyr/WallClockNode.nsl` (baked at authoring, written ~40 s later), not drift. `seedIfUnset:`
deliberately refuses to overwrite a clock that already reads a valid time, so a re-flash
will not fix it — set it through a real path, or bump the constant and clear the RTC first.

### 1.6 Then start measuring drift

The design's §10 wants the part's real ppm measured rather than read off the lid, and this
module earns that: **the IC is marked DS3231M** (MEMS, ±5 ppm) where the run of five
specifies a ±2 ppm TCXO — *and* its package (16-pin) disagrees with its marking (a genuine
DS3231M is 8-pin SO). Relabelled parts are common on these boards. Measure it.

---

## Part 2 — What actually gates a public release

### The reframe: these are two different bars

The strategy doc's falsifiable test for Path B is "one outside deployment surviving M weeks,
with #25, #56, #67, #71, #74 closed behind it." That is the **fleet-service** bar.

**Public release of nsl-tuvm is a much smaller bar.** Nobody cloning the repo to try it on a
board they own exercises OTA, signing or A/B rollback on day one. They exercise: *can I get
this running, and can I tell what it is doing.* Conflating the two costs months.

#79 already names the real blocker, and it is on nobody's feature list:

> "Today there is no entry point that does not start with 'read five repos'."

That is the gate. Everything below is roadmap.

**Half of it landed 2026-08-02: the restructure.** `tuvm`, `nsl-tuvm`, `nsl-tests` and
`porta` are one repository, `rete` — `porta/` + `nodus/{tuvm,nsl,tests}` + `protocol/`,
built by restructuring `tuvm` in place so every issue number and every `#NN` citation still
resolves. Plan:
`~/workspace/rete/docs/superpowers/plans/2026-08-02-rete-repo-consolidation.md`; design
`~/workspace/rete/docs/design/2026-08-02-rete-repo-consolidation-design.md`.

**#79 stays open, because "read five repos" was only half the sentence.** The other half is
*entry point*, and the root `README.md` written with the restructure is deliberately
minimal — the quickstart is explicitly out of that plan's scope (design §9). One repo with
no front door is a smaller problem than five repos with no front door, not a solved one.

**Next is #56 step 4 — the device test harness.** Steps 1–3 landed 2026-08-04/05; see the
ranking below for what that leaves and why step 4 was always the one that mattered.

### The four candidates, ranked

**1. porta → channels. First, and not close. — CLOSED 2026-08-02, ALL THREE PLANS DONE, HARDWARE GATE GREEN 5/5.**
The only one that *forecloses* the others. Debug-over-porta, OTA-over-porta and
agent-facing-porta all ride on the protocol — demo any of them on the v1 command protocol
and you rebuild the demo after the rewrite. #25 says it outright: "porta development gets a
stable node-side contract early." The v1 read-through conclusion still holds: **design the
node side first**, and read nodus/porta as evidence rather than spec.

Design 2026-08-01; plan 1 (porta side) and plan 2 (node side) are both built and green:

- `~/workspace/rete/porta/docs/design/2026-08-01-porta-space-sync-rewrite-design.md` — decisions
  D1–D8, three of which *correct* the ratified `PROTOCOL-NSL.md`.
- `~/workspace/rete/porta/docs/superpowers/plans/2026-08-01-porta-space-sync-core.md` — **plan 1,
  DONE** (demolition → `cval` → `space` → `wire` → loopback gate).
- `~/workspace/rete/docs/superpowers/plans/2026-08-02-porta-space-sync-node.md` — **plan 2,
  DONE 2026-08-02**: the space's monotonic seq + `changedSince:` (prims 138–140),
  `class PortaBridge` in `nodus/nsl/lib/kernel.nsl`, `nodus/tuvm/zephyr/PortaBridgeNode.nsl`, and
  `nodus/tuvm/tests/porta_sync_host.sh` — the nsl node against a real `porta serve` over UDP
  loopback, green on the first protocol run (`obs/health` reaches porta, `goal/name`
  converges to `obs/name`, `tel/log` exactly once).
- **Plan 3 — the C6 hardware gate — closed the rung, green 5/5.**
  `~/workspace/rete/nodus/tuvm/tests/porta_sync_c6.sh` — the same three legs witnessed over a real
  Thread mesh, including a deliberate post-commit ack drop, re-runnable on demand. Every
  run is a cold re-flash: leg 3 `rows=13 distinct=13, 1..13`; leg 4 `rows=26 distinct=26,
  1..26` across 4 swallowed acks. Hardware found what loopback could not: an unacked
  node's `pending` queue grew without bound and OOM-panicked at tick 48 (~96 s) on a
  128 KB heap, falsifying plan 2's N7 ("a miss costs latency and nothing else") — fixed
  by bounding `pending` at 64 rows with a counted `#streamDropped`. `sys/panic` delivery
  while the bridge is down remains undecided (#114). Design:
  `~/workspace/rete/docs/design/2026-08-01-porta-space-sync-c6-gate-design.md`; plan:
  `~/workspace/rete/docs/superpowers/plans/2026-08-01-porta-space-sync-c6-gate.md`.

Deferred out of plan 2, each filed rather than assumed: the cursor and `bootId` live in
RAM (NVS), the bridge holds the root space rather than a `goal`-write + `obs`-read pair
(G5), cadence is a constant rather than §4.4's controller, and a stream entry written
while the bridge is down is lost — fine for `tel/**`, **not** for `sys/panic`.

Settled, so it is not relitigated: **no backward compatibility** — v1 is deleted, not
deprecated, and the Toit/`st` kinds go with it. **Transport is plain UDP + CBOR**, because
CoAP rides the same UDP and does not dodge 6LoWPAN fragmentation; it earns its keep on the
bulk/OTA plane (blockwise) and DTLS, not on a control plane capped at 512 B/batch. **The
exchange needs no retransmit layer** — it is idempotent by cursor in both directions, so
loss is absorbed by the next check-in.

New issues from that session: #119 (a cold boot at cursor 0 must not re-execute old
`goal/do/*` actions — replaying a cell is idempotent, replaying `reboot` is a brick) and
#118 (node identity is a source constant in the first slice).

**2. OTA — but only the host-only slice (#25).**
Marked P1 revenue-gate, and the reasoning is the strongest in the tracker: simultaneously
the least-built and the most decision-foreclosing part, and the nRF52840's 1 MB must budget
MCUboot + A/B + NVS *up front*. The point is not to ship OTA before release — it is that the
flash map becomes a transcription of proven host behaviour instead of a guess. Cheap:
localhost UDP, no hardware. Do the slice, defer real OTA.

**3. Debug/profiling/console over porta (#56). — PROMOTED TO SECOND, 2026-08-02, David's call. IN PROGRESS: steps 1–3 landed, step 4 is the remaining work.**

#56 lists four capabilities. Where they stand as of 2026-08-05:

1. **Serial/remote console — DONE** (merged 5de48f6). `printk` *and* nsl's own output teed
   over Thread to a gateway sink, so a node on wallwart power with no data cable is
   observable at all. It earned its keep immediately: it is the only reason the bell's
   twelve unattended strikes are on record, and it diagnosed #149 **by silence**, which a
   local console could never have done.
2. **Symbolized panics — DONE.** Offline symbolizer; the build-id leg cost a spike (a
   `PT_NOTE` segment breaks the C6 image silently — no GNU build-id on this SoC, so the note
   goes in DROM via a RODATA snippet).
3. **Out-of-band periodic metrics — DONE** (merged 5de48f6, extended to the bell in
   c1798a3). `sys/metrics` published every check-in and accumulating gateway-side. The
   finding underneath it: the record was **unsendable by construction** — RECORDS §4.4
   pinned it as a cell while the conformance target scanned cells from `obs/**` alone, so no
   app had ever published it and neither document said so on its own.
4. **Device test harness — DONE 2026-08-05. `TARGET 3/3 PASS` on c6bench**
   (`bridge-health` 3/3, `pulse-coalesce` 3/3, `stale-handle` 2/2). #56 calls it "the real
   unlock; (1) and (3) are what make it possible", and it is now
   `nodus/tuvm/tests/target/run.sh`. Design:
   `docs/design/2026-08-05-target-test-harness-design.md`.

   The shape, decided by one requirement — diagnostics must eventually run on FIELD
   devices, so a scenario may not wipe a store, restart a daemon or ssh anywhere: **a
   scenario is a deployable workload, and there is no test framework on the device.** The
   workload emits TAP-lite with a DECLARED PLAN, so a probe that died halfway is failed
   rather than read as a pass. Two symmetric seams (`deploy` / `collect`) mean the field
   path is a swap of two functions.

   **`bridge-health` landed with the split.** The workload is `PortaBridgeNodeC6.nsl`
   itself — the bridge certifies itself and goes on bridging — and `porta_sync_c6.sh` kept
   only what genuinely needs privilege: the ack-drop injection and the delivery-side
   exactly-once check, now taken as a delta from a baseline instead of behind a db wipe.

   **The split paid for itself immediately by exposing a defect the wipe was hiding.** A
   rebooted node's cells were being discarded by porta as already-applied — the node's space
   counter restarts at 0, `nodes.node_seq_hw` does not — and the ack then handed the node the
   stale high-water, so neither end could tell. porta's `obs/health` for c6-bench was hours
   old while the node reported perfect delivery. `bridge-health` scored 3/3 over it on its
   first run, because its assertion and the failure shared a cause; the fix is
   `Store.SyncHighWater` (porta adopts the node's declared `hw`, a field it had been
   decoding and ignoring), a second clause on the node's assertion, and rule 5 of five in
   `protocol/PROTOCOL-NSL.md`.

**The first instance of step 4 already exists and was not called that.**
`nodus/tuvm/tests/stale_handle_c6.sh` (#151) was exactly `reset → scenario → assert →
report`: one command, flashes, drives, asserts **both** arms positively, anchors every
assertion on the build id `run.sh` archives for *that* flash, and reads the #56 collector's
log rather than opening the port (on the C6 the collector holds the CP2102N permanently and
two readers split the stream). Every preflight **fails rather than skips**, because a skip
whose message names an unverified cause is how a gate reports success for a run that never
happened. Generalising that script is a shorter step than designing a harness from scratch —
and `nodus/tuvm/tests/porta_sync_c6.sh` is a second worked example.

**Why this is more urgent than it looks.** #151 established that
`nodus/tuvm/src/backend/zephyr/timer_zephyr.c` posts through the pulse mailbox while
`timer_posix.c` posts straight into the space via `space_post`. The host path has no
mailbox, so **on host a timer event can be neither coalesced nor dropped nor staled — and
all 243 suite tests run there.** Every claim about coalescing, shedding and stale handles
needs a *target* gate or it is untested, and porting one to host deletes what it proves.
Today there are two such gates and no framework holding them.
Highest narrative value of the four — it *is* the agent story, not a supporting feature. And
it just got cheaper: #57 said the C6 unlocks the debug leg of #56, and as of 2026-07-31 the
C6 is proven and running tuvm with 44% of SRAM spare. It was third *only* by sequencing,
because it should ride on the new protocol — **and that constraint dissolved when the
protocol landed green on hardware.** Nothing now argues for doing #25's host slice first
except foreclosure, and foreclosure only bites at the moment a flash map is committed, which
is not imminent. So: #56 next, #25 after. **(#56 closed 2026-08-05 — so #25 is now.)**

**One thing ran ahead of #25, on 2026-08-05, and it is finished.** The span capture that
closed #107 left a check-in costing ~300 ms with no breakdown, and the breakdown was one
reflash away. `PortaBridge>>checkIn` is now six spans (`pullSend` / `pullWait` / `applyIn` /
`pushBuild` / `pushSend` / `pushWait`), `OEVM_SPAN_SLOTS` is 16, and the answer is
**70% VM, 30% radio — with `pushFrame` alone at 62% of the check-in and 60% of the tick**
(`docs/results/2026-08-05-c6-checkin-decomposition.md`, gate `TARGET 1/1 PASS`). The
expected culprit was 6LoWPAN fragmentation on the 512 B push frame; `pushSend` measured
3.1 ms, the cheapest span in the table, so that hypothesis is dead.

Two consequences for the queue. **#161** (the same value is CBOR-encoded three or more times
per push — `valueFits:` encodes and discards, `encodePush:` re-encodes, the trim loop
re-encodes the whole frame per dropped row) is now a named, cheap, node-side piece of work
with a measurement behind it. **#159 does not get earned**: the reason to want porta driving
`diag/spans/ctl` was a repeating window for a distribution, and the thing worth
characterising turns out to be VM-side and deterministic — three more spans and another
boot, not an authority change. It stays open with a fourth option on it (a node-chosen
free-running cadence, which adds no mechanism) that the other three must beat.

**#25 is still next.** #161 is the cheaper and more contained of the two, but it is an
optimisation with a measurement behind it and no deadline, where #25 is the decision-
foreclosing one.

**#161's precondition landed on 2026-08-06 and answered the question.** `pushBuild` is
three nested spans (`pushCollect` / `pushEncode` / `pushTrim`, 14 slots of 16), and on the
C6: **encode 118.6 ms (63.5%), collect 67.7 ms (36.3%), trim 139 µs (0.07%)**, both
partitions holding to 0.14%. So `pushBuild` is **one thing done twice, not three things** —
#161's fix (1) (keep the bytes `valueFits:` already produced) is worth up to ~21% of a
check-in, and fix (2) (incremental trim sizing) is unearned by this data.
**The bigger number is one #161 did not ask about:** 118.6 ms for a ≤512 B frame is
~230 µs *per byte*, so deleting the duplicate encode still leaves ~119 ms. The cost is the
codec itself, not how many times it runs — the next question is inside `Cbor>>encode:`,
and it is about allocation and copying rather than call count.
Two findings from the host half matter beyond #161
(`docs/results/2026-08-06-pushbuild-three-way.md`):
**(a)** the boot-to-verdict window is structurally blind to the trim loop — trimming only
starts once the capture's own 14 cells create a backlog over 512 B, and the window closes
before they are written, so a capture in this shape reports `pushTrim ≈ 0` for a loop it
never observed. Predicted on host, then confirmed on the C6 at 139 µs — so that 0.07% is
the blind spot, not a verdict on the loop. That is the thing `PortaBridgeNodeC6.nsl` said
would earn a repeating window, and it argues for **#159's fourth option** (a node-chosen
free-running cadence, no authority change) rather than for porta driving `diag/spans/ctl`.
**(b)** those same span cells starve `sys/metrics` — **#150, now measured**: the burst
drains in ~8 ticks, and until it does, the tail-trim drops the highest seq, which is
always the metrics record written immediately before the push. It turned
`porta_sync_host.sh` red, which is how it was found; the gate now waits for the run-up
instead of assuming a drain time, and no trim policy was invented.

**One correction to carry into it, because the two bars are still easy to swap.** #56 is
*not* the largest unlock to a public release of nsl-tuvm — by this document's own reasoning
above, that gate is #79's "no entry point that does not start with 'read five repos'", and
the public-release bar is much smaller than the fleet-service one. What #56 is the largest
unlock for is the **agent/fleet story** — the pitch, the demo, and the thing that makes the
platform legible to something that arrives after the node is already dead. Both are worth
doing; conflating them is what the reframe at the top of Part 2 warns costs months.

**4. Agent-friendliness.** Fourth because it is mostly not a separate workstream — see below.

### Agent-friendliness, concretely

Three of the things that make this platform agent-friendly are **already revenue gates** and
were never labelled as such:

- **Observability that outlives the node (#56).** "Metrics die with the node"; `/stats` is a
  *pull*. Pull-based introspection is a human affordance — a human is there to click. An
  agent arrives after the corpse is cold.
- **Provenance (#74).** A node cannot name its own build. An agent cannot reason about a
  fleet when it cannot tell which code is on which node. Unglamorous, load-bearing.
  **CORRECTED 2026-08-05 — less built than an earlier edit of this file claimed.** Two
  different things were conflated. The C6 *prints* a build id at boot (DROM via a RODATA
  snippet — a `PT_NOTE` segment breaks the image silently on this SoC), and the target
  harness anchors on that printed line. But **`obs/build` carries `#chip` and nothing
  else** — `PortaBridgeNodeC6.nsl:63`, `ShipsBellAlarmNodeC6.nsl:133` — so over porta a
  node still cannot name its build, which is exactly what #74 asks for. **This now blocks
  something concrete:** the target harness's field path has no anchor without it, because
  off the bench there is no boot-banner ordering to lean on. A `platform buildId` accessor
  closes both.
- **Iteration cost (#67).** Agents iterate 10–100× more than humans, so per-cycle cost
  multiplies in a way it never did for a human developer. This is the argument that survives
  the "an agent writes C fine" substitution.

Two are genuinely untracked (now noted on #79):

- **porta as a typed tool surface, not a UI.** Agents do not read dashboards. Every
  operation wants to be a callable with structured arguments and structured results — stable
  names, machine-readable errors, no scraping. A human dashboard should be a *client* of
  that surface. This is the likeliest thing to get built the wrong way round, because the UI
  is what demos.
- **Inspectable authority.** Grants are already enforced; the agent-facing half is making a
  grant *legible*, so an agent can ask "what am I allowed to do here?" and get an answer.

Plus the sandbox: #25's fourth item (N VMs + a toy porta in one CI job) is worth more for
agent-friendliness than its issue text suggests — it is what lets an agent try a fleet
operation without hardware.

### The measurement nobody is taking

Every falsifiable criterion in the strategy doc is unmeasured, and the only fleet is on the
bench. The strongest single artifact it names is **time-to-first-working-application on a
board never seen before, honestly timed, failures included**.

2026-07-31 was close to that and nobody was holding a stopwatch: "can you see the C6" to a
DS3231 seam running on hardware with four passing host tests, about two hours, including
finding a real Zephyr driver defect *by reading it before flashing*.

That is now the **third** instance of the best story — Grid-EYE wedge diagnosed from a
timing signature, C6 radio localised to four driver bugs (zephyr#114163), and now the
DS3231's missing `-ENODATA` (#103) predicted from source and confirmed on the first hardware
run. Three is a pattern, not an anecdote.

**Sequencing (revised 2026-08-05, updated the same evening):** porta→channels ✅, then
#56 — **all four steps ✅ and the issue is CLOSED** (`TARGET 3/3 PASS`, commit 16d2ce9) —
so **the #25 host slice is where the work is now**. Start timing bring-ups now, because that
measurement is free and it is the one number that would make the pitch land.

> The harness closed #56 and opened one thing worth carrying forward: it found #157 (a
> rebooted node's cells discarded as already-applied, silent at both ends) within minutes of
> first running, and the remaining sliver of that lands on **#74** — which is now the
> prerequisite for two separate things, the field `collect` anchor and the per-boot identity.
> #74 is small and it is upstream of more than its title suggests.

**A fourth instance of the best story arrived 2026-08-05, and it is a different kind.** The
three above were all *defects found by reading source before flashing*. #151 was found the
other way — by an A/B on hardware differing by one line, against a defect no host test could
ever have exhibited. It is worth telling alongside them precisely because it shows the limit
of the reading trick: `reap_oneshots()` had been correct-looking C for as long as it existed,
and #13 had already looked straight at the ABA window and chosen dropping over mis-delivery.
Nobody asked why a `SmallInteger` needed a handle at all. That question is what the harness
(step 4) makes cheap to ask again next time.

---

## Part 3 — New targets worth timing a bring-up on

### RAM is not the binding constraint

Current heap settings: ESP32 49 KB, MagTag 64 KB, **C6 128 KB**. The C6 build uses 247 KB of
437 KB SRAM with I2C, RTC, the DS3231 driver and `CONFIG_LOG` all linked in.

The binding constraint is **whether the HAL is already in the west workspace**.
`~/zephyr-esp32/modules/hal/` already carries st, stm32, rpi_pico, silabs, renesas, nxp, ti,
infineon, atmel, microchip, ambiq, gigadevice, nuvoton, wch. Pick outside that set and you
are timing a west setup, not a bring-up.

### Candidates with ≥256 KB on-chip SRAM and a board port in-tree

| Part | SRAM | Board port | Rough street price |
|---|---|---|---|
| **RP2350** (Pico 2) | 520 KB | `rpi_pico2` | ~$5 |
| **RP2040** (Pico) | 264 KB | `rpi_pico` | ~$4 |
| **STM32H743** | 512 KB | `weact/mini_stm32h743` | ~$12–15 |
| **STM32H723** | 320 KB | `nucleo_h723zg` | ~$28 |
| **STM32F412/F413** | 256/320 KB | `nucleo_f412zg` / `f413zh` | ~$20 |
| **EFR32MG24** | 256 KB | `silabs/xg24/*` | ~$30–50 |
| **ESP32-C3** | 384 KB | in-tree | ~$3 |
| **SAME51/SAMD51** | 256 KB | atmel | ~$25 |
| **Renesas RA6M4** | 256 KB | renesas | ~$20–30 |

ST alone has 127 board directories in-tree — the deepest bench available, and untouched.

### The non-obvious one: nsl uses doubles

`nodus/tuvm/src/oevm.h:187` — `struct Float { double value; }`.

Every Cortex-M4F/M33 has a **single**-precision FPU, so nsl floats have been
software-emulated on the nRF52840 all along. Two consequences:

- **RP2040's lack of an FPU costs far less than it looks** — you were already softfloat for
  doubles on every ARM part run so far.
- **STM32H7 (Cortex-M7) has a double-precision FPU** — the only candidate where nsl floats
  would run in *hardware*. Against the existing STIC/Perfstex numbers that is a genuinely
  novel data point, on a ~$12 board.

(Unrelated: `nodus/tuvm/zephyr/prj_dongle_softfloat.conf` was about matching Nordic's MPSL radio-library ABI,
not about nsl.)

### Picks

For a bring-up *timing* measurement the variable you want is **a vendor never built for**.
ESP32×3 and nRF52840 both carry months of accumulated work; timing a fifth Espressif part
measures nothing.

**First: Raspberry Pi Pico 2.** ~$5, 520 KB, board port present, HAL fetched. The most
widely owned board in the world, so the number needs no explanation to any audience. A third
architecture family (M33, and RP2350 carries RISC-V cores too). Cheap enough to buy five and
brick one.

**Second: WeAct mini_stm32h743.** ~$12–15, 512 KB, double-precision FPU, and ST is the
largest Cortex-M vendor — the "professional standard" leg the bench lacks entirely.

**Skip for this purpose:** ESP32-C3/S3 (same vendor, measures nothing); EFR32MG24 is
interesting as a *second Thread stack* comparison against the C6 work, but dev kits are
pricey and it is a narrower question.

**Pico gotcha, decide before starting the clock:** flashing is the easy part (hold BOOTSEL,
plug in USB, it mounts as mass storage, drag the UF2 — far gentler than the MagTag's
BOOT0+RESET). But Zephyr's default console on `rpi_pico` is **UART0 on GP0/GP1, not USB** —
the same trap that cost time on the MagTag. Either enable USB CDC as the console, or put a
USB-serial adapter on two jumper wires, or use a second Pico as a debugprobe. Decide at
minute zero, not minute forty.

**On timing honestly:** run the clock from a cold start and count the failures. The most
interesting content of the C6 session was a driver defect found by *reading* rather than
flashing, and that texture disappears if only the total is reported.

---

## Part 4 — Loose ends from 2026-07-31

- **#104** — the WROOM-32 does not link at all (`undefined reference to sntp_simple`);
  `prj_esp32.conf` sets `CONFIG_WIFI=y` but never `CONFIG_SNTP=y`. Pre-existing, confirmed by
  stashing. One-line fix, plus a sturdier variant (guard the body on `CONFIG_SNTP`).
- **#103** — the upstream DS3231 `-ENODATA` fix. Same family as #91–#93/#95.
- **The C6 was running Toit/Jaguar** (`test-server`, id `c3a7d184-…`) and that install was
  overwritten. If it is wanted back, it is a re-flash.
- **`ESPTOOL_PORT` must be set explicitly for the C6** — its `run.sh` profile pins
  `*Espressif*` → `/dev/ttyACM0` (native USB-JTAG), which is not cabled. Everything goes over
  the CP2102N. Three CP210x boards share the by-id glob, so identify by `esptool read_mac`.
