/* The gpio leaves of the board (rete#440 R6a): the digital pins a node reads and drives,
   and the two wire-up entries main() calls at boot. Moved verbatim from zephyr/src/main.c;
   the behaviour is unchanged. gpio_target_fire lives in src/platform.c. */
#include <zephyr/kernel.h>
#include <zephyr/init.h>          /* SYS_INIT — gpio_outputs_early_init, below */
#include <zephyr/drivers/gpio.h>
#include <stdbool.h>
#include <stdint.h>
#include "target_backend.h"

/* Task 8 (nRF52840-DK acceptance bar): bind the on-board BUTTON1 (P0.11) as the
   PHYSICAL edge source for the pulse seam, via the `pulsegpio` DT alias added in
   boards/nrf52840dk_nrf52840.overlay. Guarded on the alias's existence: for the
   native_sim gate (Task 7) no overlay is applied, so the alias is absent and this
   whole block — and the wire_pulse_gpio() call in main() — compiles out, leaving
   that build byte-for-byte identical (its edge still comes from the node's
   sim-only `gpio fire:`). On the DK, each button press runs pulse_isr in IRQ
   context, which posts the pulse for the SAME logical pin the baked node
   (PulseNodeDK.nsl) registered. */
#if DT_NODE_EXISTS(DT_ALIAS(pulsegpio))

/* Logical pin the DK node registers via `gpio pin: 13 ...` (PulseNodeDK.nsl).
   The PHYSICAL pin (P0.11) is decoupled from it — the ISR maps one to the
   other, mirroring how a driver maps a board pin to an app-level channel.
   Used ONLY on single-button boards; see pulse_logical_of() below. */
#define PULSE_LOGICAL_PIN 13

/* Debounce window, milliseconds. See
   docs/design/2026-07-23-magtag-buttons-leds-design.md D3 for WHY this lives in
   C rather than nsl, and note the reason is NOT the obvious one:
   pulse_post_isr already coalesces per (chan_id, handle_idx) and sheds rather
   than aborting when full (src/pulse_backend.h:17, G2/tuvm#20), so bounce can
   neither flood the mailbox nor kill the node. What coalescing CANNOT do is
   survive a pump — it merges only while an entry is still queued, so once
   pulse_pump drains it the next bounce edge enqueues afresh and the node sees
   several logical presses. Debounce here is therefore a CORRECTNESS measure
   (one physical press == one logical event), not load shedding.
   30 ms is conventional and NOT measured on these particular switches. */
#ifndef OEVM_BUTTON_DEBOUNCE_MS
#define OEVM_BUTTON_DEBOUNCE_MS 30
#endif

/* THE RELEASE EDGE IS INTERNAL, not an event. The debounce state machine below
   watches BOTH edges (see GPIO_INT_EDGE_BOTH in wire_pulse_gpio) — but only the
   press fires a logical pin. The release edge is consumed here purely to re-arm
   the state machine (`in->pressed = false`); it produces nothing a node can see.

   Held-state / chording ("hold A, press B") is answered by `gpio level:` instead
   (tuvm#82), which a node reads only when it cares — MagTagChordNode.nsl reads
   levels 15/14/12/11 on a press. The earlier release-event route (a settled
   release firing logical + OEVM_BUTTON_RELEASE_BASE, 100) doubled the event rate
   to carry state and was retired in tuvm#84; that is why no release pin is fired
   below and the OEVM_BUTTON_RELEASE_BASE macro is gone. */
extern void gpio_target_fire(int pin);   /* src/platform.c — ISR-safe */

/* One entry per button. `logical` is the pin an nsl node names in
   `gpio pin: N edge: ... post: ... on: <channel>`; src/platform.c's
   host_gpio_register table is already keyed per logical pin with its OWN channel
   and OWN posted message, so four buttons need no VM- or nsl-side change at all
   — only this file arms four physical pins instead of one. */
struct pulse_input {
    struct gpio_dt_spec spec;
    struct gpio_callback cb;
    int      logical;
    uint32_t last_ms;   /* uptime of the last ACCEPTED transition */
    bool     seen;      /* false until the first accepted transition (0 is a real uptime) */
    bool     pressed;   /* debounced state; a press fires only on released -> pressed */
};

static struct pulse_input pulse_inputs[] = {
    { .spec = GPIO_DT_SPEC_GET(DT_ALIAS(pulsegpio), gpios) },
#if DT_NODE_EXISTS(DT_ALIAS(pulsegpio1))
    { .spec = GPIO_DT_SPEC_GET(DT_ALIAS(pulsegpio1), gpios) },
#endif
#if DT_NODE_EXISTS(DT_ALIAS(pulsegpio2))
    { .spec = GPIO_DT_SPEC_GET(DT_ALIAS(pulsegpio2), gpios) },
#endif
#if DT_NODE_EXISTS(DT_ALIAS(pulsegpio3))
    { .spec = GPIO_DT_SPEC_GET(DT_ALIAS(pulsegpio3), gpios) },
#endif
};
#define PULSE_N_INPUTS ARRAY_SIZE(pulse_inputs)

