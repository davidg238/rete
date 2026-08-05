# shipsBellsV3 — design

**Date:** 2026-07-26
**Status:** **SEALED 2026-07-26.** Design settled in brainstorming; **no implementation
started**. Changes from here are amendments, not edits — §13 (mechanical striker) and §14
(solar) are recorded as evaluated options with their arithmetic, so neither needs re-deriving.
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

---

## 14. OPTION TO EXPLORE — solar for the electronics

**Status: not decided. An option, recorded with its arithmetic so it can be evaluated rather
than re-derived.** Applies on top of §13 (mechanical striker), where the electronics' job is
reduced to releasing a detent.

### 14.1 The arithmetic

A **150 × 75 mm** face is **112.5 cm²**.

| | |
|---|---|
| "Normally lit room" | 100–300 lux |
| Vertical derate | ceiling light shines *down*; a vertical face sees ~50–60% of the horizontal illuminance |
| Indoor PV yield (a-Si / indoor-optimised) | 5–20 µW/cm² |
| **Output while lit** | **0.3–1.3 mW** |
| **Over ~10 lit hours** | **10–47 J/day** |

Against §13.2's electronics load of **6.9 J/day** (MCU System ON + RTC, BLE advertising, and 48
detent releases), that is **met with margin even at the pessimistic end**.

Sanity check: a solar calculator's cell is ~2 cm² and runs on tens of µW in office light —
about 10 µW/cm², squarely inside that band.

### 14.2 Explicitly out of scope: motor rewind

Rewinding the weight electrically needs **~32 J/day** (13 J/day of hammer work ÷ ~40% for a
small motor and gearbox). That sits at the *optimistic* edge of the harvest range, so it would
work in a bright room and fail in a dim one — and **the failure mode is a clock that quietly
stops.** Not worth the motor, the reduction gearing, the limit switch, or the risk.

**The weight is hand-wound. Solar powers only the electronics.** Recorded so this is not
re-litigated: the arithmetic was done and the answer was no.

### 14.3 What it would change

- **Deletes the battery entirely** — no 3× AA lithium, no battery door, no ten-year cell change.
  §13.2's "≈19 years, shelf-life limited" becomes "no battery at all", which is a better
  sentence and a better object.
- The clock then **needs nothing but light and an occasional wind** — which is what a mechanical
  clock has always asked for, and an honest claim rather than a marketing one.
- Adds: a harvesting front end, an energy buffer, and a collector surface to design around.

### 14.4 What it needs

**A harvesting front end, not a diode.** At sub-milliwatt levels this wants a proper
ultra-low-power boost charger with MPPT and a defined cold-start: **TI BQ25570**, **e-peas
AEM10941**, or **ST SPV1050**. Cold-start behaviour from a fully dead buffer is the thing to
check on the datasheet before committing.

**A buffer sized for darkness, not for night: an `IFR18650` LiFePO4 cell, in a holder.**

Riding out a fortnight of drawn curtains at 6.9 J/day is **~97 J**. An IFR18650 (3.2 V,
1100–1800 mAh) holds **~17,000 J** — **175× more than needed**. It is therefore **not chosen
for capacity**, and the two reasons it *is* chosen should survive any later attempt to shrink
it:

1. **It will still be buyable in twenty years.** The 18650 is the volume cylindrical format.
   An object meant to outlive its maker should take a cell its owner's grandchild can buy.
2. **It cannot be dangerously confused with a primary cell.** The AA-sized alternative
   (`IFR14500`) is **dimensionally identical to an alkaline AA but 3.2 V against 1.5 V**. That
   invites two real failure modes: an alkaline put into the bay (which can vent or rupture if
   the harvester attempts to charge it), and the 14500 taken out and put into a 1.5 V device.
   **Physical incompatibility is a safety feature and is worth more than the ~9 cm³ saved** —
   in a wall clock with 7 kg hanging off it, volume is not the constraint.

**Why not a supercapacitor**, despite "never needs replacing" being the better heirloom story:
draining 3.4 → 2.4 V would need **~33 F**, which exists and is roughly 18650-sized — but such a
part **leaks 10–30 µA, i.e. 2.6–7.8 J/day against a 6.9 J/day load.** That roughly doubles
demand and consumes exactly the margin needed in the dim-room case. Supercaps buffer seconds to
hours well and weeks badly. LiFePO4 self-discharge is ~2–3% *per year*; not a close call.

