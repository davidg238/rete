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
`nodus/oevm/zephyr/boards/shipsBells/2026-08-03-shipsbells-3v3-piezo-driver.svg`. R2 does
**not** mitigate #151 — it holds the base down only while the pin is undriven, and a solid
tone is the pin being *driven*.

**And R2 does not close the ROM downloader either (2026-08-12, #201).** The failed flash on
2026-08-11 rang the bell for two minutes *with R2 already fitted*, so "fit a pull-down" —
#201's first recommendation and §11.9's — is a remedy that was in place at the time of the
failure. That makes the download-mode ring a **different fault from the reset-window ring
R2 fixed**: with a high-Z pin R2 holds the base at 0 V, so GPIO18 must be driven high or
weakly pulled up. R1/R2 = 10 k/47 k leave the base tracking 82% of the pin, which a ~45 kΩ
internal pull-up is more than enough to defeat (base ≈ 1.5 V) where the solenoid rig's 10 kΩ
was not (base ≈ 0.5 V). **Next bench action: park a board in `boot:0x66 DOWNLOAD`
deliberately and meter GPIO18 and the 2N3904 base** — that state waits indefinitely, so it
is the first time #105's localising test has had a target that holds still.

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
`nodus/oevm/zephyr/boards/shipsBells/board.png` shows a small red glass diode and a `201` (200 Ω)
resistor beside the SCL pad — that board's well-documented **LIR2032 trickle-charge path
from VCC to VBAT**. The cell fitted is a **CR2032, which is not rechargeable**.

`nodus/oevm/zephyr/boards/shipsBells/rtc-schematic.png` shows no such circuit and a plain
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
`nodus/oevm/zephyr/boards/esp32c6_devkitc_esp32c6_hpcore.overlay:78` carries
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
`nodus/oevm/zephyr/prj_esp32c6_rtcalarm.conf`, selected per build:

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
  `nodus/oevm/tests/target/run.sh stale-handle`.

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

> **Copy the pattern as it stands AFTER rete#184 (2026-08-09), not as it was written.** Both
> exemplars of this shape — `sensorArmEventPost:on:threshold:` and `rtcAlarmAt:post:on:` —
> pinned the message oop with `handle_new` on every arm and **never freed the previous one**,
> and no cancel API existed to call one. The handle table is 32 slots and `handle_new` aborts
> the VM rather than degrading, so on the bell (one re-arm per half-hourly strike) slot 32 was
> reached ~15.5 h after boot: a hard uptime ceiling by construction. So a NEW seam of this
> shape needs three things the original pattern did not have:
>
> 1. a place to name the outstanding pin (one static per seam is enough — there is one
>    outstanding trigger per node), freed **after a confirmed arm, never before it**;
> 2. a `handle_free` on every failure path, since a pin nothing references is still a slot;
> 3. an answer of **nil** when the backend refuses, so a caller has to carry a fallback
>    instead of assuming a trigger it never got.
>
> The GPIO seam (`platform.c`'s `host_gpio_register`) and the timer seam
> (`backend/zephyr/timer_zephyr.c:43`) already did this correctly and are the better models
> to read. Regression test: `nodus/tests/15_heap_stats/03_arm_seams_no_handle_leak.nsl`.

### 1.5 Correct the clock

It currently reads **+20 s against host**. That is the stale seed constant in
`nodus/oevm/zephyr/WallClockNode.nsl` (baked at authoring, written ~40 s later), not drift. `seedIfUnset:`
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

**Half of it landed 2026-08-02: the restructure.** `oevm`, `nsl-tuvm`, `nsl-tests` and
`porta` are one repository, `rete` — `porta/` + `nodus/{oevm,nsl,tests}` + `protocol/`,
built by restructuring `oevm` in place so every issue number and every `#NN` citation still
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
  `class PortaBridge` in `nodus/nsl/lib/kernel.nsl`, `nodus/oevm/zephyr/PortaBridgeNode.nsl`, and
  `nodus/oevm/tests/porta_sync_host.sh` — the nsl node against a real `porta serve` over UDP
  loopback, green on the first protocol run (`obs/health` reaches porta, `goal/name`
  converges to `obs/name`, `tel/log` exactly once).
- **Plan 3 — the C6 hardware gate — closed the rung, green 5/5.**
  `~/workspace/rete/nodus/oevm/tests/porta_sync_c6.sh` — the same three legs witnessed over a real
  Thread mesh, including a deliberate post-commit ack drop, re-runnable on demand. Every
  run is a cold re-flash: leg 3 `rows=13 distinct=13, 1..13`; leg 4 `rows=26 distinct=26,
  1..26` across 4 swallowed acks. Hardware found what loopback could not: an unacked
  node's `pending` queue grew without bound and OOM-panicked at tick 48 (~96 s) on a
  128 KB heap, falsifying plan 2's N7 ("a miss costs latency and nothing else") — fixed
  by bounding `pending` at 64 rows with a counted `#streamDropped`. `sys/panic` delivery
  while the bridge is down remains undecided (#114). Design:
  `~/workspace/rete/docs/design/2026-08-01-porta-space-sync-c6-gate-design.md`; plan:
  `~/workspace/rete/docs/superpowers/plans/2026-08-01-porta-space-sync-c6-gate.md`.

Deferred out of plan 2, each filed rather than assumed: the **cursor** lives in RAM
(`bootId` no longer does — rete#162 landed it in NVS on 2026-08-09; the cursor half of
#117 stays open on purpose, since persisting it changes what a reboot means and is
entangled with replaying `goal/do/*`, #119), the bridge holds the root space rather than
a `goal`-write + `obs`-read pair
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

**2. OTA — but only the host-only slice (#25). BUILT AND GATED, 2026-08-08.**
Marked P1 revenue-gate, and the reasoning is the strongest in the tracker: simultaneously
the least-built and the most decision-foreclosing part, and the nRF52840's 1 MB must budget
MCUboot + A/B + NVS *up front*. The point is not to ship OTA before release — it is that the
flash map becomes a transcription of proven host behaviour instead of a guess. Cheap:
localhost UDP, no hardware. The call was: do the slice, defer real OTA.

**That is now done.** `cd nodus/oevm && bash tests/host_ota/run.sh` prints **HOST OTA OK**
on a plan of four scenarios against three runner-wrapped VMs and a real `porta serve` over
localhost UDP: a signed deploy converges on all three nodes; a SIGKILL mid-transfer resumes
from the staged length rather than refetching; a corrupted signature is refused **outside**
the image with the old one still running and still checking in; and an image that boots but
never bridges is reverted by the trial watchdog. The state machine
(stage → verify → activate → trial → confirm | revert) is MCUboot's on purpose, so the
Zephyr rung transcribes it rather than inventing it. Design:
`docs/design/2026-08-07-code-push-host-slice-design.md`.

**What it deliberately did not do**, each with what earns it back:

- **Async fetch.** `otaStage:` blocks; invisible on localhost. Earned by a *measured*
  check-in stall on a target-class transfer (D7).
- **Multi-app.** Exactly `apps/main` is read and written (D5). Separately compiled images
  sharing one VM is a VM feature for its own rung, if a fleet ever earns it.
- **The zephyr OTA backend.** `nodus/oevm/src/backend/zephyr/ota_zephyr.c` exists and is deliberately
  **inert** — it keeps the link and the seam shape identical everywhere while refusing to
  invent a flash layout there is no board to measure. The real one lands with the real
  flash map, and that rung is now **#233** (filed 2026-08-22; until then a C6 CANNOT be
  flashed OTA, and **#175 should land first** — the degenerate retry's measured cost, a stuck
  node refetching ~200 kB every ~6 s, was measured on localhost and would saturate a
  6LoWPAN mesh).
- **Boot-loop containment / safe mode (G11).** Its own decision at Zephyr bring-up.

**And one number the rung was supposed to come back with.** D6 chose the degenerate retry
— a failed stage is not remembered, the next check-in simply tries again, with no backoff,
no attempt counter and no give-up threshold, because there was no data to choose any of
them from. The gate measured its cost: **a stuck node — bad signature, or an image that
never confirms — refetches the whole ~200 kB image every ~6 s, sustained.** That is the
criterion the design set for earning the rejected-hash memo back, and it is now met: the
knob has a price tag instead of an argument.

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
   `nodus/oevm/tests/target/run.sh`. Design:
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
`nodus/oevm/tests/stale_handle_c6.sh` (#151) was exactly `reset → scenario → assert →
report`: one command, flashes, drives, asserts **both** arms positively, anchors every
assertion on the build id `run.sh` archives for *that* flash, and reads the #56 collector's
log rather than opening the port (on the C6 the collector holds the CP2102N permanently and
two readers split the stream). Every preflight **fails rather than skips**, because a skip
whose message names an unverified cause is how a gate reports success for a run that never
happened. Generalising that script is a shorter step than designing a harness from scratch —
and `nodus/oevm/tests/porta_sync_c6.sh` is a second worked example.

**Why this is more urgent than it looks.** #151 established that
`nodus/oevm/src/backend/zephyr/timer_zephyr.c` posts through the pulse mailbox while
`timer_posix.c` posts straight into the space via `space_post`. The host path has no
mailbox, so **on host a timer event can be neither coalesced nor dropped nor staled — and
all 243 suite tests run there.** Every claim about coalescing, shedding and stale handles
needs a *target* gate or it is untested, and porting one to host deletes what it proves.
Today there are two such gates and no framework holding them.
Highest narrative value of the four — it *is* the agent story, not a supporting feature. And
it just got cheaper: #57 said the C6 unlocks the debug leg of #56, and as of 2026-07-31 the
C6 is proven and running oevm with 44% of SRAM spare. It was third *only* by sequencing,
because it should ride on the new protocol — **and that constraint dissolved when the
protocol landed green on hardware.** Nothing now argues for doing #25's host slice first
except foreclosure, and foreclosure only bites at the moment a flash map is committed, which
is not imminent. So: #56 next, #25 after. **(#56 closed 2026-08-05, and #25 followed —
built and gated 2026-08-08; thread 2 above has the status.)**

5. **Per-method profiling over `dbg/req` (#164/#165) — DONE 2026-08-06.** Not one of #56's
   original four, but riding directly on capability 1 above: `dbg/req` gets its first
   implementation since v1 was deleted, carrying a `profile <ms>` line. The node opens a
   self-limiting activation-count window and the rows leave over the console already teed
   to the collector, never over `dbg/resp` and never through the space. Cost measured at
   **0.90%** (C6, Hanoi, load factor 0.008 — the cost at a realistic load is unmeasured).
   Capacity measured two ways, on purpose, because they answer different questions:
   `methods=224 overflow=0` (host gate, load factor 0.875, the union of every path the suite
   takes, `docs/results/2026-08-06-mprof-capacity-under-bridge.md`) and
   `methods=177 overflow=0` (a 10 s window on c6spare in steady-state bridging on the real
   mesh, load factor 0.69, `docs/results/2026-08-06-mprof-window-hardware.md`). The choice
   between a larger table, Space-Saving displacement, or scoping by method identity is
   deferred on the still-missing realistic-load cost number — David's call
   (`docs/design/2026-08-06-method-profile-over-porta-design.md` D9/D10). Also closed #74:
   `platform buildId` names the running build, so the `.oimg.methods.json` sidecar the
   profiler's offsets are resolved against cannot be picked wrong.

   **Allocation attribution rode the same table — stage 1 DONE 2026-08-12** (#190's row;
   design `docs/design/2026-08-12-allocation-attribution-design.md`). The same mprof rows
   now carry per-method *allocated bytes*, hooked at the single accounting choke point of
   each heap, so "who allocates" is answerable by name rather than by bisecting builds.
   The method key is *derived* from `current_frame->closure` at the allocation site, so
   `gc.c` learns nothing about methods and `struct MethodContext` gains no field. Three
   windows, all with `allocOverflow=0` and a **zero** no-frame bucket:

   | window | distinct allocating methods | arena allocs per activation |
   |---|---|---|
   | host, full `porta_sync_host.sh` | 64 | 1 per 10 300 |
   | C6 boot (`PortaBridgeNodeC6`) | **80** | 1 per 5.6 |
   | C6 steady-state bridging, 60 s | 65 | 1 per 15.4 |

   **The steady-state population is a strict subset of the boot population** (union 80), so
   boot bounds this workload and 80 is stage 2's sizing input — a 128-slot always-on table
   at 0.63 load. Two things the measurement refuted rather than confirmed: D4's "allocations
   are two orders of magnitude rarer than activations" is wrong on target by two to three
   orders, and #166 was cleared of the C6 hang along the way when a 1024-slot build hung
   identically. What the rows say about the code: the CBOR codec is the entire steady-state
   allocation story, `ByteBuilder`'s grow-then-freeze is 7–11% of a window for what #163
   calls pure overhead, and **every Symbol-vs-Symbol comparison materializes two Strings**
   (`valueCompare:with:` rank 6 goes through `printString` because `dispatch.c` has no
   Symbol fast path for `at:`/`size`) — which is 71% of all C-heap allocation on the node
   and is #204's real answer. Results:
   `docs/results/2026-08-12-allocation-attribution-{host,c6}.md`. Stage 2 is **#203**.

   **The one open item — the hook's runtime cost — closed the same evening.** An open
   window costs **+1.88% of the node's local CPU and +0.13% of elapsed time** at the 2 s
   cadence (~0.30 µs per hooked event) at a table load of 0.77, so the always-on table D8
   wants is affordable on this evidence and D5's invented knob is still not earned.
   `ProfileCostNodeC6.nsl` runs `off | ON | off` eight times in one binary and one boot;
   the claim is a **sign test** (8 of 8, null 1/3) and not a mean, because the first
   two-arm run read +1.56% with drift a third of that. The same run discharged the
   post-#205 steady-state window that #206 was blocking (66 workload allocating methods
   vs 65 before, i.e. #205 moved boot and not steady state).
   `docs/results/2026-08-12-profile-window-cost-c6.md`, design §9.8.

   **And #166 fell to the same rig, the same evening.** A fixed-depth Hanoi burst rides
   the same arms in its own span — **inside** the bridge tick, which is the load-bearing
   choice, because Hanoi alone reproduces exactly the 0.008 load factor #166 was opened
   about. The activation hook alone: **322.7 ns/activation at load 0.78 against 106 ns at
   0.008, i.e. 3.04×**, so the quoted 0.90% was understating real-load cost by 2.6× and
   #166's suspicion is confirmed. The probe earns its place by predicting its own
   denominator (2ⁿ−1 activations, pinned against the dump) and by holding no radio: drift
   0.089% against 0.58–1.5% on the bridge spans. Still open and now a *feature*
   question, not a spike: the allocation columns' own per-event price needs per-span
   activation counts. Design §9.9.

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

**#25 went first, and is now built and gated** (see thread 2 above — `HOST OTA OK`,
2026-08-08). #161 is the cheaper and more contained of the two, but it is an optimisation
with a measurement behind it and no deadline, where #25 was the decision-foreclosing one.

**#161's precondition landed on 2026-08-06 and answered the question.** `pushBuild` is
three nested spans (`pushCollect` / `pushEncode` / `pushTrim`, 14 slots of 16), and on the
C6: **encode 118.6 ms (63.5%), collect 67.7 ms (36.3%), trim 139 µs (0.07%)**, both
partitions holding to 0.14%. So `pushBuild` is **one thing done twice, not three things** —
#161's fix (1) (keep the bytes `valueFits:` already produced) is worth up to ~21% of a
check-in, and fix (2) (incremental trim sizing) is unearned by this data.
**The bigger number is one #161 did not ask about:** 118.6 ms for a ≤512 B frame is
~230 µs *per byte*, so deleting the duplicate encode still leaves ~119 ms. The cost is the
codec itself, not how many times it runs. **Filed as #163**, and it is the larger of the
two: a bridged node spends ~19% of wall-clock inside that one method.

**#163's precondition landed on 2026-08-09, and it refuted the guess this document made
about it** (`docs/results/2026-08-09-cbor-encode-shape.md`). The guess was "allocation and
copying rather than call count". It is **call count**, and allocation is not in the picture
at all: the GC counter moves by *one* across 400 encodes of a 425 B frame. The cost is
**per byte** — ~9 activations per byte flat, 22.6 for a real push frame — and the CBOR
recursive descent is **0.5%** of it. Everything else is byte plumbing: one `acc add: b`
costs four activations, and `ByteBuilder`'s doubling growth plus `frozenBytes`' exact-size
copy are **two thirds of a flat encode**. `encodeMap:` multiplies the rate by re-copying
every entry through a per-entry buffer, one `add:` send per byte, once per level of nesting
— the real frame re-copies 704 bytes to emit 425, while Lists (which write straight
through) cost +2% for the same nesting.

The count also *predicts* the hardware number above: 9 624 activations × #164's measured
11.83 µs/activation on the C6 = **113.9 ms** against the 118.6 ms measured — 4%. So the
honest statement of the rate is **22.6 activations/byte × 11.83 µs/activation**, not
"230 µs/byte": the same number, split into the part a fix can move and the part it cannot.
One term is already sized — `ByteBuilder`'s initial capacity 16 → 512 takes the frame from
9 624 to 7 456 activations (−22.5%), though that constant is a probe and not a proposal,
since `encodeMap:` allocates two builders per entry. The fixes it picks, in order: a
byte-append costing one activation instead of four; deleting the map re-copy; sizing the
accumulator instead of growing then freezing it. #161's fix (1) is unaffected and still
true — it deletes a whole extra encode, where this is what one encode costs.
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

**ADDENDUM 2026-08-16 — "fix (2) is unearned by this data" is now REVERSED, and the two
fixes have merged into one.** The sentence above is exactly right about the data it had, and
finding (a) beside it names the reason it was wrong: the capture window is structurally blind
to the trim loop. That blind spot has now been looked past, on hardware, and the loop is not
139 µs.

**Cold boot pays one full-frame re-encode inside `pushTrim`.** Boot 39 on c6spare measured
`pushTrim` at **49 884 µs** against `pushEncode`'s 46 796 µs — the same frame, encoded twice —
where the second check-in reads 183 µs and the host reads 2 µs. The mechanism is `fitRows`
being 0 before the first push, so `pushFrame` starts at `rows size` (every candidate) and the
**step-down** branch drops one row and re-encodes until it fits. That branch is the case the
design's "never encodes more than one row beyond the answer" does not cover — that claim is
true of the **climb**.

**BUT THE FIX THAT FOLLOWED WAS LANDED AND REVERTED THE SAME DAY (#213).** `Cbor>>sizeOf:`
and `valueFits:` onto it passed every host gate — nsl 265/0, `PORTA SYNC OK`,
`go test ./...` — and then hung a C6 after six check-ins: silent, no fatal, no OOM abort, no
reboot banner. A three-arm bisect (054f110 healthy at tick 209; both commits hung at tick 6;
both plus the prefix-sum spike hung identically) puts the fault in `kernel.nsl` and rules out
the store, the backlog and the arena. **The mechanism is unexplained**, which is why the
revert (a4e6bc2) was total rather than a patch — `14_cbor/07` had pinned `sizeOf:` against
`encode:` across 27 boundary cases, so the byte count cannot be the fault. #213 carries the
next experiment: flash `38505d4` alone and learn whether merely DEFINING an unused class
hangs the node, which would be a far larger finding than the codec.

**And the prefix-sum design's premise is falsified, independently of the hang.** Sizing is
much cheaper than encoding only for FLAT values (680 vs 904 B on a 200 B string, constant in
the value's size). For a **Record** it is 8344 vs 9120 — **91%** — because the cost lives in
`encodeMap:`'s per-key ByteBuilder and a `cborEncodeInt:` fragment per integer, neither of
which a counting sink avoids. Steady-state frames carry records, so a prefix sum pays ~2x the
expensive machinery where the climb pays 1x: measured on the realistic 4-record frame,
**43 192 B -> 65 032 B per push, a 1.51x regression** on the every-2-seconds path, for
identical output. **fix (2) is therefore blocked behind #163** — making a record encode cheap
is the only move that creates room, and both fit strategies are downstream of it.

**What the prefix sum would still buy, and why the question is genuinely two-sided.** The old
search's first encode is of ALL candidates, so its ByteBuilder rung scales with the backlog —
2048 B at 8 rows, **4096 B at 16**, 8192 B at 24 — where the prefix sum is flat at 512 B.
4096 plus a header is the **4104-byte request #199 died unable to satisfy**, and this is not
historical: a 33-min window on boot 39 (999 samples, halves agreeing) reads **`lfb` MIN
524 B**, `lb` max 130 296 of 131 072, 0.364 GC/check-in. So it is bounded peak versus lower
total, on a heap with no headroom either way. **Note the arena is 128 KB on the C6**, not the
53 KB the `09_batch_size` fixture uses — that figure is a WROOM-32 measurement and should not
be quoted for this board.

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

**Sequencing (revised 2026-08-05, updated 2026-08-08):** porta→channels ✅, then
#56 — **all four steps ✅ and the issue is CLOSED** (`TARGET 3/3 PASS`, commit 16d2ce9) —
then **the #25 host slice, now built and gated** (`HOST OTA OK`; thread 2 above names the
four deferrals and the measured retry cost). Next is the Zephyr OTA backend against a real
flash map. Start timing bring-ups now, because that measurement is free and it is the one
number that would make the pitch land.

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

`nodus/oevm/src/oevm.h:187` — `struct Float { double value; }`.

Every Cortex-M4F/M33 has a **single**-precision FPU, so nsl floats have been
software-emulated on the nRF52840 all along. Two consequences:

- **RP2040's lack of an FPU costs far less than it looks** — you were already softfloat for
  doubles on every ARM part run so far.
- **STM32H7 (Cortex-M7) has a double-precision FPU** — the only candidate where nsl floats
  would run in *hardware*. Against the existing STIC/Perfstex numbers that is a genuinely
  novel data point, on a ~$12 board.

(Unrelated: `nodus/oevm/zephyr/prj_dongle_softfloat.conf` was about matching Nordic's MPSL radio-library ABI,
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

---

## ADDENDUM 2026-08-16 (later) — the #161 hang narrowed, and what it exposed underneath

The addendum above ends with #213 carrying "the next experiment". It ran, on hardware.

**`38505d4` alone is healthy to tick 216** — `ByteCounter` and `Cbor>>sizeOf:` defined, nothing
calling them, `bridge-health` PASS 3/3 then held on the board past the tick-6 death point, zero
fault lines. **Read again 2026-08-17 16:38 MST: the same boot, `bid` still 43, at tick 45 695 and
`up` 91 580 822 ms — 25.4 h unbroken, with `pdr` 0, `sdr` 0, `hnl` 0 and 16 639 GC cycles behind
it.** Tick 216 was where the session stopped watching, not where the evidence ran out; the arm is
now ~7 600x past the tick-6 death point rather than 36x, and no allocation on it has ever returned
null. So the larger fork is closed: **defining an unused nsl class in the kernel does not
hang the node**, and the solidified image, method table and class-index layout are exonerated.
`d9a0d85` is the actor, and its executable delta is one line (the other ~50 changed lines of that
commit are comment prose). **The mechanism is still unexplained.**

Two things were excluded with evidence, and one near-miss is worth carrying:

- **VM stack exhaustion is out.** `stack_overflow_abort` prints to stderr then aborts and guards
  both `push_frame` sites; VM stderr demonstrably reaches this console (#199's `VM GC Error: Out
  of Memory` lines are in the logs); `Stack Overflow` has never appeared in any console log on
  this bench. That also refutes #192's leading untested guess.
- **The optimization works.** `pushCollect`, n=2 per arm from porta's own store, no overlap:
  **-21.5%** with `sizeOf:`. This is not a fix that does nothing and breaks something.
- **Near-miss.** Boots 40 and 43 are lockstep at seq 7 and diverge by 9072 B of `lb` by seq 15,
  which reads as a clean inversion of the commit's premise. Boot 41 — the *other* hung arm —
  reads *below* the healthy sample. `lb` at a fixed seq varies ~11 KB boot to boot. One extra
  sample was the whole difference between a finding and an artifact.

**What this exposed is bigger than #161.** The expensive part of #192 and #213 is not that they
are hard; it is that **a node has no way to say it stopped.** `docs/design/2026-08-10-node-memory-
and-liveness-design.md` §5 already designs the reporting path and RECORDS §2.4 already pins
`Panic.reason: #watchdog` — but §5's seam is `k_sys_fatal_error_handler`, and a wedge raises no
fatal error, so that symbol is unreachable. Detection is now designed
(`docs/design/2026-08-16-vm-liveness-observer-design.md`, **#215**), with §12 of
the 2026-08-10 doc amended to point at it. **Built, and green on hardware 2026-08-21** — see
the #215 entry below; §11 of that design is what the bench said.

Three new items, in the order they gate each other. **The first two CLOSED on 2026-08-17 and
the third is approved and next** — kept here in order because the order is the argument:

- **#214 — DONE (80fc57a).** `pulse_pump` discarded a stale-handle event with **no counter**;
  a lost wakeup is the failure shape under investigation and `pdr` read 0 either way. Now
  counted and published as `Metrics.pst`, **verified on hardware** — `#pst: 0` from c6spare
  boot 44, with `#pco: 1` beside it proving the seam was live rather than unreached. The
  zero is the deliverable: "a timer cannot take that branch since #151" was unfalsifiable
  while the branch was uninstrumented, and is now measured. `Metrics` 101 → 108 B, frame
  378 → 385 B, 127 B of headroom left.
- **#216 — DONE (071b319, fix 2537992).** A gate that passes at tick 2 and lets the node die
  at tick 6 is a false green. A scenario is now watched past its own verdict on two
  independent signals — a POSITIVE progress predicate (the silent deaths match no fault
  grep) and a fault scan — with both manifest keys mandatory. **TARGET 3/3 PASS**, the
  predicate live across 18 consecutive polls, no false STALL on a healthy node. Its own
  first hardware run caught it claiming "still progressing" for the two scenarios that
  declare `none`; the six hostless tests missed that because every one asserted exit status
  and the FAILURE text and **none read the success message**.
- **#215 — DONE 2026-08-21, 5/5 on hardware, and the §10 promotion is landed** — the design
  docs, RECORDS §2.4 and the CLAUDE.md gate note all carry it, so
  `docs/superpowers/plans/2026-08-19-vm-liveness-observer.md` is scaffolding from here.
  **The one thing outstanding is David's**: the ladder entry in `review/program-sequence.md`
  and the three `wedge-*` rows in `review/tests/CLAIMS.md`, both drafted on #215.
  **2026-08-10 §7's staging is now complete — step 4 was the last one open.** The detector. Branch `215-vm-liveness-observer`, commits
  `722cd34..e997fc9`; plan `docs/superpowers/plans/2026-08-19-vm-liveness-observer.md`;
  what the bench said is §11 of the design doc.

  **The number that matters is `deadline`, because §2's whole argument is that it must not
  be a constant:** the detector fired at **8000 ms** against a probe's declared 1 s timer
  and at **16000 ms** against the bridge's 2 s cadence — 8 x the app's OWN period both
  times — catching each 8 ms and 16 ms past it, on the very next observer tick. `wedge-note`
  2/2 (the note crossed a reset with `up` unchanged), `wedge-detect` 2/2, `wedge-quiet` 1/1
  twice (a 10-minute timer fired at 599.99 s with zero trips). Rows 2 and 4 — the armed
  soak — closed 2026-08-21 by a **22 h 23 min soak on one unbroken boot** (tick 40 284, zero
  `WEDGE` lines, one reset and it was the flash's own). #222 no longer blocks anything.

  **Three things the tree forced that the approved design could not have known, all since
  confirmed on silicon**: `task_wdt_feed()` cannot run in ISR context and `CONFIG_ASSERT`
  is off so it would have failed silently, so the supervisor is a cooperative-priority
  THREAD and `task_wdt` sits behind it; `struct arch_esf` on this SoC has no `mcause` and
  it comes from the CSR instead; and **a HALT that leaves the hardware watchdog armed is a
  reboot loop wearing a halt's costume** — the node held halted 4 min, ~22x the 11 s
  window, only because `observer_halt()` disables it first.

  **#222 IS FIXED (2026-08-20), and it was #192 and #213 all along — one bug, three issue
  numbers.** *(Addendum 2026-08-25: the family has a FOURTH face — #206's replayed-`dbg/req`
  deaths, closed by faithfully rebuilding the 2026-08-12 store condition against a scratch
  porta and watching two boots sail through the exact tick-31 death coordinates on a current
  build. `profileWindowEnd` is a bundle of first-time selector sends, so a window ending
  after the ~17 s cache fill hit the unbounded probe and one ending before it did not —
  which is why 10 s survived and 60 s died. Probably #180's ~5 s face too; both issues carry
  the caveats. The replay-re-runs-the-window residue is #262.)* Root cause: `selector_prim_id()` in `nodus/oevm/src/dispatch.c`, an
  open-addressed selector→prim-id cache consulted on EVERY send whose selector is a Symbol,
  which never evicts, never grows and probed `while (slot != NULL)` **with no bound**. Full
  table -> no NULL to stop at -> the next uncached selector spins forever in C, below the
  interpreter, at 100% CPU, with no fault and no output while the pulse mailbox fills. That
  is the `cond=pulse-undrained pending=1` signature exactly. **`PRIM_CACHE_SIZE` is 256 on
  the C6 and 4096 on the host, and that one number is why every one of these deaths was
  target-only while the nsl suite read 265/0** — no host suite has ever filled the table.
  Proved on c6spare by a forensics dump (`prim_cache=256/256`, main `queued` not `pending`,
  1.61M cycles in 100 ms against every other thread's 0, and zero sends completing across 24
  samples). Fixed in `2461d42` by bounding the probe; `tests/test_prim_cache.c` reproduces
  the hang on the HOST for the first time via `-DPRIM_CACHE_SIZE=8`. Two refuted beliefs
  worth keeping: the reactor is NOT parked in `pumpEvents`'s K_FOREVER (#192, #213 and
  observer.c's own header all said so — a thread waiting in `k_msgq_get` is handed a put
  directly, so `pending=1` means nobody is waiting), and #192's VM-stack-exhaustion guess is
  refuted at `vm_sp=142` of `cap=4096`. Follow-on **#226 is BUILT AND TARGET-GREEN (branch
  `226-prim-id-on-symbol`) and DELETES the cache rather than sizing it**: a
  Symbol carries its own `prim_id`, baked by both image builders from the one list in
  `src/prim_names.def`, so there is no table to fill, no probe to bound and no host/target
  size divergence. It costs no image growth — `length` narrowed from a word to 16 bits pays
  for the field with every offset unchanged. Decision record:
  `docs/design/2026-08-20-prim-id-on-symbol-design.md`. It could not build for the C6 until
  `layout-esp32c6.json` was regenerated by flashing `zephyr/layoutdump/` — refused by name,
  deliberately, because narrowing in place is invisible to the existing offset==size guard.
  **That flash happened 2026-08-21 and the refusal cleared.**
  **#226 is now TARGET-GREEN on the C6 (2026-08-21).** `layout-esp32c6.json` was regenerated
  from a `layoutdump` flash on c6spare — a five-line diff, `Symbol.prim_id` at offset 6, every
  other size and offset byte-identical — and `tests/target/run.sh bridge-health`, the #222
  reproducer, went **PASS 3/3, observed 180 s past the verdict still progressing, 206 s**. The
  regeneration also caught what no host gate can: `3ddda63` deleted the `PRIM_CACHE_SIZE`
  blocks and left both `endif()`s, so `zephyr/CMakeLists.txt` had stopped parsing while the
  host suite read 262/0 green (fixed, `208f28d`). Branch is ready to merge; #222/#192/#213
  close with it.

  **The close-out landed 2026-08-21.** §8's constants table gained `wedge slack` / `observer
  tick` / `observer backstop` with their standing; §7's step 4 reads DONE; §5.1's `RebootNote`
  matches what shipped; D-P1…D-P6 moved out of the plan into §11 of the 2026-08-16 design, with
  §6 flagged superseded so nobody goes looking for `src/backend/zephyr/observer.c`; RECORDS §2.4
  names what produces `#watchdog`; and CLAUDE.md now lists all seven target scenarios with their
  class and cost. **Still David's to place:** the ladder entry and the `wedge-*` CLAIMS rows,
  both drafted on #215.

  **The close-out's own gate found one thing, which is why it was run rather than waved
  through: `TARGET 6/7`, and the red was not a VM defect (#230).** `stale-handle` passed both
  TAPs, printed its done line, and was then reported dead — it arms a 25 ms heartbeat repeater
  it never cancels, so `main:` returning leaves a live repeater nobody drains, and `8 × 25 ms`
  is the `deadline=200ms` in the dump. The detector is right; the probe was not. Wedge →
  reboot → wedge → `consec=3` → HALT, so a probe finishing correctly was reported as a death
  three times and parked the board. Fixed in two lines (`timers cancel:` the heartbeat), and
  **stale-handle is now PASS 2/2 with the detector armed** — the run stands at **7/7**. The
  rule it leaves behind is in the harness design doc: *a workload that ends must first withdraw
  every expectation it declared.* **That is the third time the instrument and the harness have
  disagreed about what a stopped node means** (#216, #223, #230), and nothing yet asserts the
  rule — the other four probes comply by luck of how they were written.

  **#199 — HALF FIXED 2026-08-21 (`196ff1c`), and the issue stays open on purpose.** A refused
  node exhausted its arena in 2 min 37 s while `misses 0` read healthy the whole way. **The
  issue's own hypothesis was wrong**: nothing accumulates. Reproduced on the host with the arena
  pinned to the C6's 131 072 B and porta answering `#err` to everything — live bytes SAWTOOTH in
  both arms (77 KB → 123 KB → 77 KB) and the push frame is a constant 493 B, so the trim holds.
  What differs is the allocation **rate**: with `acked` pinned at 0, `collectOutbound` re-scans
  and `valueFits:` re-encodes every candidate every tick, giving **1.82 GC cycles/tick against a
  healthy node's 0.88** and collapsing the largest free block from ~16.8 KB to ~2.9 KB. The C6
  died on a 4 104 B request; a refused node cannot satisfy that on **74% of its ticks** against a
  healthy node's 10%. Not unlucky. Fixed by keeping the frame the gateway did not accept —
  `checkIn`'s own comment already promised it "re-pushes the same entries" and simply rebuilt
  them from scratch to keep the promise — with `acked` advancing as the whole invalidation rule.
  **1.82 → 0.42 GC cycles/tick**, below even the control. A refusal is also finally sayable:
  counted, published as `obs/health #ref` (live in the gateway's store beside `#pto: 0`), and the
  gateway's own message printed ONCE per reason. Health 61 → 72 B, frame 385 → 396 B, 116 B
  headroom. **What is NOT fixed: the fragmentation** — still under 4 104 B on 72% of ticks — and
  **the host cannot answer whether the C6 still dies**, because it never reproduced the death
  (300 s clean where the board died in 157 s). **#232** is the gate that can: a C6 against a
  refusing gateway, watched past 157 s. Improvement is not a fix.

  **#231 — the liveness contract, design APPROVED and PLANNED 2026-08-21; EXECUTION IS NEXT.**
  `docs/design/2026-08-21-liveness-contract-design.md` (`3804c62`). Six open problems are one
  defect: **something in this system judges a node against a number the node never declared.**
  #215 solved that for the reactor by not estimating; this extends it across the wire — a node
  declares a **silence budget** and both its own detector and the gateway's sweep judge against
  that one number. Four decisions: the **kernel owns the heartbeat and it rides the reactor**
  (not C — a heartbeat a wedged node can still emit is a lie, and #215's observer already
  supplies the deadline from `target_timers[]`, so one `timers every:` feeds both consumers with
  **no new C**); the node declares `obs/liveness` and the operator **requests** `goal/liveness`,
  so the convergence view already answers "did it accept?"; **one base tick, all periods integer
  multiples**, so the radio starts and stops once; and a **24 h ceiling on the declaration**.
  **It deletes more than it adds** — D4 reverses 2026-08-10 D8, removing `k = 3`, the 10-gap
  inference window, the min-3-exchanges rule, `CadenceMs` and #217's `leg` bookkeeping, and
  **#219 and #220 close with them**; #152 and the gateway half of #225 are answered by D6. Two
  paths, no tables, no verbs. Staging puts the board-dependent kernel heartbeat **last**, behind
  three host-testable steps. **The plan is
  `docs/superpowers/plans/2026-08-21-liveness-contract.md` (`5ab67a3`), ten tasks.** Writing it
  forced three decisions the design had left open — two compiled-in
  node defaults (`cad` 30 s, `bud` 8 x cad), `goal/liveness`'s cadence half deferred to the
  kernel-tick task rather than published unhonoured, and `gw_runs` as an argued exception to
  "a feature adds rows, not tables". All three are §9 of the design record and open to reversal.

  **What landed (Tasks 1-9, `2cce318`..`2a8a91f`).** The node declares `obs/liveness` once per
  boot — 37 B measured at the worst legal values, pinned in the record catalogue — from an app
  declaration snapped onto its own grid and clamped to 24 h; four bridged apps declare their tick
  period and `ShipsBellNodeC6` declares nothing because it builds no bridge at all. porta reads
  the declaration and judges `now - last_seen > bud`, one line with no constants in it; the whole
  inference is deleted (`CadenceMs`, the 10-gap window, the min-3 rule, `k = 3`,
  `nodes.cadence_ms`, `exchanges.leg`, and `internal/wire/cadence_test.go` with them), migrated
  in place so the soak store survives. Four verdicts, not two. `goal/liveness` is a *request*:
  ask 45 s of a 2 s node and 46 s comes back, and the host gate witnesses exactly that alongside
  the declaration and the verdict — all three legs falsified before being trusted. **#225's
  gateway half is fixed and hand-proven with a real node**: porta records one row per run in
  `gw_runs`, and a 20 s outage against a 16 s budget now reads `#unknown` where 2026-08-19 read
  `#live` for ten hours. Host evidence only — 268/0 on the nsl suite, `go test ./...` green,
  `PORTA SYNC OK` — and the host suite has been wrong before (a pure codec change once passed
  265/0 and hung a C6 in six check-ins).

  **Task 10's host half also landed (`b728260`): the kernel owns the base tick.**
  `PortaBridge>>startTicking:on:` registers the period once per boot on the Timers instance it is
  handed, the four bridged apps hand theirs over, and `TimerlessNodeC6.nsl` — a node whose app
  declares **no** repeating timer, the shape #215's detector could not see at all — drives the new
  `liveness-heartbeat` target scenario. `run.sh` discovers it without a registration edit; the nsl
  suite footer now says **8 scenarios** by itself.

  **THE BOARD WORK IS DONE, 2026-08-22 on `c6spare` at HEAD.** All three scenarios #231 named
  as its closing condition passed, each watched past its own verdict:

  | scenario | verdict | time | build |
  |---|---|---|---|
  | `liveness-heartbeat` | PASS 2/2 | 267 s | `porta-v1-final-1073-gb193e08` |
  | `wedge-quiet` | PASS 1/1 | 623 s | `porta-v1-final-1073-gb193e08` |
  | `bridge-health` | PASS 3/3 | 221 s | `porta-v1-final-1076-g4a7526b` |

  `stale-handle` PASS 2/2 (203 s) was run after them as an unrelated #151 regression and is
  also green. The two build ids differ because host-side commits landed between runs; neither
  touched VM or kernel source, so the images differ only in their baked id — said plainly
  rather than rounded to "at HEAD", because a gate table that blurs which image it scored is
  the shape #216 is about.

  **And the bell now runs this code too.** `ShipsBellAlarmNodeC6`'s kernel-owned tick had been
  compiled but never executed on hardware, because the bell could not be reflashed mid-soak.
  It was reflashed to HEAD on 2026-08-22 and declares `obs/liveness{bud: 240000, cad: 30000}`
  on the wire — its own 30 s cadence times the slack of 8 — with the observer armed
  (`slack=8 tick=1000ms backstop=30000ms hw=yes`). That was the last item in this rung
  carrying risk.

  **What is still NOT built.** D3's **coalescing** is not gated by anything (it needs a second
  obligation and a wake counter — #240 item 1, still open). `goal/liveness`'s *cadence* half
  IS honoured since 2026-08-26 (#240 item 2): `adoptCad:bud:` — the one adoption point —
  re-arms the kernel tick when the cadence moves, `applyLivenessGoal` reads the record's
  `cad` like its `bud`, and the re-arm's mailbox seam (the failure no host run reaches) is
  gated by the NINTH target scenario, `cad-rearm` (`RearmProbeC6.nsl`): the probe adopts
  5000 ms from INSIDE the 2000 ms tick's own reaction — the real shape, since
  applyLivenessGoal runs inside the tick's reaction chain — and certifies the old
  registration dead, one registration throughout, and three kept new-period gaps. Host twin:
  `22_spacesync/23_liveness_cad_rearm.nsl`.

  Also **#223 DONE and promoted** (`8167463`: the observe window no longer reaches its verdict
  18 s before the evidence, and a wedge the detector caught is reported as such; the rule —
  *a window that can out-race the instrument it is watching must either wait for it or decline
  to describe what it did not see* — is now an addendum on
  `docs/design/2026-08-05-target-test-harness-design.md`), and two findings filed:
  **#227** (the Xtensa layout describes a VM that no longer exists and no board here can
  regenerate it) and **#228** (`test_oimg_serialize.py` is red at HEAD and no gate runs it).
  **Rows 2 and 4 of #215's §8 are closed — see above.**

  The A/B that got there is below, kept because its first reading was wrong. *(Superseded
  history:* The armed bridge wedges at ~31 s on every boot where the pre-#215 build ran
  **21.4 h clean on the same board**. Eight arms on c6spare that morning (full results in
  the issue) exonerate the `sys/panic` publish, porta's closed decoder and
  `panicNoteAck:` — the node dies with all three commented out, calling nothing but prim
  153 — and refute the issue's own predicted branch, which was unfalsifiable because
  removing `self reportPanicOnce.` removes the C-side READ along with the publish. It is
  also not the arena: `gcc: 0` and 59 KB of contiguous free space at the moment of death.
  Two minimal arms pass and the two that restore the ordinary nsl on top of them fail, so
  the shape is a **threshold in how much of the report path runs at tick 1**, not one bad
  line. That is #192's shape (a small nsl wrapper on the check-in path, hanging a C6 at
  tick 2-3, mechanism unknown) and #213's signature, and the resemblance is now evidence
  rather than coincidence: **work #222, #192 and #213 as one bug**, at the `pulse-undrained`
  seam (`pending=1`, `pdr=0` — a wakeup the VM never came back for).*) Other
  follow-ons: **#223** (the observe window's 10 s stall poll is shorter than the 16 s wedge
  deadline, so the harness beats the detector to the verdict and its message says "no
  fault, no reboot" 18 s before the dump lands) and **#224** (`monoMs`/`obs/health #up` are
  SMIs, so uptime wraps silently at ~12.4 days — pre-existing).

  The branch also **re-banks the T0.3 footprint ratchet**, absorbing +725 B for this rung
  and **+887 B of drift since the 2026-08-08 bank** that had already spent 75% of the 2%
  band; the split is in `nodus/oevm/tools/footprint-baseline.txt`'s own header. No engine
  `.c` file was added, so none of #63's nine source lists changed.
- **#217 — the gateway half. DONE 2026-08-18, verified on gw85224-01.** `gw/**` + the
  silence sweep: a fourth goroutine in `porta serve` that infers each node's cadence from its
  own exchange history and writes `gw/liveness` **on transition only**. Cadence came out
  **exact** on both live nodes — 30000 ms for the bell, 2000 ms for the bridge — and a node
  halted by hand flipped to `#silent` in **25.9 s against the 36 s bound**, in exactly one row
  held across seven sweep ticks. Two findings the host suite could not have produced: the
  cadence query was reading **half** every node's real period (a check-in writes two
  `exchanges` rows and `schema.go`'s comment said one), and a *fast* node's `3 × cadence`
  deadline is tight enough that porta's own replayed `dbg/req profile` stalled a healthy node
  past it — recorded as I6 and filed as **#220**, not fixed. Other follow-ons: **#218** (a node
  pushing over its own verdict), **#219** (a node already dead at migration time is never
  judged) and **#221** (`porta get`/`log`/`put` CREATE an empty store on a wrong `--db` and
  answer from it, so an empty tree read cannot be told from a wrong database) and **#225**
  (the sweep cannot record its OWN absence — the 2026-08-19 power failure took the gateway
  down for 10 h 6 min while `gw/liveness` went on reading `#live` for a node whose every
  check-in was missing, because a verdict written on transition only freezes when the
  gateway that writes it is dead). It is not
  adjacent to #215 but underneath it — §7's staging
  gates the panic-note/reboot/loop-breaker step on this one already watching, because "three
  faults halt the node" is unobservable from the gateway if a halted node is invisible.
  Note the shared principle: the sweep's deadline comes from the node's OWN measured cadence,
  never a configured constant — the same rule #215 §3 settles on for the detector.

**BENCH STATE, 2026-08-22 — all three boards on ONE build, each in its OWN porta space.**

```
board     porta space   build
c6 (bell) c6            porta-v1-final-1081-gd5b5545-20260822T163623   #live
c6spare   c6spare       porta-v1-final-1081-gd5b5545-20260822T164025   #live
c6bench   c6bench       porta-v1-final-1081-gd5b5545-20260822T164608   #live
```

That is the first time the three have shared a build **and** been separable in the record.
Until #186 landed the same afternoon, `PortaBridgeNodeC6` answered the literal `'c6-bench'`
and `ProfileCostNodeC6` answered it too, so a space held two boards and two apps at once.

- **`c6` (the bell)** was reflashed TWICE on 2026-08-22: first to `gb193e08` (retiring a build
  72 commits behind that predated `2461d42`, the selector-prim-cache fix), then to
  `gd5b5545` to pick up #186. It checks in as **`c6`** now, not `c6-bell`.
- **`c6spare`** carries `PortaBridgeNodeC6` from a `bridge-health` run that passed 3/3.
  Still `TARGET_DEFAULT_BOARD`.
- **`c6bench` lives on gw85224-01** and is flashed from here over ssh (build local, `scp
  zephyr.bin`, `esptool write-flash … 0x0`, hash verified) — #181's remote deploy path
  working by hand.

**THE OLD SPACES ARE HISTORY, NOT MIGRATED.** `c6-bell` and `c6-bench` stop receiving
writes as of 2026-08-22. Their rows are intact and still readable; nothing was re-keyed,
because #186 warns that repointing a space holding five days of history makes the old rows
unattributable in a *different* direction. There is no back-compat requirement, so this is
a cut-over with a date, and the date is the thing to search on.

**Every node needs a `goal/name`** — written 2026-08-22 for `c6`, `c6spare` and `c6bench`.
`bridge-health` asserts convergence of `goal/name` -> `obs/name`, and its `.env` no longer
names a literal board.

**The JTAG cables are unplugged, and that is now the convention.** Each C6 devkit enumerates
*two* host adapters — a CP2102N and the SoC's USB-Serial-JTAG — and the bench had been spending
two ports per board. **The target harness only ever needed one**: `deploy_flash.sh:53` sets
`ESPTOOL_PORT` *and* `CONSOLE_PORT` to the single CP2102N resolved from the collector env, and
`flash-bell.sh` pins the same one deliberately ("The CP2102N, not the USB-JTAG. The JTAG path is
the one that failed and parked the bell in 2026-08-11"). Only the hand-driven `zephyr/run.sh`
used the JTAG node. So the fix for port scarcity was one port per board, not OTA and not a hub.

**CONSEQUENCE, not yet addressed:** `zephyr/run.sh`'s own C6 flash path resolves
`/dev/serial/by-id/*Espressif*`, which now matches nothing. `flash-bell.sh` is unaffected because
it pins `ESPTOOL_PORT` explicitly, and the harness is unaffected for the same reason — but a bare
hand-driven `run.sh --board esp32c6 --flash` can no longer find a port on this bench.

Bench: c6spare was left soaking on `ga4e6bc2-dirty-20260816T151140` (the `38505d4`-only
build) and reached **~25.4 h / tick 45 695 on one unbroken boot**, `pdr`/`sdr` 0 and no
allocation ever returning null across 16 639 collections. **That control was then SPENT on
2026-08-17**: the board was reflashed for the target gates, and it then carried
`StaleHandleProbeC6` from the last scenario of that run. **As of 2026-08-18 it carries
`PortaBridgeNodeC6` again** (build `porta-v1-final-1026-gb8157ea-20260818T184600`), reflashed
to give #217's falsification a disposable node to halt; it is live in the space as `c6-bench`
at a 2 s cadence. Hours 26+ bought nothing for the
question it served — 25 h is ~7 600x past the longest death it controlled for, and its build
had fallen behind HEAD. `c6-bell` remains the long-duration board. The bell was not touched.

**One field name worth pinning, because it was misread inside this session.** In `Metrics`,
**`hnl` is `handlesLive` and `hnhw` is `handlesHighWater`** (RECORDS §4.4 gives the long
names in order). The bell's steady `hnl: 1, hnhw: 2` is one live M6 handle and a high water
of two — entirely ordinary, and NOT a null allocation or any #190 signal. The C-heap
failure counters live in the separate `CHeap` record as `fal`.

---

**2026-08-26 session close.** #247 closed at `1394c9f` (compiler positions, DNU naming incl.
app classes via the .oimg v10 classnames section; remainder → #265). Gatekeeper exists as a
process (`4e1469d`, gw85224-01:6971): the dev status page + docs tree at `/dev`, `/health` +
`/manifest` proxied to porta — the shell only; the query surface is the 2026-08-22 first-cut
design's build, and **that note ratified D3=(b) on 2026-08-22** — #198's "ratify D3 first"
body is stale, corrected by comment. The doit is filed (#266, hard-gated on #119); the
four-capability frame (observe/probe/replay/bounded-act) is recorded in
`review/WHY-AGENTS.md`. #201/#202 deferred while the bell is piezo. **Queued next: the
space-sync durability trio #114/#117/#119** — one rung, NVS-backed cursor/bootId + replay
guard, design record first; then #232 as the next board session (also .oimg v10's first
on-hardware boot). Live status view: `http://gw85224-01:6971/` (refresh = re-scp
`docs/status/dev-status.html`, never a restart).

**2026-08-26 session close (second session).** The durability trio landed and pushed
(`b8ab562..4ebcdda`, ten commits incl. the design record `5a9e33d`): **#117, #119 and #262
are CLOSED.** What the rung actually was: bootId NVS had already landed (#162/#215) and the
cell half of replay was already latest-value, so the build reduced to the porta-side stream
guard — `nodes.cursor_hw`, an acked-as-applied high-water bounded by `served_hw` (what porta
actually delivered), plus a pull door that REFUSES a cursor beyond the store's out-seq
(`#err`, visible in `obs/health.ref`). The node's cursor is **deliberately RAM** (design D3;
the NVS-key half of the queue line was dropped on purpose — persistence costs a stranding
mode, and the guard makes cursor-0 safe). The rung's final review caught the approved D2
clamp CREEPING (out_seq-bounded receipts marked never-served rows applied); D2 is corrected
in the design record, and PROTOCOL-NSL §1/§3/§4.1/§5.1/§7 + RECORDS §4.4/§4.5 + CLAUDE.md
now all agree. Design record:
`porta/docs/design/2026-08-26-space-sync-durability-trio-design.md`. **#114 is DECIDED, not
closed** (must-deliver = re-offered-until-acked from a durable source; sys/panic's source is
the RebootNote) — its implementation plus two node-side riders (stale PortaBridge header
wording; shed/counted-drop entries are covered by the receipt without executing) ride
**#232, still the next board session**. #266's hard gate is now released. **Deploy note:**
the new porta binary migrates the live store (adds `cursor_hw`/`served_hw`) on first open —
a restart costs nodes an outage, so ship it at a deliberate moment; a node that was down
across the upgrade replays its old streams ONCE more on its first cold boot before the
guard latches (expected, not a failed fix).

**2026-08-26 session close (third session — the #232 board session).** **#232, #199 and
#114 are CLOSED.** The `porta-refuse` scenario exists (tenth target scenario, injector =
`porta serve --test-refuse-all`, supported) and is GREEN on c6spare with the full 240 s
window: tick 150, `misses 0 ref 150`, 300 s of continuous refusal against the 157 s death
record, no fault — #199's fix is verified, not inferred, and corroborated from the
injector's own log (313 refusals). Also .oimg v10's first on-hardware boot. #114 landed as
`PortaBridge>>offerPanic` (ack on porta's receipt, never on hope) — and moving it found
that **nothing had ever subscribed sys/panic: must-deliver had never once delivered**;
the bridge now owns the subscription, and `porta_sync_host.sh` leg 9 (new) asserts a
planted note ARRIVES as a porta row. Both riders landed: the PortaBridge header wording,
and `obs/health.rej` (a counted inbound drop is permanent under the replay guard, so it is
visible now — manifest/goldens/batch-guard updated, Health 83 B, frame 407 B). **The
pending gateway deploy happened**: the durability-trio + refuse + rej binary is live on
gw85224-01 (`porta.pre232` beside it, manifest sha `7556b7fa` re-armed, ~45 s outage,
nodes recovered; old 6-field Health from un-reflashed nodes is accepted). Host gates at
close: nsl 278/0 all aux green, go test green, PORTA SYNC OK 9/9. c6spare left running
bridge-health's image against the live gateway. **Queued next: #265 (frozen runtime
errors + G4 line-table) or the #198 gatekeeper five-question spike — both unblocked;
board time owes nothing.**

---

## ADDENDUM 2026-08-27 — the wedge detector was rebooting a healthy node (rete#269)

**Supersedes an earlier addendum of the same date**, which said a node could not hold its
declared cadence under silence and named `cad > 2 x replyBudget` as the law. The control run
refuted that premise instead of completing it. Corrected here rather than left beside the
correction. Full record: the 2026-08-27 addendum to
`docs/design/2026-08-21-liveness-contract-design.md`.

**No node was failing.** `observer.c`'s `pulse-undrained` condition judged the mailbox's
**depth at a sampling instant**, which aliases. At `cad: 2000` under silence the tick costs
~4 s of `awaitReply` spin, so the mailbox empties for ~1 s inside every 4.007 s tick — but
the observer samples every 1001 ms, four samples span 4004 ms against a 4007 ms cycle, and
that 3 ms beat walks the one sample that catches the empty window out of it. `held` then runs
free to exactly `8 x cad`. It also explains the ~40% variance in time-to-death that no
allocation story ever accounted for: it depends where the beat starts.

**Proven both ways on c6spare.** Through the 16 s the detector called undrained, `drained`
climbed **35 -> 39** and the console printed **ticks 34-38**. Through a real wedge
(`wedge-detect`), `drained` sat **frozen at 30**. `pending` reads `1` in both, so no dump
ever taken can separate them on its own evidence.

**Fix: progress, not depth.** `pulse_drained()` — a monotonic count of items the pump
delivered — added to the seam and both backends; the observer resets when the mailbox is
empty **or** that counter moved. A counter integrates across the interval, so it cannot
alias, and a stuck VM never moves it. No new tunable. `porta-silent` **PASS 2/2** (357 s
alive under the condition that killed it at 111.7 s, zero WEDGE lines) and `wedge-detect`
**PASS 2/2** still firing. Host: nsl 278/0 all aux green, `go test` green, `PORTA SYNC OK`.

**Dropped:** the `replyBudget` derivation. It targeted a mechanism that does not exist.
**Kept, as a smaller real defect:** a node under silence delivers 4.007 s while declaring
`cad: 2000`, so `obs/liveness` misreports it to porta's `gw/liveness` verdict — an honesty
bug against D2, wanting its own issue. **Kept as data:** the control arm at `cad: 30000` held
**mean 30.000 s, sd 0.089, n=34** over 1050 s against the same silent injector.

**Opened: rete#270.** Because `pending` is uninformative, every reboot attributed to a node
wedge since #215 is unverified — 58 `deadline=16000ms` events in the console archive, none in
a log that also carries an OOM. Not affected: #199 (a real arena death, `requested 4104`, in
logs with no wedge) and #232's refusing path (returns on the first datagram, never
saturates).

**Next, in order:** (1) rete#270's sweep of the 58 events; (2) the cadence-honesty issue
above; (3) whether `wedge-quiet`'s premise needs strengthening — it gates a node with a slow
*declared* period and never covered one whose *delivered* period drifts, which is the case
that bit.

Bench note that cost this session's forensics: the `RebootNote` is in `.rtc_noinit`
(`reboot_note.c:29`) — it survives a **reset**, NOT a power cycle.
