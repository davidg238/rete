# shipsBellsV3 — design

**Date:** 2026-07-26
**Status:** design settled in brainstorming; **no implementation started**
**Predecessor:** the MagTag ship's-bell clock (tuvm#94) — v2 in all but name
**Related:** tuvm#100 (RTC timebase), tuvm#99 (lazy WiFi), tuvm#86 (bell), tuvm#83 (deep sleep)
**Scope of this document:** a **run of five**, not a product. See §3.

---

## 1. What it is

A ship's bell clock with **no clock face**. A bell, a hammer, a chime/silent lever, and a
charging socket. It strikes the half-hours, 1–8 bells through each four-hour watch, and it
tells you the time in the only language it speaks.

It connects to nothing. No WiFi, no NTP, no Thread, no account, no cloud, no pairing beyond a
single tap, no firmware updates, and nothing that stops working when a service is retired.

**It is wound once a year, with a cable.** A mechanical clock of this character wants winding
weekly. This one wants plugging in annually. That is the ritual, not an apology for a battery.

## 2. The interaction, start to finish

1. **Unbox, tap your phone to the base.** A passive NTAG213 sticker carries the app link; a
   phone that has never seen this product opens the app, or its store page if not installed.
   No card in the box, no URL to type, no QR code.
2. **The app shows one screen:** the current time, and **Set**. It connects over BLE.
3. **The clock strikes the current bell count.** That is the confirmation — no LED, no screen,
   no "connected" toast.
4. From then on it strikes the half-hours. Forever.
5. **Front lever: `CHIME` / `SILENT`.** The idiom off a mantel clock; needs no explanation.
6. **Underneath the base: a `DOG WATCH` switch.** Anyone who knows what a dog watch is will
   turn the clock over and find it; anyone who doesn't will never see it.
7. **Once a year** it strikes a distinctive low-battery pattern. Take it off the wall, plug it
   in, hang it back up.

**Before the time is set, it does not strike.** Never a wrong bell. (The rule tuvm#94 learned
the expensive way: never strike at boot.)

**Time survives everything short of the coin cell.** The RTC is coin-cell backed, so a flat
main battery, a battery swap, or a decade of moves never costs a re-tap.

## 3. Scope: a run of five

These five are built **under FCC §15.23 (home-built devices)**: not marketed, not built from a
kit, five or fewer, for personal use. That exemption is the whole of the compliance posture and
it evaporates on the first sale or the first advertisement.

**Constraint, stated so it cannot be forgotten: these five must not be sold, offered for sale,
or advertised.** Selling requires FCC Part 15B SDoC (~$1,500–3,000, one-time, one lab visit);
that is a separate decision taken after the market question is answered by other means.

The run of five proves: **does it sound beautiful, and does anyone want one?** Everything else
is downstream of those two answers.

## 4. Decisions and why

| Decision | Choice | Why |
|---|---|---|
| MCU | **nRF52840**, certified module (Fanstel BT840-class) | Already a first-class tuvm target (`prj_dk.conf`, `boards/nrf52840dk_nrf52840.overlay`). 256 KB RAM, so the ESP32-S2 arena squeeze that dominated #94 does not apply. Module carries its own FCC ID/IC for the radio. |
| Firmware | **nsl on tuvm** | The expensive part exists and is witnessed on hardware: bell-count schedule, monotonic ratchet, strike phrasing, sync branches, 227-test suite. |
| Data link | **BLE** | The module's grant covers it. Zero radio certification. |
| App discovery | **Passive NTAG213 sticker** | No power source, no oscillator — not a transmitter, not a digital device, **not regulated**. Keeps tap-to-install for ~20¢ with no compliance impact. |
| Timebase | **External RTC, ±2 ppm class**, coin-cell backed | See §5 — this is the empirically-driven decision. |
| Power | **Internal 18650, recharged ~annually. No cable or wall wart supplied.** | Uncorded on the wall. See §7 for the arithmetic. |
| Architecture | **Always on. The VM never dies.** | See §6 — this deletes rather than re-solves the hardest bug class from #94. |
| Acoustic element (run of 5) | **Standard doorbell chime, REAR solenoid** | Proves the concept with a $13 part. See §8. |
| Night behaviour | **Physical lever**, not an app setting or a sensor | Discoverable without the app, works when the phone is dead, keeps the NFC/BLE payload to "the time". |
| Display | **None** | The bell is the output device. |

