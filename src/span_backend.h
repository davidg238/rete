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
   This is the only one that is 32-bit and therefore wrap-limited.

   ---------------------------------------------------------------------------
   TWO FUNCTIONS, AND THE SUBTRACTION BELONGS TO THE BACKEND. This is not
   decoration: it is the fix for a wrap defect the 2026-07-21 WROOM-32 run caught,
   and getting it wrong is silent.

   span_stamp() answers an OPAQUE free-running counter in whatever unit the backend
   finds cheapest — cycles on Zephyr, microseconds on posix. It is NOT a
   microsecond count and must never be treated as one. Only span_elapsed_us() may
   interpret it.

   The seam originally offered one function, `span_now()`, documented as
   free-running microseconds, and callers did `span_now() - start`. The argument
   was that an unsigned 32-bit difference is correct across a wrap. That argument
   is sound ONLY when the counter wraps at 2^32 IN THE UNITS THE SUBTRACTION
   HAPPENS. The Zephyr impl converted cycles to microseconds before returning, so
   its value domain wrapped at 17,895,697 (2^32 cycles at 240 MHz expressed in us)
   — not at 2^32 — and a u32 subtraction across that wrap produced ~4.28e9 instead
   of a small delta. A 23 ms thermal job was recorded as 4,277,094,806 us, landing
   in the >=1 s bucket and printing as a NEGATIVE number once tagged as an SMI.

   So the backend now owns both halves: it stamps in its own units and it converts
   the difference. A u32 subtraction in cycles is correct across the real hardware
   wrap, and the cycles->us conversion is applied to a SMALL difference where it
   cannot overflow.

   The wrap period is what it always was — 268 s on the C6, 17.9 s on the WROOM-32
   — and the constraint is unchanged: a span longer than that reads as a short one.
   That is acceptable because a job-latency span measuring more than 17 seconds is
   already a failure by any other measure. Never use a stamp as an absolute
   timestamp, and never compare two of them with < or >.
   --------------------------------------------------------------------------- */
#include <stdint.h>

/* An opaque free-running counter in backend units. Pair ONLY with the function
   below; it is not microseconds and arithmetic on it is the backend's business. */
uint32_t span_stamp(void);

/* Microseconds elapsed since `start` (a value from span_stamp()), computed
   wrap-correctly in the backend's own units before conversion. */
uint32_t span_elapsed_us(uint32_t start);
#endif /* OEVM_SPAN_BACKEND_H */