**Do not feed the cell to the MCU directly.** A freshly charged LiFePO4 sits at 3.6 V — exactly
the nRF52840's VDD ceiling. Set the harvester's over-voltage point to ~3.5 V and take a
regulated ~3.0 V from its buck output.

**Buy from a real distributor.** 18650 capacity fraud on marketplaces is rampant; "6000 mAh
LiFePO4 18650" is a physical impossibility.

**Cell type:** amorphous silicon indoor panels, or purpose-made indoor PV — search
`Epishine LEH3`, `indoor photovoltaic module`, `amorphous silicon solar cell indoor`. Standard
crystalline silicon performs badly at these light levels and is the wrong part.

### 14.5 Point it up, not out

Indoor light comes from the ceiling. **Angling the collector — a sloped top or a modest shelf —
plausibly doubles the harvest**, and ~60 cm² horizontal likely beats 112 cm² vertical. This is
free, and it is an industrial-design decision that has to be taken early because it shapes the
case.

### 14.6 What to measure before deciding

Per §10's discipline: **the room dominates this by ±3×, so it is a measurement, not an
estimate.**

> Put a candidate cell where the clock will actually hang, with a logger, and record a week.
> That single measurement decides §14 outright — and unlike the strike-energy measurement it
> can run in the background while other work proceeds.

---

## 15. ADDENDUM (2026-07-28) — the bench rig for §11's first two measurements

The design above is sealed; this records how §11.1 (coil resistance) and §11.2 (minimum clean
pulse) will actually be taken, and one refinement to §8 that came out of specifying it.

### 15.1 The bench trial runs at 12 V, on parts already on hand

Not a change to §8's 20–24 V rail — a deliberately gentler first strike, to answer "does the
plunger throw at all" in the direction that cannot break anything.

**Circuit:** `zephyr/boards/shipsBells/2026-07-28-shipsbells-12v-bench-driver.svg` / `.png`.
High-side P-channel (IRF9Z14) with a 2N3904 level shifter, because a 3.3 V GPIO cannot drive a
non-logic-level MOSFET and this arrangement is fail-safe **off** with one transistor. The build
proper still wants §8's low-side topology with a logic-level part (IRLZ44N / IRLB8721).

### 15.2 The §7 coil resistance is an assumption, and it swings the answer

§7's 0.5 J rests on **15 Ω, assumed**. Real chime coils are often 8–12 Ω, in which case 12 V is
approximately the rated drive rather than half of it:

| measured R | current at 12 V | force vs 16 VAC rated |
|---|---|---|
| 8 Ω | 1.50 A | 1.4× |
| 10 Ω | 1.20 A | 1.1× |
| 15 Ω (assumed) | 0.80 A | 0.56× |
| 20 Ω | 0.60 A | 0.34× |

Note that 16 VAC is **not** 22 V for this purpose: RMS is what heats the coil and what average
force follows, so 16 VAC ≈ 16 VDC of pull. This corrects a loose reading of "16 VAC rectifies to
~22 V peak" — the peak matters for component ratings, not for pull.

### 15.3 Refinement to §8: a Schottky flyback is the slowest-decay choice

§8 says that if the strike sounds mushy, replace the plain flyback with a diode + zener to force
a faster current decay. Worth naming the mechanism, because it changes what a bad result means:
a Schottky's ~0.4 V forward drop lets the freewheel current coast down over the full L/R time,
so the hammer stays pressed on the metal and damps the ring.

**Therefore: a strike that sounds *dead* is a diode problem; a strike that sounds *weak* is a
voltage or pulse-width problem.** Do not chase the first with more volts.

### 15.4 A 40 V Schottky is fine at 12 V and tight at 24 V

1N5819 is the top of its family (5817 = 20 V, 5818 = 30 V, 5819 = 40 V). Against the bench 12 V
that is 3.3× margin; against §8's 20–24 V rail it is ~1.7×. **The build proper wants a 60 V
part** — MBR360 / SB360.

---

## 16. ADDENDUM (2026-07-31) — the RTC seam exists, and §6 gained a mechanism it was missing

