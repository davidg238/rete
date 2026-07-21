/* Host impl of the span_backend.h microsecond clock seam (tuvm#61).

   Under the DEFAULT virtual clock the host has no real time at all — nothing moves
   but the nsl-level advanceMillis: prim — so answering CLOCK_MONOTONIC here would
   make every nsl span test depend on how fast the machine ran, which is exactly the
   determinism timer_posix.c exists to protect. Instead we scale the virtual
   millisecond clock to microseconds, so advanceMillis: moves the span clock by an
   exact amount and a bucket assertion is not a race.

   Tests needing sub-millisecond resolution use spanRecord:us: to feed samples
   directly and involve no clock at all — see the design's validation section. */

/* clock_gettime/CLOCK_MONOTONIC are POSIX, not C99, and -std=c99 defines
   __STRICT_ANSI__ which hides them. Same declaration and same reason as
   timer_posix.c:5 — keep the two in step. */
#define _POSIX_C_SOURCE 200809L

#include "../../span_backend.h"
#include "../../timer_backend.h"
#include <stdlib.h>
#include <time.h>

/* Mirrors timer_posix.c's OEVM_TIMER_WALLCLOCK knob. Read once: the two files must
   agree about which clock is live, and re-reading getenv per call would let them
   disagree mid-run if the environment were mutated. */
static int wallclock_mode(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *e = getenv("OEVM_TIMER_WALLCLOCK");
        cached = (e && *e && *e != '0') ? 1 : 0;
    }
    return cached;
}

uint32_t span_now(void) {
    if (!wallclock_mode()) {
        /* Virtual clock: deterministic, advances only via advanceMillis:. */
        return (uint32_t)(timer_backend_millis() * 1000ULL);
    }
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        /* Truncated to 32 bits deliberately — see span_backend.h: only DELTAS are
           meaningful, and a u32 delta is correct across the wrap. */
        return (uint32_t)((uint64_t)ts.tv_sec * 1000000ULL +
                          (uint64_t)ts.tv_nsec / 1000ULL);
    }
}
