# The GPIO output + level-read seam

**Date:** 2026-07-23
**Status:** design, approved, not implemented
**Issue:** tuvm#82
**Follows:** `feat/magtag-esp32s2-port` — a6656d3 (chording via release events, proven
on hardware). The button seam is plural and debounced; nothing can drive a pin *out*.
**Companion:** `docs/design/2026-07-23-magtag-buttons-leds-design.md` — the board-level
doc. That one decides which pins exist and why; this one decides the seam that reaches
them. Its "Deferred" section proposed the level read; this document is that proposal
settled.

## Scope

Two primitives — a GPIO output write and a logical-level read — reachable from nsl,
working on both the host and the ESP32-S2 MagTag. That lights the red LED (GPIO13) and
makes the NeoPixel power gate (GPIO21) drivable.

**Out of scope:** the NeoPixels themselves. Those are WS2812 addressable LEDs needing a
precise-timing `led_strip`/RMT driver, which is not a pin level and is materially bigger.
This rung delivers their *prerequisite* — the power gate — and nothing more.

## The problem

The gpio seam exposes only `gpioPin:edge:post:on:` and `gpioFire:`. Both are inbound: an
edge becomes a channel post. There is no outbound direction at all, and no way to ask a
pin what it is doing right now.

Adding one is not a mechanical edit, which is why it was deliberately left undone by the
button work. `src/prim_ids.h` is an **append-only, counted-not-computed** table, and the
change spans two repos. The shape has to be right the first time.

## Decisions

### D1 — Two prims in one append, not one now and one later

`gpioSet:to:` **and** `gpioLevel:` land together.

The read is not a nice-to-have bundled for convenience. It serves both directions: it
reads back an output, and it answers *"is button A held right now"* — the zero-mailbox-
traffic answer to chording that a6656d3's release events are a proven stand-in for. It is
also what a woken node needs, because after a deep-sleep wake the first thing it sees is a
*level*, not an edge (companion doc, D7).

Landing them separately would pay the cross-repo append cost twice for one coherent idea.

**No `toggle:`.** It is `set: n to: (gpio level: n) not` once the read exists — one extra
round trip on a path nobody has shown to be hot, against a prim id that an append-only
table can never reclaim.

### D2 — Pin numbers are silkscreen GPIO numbers; polarity lives in the overlay

`n` is the board's GPIO number, matching the button seam's existing `gpio pin: 15 ...`
(itself chosen so an nsl node reads like the silkscreen). It is resolved against a table
of devicetree-declared specs and driven with `gpio_pin_set_dt` / `gpio_pin_get_dt`.

Those `_dt` calls are the decision. They honour the `GPIO_ACTIVE_LOW` flags the overlay
already declares and documents, so:

- `gpio set: 21 to: true` means **NeoPixels powered**, though the gate is ACTIVE_LOW.
- `gpio level: 15` returning `true` means **BUTTON_A is pressed**, though it is ACTIVE_LOW.

The rejected alternative — raw pin numbers with `gpio_pin_set_raw` — reaches any pin with
no overlay edit, but pushes polarity into every caller, turns the overlay's flags into
dead decoration, and lets a typo'd pin number drive an e-paper SPI line, since 35/36/37
sit on the same controller. A slot-index API (`gpio out: 0 set: true`) was also rejected:
it is board-portable by construction, but it discards the silkscreen addressing the button
seam deliberately chose, and forces every node to carry a board-specific slot key.

`level:` returns a **Boolean**, not 0/1 — `true` is "active", which is what the DT flag
already means.

### D3 — An undeclared pin raises, and the raise is written in nsl

A pin the overlay never declared is a **constant in the source that is wrong**, not a load
condition. It cannot be produced by traffic. Returning `false` for it would hand a caller
a plausible answer that is simply incorrect, and the node would carry on — the symptom
being an LED that looks broken, indistinguishable from the documented MagTag power-gate
trap. So it raises. Code that probes a board it does not know handles the exception.