The `rtc` seam §6 called for is built and running (tuvm#100), on an **ESP32-C6** rather than
the nRF52840 of §9. Four things this changes or settles.

### 16.1 There will be more than one version; this one is wall-powered

The C6 rig is not a change of production MCU. David's framing: **"there will be multiple
versions"**, and this one is mains-powered *because of the striker's power demand*. So §7's
energy budget and §9's 18650 remain the design of record for the battery version, and this
board is where the firmware is developed against real hardware.

What wall power does change, for this variant only: §6's "the VM never dies" becomes free
rather than earned (it was an argument about nRF52840 System-ON at ~3 µA), and the
battery-level SAADC, the low-battery strike pattern and "inhibit striking while charging"
have nothing to act on. None of that invalidates the sealed design; it means the two
variants diverge below the seam, which is what a seam is for.

### 16.2 The seam is `wallClock`, not `rtc` — the name was already taken

`rtcValid` / `rtcGet:` / `rtcPut:value:` (tuvm#94 stage 5) are the deep-sleep RAM
**retention** store: a different device answering a different question. The new prims are
`wallClock` and `wallClockSetDate:time:`.

`wallClock` answers **seconds since local midnight (0..86399), or nil** — deliberately the
identical shape to `ntpSyncTz:`. The striking arithmetic therefore cannot tell which
timebase produced the number, so the two are interchangeable and the logic proven in
`nsl-tests/20_bell/05` is reused verbatim. There is **no tz argument**: NTP hands out UTC
and the product applies its own offset, whereas an RTC holds the local time written to it,
so a second offset would double-correct.

Setting takes a packed decimal date (`20260731`) plus seconds-of-day, in **one call**. Two
calls — a date write then a time write — leave a window where the part holds halves of two
different instants, and the second write can land after midnight has moved the day on.

### 16.3 "Never strike before the time is known" now has a mechanism, and it needed one

§6 lists *never-strike-before-the-time-is-known* among what ports from #94. On this hardware
that invariant was **unenforceable through the documented API**, and the gap is not obvious:

Zephyr's `rtc_get_time()` documents `-ENODATA` for "time has not been set"
(`include/zephyr/drivers/rtc.h:212`). **The DS3231 driver never returns it.** It reads the
seven time registers and converts them (`drivers/rtc/rtc_ds3231.c:339`), never consulting
the part's oscillator-stop flag. A never-set part — or one whose CR2032 died with mains off
— answers `rc=0` and a well-formed `2000-01-01 00:00:00`.

**00:00:00 is seconds-of-day 0. Midnight. Eight bells.** A flat backup cell would ring the
loudest thing the product does, repeatedly, at the moment it is least expected. This is a
§12-class risk that §12 does not list.

Predicted by reading the driver before flashing; the first hardware run confirmed it (the
module reported year 2000). The guard rejects an implausible year, since an unset DS3231
reports 2000. That is weaker than the oscillator-stop flag — it cannot catch a part that
stopped *this* year — but OSF is unreachable (`rtc_ds3231_get_ctrl_sts()` is static and
compiled only under `CONFIG_RTC_ALARM`), and the year catches the shelf-stored failure that
actually threatens a wall-hung clock. The proper fix is upstream: tuvm#103.

**Add to §12's risk table:** *an RTC that answers a valid-looking time it does not actually
know* — severity **High** (it strikes eight, at night, from a dead battery), mitigated by
the year guard now and by tuvm#103 eventually.

### 16.4 The strike trigger is Alarm2, not a 1 Hz square wave

Settled while specifying the wiring. Alarm2 with a minutes-match mask fires on a boundary
you set; after each strike the node writes the next half-hour into it and the MCU does
nothing at all until the boundary — no polling, no counting edges, no accumulated error, and
the wake instant is the RTC's own rather than the MCU's. A 1 Hz square wave is a poll loop
wearing a hardware hat, and a missed edge is silent.

This is consistent with §6's deletion of the disciplining loop: the RTC is not something to
measure against, it is the thing that says when.

Note for the build: INT/SQW stays asserted until `A2F` is cleared over I²C — the flag clear
deasserts the line, *not* the read of the time.

### 16.5 The bench part is a DS3231M, and its lid may be lying

§95 already distinguishes DS3231M (MEMS, ±5 ppm) from the ±2 ppm TCXO §97 specifies for the
run of five. The module on the bench is marked **DS3231M** — so the prototype's timebase is
the ±5 ppm class, which is ample for firmware development and is *not* the shipping part.

Additionally: its package is 16-pin, and a genuine DS3231M is 8-pin SO. Marking and package
disagree. Relabelled parts are common on these modules, so §10's "measured, not assumed"
discipline applies to this part's ppm specifically — measure it, do not read it off the lid.

---

## 17. ADDENDUM (2026-08-01) — the bench answered §12's gate, and §15.1 has an error

Bench session 2026-07-31/08-01, on the C6 rig of §16. Four things this settles and one it
corrects. Tracker: #102 (this design), #100 (the RTC seam), #105 (the open hazard).

### 17.1 §12's decision gate: the doorbell loses, the phone gong wins

§12's top risk was *"the chime unit sounds like a doorbell, not a bell — High, it is the whole
point."* Struck on the bench, it does. **The doorbell chime is out.** The acoustic element is
now **one of the two gongs from an old Bell telephone** — the fallback §12 already named — and
the striker is a **uxcell XRN-0530** push-pull solenoid (12 V, 5 N over 10 mm stroke). Photo and
listing: `uxcell_solenoid.png`, beside this file.

That solenoid does **≈0.05 J** of mechanical work at rated force, which is almost exactly §7.1's
"the hammer wants ~0.05 J". Encouraging, but it is a coincidence of specification, not a
measurement — §7's 8% efficiency claim is untouched by it.

### 17.2 §11.2 is answered, and its ladder was aimed too high

**10 ms throws the plunger solidly. 5 ms fails.**

§11.2 walks 100 → 50 → 30 → 25 → 20 → 15 → 10 ms on the assumption that 10 ms is near the
limit. It is not — the minimum clean pulse lies **between 5 and 10 ms, below the design's
floor**. That reduces strike energy proportionally and feeds straight back into §7's budget and
§9's 18650 sizing, both of which were computed against much longer pulses.

**This is a bracket, not a threshold.** Each rung was a single shot, and a solenoid near
threshold is precisely where shot-to-shot variation lives — plunger start position, residual
magnetism, supply sag. §10's discipline applies to this number as much as any other: the real
figure wants ~5 repeats per rung, with 12 V confirmed *under load* rather than open-circuit.

### 17.3 CORRECTION to §15.1 — the bench schematic has no base pull-down

**`2026-07-28-shipsbells-12v-bench-driver.svg` is missing a resistor**, and the omission is the
kind §8 spends three layers defending against.

R2 (10 kΩ, gate → +12 V) holds the *MOSFET* off when the NPN is off. **Nothing holds the NPN
off when the GPIO is not driven.** Note 2 on that schematic — *"GPIO low, or MCU unplugged →
MOSFET OFF"* — is true for a physically removed wire, i.e. a floating base. It is not true for
a pin sitting at an indeterminate reset level.

§11 item 3 *does* say to confirm a gate pull-down — but it says it about **"the blue MOSFET
module"**, a different candidate driver which has one. **The protection the design assumed was
present is on the part that was not used.** A 10 kΩ base-to-GND resistor is now fitted.

**The general rule, which belongs in §8 and not only here: a declared GPIO output is OFF only
while something is actively driving it that way.** For an LED that distinction is free. For a
coil it is not — and the windows in which nothing drives the pin are much longer than they look:

| window | GPIO driven? | duration |
|---|---|---|
| esptool download mode (the whole flash) | no | **seconds** |
| ROM boot → second-stage bootloader | no | ~100–200 ms |
| Zephyr init → wherever the app configures the pin | no | ~100 ms+ |

Firmware can shrink the last row and nothing else. Only a resistor at the transistor makes the
off-state a property of the circuit rather than of software being alive.

### 17.4 The striker's cadence is NOT the DAC bell's

The 160 ms / 600 ms pair (within a pair / between pairs) that `bell_strike_pattern` uses for the
MagTag's DAC bell was **"way too fast" on both gaps** when driving a physical bell.

Neither figure is wrong: a synthetic tone decays in a few hundred milliseconds, while a real
bell rings for seconds, so the rhythm that suits one crowds the other. **These are two separate
constants and the design should carry both.** Provisionally 500 / 1600 ms on the striker, not
yet settled by ear — it is a property of the bell on the bench, so it is tuned, not chosen.

### 17.5 Timekeeping decisions taken (detail in #100)

- **One NTP sync a day, a few minutes after midnight. One request; it sets date and time.** At
  ±5 ppm that is 0.43 s to correct, so no boundary-avoidance and no ratchet gymnastics are
  needed. Per-watch (6×/day) was considered and rejected: same work six times, buying
  detection-within-4-hours instead of within-a-day.
- **The bell never reads the date.** Ship's bells are a function of time-of-day alone. The date
  exists only because the RTC will not accept a time without one — so a wrong date is
  *inaudible*, and this is hygiene rather than correctness.
- **FTD, not MTD**, for this mains-powered version: +8 KB SRAM, and the mesh promoted it to
  Router. §9's battery version stays MTD, where not keeping the radio up is the entire point.

### 17.6 Still open

**A ~1 s coil energisation at boot, cause unknown** (#105). Firmware now configures the pin at
`PRE_KERNEL_1`, and the base measures ~0 V through reset — so something appears to be actively
*driving* GPIO18 high, not merely leaving it floating. The test that would localise it (drain
node, coil disconnected, watched through a full flash cycle) has not been run.

Deprioritised, not dismissed: the solenoid is disconnected and a piezo buzzer replaces it for
firmware work. **It must be answered before a coil is reconnected.**

## 18. ADDENDUM (2026-08-03) — the piezo bench rings, and it hears #105

Bench session 2026-08-03, C6 rig of §16 with §17.6's piezo now fitted and the coil off the
board. The pattern path is exercised end to end for the first time. Tracker: #102 (this
design), #105 (the boot hazard, now audible).

### 18.1 The driver is a 3.3 V low-side NPN, and the 12 V rail is gone

`2026-08-03-shipsbells-3v3-piezo-driver.svg` (and `.png`), beside this file. The IRF9Z14, R2
(10 k gate pull-up), R3 (1 k gate series) and the whole +12 V rail are **deleted**, not
bypassed — its note 1 is the reason: leave R2/R3 in with the MOSFET gone and +12 V still
reaches the collector through 11 kΩ, back-feeding both the piezo and the 3V3 rail. The 2N3904
is now the switch rather than a level shifter, and §17.3's 10 k base-to-GND resistor stays.

**Polarity is unchanged — still active-high.** No firmware inversion, and the overlay's
`GPIO_ACTIVE_HIGH` on GPIO18 keeps meaning de-energised-at-boot exactly as §8 requires.

The element is a continuous-tone piezo (3–24 V, 15 mA @ 12 V) running at the very bottom of its
range, ~3.1 V across it after V_CE(sat). Audibly quieter than 12 V would be, and loud enough
across a room.

### 18.2 What actually rang

Image `porta-v1-final-791-g500ff26-20260803T211803` — `ThreadNtpNode.nsl`, built
`TUVM_SOLENOID_CONSOLE=1`, no boot strike compiled in. Struck from the console by typing;
captured by the #56 collector, so the transcript below is a log read, not a claim from memory:

| typed | result |
|---|---|
| `10` | `strike: 10 ms fired on=1 off=1` |
| `b4` | 4 strikes, two pairs, set gap **1602 ms** |
| `b8` | 8 strikes, four pairs, set gaps **1603/1602/1602 ms** — 6.8 s end to end |
| `b3` | pair, then the **lone final stroke** after a full set gap |
| `b2030` | `bells: 2030 REFUSED (allowed 1..8)` |

Every strike answered `on=1 off=1`, which is the seam reporting the pin was *declared and
driven* rather than silently absent — the distinction `gpio_target_set`'s return exists for.

**`b3` is the rung worth keeping.** Three is the smallest count that exercises both branches of
`strike_bells`' gap choice AND the `break` before the trailing gap, so the odd tail — a bell
convention, not a loop detail — is now witnessed rather than assumed.

Pair gaps read 400–600 ms in the log against a 500 ms setting. That is **host-side line-arrival
jitter in the collector, not firmware jitter**: the set gaps land within 3 ms because they
straddle a longer quiet period. Timestamps from a log collector are evidence of ORDER and of
the long intervals; they are not a timing measurement, and nothing here should be read as one.

### 18.3 What this does NOT settle — three of them, and §17.4 is the one that matters

**§17.4's 500/1600 ms remains unsettled.** The cadence sounded right on the piezo and that is
not evidence about the bell: §17.4's whole argument is that gap length is a property of the
acoustic decay, and a continuous-tone piezo has none — it is a tone burst that stops when the
pin drops, the opposite end of the decay range from a gong that rings for seconds. The piezo can
prove the *pattern* (counts, pairs, the odd tail, the guards); only the gong can settle the
*rhythm*. Do not promote these numbers off this bench.

**§11.2's pulse bracket is untouched.** 10 ms here is a tone width, not a plunger throw; there
is no coil, so the 5–10 ms minimum-clean-pulse bracket stands exactly where §17.2 left it,
still owed its ~5 repeats per rung.

**The firmware caps are now the wrong shape for the load, and were deliberately left alone.**
`STRIKE_MAX_MS` 250, the 1 s cooldown and `BELL_MAX_WIDTH_MS` 50 all ration coil duty cycle
(§7's ~10 W while held). Schematic note 7: with no coil there is no heating and none of them
applies. They stay because **the coil is disconnected, not gone**, and a guard removed for a
bench session is a guard absent when the coil returns.

### 18.4 #105 is no longer inferred — it is audible

**The piezo beeps loudly at power-on and after every flash, in an image with no boot strike
compiled in.** `TUVM_SOLENOID_PULSE_MS` was unset, so `solenoid_oneshot()` is not in the binary;
nothing in this image strikes without a keystroke. §17.6 reached "something appears to be
actively driving GPIO18 high" from a base measuring ~0 V through reset. That inference is now a
sound anyone in the room can hear, on every power cycle.

This does not localise the cause and does not change §17.6's verdict — it **must** still be
answered before a coil is reconnected. What it changes is the cost of investigating: the fault
now announces itself, so the drain-node test can be run against something observable instead of
against a theory. Fitting the piezo turned an invisible hazard into an instrument.

### 18.5 The bell cannot ring itself, and this is the next gap to close

Stated plainly because the piezo working invites the opposite conclusion: **nothing in this
image rings the watch.** Three independent reasons, each verified on the flashed build —

- `ThreadNtpNode main:` reads the RTC, syncs, writes back, prints `NTP OK` and returns. No loop,
  no schedule.
- `CONFIG_RTC_ALARM is not set` in the image (`build-esp32c6-B/zephyr/.config`). §16.4 names
  Alarm2 as the strike trigger, and `prj_esp32c6.conf` holds it off deliberately until the alarm
  seam lands.
- `strike_console` is the only strike path compiled in, and it needs a UART. **Unplug the USB
  cables and the sole trigger leaves with them** — a wall-powered board would beep once at boot
  (§18.4) and then stay silent through every half hour.

The distance from here to a bell is therefore: the alarm seam (§16.4's Alarm2 on GPIO10, with
`CONFIG_RTC_ALARM=y` in the same commit), a node that computes the count from time-of-day, and
the strike path moved from C's console thread to the gpio seam from nsl, where §11.2 always said
the product path belongs.

## 19. AMENDMENT (2026-08-03, same evening) — the solenoid is off this device for good

**Stated by David after hearing the piezo ring: the solenoid is gone from this device, because
it sounds bad.** The piezo is not a stand-in on this board any more — it is the ringer. "Ugly,
but it works and you can hear it easily."

**This corrects §18.3 for this device.** That section kept §8's coil-era guards on the grounds
that "the coil is disconnected, not gone". On this board it is now gone, and the guards
(`STRIKE_MAX_MS`, the 1 s cooldown, the 50 ms pattern width cap) are guarding a load that is not
there. They are harmless where they sit — in `strike_console`, which §11.2 calls bench
instrumentation — so nothing needs deleting today; but no future work on this device should
reason about coil duty cycle, and §11.2's 5–10 ms bracket describes a load this board no longer
carries.

**The acoustic comparison, recorded because it is the reason and it is measurable by ear:**

| ringer | verdict |
|---|---|
| MagTag DAC beeper | **tiny** — audible only standing right beside it |
| doorbell chime (§12's gate) | sounds like a doorbell — rejected §17.1 |
| solenoid + telephone gong (§17.1) | **sounds bad** — off this device, 2026-08-03 |
| C6 piezo, 3.3 V NPN driver (§18.1) | ugly, but audible across a room, and it works |

**What this does NOT settle, and it is deliberately left open:** whether §17.1's gong-and-striker
decision survives for §3's run of five, or whether "the solenoid sounds bad" generalises off this
bench. §17.1 chose that pairing on evidence and this amendment was stated about *this device*.
The mechanical-striker alternative (§13) is untouched either way — it was always about *how the
hammer is released*, not about what it hits.

**Consequences that follow immediately.** #105's unexplained boot energisation is still real and
still audible (§18.4), but on this device it no longer gates a coil — it is a beep, not a burnt
winding, and its priority should be re-read in that light. §17.6's "must be answered before a
coil is reconnected" remains correct as written; there is simply no coil here to reconnect.

**Also settled this evening, for the always-on node** (design record:
`docs/design/2026-08-03-c6-shipsbell-always-on-node-design.md`): the device is wall-powered and
**never deep-sleeps** — it interrogates the RTC instead. The DS3231 interrupt line **is wired**,
and polling is a deliberate first step with the interrupt a later upgrade **on this same board**,
not only on §9's battery version. That narrows §16.4: Alarm2 is deferred here, not scoped away.

## 20. ADDENDUM (2026-08-04) — the bell rings itself. §18.5 is closed.

**§18.5 said "nothing in this image rings the watch". That is no longer true of this
device.** `zephyr/ShipsBellNodeC6.nsl` is flashed and running: it never returns, polls
the DS3231 once a second, derives the half-hour slot from the clock, and strikes the
watch through the gpio seam from nsl — which is where §11.2 always said the product
path belonged. Design record and the bench witness:
`docs/design/2026-08-03-c6-shipsbell-always-on-node-design.md` (§7 addendum).

Taking §18.5's three reasons in order:

- **`main:` returns** — the new node does not. It is a reactor: `pumpEvents` +
  `drain`, forever.
- **`CONFIG_RTC_ALARM is not set`** — still not set, and deliberately. §16.4's Alarm2
  is DEFERRED on this device, not scoped away (§19): the INT/SQW line is wired to
  GPIO10, and the node reacts to a `clock/tick` channel that a 1 s poll feeds today
  and an ISR can feed later with nothing above the channel changing. The cost of
  polling is one bounded thing: a strike lands up to a second late.
- **`strike_console` needs a UART** — it is still compiled in as the bench instrument
  §11.2 calls it, but it is no longer the only strike path. **Unplug the data cables
  and the bell keeps ringing.**

**Witnessed 2026-08-04 at the bench**, four boundaries at a 60 s test slot: 8, 1, 2, 3
bells, ascending, each struck once, each within 1.2 s of its boundary, each confirmed
by ear — including the odd tail on three. Then rebuilt at 1800 s.

**The cadence is now a result on this device, not a placeholder.** 10 ms width, 500 ms
pair gap, 1600 ms set gap — §3 of the node design said 50 ms, which was a misreading of
`BELL_MAX_WIDTH_MS`; 10 ms is what §18.2 actually rang. §18.3's refusal to promote
these numbers to a **gong** stands untouched, and under §19 there is no gong on this
board to promote them to.

**#105 is untouched and is now the first sound of every boot.** It fired on all three
of today's flashes. It gates a coil, and there is no coil here (§19) — but it is the
one part of this device that still rings without being asked.

## 21. ADDENDUM (2026-08-04) — §16.4's alarm is built, and the pin was GPIO11

**The interrupt trigger §16.4 settled is implemented, flashed and witnessed**, as a SECOND node
beside the poll one (`zephyr/ShipsBellAlarmNodeC6.nsl`; `ShipsBellNodeC6.nsl` is untouched and
stays the poll demonstration). Design record: `docs/design/2026-08-03-c6-shipsbell-always-on-node-design.md`.

**Alarm1, not Alarm2.** §16.4 named Alarm2; Alarm1 is the one with a SECONDS register, and the
node wants a lead measured in seconds — it wakes at boundary − 5 s and counts down to the
boundary with a local one-shot, so the strike instant is a timer over 5 s rather than an I²C
read inside the interrupt path. Everything else §16.4 settled is unchanged, including the
re-arm after every strike.

**§16.4's "a missed edge is silent" is answered by a WATCHDOG, not by a poll.** After each
strike the node also arms a one-shot for the next boundary + 2 s. If the alarm arrives, the
countdown rings and the watchdog finds the slot already rung; if it never arrives, the watchdog
rings 2 s late and PRINTS THAT IT DID. The MCU still holds exactly one armed timer between
boundaries, so §9's battery version is not foreclosed the way a backstop poll would foreclose
it. It is not a collision with an 8-bell pattern (6.88 s) because the ratchet advances when the
ring STARTS, not when it finishes.

### The pin is GPIO11. §1.2 of the next-steps doc and every reference above say GPIO10.

**The bench wire runs SQW → GPIO11.** Measured 2026-08-04, after the alarm failed to arrive at
four consecutive boundaries while every software layer reported success.

The evidence chain is worth keeping, because the fault was invisible from every direction but
one. `rtc_alarm_set_time` returned 0. The part was correct: `ctrl=45` → `INTCN=1`, `A1IE=1`, and
`status` showed **A1F setting on schedule, every cycle** — the DS3231 was matching the alarm and
pulling INT/SQW low exactly as asked. And GPIO10 read *inactive* at the same instant.

**An unconnected input with a pull-up is indistinguishable from an idle open-drain interrupt.**
Both read high forever. What separated them was reading the part's FLAG and the PIN in the same
breath: flag set AND line high is not a timing question, it is a wiring question. Neither
measurement alone would have said anything.

The watchdog carried the bell correctly through the entire diagnosis — every boundary rang, 2 s
late, with `watchdog: no alarm for slot NN` in the log. The feature that made the failure
survivable is the same one that reported it.

### 22. The wallwart resets the board, and the chirping is #105 on repeat

**Filed as #149.** On wallwart power alone the piezo chirps continuously. The remote
console (#56 §10) settled it by what was MISSING: nine minutes of audible chirping and
**not one line over Thread**, where a healthy boot reaches `thread: attached` in ~300 ms
and then talks. The board never gets far enough to attach — it is resetting in a tight
loop, and every reset fires §18.4/#105's ~1 s energisation of GPIO18. Corroborated
directly: chirping while a connector was moved, silence the instant it seated.

**RESOLVED 2026-08-04, and the candidate list above was wrong.** It was not the cable, the
connector or CC negotiation. **3.3 V was being brought directly into the C6 module**;
feeding **5 V and letting the onboard LDO make 3.3 V** stopped it. Both symptoms went at
once — the ~1 s startup chirp and the continuous chirping.

Verified by a **5 h 38 m unattended run with zero resets** (last boot 16:52:03; all six of
the day's boots precede it) and **12 consecutive correct strikes**, slots 34–45. #149
closed on that evidence.

The lesson is about the shape of the guess, not the guess: every candidate named above was
*downstream of the connector*, because the reproducer was "chirping while a connector was
moved, silence the instant it seated". That observation was real and pointed at the wrong
half — a marginal rail browns out on movement too, and the module was being fed a rail with
no headroom for the LDO to regulate away. **A 2 A rating is still not a delivery; it just
was not the delivery that failed here.**

**#105 is therefore a power-integrity readout.** On a board with no console, continuous
chirping means "I am resetting" — the second time on this rig that fitting an audible
load turned an invisible fault into an obvious one.

### Unplugging the data cables does not silence a console-driven striker

**REFUTED AS THE CAUSE of the chirping, kept as a real hazard.** It fitted the 0.5 s
cadence — a `bN` pattern's pair gap is 500 ms — and it was wrong: the current image has
`TUVM_SOLENOID_CONSOLE` compiled out entirely and still chirps (§22, #149). The hazard
below is genuine and the build change was right; it simply was not this fault.

Stated because §18.5 says the opposite by implication. `strike_console` reads the console UART;
pull the cable and the input does not go quiet, it goes UNDRIVEN, and a floating RX is noise a
strike parser will happily act on. An unattended board must not have `TUVM_SOLENOID_CONSOLE`
compiled in — the product strike path is the nsl gpio seam (§11.2, D3), and the console striker
is bench instrumentation that belongs on a bench.
