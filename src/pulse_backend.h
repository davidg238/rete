#ifndef OEVM_PULSE_BACKEND_H
#define OEVM_PULSE_BACKEND_H
/* The ISR→channel pulse seam. Exactly one impl is linked per OEVM_TARGET:
   backend/posix/pulse_posix.c (in-process ring, non-blocking pump) for the host
   gate; backend/zephyr/pulse_zephyr.c (k_msgq, ISR-safe put, blocking pump) on
   target. An interrupt calls pulse_post_isr ONLY; the run-to-completion main loop
   calls pulse_pump at the safe point. C never holds a raw oop — the mailbox carries
   an M6 handle index, resolved to an oop (handle_get) inside pulse_pump, then handed
   to space_post_id in thread context. See the design spec §2. */
#include <stddef.h>

#ifndef OEVM_PULSE_MAILBOX_DEPTH
#define OEVM_PULSE_MAILBOX_DEPTH 16
#endif

/* ISR-safe: enqueue (chan_id, handle_idx). Non-allocating, non-blocking.
   Aborts "pulse mailbox full" when the mailbox is at capacity. */
void pulse_post_isr(int chan_id, int handle_idx);

/* Thread context, at the run-to-completion safe point. Drains every queued item
   (posix: returns 0 at once when empty; zephyr: blocks for the first item, then
   drains all) and for each calls space_post_id(chan_id, handle_get(handle_idx)).
   Returns the number of items delivered. */
int pulse_pump(void);
#endif /* OEVM_PULSE_BACKEND_H */
