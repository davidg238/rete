#ifndef OEVM_PULSE_BACKEND_H
#define OEVM_PULSE_BACKEND_H
/* The ISR→channel pulse seam. Exactly one impl is linked per OEVM_TARGET:
   backend/posix/pulse_posix.c (in-process ring, non-blocking pump) for the host
   gate; backend/zephyr/pulse_zephyr.c (k_msgq, ISR-safe put, blocking pump) on
   target. An interrupt calls pulse_post_isr ONLY; the run-to-completion main loop
   calls pulse_pump at the safe point. A HEAP payload rides as an M6 handle index,
   resolved to an oop (handle_get) inside pulse_pump, then handed to space_post_id in
   thread context — C never holds a raw heap oop across the queue, because the GC can
   move it. An IMMEDIATE payload rides by value; see pulse_post_isr_value. See the
   design spec §2. */
#include <stddef.h>
#include <stdint.h>
#include "oevm.h"     /* oop — an immediate payload rides in the item by value */

#ifndef OEVM_PULSE_MAILBOX_DEPTH
#define OEVM_PULSE_MAILBOX_DEPTH 16
#endif

/* ISR-safe: enqueue (chan_id, handle_idx). Non-allocating, non-blocking.
   SHEDS the post when the mailbox is at capacity — it never aborts/k_oops. tuvm#34:
   the space ring behind this seam retired abort() under G2 (tuvm#20) precisely
   because a load condition must not be a whole-node kill, but this seam kept it, so
   a burst arriving while the reactor was not pumping still killed the node from ISR
   context (tuvm#54: a blocking HTTP render starving the pump). Every shed post is
   counted — see pulse_drops. */
void pulse_post_isr(int chan_id, int handle_idx);

/* ISR-safe: enqueue (chan_id, immediate). Same shed and coalesce rules as above; the
   ONLY difference is that the payload rides in the item instead of behind a handle.
   #151. An M6 handle exists to keep a HEAP oop findable across a GC that can move it.
   An immediate — a SmallInteger token, say — cannot move and needs no pinning, and
   pinning it anyway is not merely wasteful, it is unsound: the handle must be freed
   at some point, and freeing it while a post that names it is still queued makes that
   post resolve NULL, at which case pulse_pump DISCARDS it (tuvm#13's ABA guard, which
   is right to prefer a loss over a mis-delivery). That is how a fired one-shot's
   timer event was destroyed by the NEXT, unrelated registration — measured on a C6,
   and the cause of a ship's bell holding its striker on indefinitely.

   Callers MUST pass an immediate. A heap oop passed here would be an unrooted
   reference across a GC; use pulse_post_isr and a handle for those. */
void pulse_post_isr_value(int chan_id, oop immediate);

/* tuvm#34 telemetry: monotonic count of posts shed because the mailbox was at
   capacity. Read from thread context; surfaced through `platform sysStats` so the
   never-silent law holds end to end. A non-zero value means the reactor was not
   pumping fast enough — the condition that used to be a crash. */
uint32_t pulse_drops(void);

/* tuvm#34 telemetry: monotonic count of posts that merged into a pending post from
   the SAME source instead of taking a new slot. Unlike pulse_drops this is healthy —
   a repeating timer needs one pending wakeup, not N — but it is counted so the two
   are never confused: a rising drops count is the alarm, a rising coalesced count is
   the mechanism working. */
uint32_t pulse_coalesced(void);

/* Thread context, at the run-to-completion safe point. Drains every queued item
   (posix: returns 0 at once when empty; zephyr: blocks for the first item, then
   drains all) and for each calls space_post_id(chan_id, handle_get(handle_idx)).
   Returns the number of items delivered. */
int pulse_pump(void);

/* Non-blocking variant: drains every currently-queued item and returns
   immediately (0 when empty) — never blocks for a first item. The blocking
   pulse_pump() assumes pulse is the ONLY event source (a pure pulse/timer node
   can sleep until its ISR fires). When a second source must be polled in the same
   run-to-completion loop (e.g. an HTTP listener — #46), the pump must not block on
   pulse, or the other source is never serviced; pumpEvents uses this variant then.
   On posix (already non-blocking) it is identical to pulse_pump(). */
int pulse_pump_nb(void);
#endif /* OEVM_PULSE_BACKEND_H */
