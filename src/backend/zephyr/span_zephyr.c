/* Zephyr impl of the span_backend.h clock seam (tuvm#61).

   k_cycle_get_32() is a register read of the platform cycle counter:
     ESP32-C6     16 MHz -> 62.5 ns resolution, wraps every 268 s
     ESP32 WROOM 240 MHz ->  4.2 ns resolution, wraps every 17.9 s
   Both are far finer than k_uptime_get()'s 1 ms, which is why this seam exists at
   all: a VM-compute segment is hundreds of microseconds and a millisecond clock
   cannot see it. (Verified from the built .config files, 2026-07-21:
   CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC is 16000000 on the C6 and 240000000 on the
   WROOM; CONFIG_SYS_CLOCK_TICKS_PER_SEC is 1000 and 10000 respectively.)

   THE STAMP IS CYCLES, NOT MICROSECONDS, and that is the whole point. This file
   used to answer k_cyc_to_us_floor32(k_cycle_get_32()) from a single span_now(),
   leaving callers to subtract two of those. The 2026-07-21 WROOM-32 run found what
   is wrong with that: converting first squeezes the counter's value domain to
   0..17,895,696, so it no longer wraps at 2^32 and a u32 subtraction across the
   wrap yields ~4.28e9 rather than a small delta. A 23 ms thermal job was recorded
   as 4,277,094,806 us. Subtracting in CYCLES is correct across the real hardware
   wrap, and converting the (small) difference afterwards cannot overflow.

   CONFIG_TIMING_FUNCTIONS is not enabled on either build, so Zephyr's timing_*
   API is unavailable — k_cycle_get_32 is the right primitive and needs no new
   Kconfig. */
#include "../../span_backend.h"
#include <zephyr/kernel.h>

uint32_t span_stamp(void) {
    return k_cycle_get_32();
}

uint32_t span_elapsed_us(uint32_t start) {
    /* Unsigned wraparound is defined behaviour and is exactly what is wanted here:
       the difference is correct whenever the span is shorter than the counter's
       own wrap period (17.9 s on the WROOM-32, 268 s on the C6). */
    return k_cyc_to_us_floor32(k_cycle_get_32() - start);
}