## 5. Why the RTC is not optional — measured, not assumed

The MagTag clock (tuvm#94) ran 22½ hours on the ESP32-S2's internal RC slow clock with a
disciplining loop correcting it at every four-hour watch-end. Six independent rate measurements:

| | |
|---|---|
| Mean | **+10,511.5 ppm** |
| σ | **≈ 333 ppm** |
| Range | 986 ppm |
| Resulting bell error | `−4, +14, −8, +7, −7, +3` s — **RMS 8.0 s, envelope ±15 s** |

The RC **wanders about a stable centre** rather than drifting, so no model can track it, and
**correcting at the watch-end cannot touch drift accumulated between watch-ends**: ±333 ppm over
four hours is ±4.8 s, and that is the floor however clever the endpoints are.

A ±2 ppm part over the same four hours is **±0.029 s**. A ±1 ppm part, ±0.014 s. Against a
measured RMS of 8.0 s and an envelope of ±15 s, that is **two to three orders of magnitude**,
and it is why every gram of software cleverness in #94's disciplining loop deletes rather than
ports.

**Part choice.** Note that DS3231**M** is ±5 ppm (MEMS) — not a 2 ppm part.

- **Run of five:** a **DS3231 precision breakout** (±2 ppm class, ~800 nA backup). Available,
  documented, `Kconfig.ds3231` and the `maxim,ds3231-rtc` binding already in the NCS tree.
- **If a PCB ever happens:** **RV-3028-C7** — ±1 ppm with built-in calibration, **45 nA**
  backup, smaller and cheaper. `Kconfig.rv3028` also present.

Either way the binding is against Zephyr's generic `rtc` API, so the part is a devicetree
choice. That seam also serves tuvm#100.

## 6. Firmware architecture

**The VM never dies.** nRF52840 System ON with RAM retained and the RTC running is ~3 µA —
**0.1 Wh/year**, about 1% of the budget. So the entire deep-sleep state-loss bug class from
#94 stage 5 — RTC-retention magic words, the cold-boot branch, awake-time accrual, the
ratchet-across-reboot — **does not exist here.** State lives in ordinary objects. The reactor
is a loop.

What ports from #94, and is already proven on hardware:

- the half-count → bell-count schedule and its 24-hour wrap
- the monotonic ratchet
- the strike phrasing (pairs, lone final stroke if odd)
- never-strike-before-the-time-is-known

What deletes entirely:

- `calPpm`, `driftPpm`, `calPpmSeed` and the whole disciplining loop — **including defect 7**
  (the gain-1 integrator that made the MagTag clock 1.67× worse than not correcting at all).
  With a ±2 ppm part there is nothing to discipline.
- the deep-sleep arming formula, awake accrual, and the RTC-data magic word
- WiFi, SNTP, DNS, and the ~8 s average awake window they cost (tuvm#99)

What is new:

- an **`rtc` seam** binding Zephyr's RTC API (shared with tuvm#100)
- a **BLE set-time service** — one characteristic. The app writes a full date and time (that is
  what Zephyr's `rtc_set_time` takes); **the striking schedule uses time-of-day only** and the
  date is carried but unused. The device answers with its own clock before setting, so the app
  can show the offset it corrected.
- **strike-energy control** as a pulse-width constant (see §8)
- **battery level** via SAADC through a divider, and the low-battery strike pattern
- **inhibit striking while charging** — sensible behaviour, and it keeps the noisiest circuit
  in the product provably idle in the one condition where a conducted-emissions test would apply

**Dog watches.** The `DOG WATCH` switch selects between the plain six-watch day (1–8 bells,
six times) and the authentic convention: 16:00–18:00 rings 1,2,3,4 and 18:00–20:00 rings
1,2,3 then **eight bells at 20:00**. A 48-entry lookup table. Read the switch **as a level at
strike time**, never as an interrupt — no edges, no debounce, and the EDGE_TO_ACTIVE bug class
from the MagTag buttons never arises. Same for the chime/silent lever.

## 7. Energy budget

**216 strokes/day** (six watches × 36). The dog-watch day is slightly *cheaper* — the two dog
watches ring 1,2,3,4 and 1,2,3,8, i.e. 24 strokes where a full watch rings 36, giving
**204/day** — so 216 is the worst case and the budget below is built on it.

Estimated 0.5 J per strike at a 30 ms pulse — **this is the one number in the document that is
assumed rather than measured** (see §10).

| Item | Per year |
|---|---|
| Striking, 30 ms pulse, 80% boost efficiency | **13.7 Wh** |
| Striking, 15 ms pulse | 6.9 Wh |
| MCU System ON + RTC (~3 µA) | 0.1 Wh |
| BLE advertising, 2 s interval (~15 µA) | 0.5 Wh |
| **18650, 3000 mAh** | **10.8 Wh (≈9.7 Wh usable)** |

**Breakeven pulse width for a 12-month interval:**

| condition | max pulse |
|---|---|
| Never silenced | **~21 ms** |
| Silent hours used nightly (−37%) | **~34 ms** |

The second row settles the cell choice: **with the lever used at night, an 18650 clears a year
at any plausible pulse width.** A 21700 was insurance against a case needing both a long pulse
and an owner who never silences it.

**Running flat is a non-event** — the coin cell keeps the RTC, so the clock stops striking,
you charge it, and it resumes at the correct time with no re-tap. Margin against a benign
failure is worth little, which is the real reason not to pay for the larger cell.

At ~2.5%/year calendar ageing an 18650 is at ~78% after a decade — still ~10 months with silent
hours. Acceptable for a twelve-year-old clock.

**BLE advertises continuously at a 2 s interval** (~5% of the budget) rather than behind a
`SET` button. Buying back 0.5 Wh/year is not worth a control on a product whose claim is that
there is nothing to learn.

## 8. The strike

**Acoustic element for the run of five: a standard doorbell chime unit (~$13).**

A two-note chime is asymmetric on purpose:

- **FRONT** — the plunger strikes bar A outbound and the spring return strikes bar B. That is
  the "ding-dong". **Wrong** for a ship's bell, which needs one note struck N times.
- **REAR** — second solenoid, single bar, return stopped by a rubber bumper. **"Ding" only.
  This is the one to use.** Confirm by hand on the actual unit before wiring.

The vintage telephone gong remains the better *sound* and stays the candidate for anything past
the five — but salvaged ringers are not a supply chain, and the doorbell answers the concept
question for $13.

**Driver: low-side N-MOSFET, one per solenoid.**

```
V_strike (+) ──┬── solenoid ──┬── MOSFET drain
               │              │
          reservoir      flyback diode (cathode to V_strike)
               │              │
GND ───────────┴──────────────┴── MOSFET source
                    GPIO ──R_gate── gate ──100 kΩ── GND
```

Non-negotiables:

- **A flyback diode.** The blue "N-MOS trigger switch" modules have none; switching an
  inductive load without one puts the collapse energy into the MOSFET's avalanche rating 216
  times a day.
- **A 100 kΩ gate pull-down**, so the coil is off while the MCU is in reset or unflashed. The
  chime coil is momentary-duty and cooks in well under a minute if held on.
- **A hardware duty backstop** that does not depend on firmware being alive — a ~1 A polyfuse
  in series. Never let a crashed VM be the only thing between a GPIO and a burnt coil.
- **A deliberately slow gate edge** (large `R_gate`). A 30 ms pulse needs no fast switching,
  and the turn-off edge is the dominant EMC risk in the whole product. This costs nothing now
  and is expensive to retrofit after a failed scan.

**Rail:** 18650 (3.0–4.2 V) boosted to ~20–24 V into a **4700 µF / 35 V reservoir**. The cells
never supply the peak — the cap does, recharged over the intervening 30 minutes. **Gate the
boost converter**; its 10–50 µA quiescent would otherwise be a fifth of the budget.

**Strike loudness is pulse energy**, one firmware constant. That is what makes the v2 light
sensor a populate-and-flash change rather than a mechanism.

**If the strike sounds mushy**, replace the plain flyback diode with a diode + zener (or a TVS
across the MOSFET) to force a faster current decay and get the hammer off the metal sooner.

**A charge pump is the wrong converter** — efficient only near integer ratios. Use an inductive
boost; at 3.7 V → 22 V the difference is 50–70% against 85–92%, which is months of runtime.

## 9. Hardware summary

| Block | Run of five |
|---|---|
| MCU | Certified nRF52840 module on a carrier. A **Feather-class nRF52840 board** (e.g. Raytac MDBT50Q-based) brings USB, a Li-ion charge IC and a battery connector in one part — worth using for five units rather than building a charger. Fanstel BT840 is the production part. |
| RTC | DS3231 precision breakout, I²C, CR2032 on `VBAT` |
| Battery | 1× 18650 in a **holder** — user-replaceable. A sealed pouch makes an heirloom disposable in 15 years. |
| Charging | USB-C receptacle, 5.1 kΩ CC pulldowns. Average draw is milliwatts; a legacy 500 mA source is ample, so no USB-PD, no e-marker. |
| Strike | Boost + 4700 µF reservoir + MOSFET + flyback + polyfuse |
| Acoustic | Doorbell chime, REAR solenoid |
| Controls | `CHIME`/`SILENT` lever (front), `DOG WATCH` switch (underneath) — both read as levels |
| App link | NTAG213 sticker in the base |
| Light sensor | **v2** — footprint reserved, unpopulated |
| Enclosure | Hand-made, wall-hung. Charging port on the back or underside; it is seen once a year. |

## 10. Measured vs assumed

Following this program's habit of measuring rather than estimating, the assumptions are named:

**Assumed, and load-bearing:**

- **0.5 J per strike at 30 ms.** This single number sets the battery interval, the reservoir
  size, and the boost rail. **Measure it first.**
- Boost efficiency 80%.
- That the REAR solenoid on the specific chime unit is genuinely single-note.

**Measured, and trustworthy:**

- The RC-oscillator wander that justifies the RTC (§5) — six samples, tuvm#94.
- nRF52840 System ON current, from the datasheet and consistent with the DK footprint work.

**The first bench session, in order:**

1. **Coil DC resistance and inductance** on both the doorbell solenoids and the phone bell.
2. **The minimum pulse width that still gives a clean strike.** This decides §7 outright — and
   the same measurement characterises both candidate acoustic elements in one sitting.
3. Confirm the blue MOSFET module's gate pull-down (TRIG→GND should read ~10 kΩ) and read the
   markings on Q1/Q2 (`4184` ⇒ AOD4184, 40 V logic-level).
4. One strike from a GPIO, into the reservoir, at the measured pulse width.

## 11. Deferred

- **Light sensor** (v2) — softer stroke when the room is dark. Footprint on the board now.
- **The vintage phone gong** as the shipping acoustic element, and the H-bridge that its
  polarised rocking armature wants (forward pulse to strike, reverse pulse to snatch the
  armature clear so the gong rings free).
- **Selling anything.** Requires FCC Part 15B SDoC. Test demand with a waitlist and a fair
  stand first — neither is an offer for sale.
- **EU/UK** — RED, EN 301 489 on the finished product, EN 62368-1. The expensive jurisdiction;
  defer explicitly.

## 12. Risks

| Risk | Severity | Mitigation |
|---|---|---|
| The 0.5 J estimate is badly wrong | **High** — it sets the battery, cap and rail | Measure it in the first bench hour. Everything downstream is parameterised on it. |
| The chime unit sounds like a doorbell, not a bell | High — it is the whole point | Cheap to find out. The phone gong is the fallback, and the driver design already anticipates it. |
| Solenoid coil burnt by a firmware fault | Medium — the vintage bell is irreplaceable | Gate pull-down, polyfuse, firmware on-time cap. Three independent layers. |
| Conducted emissions if this ever goes to sale | Medium | Slow gate edge, tight strike loop, snubbing — designed in now, not retrofitted. |
| Li-ion in a hand-built enclosure | Medium | Branded protected cell, a proper charge IC, a holder not a pouch. |
| nsl working set on nRF52840 | **Low** | 256 KB RAM. This was #94's dominant risk and it simply is not one here. |

---

## 13. The mechanical striker — a costed alternative behind the same seam

A weight on a line drives the hammer; the electronics only **releases a detent**. Recorded here
as a genuine alternative, not a footnote, because the arithmetic favours it strongly.

### 13.1 Why the physics favours it

**A solenoid is roughly 8% efficient** at turning electricity into hammer kinetic energy — the
rest is I²R heat in the coil. The 0.5 J in §7 is what the *solenoid* costs, not what the *bell*
needs.

Working backwards from a real movement: a one-day cuckoo's strike weight is ~350 g over ~1.5 m,
so **~5 J/day**, driving ~180 operations including bellows and the fly governor. That is
**~0.03 J per stroke, all-in**. A bell hammer is heavier than a whistle — call it
**0.04–0.08 J**.

So the hammer wants ~0.05 J. A solenoid burns 0.5 J to deliver it; **a falling weight delivers
it at near unity**, less train friction.

### 13.2 The electronics pays for the release, not the strike

A detent is lifted **once per striking sequence — 48/day**, not once per stroke (216/day).

| load | per year |
|---|---|
| Detent releases (48/day × ~20 mJ) | **0.1 Wh** |
| nRF52840 System ON + RTC (~3 µA) | 0.1 Wh |
| BLE advertising (2 s interval) | 0.5 Wh |
| **Total** | **≈ 0.7 Wh** |
| **3× AA lithium (Energizer L91)** | **13.5 Wh** |

**≈19 years of load against a 20-year shelf life.** The cells get changed because they are old,
not because they are flat.

### 13.3 Three consequences that were not the goal

**L91 primaries do not leak.** This matters more than the energy: **leaking alkalines are the
commonest way a decade-lived battery device dies**, and a ruined heirloom is a far worse failure
than a flat one.

**The electronics get simpler.** No Li-ion, no charge IC, no protection, no USB-C receptacle,
no CC resistors, no charging-inhibit logic, no recharge ritual. Most of §7 and half of §9
delete.

**Conducted emissions drops off cleanly** — literally, not arguably. §15.107(c) exempts devices
that only employ battery power and contain **no provisions for operation while connected to AC**.
With no charging port that is met on its face. Ship without cells and the lithium transport
paperwork goes too.

### 13.4 Let the electronics count — no count wheel

A traditional count wheel strikes a fixed 1–12 sequence. Ship's bells need 1–8 plus the
dog-watch variant, which would otherwise mean cutting custom wheels.

Instead: **release the detent, count strokes with an optical or hall sensor, re-latch after N.**
A bistable latching solenoid does it in two short pulses per sequence with zero holding current.
This keeps `DOG WATCH` a firmware table rather than a machining problem, and it is what lets
this option compose with the design above unchanged.

**This is the project's real mechanical problem:** disabling the movement's own count-wheel stop
and fitting an electrically-controlled detent in its place, ideally on a slow wheel rather than
the fly.

### 13.5 Weight sizing

| weight × drop | energy | interval at ~0.06 J/stroke |
|---|---|---|
| 2 kg × 1.5 m | 29 J | ~2 days |
| 3 kg × 2.0 m | 59 J | ~4½ days |
| 5 kg × 1.5 m | 74 J | ~5½ days |
| **7 kg × 1.5 m** | **103 J** | **~8 days — the classic longcase interval** |

A weekly wind, for an owner who already winds clocks. That is a feature.

### 13.6 The cost, honestly

**It stops being an electronics project and becomes a horology project.** A strike train needs
gears, a fly governor for the rhythm, a detent, and a hammer with proper lift and release.

**Buy, do not build.** A **cuckoo clock movement** is weight-driven, already has a strike train,
fly and detent, and is designed to be released by a lever. Gut the going train and drive the
release electrically — a weekend of fettling rather than a winter of machining.

Two further costs: **7 kg on a wall needs real mounting**, and a mechanical train can jam, wants
levelling and eventually oil. An heirloom that stops is a repair visit where a solenoid version
needs a battery.

### 13.7 Sourcing

**The vocabulary matters more than any part number** — these are the terms that make a search
return clockmakers instead of gift shops:

| term | what it is |
|---|---|
| **strike train** | the separate gear train that drives the striking (not the going train) |
| **fly** / fan fly governor | the air brake that sets the *rhythm* of the strike — the thing that makes it sound like a clock |
| **count wheel** / locking plate | the older way of counting strokes — **the part to disable** |
| rack and snail | the newer way of counting; also to be bypassed |
| **detent** / locking lever | what holds the train stopped — **the part to replace with a solenoid** |
| **warning** | the partial release just before the strike |
| hammer tail, lifting pins | how the hammer is raised and dropped |
| **maintaining power** | keeps the train driven while winding |

**Movements — the concrete search:**

- **`Regula 25`** — the standard 1-day cuckoo movement, weight-driven, ~$40–90. `Regula 34` is
  the 8-day. Also `Hubert Herr`. Search `cuckoo clock movement Regula 25`.
- Generic: `cuckoo clock movement replacement`, `1-day cuckoo movement`, `8-day cuckoo movement`
- Higher end, weight-driven: `Hermle weight driven movement`, `Kieninger movement` (€150–400 —
  probably over-buying for a prototype)
- **Cheapest route: salvage.** eBay `broken cuckoo clock` or `cuckoo clock for parts` — $20–40
  and the movement is usually fine; it's the bellows or the case that failed.

**Suppliers to search:** Klockit, Timesavers, Merritt's Antiques, Ronell Clock Co (US);
Cousins UK (very large horological catalogue); Perrin (Canada).

**A note on what you actually want from a cuckoo movement:** the strike train, the fly, and the
gong hammer. **You do not want the bellows** — removing them cuts the load and lengthens the
wind interval.

### 13.8 The decision gate

**The seam already isolates this.** The strike is a single call, `strike: n`; whether it drives
a MOSFET or trips a detent lives entirely below it. Schedule, ratchet, dog watches, BLE
set-time and the levers are all unchanged either way. **So do not fork the design — sequence
it.**

But sequencing carries one real risk: **if the sound is the whole product, a run of five could
answer "nobody wants this" to a question the mechanical version would have answered "yes" to.**

Therefore:

> **Bench hour one is now a decision gate.** Strike the doorbell solenoid and listen. If it
> sings, build the five and hold the mechanical striker as v4. **If it sounds like a doorbell,
> promote the mechanical path immediately** rather than hand-finishing five objects that sound
> wrong.