/* LOGICAL PIN POLICY, and why it is conditional rather than uniform.
   Single-button boards (nRF52840-DK P0.11, dongle P1.06) keep the historic 13,
   because PulseNodeDK.nsl and ThreadTimerNode already register `gpio pin: 13`
   and those are proven rung-7/8 gates — renumbering them here would break a
   working node to no purpose. Multi-button boards have no such legacy, so they
   take the DT spec's own GPIO number, which makes an nsl node read like the
   board silkscreen (`gpio pin: 15` IS the MagTag's BUTTON_A). Deriving it from
   the spec rather than hardcoding a per-board table also keeps board knowledge
   out of this shared file. */
static inline int pulse_logical_of(const struct pulse_input *in) {
    return (PULSE_N_INPUTS == 1) ? PULSE_LOGICAL_PIN : (int)in->spec.pin;
}

static void pulse_isr(const struct device *port, struct gpio_callback *cb,
                      uint32_t pins) {
    ARG_UNUSED(cb);
    uint32_t now = k_uptime_get_32();

    for (size_t i = 0; i < PULSE_N_INPUTS; i++) {
        struct pulse_input *in = &pulse_inputs[i];
        /* Demux: one callback per input, but the handler is shared, so match on
           (port, pin) rather than trusting which cb fired. Zephyr passes the pin
           bitmask in `pins` — the single-button version ignored it. */
        if (in->spec.port != port || !(pins & BIT(in->spec.pin))) {
            continue;
        }
        /* DEBOUNCED PRESS/RELEASE STATE MACHINE. Arrived at by measurement, after
           two simpler designs were tried on hardware and failed:

           (1) Edge-only ("fire on every interrupt, lock out for 30 ms"). The seam
               fired TWICE per press: hold BUTTON_A for 3 s and the second event
               lands 2944 ms later, exactly at release. Ten deliberate presses gave
               thirteen events, the extras 196/255/280 ms after their press — one
               per press held longer than the lockout, since a shorter press has
               its release swallowed by the window. That is why it first looked
               like intermittent contact bounce and is not: GPIO_INT_EDGE_TO_ACTIVE
               does NOT mean press-only here, despite Zephyr inverting the trigger
               for ACTIVE_LOW pins and gpio_esp32.c mapping TRIG_LOW -> NEGEDGE.
               A time lockout CANNOT fix this: the release happens an arbitrary
               time after the press, so no window is wide enough.

           (2) Edge + level check ("ignore interrupts where the pin reads
               inactive"). Better but still wrong: five 3 s holds gave EIGHT
               events, i.e. three releases still leaked. Sampling the level at ISR
               entry is a coin flip against a bouncing contact — the release
               bounces, and roughly half the time the ISR samples during a brief
               active glitch and fires.

           So: track the debounced STATE, and require a settled release before
           another press can fire. A press fires only on a genuine
           released -> pressed transition; bounce in either direction lands inside
           the 30 ms window and is absorbed.

           This needs a release interrupt to re-arm, which is exactly why
           GPIO_INT_EDGE_BOTH is requested in wire_pulse_gpio() rather than
           EDGE_TO_ACTIVE. Asking for both edges makes the re-arm GUARANTEED on any
           compliant driver instead of depending on what edge-to-active happens to
           mean — which is the assumption that broke here in the first place.
           gpio_pin_get_dt returns the LOGICAL level (1 == active), honouring
           GPIO_ACTIVE_LOW from the devicetree, so this stays polarity-correct on
           every board.

           Unsigned subtraction is wrap-correct across the ~49.7-day
           k_uptime_get_32() rollover. */
        int lvl = gpio_pin_get_dt(&in->spec);
        if (lvl < 0) {
            continue;   /* read failed; nothing sane to report from an ISR */
        }
        if (in->seen && (uint32_t)(now - in->last_ms) < OEVM_BUTTON_DEBOUNCE_MS) {
            continue;   /* still settling — absorbs bounce on BOTH edges */
        }
        if (!in->pressed) {
            if (lvl != 1) {
                continue;   /* already released; nothing to do */
            }
            in->pressed = true;
            in->seen    = true;
            in->last_ms = now;
            gpio_target_fire(pulse_logical_of(in));   /* -> pulse_post_isr (non-blocking) */
        } else {
            if (lvl != 0) {
                continue;   /* still held */
            }
            in->pressed = false;   /* settled release: re-arm, no event fired */
            in->seen    = true;
            in->last_ms = now;
            /* Release fires nothing: held-state is read via `gpio level:` (#82),
               not carried on a release event (#84). The transition above is the
               whole purpose of watching this edge — it re-arms the next press. */
        }
    }
}