*(An earlier draft argued for a non-fatal `false` by analogy with tuvm#20. The analogy was
wrong and is recorded here so it is not re-made: #20 removed *load* conditions — a full
mailbox, an exhausted timer table — that killed a node. A wrong pin literal is neither.)*

**The raise is written in nsl, not C.** `build_and_raise` is `static` in `src/dispatch.c`
and unreachable from `platform.c`; adding a C-side raise path would be new machinery. It
is also unnecessary, because the kernel already has this exact division — `Vector>>at:`
bounds-checks in nsl and raises `self error: 'index out of bounds'`
(`nsl-tuvm/lib/kernel.nsl:143`) over a raw C accessor.

So the C leaf returns `nil` for an undeclared pin — unambiguous, since a level is a
Boolean and can never be nil — and the `GpioPulse` wrapper converts:

```smalltalk
public set: n to: aBool = (
  (seam gpioSet: n to: aBool) == nil ifTrue: [ ^ self error: 'gpio pin not declared' ].
  ^ self )

public level: n = (
  | v | v := seam gpioLevel: n.
  v == nil ifTrue: [ ^ self error: 'gpio pin not declared' ].
  ^ v )
```

That yields a real crash Record with a tag `on:do:` can filter, and costs no prim id.

No `levelIfAbsent:` variant. If defensive probing ever needs one it is a two-line nsl
method over the same leaf — deliberately not built now.

### D4 — Device: a `gpioout*` alias table, mirroring `pulsegpio*`

`zephyr/src/main.c` gains a `gpio_outputs[]` table built from new `gpioout` /
`gpioout1..3` aliases via the same `DT_NODE_EXISTS` compile-out pattern that already
serves `pulsegpio1..3`. Four slots, matching the input side — the MagTag needs two, and
extending the ceiling later is one `#if` block. Boards that declare none compile the
table away entirely and are untouched — the nRF52840-DK and dongle included.

The MagTag overlay (`zephyr/boards/esp32s2_saola_esp32s2.overlay`) adds:

```
gpioout  = &red_led;         /* GPIO13 */
gpioout1 = &neopixel_power;  /* GPIO21 — the power gate, ACTIVE_LOW */
```

**The alias names never reach nsl.** A node names pin 13 and pin 21; `spec.pin` is what
the lookup matches on, exactly as `pulse_logical_of()` already derives a logical pin from
the spec on multi-button boards.

`set:to:` searches the outputs table. `level:` searches outputs **and** the existing
`pulse_inputs[]` — that second search is the whole chording payoff, and it is free,
because that table already exists and already holds the button specs.

Outputs are configured `GPIO_OUTPUT_INACTIVE` at boot, in a `wire_gpio_outputs()` beside
the existing `wire_pulse_gpio()`. Inactive, not active: the power gate must not come up
powered.

### D5 — Host: one pin→level table, independent of `gpioFire:`

`src/platform.c` keeps a pin→level table. `set:to:` creates a slot on first write. A pin
registered by `gpioPin:edge:post:on:` counts as declared, at initial level `false`, so a
host test can model a button without hardware.

`gpioFire:` stays **independent** of the level table: firing is an *edge event*, set/level
is the *line's state*. Conflating them would make it impossible to write the interesting
host test — "A held while B is pressed" — which needs a level set on one pin and an edge
fired on another.

This keeps the seam's decision logic host-testable, the same posture the companion doc's
D8 takes for the wake mask.

### D6 — No new source files

`src/platform.c` dispatches the two cases to `gpio_target_set(pin, level)` and
`gpio_target_level(pin)`, declared `extern` under `#ifdef __ZEPHYR__` — defined in
`zephyr/src/main.c` beside the DT tables — and defined as the D5 host table otherwise.

