# MagTag deep-sleep ship's-bell clock — design (tuvm#94 stage 5)

**Date:** 2026-07-24
**Status:** designed, not built. Plan + implementation in a fresh session.
**Branch:** `feat/magtag-esp32s2-port`.
**Predecessor:** stages 1–4 (mains-powered reactor, `zephyr/MagTagClockNode.nsl`) are
shipped and witnessed. This stage is a re-architecture of the same appliance for a
mostly-asleep power profile, not an increment on the reactor.

---

## 1. Why this is a re-architecture, not a feature

The stages 1–4 clock is a **reactor**: one VM boots and drains an event loop forever;
timers fire strikes and syncs inside that single lifetime, and all state lives in RAM.
Deep sleep is incompatible with that model at the root — **between wakes the CPU is off
and the VM does not exist.** The clock becomes a **run-once cycle**:

```
wake → read RTC state → decide → act → write RTC state → arm timer wake → deep sleep
```

Each wake is a fresh VM boot that lives a few seconds and dies. There is no drain loop;
`deepSleep:` *is* the idle point. The node collapses from a reactor to a **straight-line
script** — no channels, no VM timers, no `whileTrue: [pumpEvents. drain]`. This also
*removes* the entire class of bug that cost the overnight run in stage 2 (a timer
registered on one `Timers` instance and dispatched on another): there are no VM timers
left to misregister.

Two wake kinds:

- **Strike wake** — every 30 min. Read state, strike the bell, write state, sleep.
  No WiFi, no e-paper. A few seconds awake. This is the overwhelming majority of wakes.
- **Watch-end wake** — every 4 h, at the six "eight bells" moments
  (00:00, 04:00, 08:00, 12:00, 16:00, 20:00). Everything a strike wake does, **plus**
  bring up WiFi, sync NTP, compute drift, and repaint the panel — **in that order, so a
  sync failure is reported on the glass immediately.**

### The timebase changes underneath — the real risk