void wire_pulse_gpio(void) {
    for (size_t i = 0; i < PULSE_N_INPUTS; i++) {
        struct pulse_input *in = &pulse_inputs[i];

        if (!gpio_is_ready_dt(&in->spec)) {
            printk("pulsegpio[%u] device not ready\n", (unsigned)i);
            continue;   /* one dead pin must not cost the others */
        }
        in->logical = pulse_logical_of(in);
        gpio_pin_configure_dt(&in->spec, GPIO_INPUT);
        /* BOTH edges, not EDGE_TO_ACTIVE — deliberate, and the reason is measured
           rather than stylistic. pulse_isr fires a press only on a debounced
           released -> pressed transition, so it MUST see the release in order to
           re-arm. Requesting both edges makes that guaranteed on any compliant
           driver, instead of depending on what edge-to-active happens to mean on a
           given one: on this board EDGE_TO_ACTIVE already delivered release
           interrupts anyway, which is precisely the bug that made a press fire
           twice. Asking for what we actually need removes the assumption. */
        gpio_pin_interrupt_configure_dt(&in->spec, GPIO_INT_EDGE_BOTH);
        /* A callback per input rather than one shared mask: gpio_add_callback is
           per-port, and nothing guarantees a board puts every button on one port
           (the MagTag does; the next board might not). */
        gpio_init_callback(&in->cb, pulse_isr, BIT(in->spec.pin));
        gpio_add_callback(in->spec.port, &in->cb);
    }
}
#else
void wire_pulse_gpio(void) { }   /* native_sim: no physical button */
#endif

/* tuvm#82: the OUTBOUND half of the gpio seam. Outputs are declared as gpioout /
   gpioout1..3 aliases, exactly as inputs are declared as pulsegpio / pulsegpio1..3,
   and the whole table compiles away on a board that declares none. An nsl node
   names the SILKSCREEN gpio number; the alias names never leave this file, which is
   why the lookup below matches on spec.pin. */
#if DT_NODE_EXISTS(DT_ALIAS(gpioout))
static const struct gpio_dt_spec gpio_outputs[] = {
    GPIO_DT_SPEC_GET(DT_ALIAS(gpioout), gpios),
#if DT_NODE_EXISTS(DT_ALIAS(gpioout1))
    GPIO_DT_SPEC_GET(DT_ALIAS(gpioout1), gpios),
#endif
#if DT_NODE_EXISTS(DT_ALIAS(gpioout2))
    GPIO_DT_SPEC_GET(DT_ALIAS(gpioout2), gpios),
#endif
#if DT_NODE_EXISTS(DT_ALIAS(gpioout3))
    GPIO_DT_SPEC_GET(DT_ALIAS(gpioout3), gpios),
#endif
};
#define GPIO_N_OUTPUTS ARRAY_SIZE(gpio_outputs)

