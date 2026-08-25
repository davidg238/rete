#ifndef OEVM_TIMER_BACKEND_H
#define OEVM_TIMER_BACKEND_H
/* The timer seam. Exactly one impl is linked per OEVM_TARGET:
   backend/posix/timer_posix.c   — a deterministic VIRTUAL clock BY DEFAULT. Nothing
                                   moves it but the nsl-level `advanceMillis:` prim, so
                                   timer firing order is exactly reproducible across runs
                                   and across Path A/B. This is the host test rig's clock.
                                   Setting OEVM_TIMER_WALLCLOCK=1 switches it to a real
                                   monotonic clock drained by timer_backend_poll() — for
                                   host binaries that run a REAL server loop rather than a
                                   test script, where no one ever calls advanceMillis: and
                                   a virtual clock would leave every timer frozen forever.
                                   The suite never sets it, so determinism is untouched.
   backend/zephyr/timer_zephyr.c — a REAL clock (k_timer + k_uptime_get). The expiry ISR
                                   posts through the existing pulse mailbox
                                   (pulse_post_isr), which pulse_pump drains in thread
                                   context onto the reserved `sys/timer` channel — the
                                   identical path the virtual clock posts on. This is
                                   sanctioned by pulse_backend.h's own contract: "A
                                   GPIO/timer ISR calls pulse_post_isr".

   The C side carries DATA ONLY (a token, a millisecond count). Which channel, what
   message, and all cancel bookkeeping live in nsl's `Timers` class (lib/kernel.nsl), so
   C stays one dumb registry plus one posting path. See the rung-8 design spec §2.

   ---------------------------------------------------------------------------
   THERE ARE THREE CLOCKS IN THIS TREE (tuvm#52, widened by #61). Picking the wrong
   one is silent, so they are enumerated in both seam headers:

     oevm_millis()           src/vm.c          the in-language `millis` hook. REAL
                                               even on the host — it is not the
                                               virtual clock, which is the whole
                                               reason #52 exists.
     timer_backend_millis()  THIS FILE         the timer seam's clock. VIRTUAL by
                                               default on the host (advanced only by
                                               `advanceMillis:`), real on target.
                                               64-bit; never wraps in practice.
     span_stamp()            src/span_backend.h  the job-latency counter. 32-bit,
                                               wrap-limited, and NOT in
                                               microseconds — an opaque backend
                                               unit that only span_elapsed_us() may
                                               interpret.

   Only the third is wrap-limited, and #61's device run proved that matters: the
   wrap argument holds only in the units the subtraction happens in, which is why
   that seam owns its own subtraction. See span_backend.h before touching it.
   --------------------------------------------------------------------------- */
#include <stdint.h>

void     timer_backend_after(int token, uint64_t ms);   /* one-shot */
void     timer_backend_every(int token, uint64_t ms);   /* repeating */
void     timer_backend_cancel(int token);               /* MUST release any M6 handle it holds */
uint64_t timer_backend_millis(void);                    /* monotonic ms */

/* Advance the virtual clock by `ms` and post every now-due token; answers the number of
   fires. HOST ONLY — the target's clock is real and advances on its own, so the zephyr
   impl is a no-op answering 0. Backs the `advanceMillis:` prim. */
int      timer_backend_advance(uint64_t ms);

/* Fire any timer that has come due on a REAL clock, posting exactly as advance does.
   Called from pumpEvents at the safe point. A no-op unless the host backend is in
   wall-clock mode (OEVM_TIMER_WALLCLOCK=1); a no-op on zephyr, whose expiry ISR already
   posts through the pulse mailbox. Answers the number of fires. */
int      timer_backend_poll(void);
#endif /* OEVM_TIMER_BACKEND_H */
