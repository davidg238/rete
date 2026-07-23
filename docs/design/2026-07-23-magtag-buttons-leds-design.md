# MagTag buttons + LEDs, and the sleep-wake seam behind them

**Date:** 2026-07-23
**Status:** design, not implemented
**Follows:** `feat/magtag-esp32s2-port` (c3bec71) — the VM boots on the MagTag on
internal SRAM, `TIMER OK`, no board port written.

## Scope

Four buttons and two LED sources on the Adafruit MagTag (ESP32-S2-WROVER), under
Zephyr, reachable from nsl. The IL0373 e-paper is **out of scope** here and gets
its own doc. Deep sleep is in scope only as far as *deciding the seam* — the
running-node path is what gets built first.

## The one distinction that shapes everything

Two mechanisms are easy to conflate because porta/nodus name them with one
vocabulary. They are not the same layer and do not share machinery:

| Layer | Mechanism | Debounce? |
|---|---|---|
| **Running node** | GPIO ISR → `pulse_post_isr` → channel → reaction | Yes — this is where it belongs |
| **Sleeping node** | ext1 wake mask, no CPU running | Meaningless — there is no ISR to debounce |

A declaration like `gpio-low:11` is meaningful to **both**, which is why sharing
the vocabulary is worth doing. But the running-node path is a Zephyr GPIO
callback, and the sleep path is an ESP-IDF HAL call made *before* powering off.
Nothing is shared but the pin number and the direction.

Build the first row. The second row is a later rung and is not blocking.

## Decisions

### D1 — One channel per pin

Buttons are pin-addressed, matching the trigger vocabulary, which is pin-addressed
(`gpio-low:<pin>`). The alternative — one channel with four logical pins — needs a
demux in every consumer and has no compensating advantage now that per-pin
identity is the declared model elsewhere in the program.

### D2 — Borrow porta/nodus's *shape*, not its wire format

**nodus is v1, in Toit. This is v2, applying the lessons** (David, 2026-07-23) —
the same move already made when commands were dropped in favour of channels. So
the relationship is not "conform to a peer system"; it is "v1 taught us
something, and v2 is free to do better".

That has a direct consequence for every reference to porta/nodus in this
document: read them as **evidence about what worked**, never as a specification.
Where v1's model is wrong on current hardware (D5's ext1 note) or wrong in
principle, v2 diverges without apology. **Similar, not a perfect match** — take
the concepts, do not chase byte-level compatibility, and do not let a mismatch in
v1's docs block v2's design.

The idea worth taking is not the strings, it is that **triggers are declarative,
pin-addressed, and carry a direction — so one declaration can drive two
mechanisms**. `gpio-low:11` tells the running-node path which edge to wire *and*
tells the sleep path which ext1 bit to arm. That is the whole payoff, and it is
what makes the vocabulary worth borrowing even if tuvm never speaks porta's wire
format.

So: name things `gpio-low:<pin>` / `gpio-high:<pin>` because those names are good
and already mean something to this program's authors. MagTag buttons are
active-LOW, so a button is `gpio-low:<pin>`. Diverge freely where tuvm's needs
differ.

*(Footnote, deliberately not a blocker: `triggers.toit` parses and emits a colon
form while `nodus/tool/README.md:62` and `porta/docs/CLI.md:215` document an
equals form as "the flag-string grammar" — most likely a deliberate
CLI-flag-vs-map-key split. Irrelevant unless tuvm ever needs to interoperate on
the wire, which is not this design.)*

### D3 — Debounce in C, in the ISR — but not for the usual reason

The intuitive justification ("stop bounce flooding the channel") is **already
false in this codebase**, and it matters that the real reason is stated instead.

`pulse_post_isr` (`src/backend/zephyr/pulse_zephyr.c`) already:

- **coalesces** per `(chan_id, handle_idx)` — a source with a post still pending
  gains nothing from a second (tuvm#54/#55); and
- **sheds and counts** when the mailbox is full, never aborting
  (`src/pulse_backend.h:17`, settled under G2/tuvm#20).

So a bouncing button **cannot** flood the mailbox and **cannot** kill the node.

What coalescing cannot do is survive a pump. It merges only while an entry is
still queued; once `pulse_pump` drains it, the next bounce edge enqueues afresh.
So bounce still reaches the node as several *logical presses*.

**Therefore: debounce is a correctness requirement — one physical press must
equal one logical event — not a load-shedding measure.** Coalescing is
structurally incapable of providing it.

Implementation: per-pin last-accepted timestamp in the ISR, ignore edges within
~20–50 ms. Fire on the **first** edge and swallow the tail, so latency stays at
zero. Doing it in nsl instead would cost a `space_post_id` plus a full reaction
fire per bounce edge.

### D4 — Extend the existing logical-pin seam; do not replace it

`zephyr/src/main.c` already maps a **physical** pin to a **logical** pin
(`PULSE_LOGICAL_PIN 13`), described there as "mirroring how a driver maps a board
pin to an app-level channel". That indirection is exactly what plural buttons
need, and `pulse_isr` currently does `ARG_UNUSED(pins)` while Zephyr hands it a
**pin bitmask** — so one `gpio_callback` with a 4-bit mask serves all four
buttons and demuxes on `pins`. All four MagTag buttons are on one GPIO
controller, so this is a single callback.

**Keep `pulsegpio` as button 0 and add `pulsegpio1..3`.** The nRF DK and dongle
overlays use the singular alias; leaving it as index 0 means they keep working
untouched.

DT flags are load-bearing: the MagTag has **no on-board pull-ups**
(vendor guide), so each pin needs `GPIO_ACTIVE_LOW | GPIO_PULL_UP`. With those,
the existing `GPIO_INT_EDGE_TO_ACTIVE` already means the falling edge and the
line survives unchanged.

### D5 — Deep sleep: ext1 ANY_LOW, armed via the HAL from app code

Wake-on-any-button is directly supported on this part; the feared
"all four pressed at once" constraint does **not** apply to the S2.

- `soc_caps.h:432` — `SOC_PM_SUPPORT_EXT1_WAKEUP (1)`.
- `esp_sleep.h:37-40` — post-ESP32 chips define `ESP_EXT1_WAKEUP_ANY_LOW = 0`
  and `ANY_HIGH = 1`; `ALL_LOW` is **deprecated**: *"no longer supported after
  ESP32, please use ESP_EXT1_WAKEUP_ANY_LOW instead"*.
- All four buttons (GPIO 11/12/14/15) are marked **RTC** on the vendor pinout and
  sit inside the S2's RTC-GPIO range, so they are ext1-eligible.

**Zephyr does not expose this.** `esp32s2/Kconfig:13` gives `select HAS_POWEROFF`,
but `poweroff.c` is 15 lines (RTC domain on, `esp_deep_sleep_start()`) with **no
wake source**, `power.c` handles only light-sleep standby, and `grep ext1` across
`zephyr/soc/espressif/` returns nothing. So the app calls
`esp_sleep_enable_ext1_wakeup()` from the HAL — which is already linked — before
`sys_poweroff()`. Contained app C, not a Zephyr patch.

**Cross-repo note, not fixed here:** `nodus/src/wake_arm.toit` models ext1 as
"one global ANY_HIGH or ALL_LOW" with a boolean `on-any-high`. On S2/S3/C6 mode
`0` is ANY_LOW, not ALL_LOW. The code passes the right value so it works — and
behaves *better* than documented — but the stated semantics are wrong on every
chip after the original ESP32.

**Also before sleeping:** drive the NeoPixel power gate (GPIO21, itself RTC) to
input/HIGH. Per the vendor guide, leaving it output-LOW keeps the NeoPixels'
quiescent draw alive through deep sleep, which defeats the point.

### D6 — Declare the node's halting shape; do not infer it

The second idea worth lifting from porta, and the one with immediate payoff.

porta declares a per-app `lifecycle` — `run-once` or `run-loop` — with the
reasoning stated plainly: *"The halting behaviour of a container cannot be
inferred, which is why `lifecycle` is declared at install rather than guessed"*
(`porta/docs/ARCHITECTURE.md:158-168`).

**tuvm already has this distinction, implicitly, and it is already a hazard.**
`zephyr/TimerNode.nsl` documents it in its own comment: *"this node RETURNS, so
it stops draining… Every other node blocks forever draining, so this node is the
only one that can leak a live repeater."* The leak is possible **because** the
shape is implicit — nothing declares that TimerNode halts, so nothing can check
that a repeater outlives its drainer. Declaring the shape turns a comment into
something enforceable.

Not a prerequisite for buttons. Recorded here because the button work is what
surfaced it, and because D7 below cannot be reasoned about without it.

### D7 — Deep sleep implies run-once, so the sleeping button node is a cycle

porta again: *"a `run-loop` daemon on a `deep-sleep` node is killed by each
sleep, so a long-lived daemon requires `always-on`"* (`ARCHITECTURE.md:165-168`).
Power mode and halting shape are not independent.

For the MagTag that settles the node's shape once sleep is in play. A sleeping
button node cannot be a `whileTrue:` drain loop, because the sleep destroys it.
It is a **cycle**: wake → identify which pin woke → handle → re-arm → sleep. The
ext1 wake replaces `pulse_pump`'s blocking `K_FOREVER` as the thing the node
idles on.

Two consequences worth stating before anyone writes the node:

- **The ISR/debounce path (D3) does not exist during sleep.** There is no CPU. The
  wake is a level condition, and the first thing the woken node sees is a *level*,
  not an edge — so it must read pin state on wake rather than expect a pulse.
  Debounce still matters afterwards, for the run-time portion of the cycle.
- **Which pin woke you is a separate question** from which pins are armed. ext1
  reports a wake-cause bitmask; that read is what maps back to a per-pin channel
  (D1). Without it, a four-button ANY_LOW wake tells you only *that* a button was
  pressed.

### D8 — Resolve the wake mask as a pure function

`nodus/src/wake_arm.toit` is worth copying as a *pattern* regardless of its
semantics drift: the arming decision is a **pure function** of the declared pins
plus the pins' current levels, with the actual `esp_sleep_enable_ext1_wakeup()`
left on-device. That makes the interesting part — which pins, which direction,
what happens when declarations conflict — testable on the host with no hardware.

Given tuvm's existing host test suites, this is the shape to use. The device-side
residue is then a single HAL call that is hard to get wrong.

## LEDs

- **Red LED** — GPIO13, plain `gpio-leds`, nothing special.
- **NeoPixels** — 4×, data GPIO1, and a **power gate on GPIO21 that must be
  driven output-LOW or they stay dark**. This gate is not part of any standard
  Zephyr LED binding, so it needs an explicit GPIO in the overlay/app. Expect to
  waste time here if it is forgotten — the symptom is a strip that looks broken.

## Deferred, but deliberately not designed out

**Chording** ("hold A, press B"). Current `EDGE_TO_ACTIVE` is press-only: the node
gets a doorbell, never a state. The obvious route — `EDGE_BOTH` with separate
press/release logical pins — doubles event rate and forces debouncing *release*
too, which typically bounces worse than press.

The better route is to not make it an event: expose a **level read**
(`gpio_pin_get_dt`) as "is button N down right now". Zero mailbox traffic;
chording becomes *"on B pressed, ask whether A is held"*. This composes with
press-only events rather than replacing them, so building press-only now costs
nothing later. A level-read primitive is not a new concept in this program —
`nodus`'s `resolve-wake-arm` already takes `--levels/Map` read on device.

## Risks

1. **GPIO15 is also XTAL_32K_P.** BUTTON_A dies if anything ever enables a 32 kHz
   crystal. The MagTag fits none, so this is latent, not live — but it is a
   silent failure if it ever changes.
2. **Debounce window is a guess.** 20–50 ms is conventional; these specific
   switches have not been measured. Measure before fixing a number in code.
3. **The stale-comment cluster.** `zephyr/TimerNode.nsl` and
   `src/backend/zephyr/timer_zephyr.c:71` both still describe `pulse_post_isr` as
   k_oops-ing on a full mailbox, which has been false since G2/tuvm#20. Worse,
   `timer_zephyr.c:71` still *does* `k_oops()` on timer-table exhaustion,
   justified as "exactly as pulse_post_isr does" — a load condition killing the
   node, which is the class of bug #20 set out to remove. Not part of this work,
   but it sits directly under the seam this design extends.

## Close-out (promotion rule)

Not done until: `zephyr/docs/BOARD_PORTS.md` gains the MagTag row — including
that the Zephyr SDK needs `xtensa-espressif_esp32s2_zephyr-elf` installed, which
is outside the repo and invisible to git — and the ladder records the button seam
going plural.