void wire_gpio_outputs(void) {
    for (size_t i = 0; i < GPIO_N_OUTPUTS; i++) {
        if (!gpio_is_ready_dt(&gpio_outputs[i])) {
            printk("gpioout[%u] device not ready\n", (unsigned)i);
            continue;   /* one dead pin must not cost the others */
        }
        /* INACTIVE, not ACTIVE. On the MagTag gpioout1 is the NeoPixel POWER GATE,
           declared GPIO_ACTIVE_LOW, and coming up powered is exactly the quiescent
           draw the vendor guide says to avoid. INACTIVE therefore drives that line
           physically HIGH — the opposite of what the flag's name suggests, and
           correct. Do not "fix" this to GPIO_OUTPUT_ACTIVE. */
        gpio_pin_configure_dt(&gpio_outputs[i], GPIO_OUTPUT_INACTIVE);
    }
}
/* AS EARLY AS THE OS ALLOWS, because a declared output is a load that is OFF only
 * while something drives it that way (2026-07-31, shipsBellsV3 bench).
 *
 * Calling this from main() — where it used to live, and still is, harmlessly — is
 * far too late for anything that can be damaged by being left on. The 12 V strike
 * driver AS WIRED IN JULY 2026 had no base pull-down on its level-shifter, so its
 * coil was energised for every microsecond GPIO18 was not actively held low;
 * main() runs after ALL init levels, after kernel_boot() and after
 * oimg_load_blob(), which on this board is hundreds of milliseconds of coil.
 * (That rig is gone — the 12 V solenoid was retired for a 3.3 V piezo, and that
 * driver DOES have a base pull-down. Read the paragraph below before acting on
 * this one: the two rigs have been conflated twice and it cost a diagnosis both
 * times.)
 *
 * PRE_KERNEL_1 at priority 50: CONFIG_GPIO_INIT_PRIORITY defaults to
 * KERNEL_INIT_PRIORITY_DEFAULT (40, drivers/gpio/Kconfig:48), so 50 is the first
 * slot at which gpio_pin_configure_dt() is legal — the controller exists and
 * nothing else has run.
 *
 * THIS IS A MITIGATION, NOT A FIX, and the distinction matters enough to write
 * down. It cannot help during esptool's download mode, the ROM bootloader, a
 * brown-out, or a CPU that never reaches init at all — and the ROM downloader is
 * not a window at all, it is a STABLE state that waits for a host forever
 * (rete#201: two minutes of continuous bell at 11pm on 2026-08-11).
 *
 * A pull-down at the transistor is the right shape of answer, and IT IS ALREADY
 * FITTED — R2, 47k base-to-GND, on the 3.3 V piezo driver since 2026-08-05. It
 * closes the reset case and it did NOT close the download-mode case, so do not
 * write "only a pull-down fixes this" here again. With a genuinely high-Z pin R2
 * holds the base at 0 V; the bell rang anyway, so in download mode GPIO18 is
 * either driven high or weakly pulled up — and against a ~45k pull-up R1/R2
 * (10k/47k) leave the base at ~1.5 V, hard on. Unresolved, rete#105/#201; the
 * settling measurement is a meter on GPIO18 with the board parked in
 * boot:0x66 DOWNLOAD. See zephyr/boards/shipsBells/2026-07-26-shipsbellsv3-design.md §8:
 * "Never let a crashed VM be the only thing between a GPIO and a burnt coil."
 */
static int gpio_outputs_early_init(void) {
    wire_gpio_outputs();
    return 0;
}
SYS_INIT(gpio_outputs_early_init, PRE_KERNEL_1, 50);
#else
void wire_gpio_outputs(void) { }
#endif

/* The two leaves src/platform.c calls under #ifdef __ZEPHYR__. Return conventions
   match the host definitions they replace: set answers 1/0, level answers 0/1/-1,
   where -1 means "no board declared this pin" and becomes nil in nsl.
   LOGICAL levels throughout (gpio_pin_set_dt / _get_dt), so the overlay's
   GPIO_ACTIVE_LOW flags carry the polarity: true == NeoPixels POWERED, and
   true == button PRESSED. */
int gpio_target_set(int pin, int level) {
#if DT_NODE_EXISTS(DT_ALIAS(gpioout))
    for (size_t i = 0; i < GPIO_N_OUTPUTS; i++)
        if ((int)gpio_outputs[i].pin == pin)
            return gpio_pin_set_dt(&gpio_outputs[i], level) == 0;
#endif
    ARG_UNUSED(pin); ARG_UNUSED(level);
    return 0;   /* not declared as an output on this board */
}

int gpio_target_level(int pin) {
#if DT_NODE_EXISTS(DT_ALIAS(gpioout))
    for (size_t i = 0; i < GPIO_N_OUTPUTS; i++)
        if ((int)gpio_outputs[i].pin == pin) {
            int v = gpio_pin_get_dt(&gpio_outputs[i]);
            return (v < 0) ? -1 : (v != 0);
        }
#endif
#if DT_NODE_EXISTS(DT_ALIAS(pulsegpio))
    /* Searching the INPUT table too is the whole chording payoff: a node can ask
       "is BUTTON_A held right now" for zero mailbox traffic — the sole chording
       route since #84 retired the doubled press/release event stream. The table
       already exists and already holds the button specs, so this costs nothing.
       Match on the LOGICAL pin (pulse_logical_of), not spec.pin: a node names the
       logical pin it registered with `gpio pin: N ...`, and on single-button boards
       that pin is decoupled from the physical one (PULSE_LOGICAL_PIN 13 vs P0.11).
       Matching on spec.pin would make `gpio level: 13` on the nRF DK find nothing
       and raise #gpioUndeclared for a button that IS registered — and disagree with
       the host, which keys host_gpio_register by the logical pin. The READ still
       goes through the physical spec. On the MagTag logical==spec.pin, so this is
       identity there; it fixes the single-button case. */
    for (size_t i = 0; i < PULSE_N_INPUTS; i++)
        if (pulse_logical_of(&pulse_inputs[i]) == pin) {
            int v = gpio_pin_get_dt(&pulse_inputs[i].spec);
            return (v < 0) ? -1 : (v != 0);
        }
#endif
    ARG_UNUSED(pin);
    return -1;
}