Awake, strikes ride the 40 MHz crystal (10–50 ppm, the basis for stage 4's "±1 s/day is
fine"). **Asleep, sleep durations are counted by the RTC slow clock, which on this part
defaults to an internal ~90 kHz RC oscillator** (`SOC_CLK_RC_SLOW_FREQ_APPROX 90000`).
`esp32s2_wrover_n4r2.dtsi` declares no 32.768 kHz crystal, so `XTAL32K` — though the
silicon supports it — is not available on this module. RC oscillators drift with
temperature and voltage. ESP-IDF calibrates the RC against the main crystal at sleep
entry, so the *commanded* duration is compensated; what remains is drift *during* the
sleep, which is **unmeasured on this hardware**. At 100 ppm a day is 8.6 s; at 1 % it is
14 min. Syncing every 4 h rather than daily keeps that drift both **bounded** (never more
than 4 h stale) and, more importantly, **measured** — each watch-end sync yields a real
drift number the panel displays.

---

## 2. The ratchet — never bell behind, only ahead

The double-strike hazard is concrete and must be designed out. It does **not** come from
the ~2 s of wake work; it comes from **a sync correcting the count backward.** Scenario:
the RC oscillator runs fast, so the board wakes for the 04:00 strike while true time is
03:59:57. It strikes 8 bells (early). Then, because 04:00 is a watch-end, it syncs — and
NTP says "actually it's still 03:59." A naive design sets the count back; the *real* 04:00
boundary arrives 3 s later and it strikes 8 bells **again**.

**The rule (the invariant the whole design rests on):**

> The half-hour index is a **monotonic counter that only ever increments.** A sync may
> push it *forward* (to skip strikes a dead battery or failed wake missed) but may
> **never** move it backward. When NTP says we are behind where we have already struck,
> we do **not** re-strike — we hold the count and lengthen the next sleep so real time
> catches up. Drift never touches the count; it only ever adjusts the **sleep duration**
> (phase). The count is a ratchet.

Mechanically:

- **Strike:** `target = lastStruckHalf + 1; strike target; lastStruckHalf = target`.
  A pure increment — a strike wake **cannot** double-bell by construction.
- **Sync correction:** compute `trueHalf = floor(trueLocalSec / 1800)`.
  - `trueHalf > lastStruckHalf` → we fell behind (missed wakes, or the clock ran slow):
    advance `lastStruckHalf = trueHalf` **silently** — do not ring the skipped bells
    (nobody wants five bells at 6 am). Re-phase.
  - `trueHalf ≤ lastStruckHalf` → the clock ran fast, we are ahead of real time: **leave
    the count alone** and re-aim the next sleep at the true boundary of
    `lastStruckHalf + 1`. Never re-strike.

---

## 3. Retained state (`.rtc.data`)

The S2 linker provides an 8 KB `rtc_slow_seg` (0x50000000) and a `.rtc.data` section that
survive deep sleep. State is a fixed struct there, read at the top of every wake and
written before sleep, through a new prim seam. **Every field fits a 30-bit SMI**
(seconds-of-day ≤ 86399; 4 h commanded ≤ 14400; days of staleness ≤ ~10^6; index ≤ 47),
which is what makes the seam an indexed SMI get/put rather than a struct marshalling
problem — and it sidesteps the single-class-node limitation that `Array`/`basicNew:` are
unreachable from application code.

| # | field | purpose |
|---|---|---|
| 0 | `magic` | validity guard. `.rtc.data` is **undefined** (not zeroed) on cold power-on, so a mismatch means "no valid state" → free-run from 00:00 and sync. One `uint32` compare, one branch. |
| 1 | `lastStruckHalf` | **the ratchet** — index of the last half-hour struck; only increments. |
| 2 | `bootTicks` | strikes since cold boot; drives `up` (wall-clock since boot, spanning sleeps). |
| 3 | `anchorLocalSec` | true seconds-since-local-midnight at the last good sync — the wall-clock anchor. |
| 4 | `elapsedCommandedSec` | Σ commanded sleep durations since the anchor. `predictedNow = (anchor + elapsed) mod 86400`. |
| 5 | `driftPpm` | last measured drift rate, signed. Displayed; available to pre-scale future sleeps (not required for correctness). |
| 6 | `staleSec` | seconds since the last *good* sync (accumulated by sleeps, zeroed on success) → the `sync 0d/14h` row. |
| 7 | `drift24Max` | rolling max measured drift over the last 6 syncs (24 h) → `24h` row. |
| 8 | `drift24Min` | rolling min over the same window → the row below `24h`. |
| 9 | `syncsInWindow` | count of syncs in the current 24 h window (0–5); resets max/min at 6 so the window rolls. |

Two quantities are derived each wake:

- **Next sleep duration** = `nextBoundarySec − predictedNow` (seconds to the next
  half-hour), so strikes stay phase-aligned. Optionally pre-scaled by `driftPpm`.
- **Measured drift** (watch-ends only) = `trueLocalSec − predictedNow`, over
  `elapsedCommandedSec` → ppm. This is the number `ntp`/`24h` report — a real measurement
  every 4 h, replacing the reactor's eternal `+0.0`.

**The 3-attempts-then-quit retry needs no persistent state.** Three SNTP tries fit in one
~15 s awake window, so "quit until the next watch-end" is simply *schedule nothing extra*
— the next attempt is the next watch-end, 4 h on. **3 attempts everywhere** (the boot/
watch asymmetry from the original ask is dropped as low value); the only cold-boot
special case is "no valid RTC → free-run from 00:00," gated by `magic`.

---

## 4. NTP, and why 3 attempts is enough

The failure is almost never NTP itself. `pool.ntp.org` is a DNS-rotated set of volunteer
servers whose aggregate uptime is effectively 100 %, and `sntp_simple` re-resolves DNS
per call, so three attempts can reach three different servers. What actually fails is the
**local hop** — WiFi association on a cheap AP, a slow DHCP lease, a DNS timeout — which
is transient and ~seconds-scale, and where the first-attempt miss is very often just
"WiFi hadn't finished associating." A single SNTP sample is accurate to ~10–100 ms,
four orders finer than the half-hour strikes, so **precision is a non-issue; only
availability matters, and it degrades gracefully** — a wholly failed watch-end just means
4 more hours of unmeasured drift, which `sync 0d/14h` makes visible while the ratchet
keeps striking on the RC oscillator.

**Refinement:** a short backoff (~2–3 s) between the 3 attempts, so a still-associating
WiFi stack has time to come up — the single most common miss. 3 × (4 s timeout + backoff)
is one ~15 s window, no persistent state.

---

## 5. The panel (painted only at watch-ends, after the sync)

Ordinary strike wakes never touch the e-paper — ~6 refreshes/day, comfortably inside the
180 s guidance (#89). The exact-time and `bells N` rows are **dropped**: painted only at
4-h boundaries, they would show a value accurate for an instant and then decay for hours.
`heap`/`gc` are **repurposed**: in run-once each wake is a fresh short-lived VM, so they
no longer trend — `gc` is dead, and `heap` becomes the per-wake RAM-fits check.

```
 0  tuvm            static label
 1  ----------
 2  heap  NN/NNK    per-WAKE peak live / arena — the sleep build's RAM gate
 3  ppm     +NNN    learned drift RATE, signed   (was gc)
 4  up   Nd00:00    wall-clock since cold boot (bootTicks × 30 min, persists via RTC)
 5  ----------
 6  ntp   +N.Ns     last MEASURED drift, seconds — or FAIL (this watch), or -- (never synced)
 7  24h    +N.N     max drift over the last 6 syncs (24 h)
 8         +N.N     min
 9  sync  0d/14h    age since last good sync  (Nd/NNh, the `up`-style format)
```

`ntp FAIL` + a climbing `sync` row is the immediate failure report the watch-end paint
exists to give. Row 0 stays a static label rather than a timestamp, since any displayed
clock time is stale between paints; the `up`/`sync` cumulative rows carry the "is it
alive and current" signal instead. 12-column hard limit unchanged; `line:val:`
right-justifies each value (`sync␣␣0d/14h` = 12).

---

## 6. New prims (4; `ntpSyncTz:` reused)

Counted, appended after `ntpSyncTz:` (130). Same five-site recipe (`prim_ids.h`,
`dispatch.c` enum + `PRIM_NAMES` + parity, `platform.c` host leaf + case,
`zephyr/src/main.c` target leaf, `kernel.nsl` forwarder).

| prim | selector | answers | notes |
|---|---|---|---|
| 131 | `rtcValid` | bool | `magic` matches → true. False → free-run-from-00:00 path. |
| 132 | `rtcGet:` | SMI | field by index (table §3). |
| 133 | `rtcPut:value:` | self | field by index. |
| 134 | `deepSleep:` | — (never returns) | arms `esp_sleep_enable_timer_wakeup(seconds×1e6)` then `sys_poweroff()`. Per #83, Zephyr's `poweroff.c` arms **no** wake source, so this is a direct HAL call from app code before poweroff; the HAL is already linked. Host leaf: `exit(0)` after logging, so a host run terminates rather than hangs. |

`ntpSyncTz: offsetSeconds` (130) is reused unchanged — it already answers
seconds-since-local-midnight or nil.

Struct layout, `magic` constant, and field indices live in one place in C
(`zephyr/src/main.c`, beside the wifi/ntp leaves); the node carries the indices as method
constants. The host leaf backs the struct with a plain `static` (survives within a
process, not across a simulated sleep — which is exactly enough to host-test get/put and
the cold-boot-magic path).

---

## 7. Control flow (the node, pseudocode)

```
main:
  valid := rtcValid
  valid ifFalse: [ initColdState ]          "magic, lastStruckHalf:=next-from-00:00, staleSec:=big"
  half := rtcGet: lastStruckHalf
  half := half + 1                           "the strike about to happen"
  isWatchEnd := (bellsForHalfHour: half) = 8 "8 bells => watch end"

  strike (bellsForHalfHour: half)            "blocks to completion BEFORE any sleep"
  rtcPut: lastStruckHalf value: half
  rtcPut: bootTicks value: (rtcGet: bootTicks) + 1

  isWatchEnd ifTrue: [
     trueSec := trySyncUpTo3                  "nil on total failure"
     trueSec isNil
        ifFalse: [ applySync: trueSec ]       "measure drift, ratchet-forward, re-anchor, zero staleSec"
        ifTrue:  [ markSyncFailed ]           "staleSec += interval; ntp row := FAIL"
     paintStats ]                             "AFTER the sync, so FAIL shows"

  sleepSec := secondsToNextBoundary(predictedNow)
  rtcPut: elapsedCommandedSec value: (rtcGet: elapsedCommandedSec) + sleepSec
  deepSleep: sleepSec                          "never returns"
```

`applySync:` enforces the §2 ratchet: forward-only on the count, phase-only on the sleep.

---

## 8. Edge cases

- **Midnight** is a watch-end (8 bells at 00:00), so it syncs right after announcing
  midnight — matching the original ask. `half` wraps 47→0, `trueLocalSec` wraps at 86400.
- **Cold boot, no valid RTC:** `rtcValid` false → free-run from 00:00, attempt the sync;
  if it fails, keep free-running and correct at the first good watch-end. No time is
  known until the first success — honest, and identical in spirit to stage 4's
  pre-first-sync behaviour. **A cold boot NEVER strikes** — see the §8 amendment below.
- **Strike never cut off:** the DAC strike blocks ~0.5–4 s; sleep is only entered after
  `struck`. Ordering in §7 guarantees it.
- **Display fault:** paint is best-effort (existing #88 catch); a panelless or faulted
  board keeps striking headless.
- **RAM:** each wake is a *fresh, short-lived* VM, so the working set is smaller than the
  reactor's, and stage 4's measured 20480 arena (96.64 % dram0) is the starting point —
  re-verified on the `-m32` host sweep before flashing, per the standing rule that a
  figure which *links* proves nothing about whether the arena holds the VM's own stack
  (`VM_STACK_INIT_WORDS × oop_size` floor).

---

## 9. Testing

**Host (pure math + state machine), the stage-4 pattern:**

- Ratchet monotonicity — `trueHalf < lastStruckHalf` ⇒ count unchanged, **no strike**.
- Ratchet catch-up — `trueHalf > lastStruckHalf` ⇒ advances, **silent** (skipped bells
  not rung).
- Drift computation — `predictedNow` vs injected `trueSec` ⇒ correct signed seconds + ppm.
- Seconds-to-next-boundary — the phase math (stage 4's `msToBoundary`, in seconds), incl.
  the boundary-exact case (a full interval, never 0).
- Staleness formatting — `0d/00h`, `0d/14h`, `2d/12h`.
- Midnight wrap — half 47→0, seconds 86400→0.
- RTC seam — get/put round-trip; `rtcValid` false on a mismatched `magic` ⇒ cold-state
  defaults. Driven via the host static-struct leaf.

The wake handler is a **pure function of (RTC struct, injected NTP answer)**, so the host
test calls it repeatedly with a simulated struct and scripted sync results — the entire
state machine is exercised without a real sleep.

**Device (compressed cadence):** a short "half-hour" so several strike-wakes, a watch-end
sync, a paint, and a real `deepSleep:`→timer-wake→reboot cycle all show within minutes on
the UART console — the same 6× demo trick as stage 3, now compressing the sleep too. The
witness is: strikes advance monotonically across real sleep/wake reboots (state survived
`.rtc.data`), the watch-end paints a measured drift, and a forced sync failure paints
`ntp FAIL` with a climbing `sync` row. **True deep-sleep power draw is not host-testable;**
the compressed device run is the only witness for the sleep/wake cycle itself.

---

## 10. Scope boundaries

- **Button wake (ext1) is out of scope** — that is tuvm#83, a separate rung. This design
  uses **timer wake only**. If both land, the wake handler gains a "why did I wake"
  branch (`esp_sleep_get_wakeup_cause`), but the bell path is timer-driven regardless.
- **Drift *compensation*** (pre-scaling sleeps by `driftPpm`) is designed-in as a stored
  field and an optional multiply, but **measurement comes first** — the first days of
  `ntp`/`ppm` readings tell us whether compensation is worth enabling. Correctness never
  depends on it; the ratchet + 4-h re-sync bound the error without it.
- **Power measurement** (actual µA asleep, battery-life estimate) is follow-up, not part
  of this rung. Driving GPIO21 (NeoPixel power gate) inactive before sleep is noted here
  (per #83) so the strip's quiescent draw does not survive the sleep.

## Addendum 2026-07-25 (evening) — decisions made during the 1-min demo pass

Four amendments to this design, all implemented (`01aa7fa`), witnessed at a 60 s slot, and
running in the shipping build; the evening comment on tuvm#94 carries the full witness log.

- **§3 amendment — `elapsedSec` accrues awake time, not just sleep.** The original accrual
  (`elapsed += sleepSec`) omitted the wake's own duration (boot+WiFi+strike, ~15-20 s), so
  predicted time ran slow by ~1% at the 1800 s slot — which would have put ~+9700 ppm of boot
  overhead into the very drift figure §5 exists to report. Now `elapsed += sleepSec +
  (0 millis // 1000)`, taken at arming. This is what makes the ppm row an oscillator
  measurement rather than an overhead measurement.
- **§7 amendment — the cadence is one parameterized constant, `slotSec` (1800).** A demo build
  flips it to 60 and every derived quantity scales (boundary math, trueHalf, stale accrual
  8×slot, up row, midnight wrap 86400//slot). Scaling the slot keeps every number honest;
  compressing only the physical sleep (the Task-9 witness trick) is for state-machine
  exercise only, since it makes a clock that is wrong between syncs — stage 4's rejected
  6×-clock, rediscovered.
- **§5 amendment — cold boot paints a placeholder panel** (dashes for the never-synced
  `ntp`/`sync` rows, zeros elsewhere). Rationale: distinguishes "display dead" from "not yet
  painted" for up to a whole watch, and wipes the stale image a reflash leaves (a reflash is
  always a cold start — the BOOT0+RESET chord is a POWERON-class reset and RTC slow memory
  does not survive it). Strike wakes still never paint.
- **§5/§10 amendment — display bringup is lazy** (inside `display_target_available()`), because
  the uc81xx driver's `display_blanking_off()` performs a full white refresh of RAM its init
  just cleared: a boot-path bringup therefore wipes the glass on EVERY wake, which the
  always-on stages could never observe (they boot once). Wakes that never paint now never
  touch the panel. The same lazy-seam treatment for WiFi is filed as tuvm#99.
- **§8 amendment — a cold boot syncs FIRST and NEVER strikes** (2026-07-24 evening, David;
  commits `d2f82ed` / `6bdf90a`). §8 above always called for the boot sync, but the reactor
  rewrite dropped stage 4's boot-sync retry and nothing replaced it: `main:` reached the
  strike path unconditionally, so a cold boot rang half 1 and then free-ran, wrong, until
  the first watch-end — **up to 4 hours of wrong bells**. Witnessed: a RESET at 20:00:56 rang
  1 bell, then 2/3/4/5 on the half-hours, every one of them wrong against true ship's time.
  Cold boot now takes its own path (`coldBoot:`) and never reaches the strike:

  > sync → `applySync:` advances the ratchet from the free-run 0 to true time (silent; the
  > skipped bells are **not** rung) → paint the **synced** panel → sleep to the next **true**
  > boundary. The first bell the house hears is that boundary's, with the right count.

  Two consequences fold back into earlier sections. The **§5 placeholder paint moves**: it
  now happens in `coldBoot:` *after* the sync (reading `ntp sync` / `sync 0d/00h`), not in
  `initColdState:` before it, so the panel never shows a state we already know is about to
  be corrected — the dashes now appear only when NTP actually fails. And the arming interval
  on a synced cold boot is **partial, not a slot** (witnessed: `deep sleep 892 s` for a
  22:15:08 boot), which is the phase alignment §3 always specified but which no cold-boot
  path previously exercised.

  The rule this leaves is three lines with no exceptions, and **midnight is no longer
  special** — it is simply one of the six watch-ends: *strike at every boundary; sync + paint
  at boot and at every 8 bells; radio on only for those.* (The third line is tuvm#99, not yet
  built — the radio is still up on all 48 wakes.)

  Device witness, 2026-07-24: cold boot 22:15:08 → `ntp: ok, local sod=80108` →
  `sync true 80108 predicted 0 -> advance half 44` → `deep sleep 892 s` → **no bell at boot**
  → `wake half 45 bells 5` at 22:30:14, correct on the wall clock. Host gate:
  `tools/test_deepsleep_node.py` pins all three branches (synced-no-strike, right-count-first-
  strike, ntp-fail-still-silent); suite 227/0, all aux gates green.
