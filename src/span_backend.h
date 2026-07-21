#ifndef OEVM_SPAN_BACKEND_H
#define OEVM_SPAN_BACKEND_H
/* The microsecond clock seam for job-latency spans (#61). Exactly one impl is
   linked per OEVM_TARGET, the timer_backend.h precedent:
     backend/posix/span_posix.c   — CLOCK_MONOTONIC in wall-clock mode; under the
                                    DEFAULT virtual clock it answers
                                    timer_backend_millis() * 1000 so the nsl suite
                                    stays bit-reproducible.
     backend/zephyr/span_zephyr.c — k_cycle_get_32(), 62.5 ns on the C6 and 4.2 ns
                                    on the WROOM-32.

   WHY THIS SEAM EXISTS AT ALL: timer_backend_millis() is k_uptime_get() on target,
   which is genuinely 1 ms granular (CONFIG_SYS_CLOCK_TICKS_PER_SEC is 1000 on the
   C6, 10000 on the WROOM-32, but the API is milliseconds either way). A VM-compute
   segment is hundreds of microseconds, so 1 ms cannot see it.

   THIS IS THE THIRD CLOCK IN THE TREE (tuvm#52). The other two:
     oevm_millis()           — the in-language `millis` hook (src/vm.c)
     timer_backend_millis()  — the timer seam's clock (src/timer_backend.h)
   span_now() is the ONLY one that is 32-bit and therefore wrap-limited: it wraps
   every 268 s on the C6 and every 17.9 s on the WROOM-32. That is safe here and
   ONLY here, because spans use unsigned 32-bit DIFFERENCES, which are correct
   across a wrap so long as the interval is shorter than the wrap period. Never use
   span_now() as an absolute timestamp, and never compare two of them with < or >. */
#include <stdint.h>

uint32_t span_now(void);   /* free-running microseconds; use ONLY via u32 deltas */
#endif /* OEVM_SPAN_BACKEND_H */
