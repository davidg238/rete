/* tuvm on Zephyr — native_sim bring-up gate for the ISR->channel pulse seam
   (Task 7). Boots the solidified VM (Model A: a per-program baked image, no
   .oimg blob at all — see tools/ns_compile_oevm.py), then runs the image's
   own `main:`, which registers a channel reaction, fires one simulated GPIO
   edge, and loops draining the pulse mailbox via
   `platform pumpEvents. space drain` (see ../PulseNode.ns). On native_sim
   the zephyr pulse backend's pulse_pump (src/backend/zephyr/pulse_zephyr.c)
   BLOCKS (K_FOREVER) once the one-edge mailbox is drained, so
   run_solidified_entry_point() never returns for this gate node — that is
   expected (the gate is checked by console output under `timeout`, not by
   process exit).

   This boot sequence mirrors tuvm/src/oevm_main.c (the host Model-A
   appliance driver) EXACTLY, swapping only the console (wired through
   src/backend/zephyr/platform_zephyr.c) and the in-language millis clock
   (k_uptime_get() instead of clock_gettime(), via the oevm_millis hook
   src/vm.c already reserves for "the Zephyr appliance"). Unlike the sibling
   oevm project's ns-zephyr/app/src/main.c, tuvm's GC does NOT do a
   conservative C-stack root scan (no gc_set_stack_base function exists in
   this engine — GC roots come from vm_stack/current_frame plus the explicit
   hooks below), so no stack-anchor call is needed here. */
#include <zephyr/kernel.h>
#include <stdint.h>
#include <stdio.h>
#include "oevm.h"
#include "oimg.h"

/* Task 8 (nRF52840-DK acceptance bar): bind the on-board BUTTON1 (P0.11) as the
   PHYSICAL edge source for the pulse seam, via the `pulsegpio` DT alias added in
   boards/nrf52840dk_nrf52840.overlay. Guarded on the alias's existence: for the
   native_sim gate (Task 7) no overlay is applied, so the alias is absent and this
   whole block — and the wire_pulse_gpio() call in main() — compiles out, leaving
   that build byte-for-byte identical (its edge still comes from the node's
   sim-only `gpio fire:`). On the DK, each button press runs pulse_isr in IRQ
   context, which posts the pulse for the SAME logical pin the baked node
   (PulseNodeDK.ns) registered. */
#if DT_NODE_EXISTS(DT_ALIAS(pulsegpio))
#include <zephyr/drivers/gpio.h>

/* Logical pin the DK node registers via `gpio pin: 13 ...` (PulseNodeDK.ns).
   The PHYSICAL pin (P0.11) is decoupled from it — the ISR maps one to the
   other, mirroring how a driver maps a board pin to an app-level channel. */
#define PULSE_LOGICAL_PIN 13

extern void gpio_target_fire(int pin);   /* src/platform.c — ISR-safe */

static const struct gpio_dt_spec pulse_in =
    GPIO_DT_SPEC_GET(DT_ALIAS(pulsegpio), gpios);
static struct gpio_callback pulse_cb;

static void pulse_isr(const struct device *port, struct gpio_callback *cb,
                      uint32_t pins) {
    ARG_UNUSED(port); ARG_UNUSED(cb); ARG_UNUSED(pins);
    gpio_target_fire(PULSE_LOGICAL_PIN);   /* -> pulse_post_isr (non-blocking) */
}

static void wire_pulse_gpio(void) {
    if (!gpio_is_ready_dt(&pulse_in)) {
        printk("pulsegpio device not ready\n");
        return;
    }
    gpio_pin_configure_dt(&pulse_in, GPIO_INPUT);
    gpio_pin_interrupt_configure_dt(&pulse_in, GPIO_INT_EDGE_TO_ACTIVE);
    gpio_init_callback(&pulse_cb, pulse_isr, BIT(pulse_in.pin));
    gpio_add_callback(pulse_in.port, &pulse_cb);
}
#else
static inline void wire_pulse_gpio(void) { }   /* native_sim: no physical button */
#endif

/* XIP stubs — Model A never loads an .oimg blob; oimg.h's is_in_blob() checks
   oimg_blob_base first and returns 0 while it is NULL, so the XIP accessor
   paths are never taken. These definitions must live here (mirroring
   src/oevm_main.c) because src/oimg_loader.c is deliberately NOT linked into
   this build — see zephyr/CMakeLists.txt's comment for why. */
const uint8_t *oimg_blob_base = NULL;
const uint8_t *oimg_blob_end  = NULL;
oop builtin_registry[BUILTIN_COUNT] = {0};
oop oimg_singleton[3] = {0};

extern void init_solidified_vtables(void);
extern void init_reflection_symbols(void);
extern oop  run_solidified_entry_point(void);
extern void (*gc_additional_roots_hook)(void);
extern uint64_t (*oevm_millis)(void);

static void oevm_roots_callback(void) {
    /* Standard GC roots (vm_stack, current_frame, g_active_stack) are scanned
       automatically by gc.c; this gate needs no additional roots. */
}

static uint64_t zephyr_millis(void) { return (uint64_t)k_uptime_get(); }

int main(void) {
    /* Unbuffered stdout: native_sim's console is a thin wrapper over the
       host's own stdout. Without this, libc line-buffers and a crash
       mid-run could hide output that already happened in-language. */
    setvbuf(stdout, NULL, _IONBF, 0);

    gc_additional_roots_hook = oevm_roots_callback;
    oevm_millis = zephyr_millis;

    init_solidified_vtables();
    init_reflection_symbols();

    wire_pulse_gpio();   /* Task 8 (DK): arm BUTTON1 as the pulse edge; no-op on native_sim */

    oop result = run_solidified_entry_point();  /* runs PulseNode>>main:args: —
        registers the reaction, fires the simulated edge, then blocks forever
        in [ true ] whileTrue: [ platform pumpEvents. space drain ] once the
        mailbox is drained (see pulse_zephyr.c's pulse_pump: K_FOREVER). */
    (void)result;

    /* Not normally reached (see above) — kept so a future node whose main:
       actually returns doesn't fall off main() into an undefined return. */
    for (;;) {
        k_sleep(K_FOREVER);
    }
    return 0;
}