`__ZEPHYR__` conditional compilation inside a shared `src/*.c` is established precedent
(`src/gc.c`, `src/vm.c`), so this introduces no new mechanism. Crucially it adds **zero
new `.c` files**, which avoids the nine-enumerated-source-list edit that adding one costs
across the two repos (tuvm#63).

The direction is the mirror of the existing arrangement: today `platform.c` *defines*
`gpio_target_fire` and `main.c` calls it inbound; here `platform.c` *calls* outbound into
`main.c`.

### D7 — Chording migration is deferred until this rung is proven

a6656d3 works on hardware (`held=ABC`). The level read is the better mechanism — it costs
no mailbox traffic because the node asks only when it cares — but replacing a proven seam
in the same rung that first introduces its replacement risks losing both.

So: land the read, prove it on the bench, **then** open a follow-up to migrate chording
onto `level:` and delete `OEVM_BUTTON_RELEASE_BASE` (`zephyr/src/main.c`). Explicitly
gated on the LED rung being done.

## Touch list

Two repos. Prim ids **arrived at by counting `PRIM_NAMES`, not by arithmetic from 122** —
the standing rule at the top of `src/prim_ids.h`.

| File | Change |
|---|---|
| `src/prim_ids.h` | two ids, appended, counted, with the usual provenance comment |
| `src/dispatch.c` | two enum slots + two `PRIM_NAMES` entries, appended last |
| `src/platform.c` | two `platform_dispatch` cases; host level table; the `__ZEPHYR__` extern split |
| `zephyr/src/main.c` | `gpio_outputs[]`, `wire_gpio_outputs()`, `gpio_target_set`, `gpio_target_level` |
| `zephyr/boards/esp32s2_saola_esp32s2.overlay` | `gpioout` / `gpioout1` aliases |
| `nsl-tuvm/lib/kernel.nsl` | `GpioPulse` methods (~2090) **and** the platform seam accessors (~2322) |

## Verification

**Host** — an nsl node in the suite that: writes a pin and reads it back; reads a pin
registered as an input; and catches the raise on an undeclared pin via `on:do:`, asserting
the tag rather than merely that something failed.

**Device** — two gates on the MagTag over the soldered UART console:

1. Red LED follows BUTTON_A presses (`gpio set: 13 to: ...` driven from a reaction).
2. A node prints held state read via `gpio level:` for one button while another is
   pressed — the chording behaviour, obtained with no release events.

The power gate is verified only as *drivable* (`set: 21 to: true` returns without raising);
nothing lights until the ws2812 work, so a dark strip at this rung is expected, not a bug.

## Risks

1. **`level:` on an output reads back the driven state, not the pin's electrical
   reality.** `gpio_pin_get_dt` on an output configured without input buffering returns
   what was last written. Fine for the LED; worth knowing before anyone reads it as a
   fault detector.
2. **Boot state of the power gate.** `GPIO_OUTPUT_INACTIVE` on an ACTIVE_LOW gate drives
   the line HIGH, which is the vendor's "not using the NeoPixels" state. Correct — but it
   is the opposite physical level from the flag's name, and the kind of thing that gets
   "fixed" wrongly later.
3. **A node written against pin numbers is board-specific.** That is the accepted cost of
   D2's silkscreen addressing. The mitigation is that an undeclared pin raises loudly (D3)
   rather than silently doing nothing on the wrong board.
4. **`set:to:` cannot raise on the host, only on target.** D5 has the host create a slot on
   first write, because there is no overlay to check against — so a typo'd *output* pin
   passes every host test and raises on the board. Surfaced while planning the
   implementation, and accepted: the alternative is a host-side declaration list that
   nothing derives from the real board, which would be a second source of truth. The
   *level read* has no such gap, and is where the raise gets its host coverage.

## Close-out (promotion rule)

Not done until:

- `review/program-sequence.md` records the gpio seam gaining an outbound direction and a
  level read.
- `review/tests/CLAIMS.md` carries the host claim (raise on undeclared pin) and the two
  device gates.
- The chording follow-up issue is opened and linked from tuvm#82 (D7).
