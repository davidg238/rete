/* Zephyr impl of the span_backend.h microsecond clock seam (tuvm#61).

   k_cycle_get_32() is a register read of the platform cycle counter:
     ESP32-C6     16 MHz -> 62.5 ns resolution, wraps every 268 s
     ESP32 WROOM 240 MHz ->  4.2 ns resolution, wraps every 17.9 s
   Both are far finer than k_uptime_get()'s 1 ms, which is why this seam exists at
   all: a VM-compute segment is hundreds of microseconds and a millisecond clock
   cannot see it. (Verified from the built .config files, 2026-07-21:
   CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC is 16000000 on the C6 and 240000000 on the
   WROOM; CONFIG_SYS_CLOCK_TICKS_PER_SEC is 1000 and 10000 respectively.)

   The wrap is the CALLER's problem and src/span.c handles it: spans subtract two
   u32 samples, which is correct across a wrap so long as the interval is shorter
   than the wrap period. The tighter of the two boards is the WROOM at 17.9 s; a
   span longer than that is a hung job, not a measurement.

   CONFIG_TIMING_FUNCTIONS is not enabled on either build, so Zephyr's timing_*
   API is unavailable — k_cycle_get_32 is the right primitive and needs no new
   Kconfig. */
#include "../../span_backend.h"
#include <zephyr/kernel.h>

uint32_t span_now(void) {
    return k_cyc_to_us_floor32(k_cycle_get_32());
}
