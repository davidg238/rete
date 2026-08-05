#include "../../timer_backend.h"
#include "../../pulse_backend.h"
#include "../../space.h"
#include "../../oevm.h"
#include <zephyr/kernel.h>

/* Target timer backend (rung 8): a real k_timer per live registration. The expiry
   callback runs in ISR context, so it does exactly what a GPIO edge ISR does and nothing
   more — it calls pulse_post_isr_value (non-allocating, non-blocking) with the chan_id
   resolved at REGISTRATION time and the token as an IMMEDIATE (#151). pulse_pump then
   calls space_post_id in thread context, posting TAG_SMI(token) onto the reserved
   `sys/timer` channel — the identical path the host virtual clock posts on via
   space_post (backend/posix/timer_posix.c). pulse_backend.h's contract already
   sanctions this: "A GPIO/timer ISR calls pulse_post_isr".

   A timer holds NO M6 handle. It used to pin TAG_SMI(token) behind one, which was
   always unnecessary — a handle exists to keep a HEAP oop findable across a GC that
   can move it, and an immediate cannot move — and turned out to be unsound: see
   timer_expiry below for what freeing that handle destroyed (#151).

   Consequences worth knowing: because the tick lands in the pulse mailbox, a node's
   `platform pumpEvents` (which BLOCKS K_FOREVER on that mailbox) idles the CPU between
   ticks and yields to the OpenThread/net thread. No nsl code ever runs in ISR context. */

#ifndef OEVM_MAX_TARGET_TIMERS
#define OEVM_MAX_TARGET_TIMERS 8
#endif

struct TargetTimer {
    struct k_timer t;   /* first member: timer_expiry recovers the slot via CONTAINER_OF */
    int token;
    int chan_id;
    int repeating;
    int live;
};
static struct TargetTimer target_timers[OEVM_MAX_TARGET_TIMERS];

/* ISR context. Posting to the pulse mailbox is the ONLY thing permitted here — and
   since #151 that post carries the token BY VALUE, so nothing outside this function
   owns anything that has to be released.
 
   Retiring the slot here is what deletes the bug. It used to be done by a lazy
   reap_oneshots() at the top of every registration, because handle_free is not
   ISR-safe; that reap freed the M6 handle a still-queued post named, so the pump
   resolved NULL and DISCARDED the event (tuvm#13's ABA guard, correctly preferring a
   loss to a mis-delivery). A fired one-shot's event was therefore destroyed by the
   next, unrelated registration — measured on a C6 as `control 1 / test 0` across a
   one-line A/B, and the reason a ship's bell held its striker on until power-cycled.
   With no handle there is nothing to free, and `live = 0` is a plain int store that
   IS ISR-safe, so the reap has no remaining job and is gone. */
static void timer_expiry(struct k_timer *t) {
    struct TargetTimer *e = CONTAINER_OF(t, struct TargetTimer, t);
    if (!e->live) return;
    pulse_post_isr_value(e->chan_id, TAG_SMI(e->token));
    if (!e->repeating) e->live = 0;   /* the slot is reusable; the post is self-contained */
}

static void timer_start(int token, uint64_t ms, int repeating) {
    for (int i = 0; i < OEVM_MAX_TARGET_TIMERS; i++) {
        struct TargetTimer *e = &target_timers[i];
        if (!e->live) {
            e->token      = token;
            e->chan_id    = space_intern("sys/timer", 9);   /* the reserved channel */
            e->repeating  = repeating;
            e->live       = 1;
            k_timer_init(&e->t, timer_expiry, NULL);
            k_timer_start(&e->t, K_MSEC(ms), repeating ? K_MSEC(ms) : K_NO_WAIT);
            return;
        }
    }
    printk("timer table full\n");
    k_oops();   /* Tier-2 crash, exactly as pulse_post_isr does when its mailbox is full */
}

void timer_backend_after(int token, uint64_t ms) { timer_start(token, ms, 0); }
void timer_backend_every(int token, uint64_t ms) { timer_start(token, ms, 1); }

void timer_backend_cancel(int token) {
    for (int i = 0; i < OEVM_MAX_TARGET_TIMERS; i++) {
        struct TargetTimer *e = &target_timers[i];
        if (e->live && e->token == token) {
            k_timer_stop(&e->t);
            e->live = 0;   /* nothing to release: the token rides by value (#151) */
        }
    }
}

uint64_t timer_backend_millis(void) { return (uint64_t)k_uptime_get(); }

/* No virtual clock on target — the clock is real and advances on its own. The
   `advanceMillis:` prim therefore does nothing here and answers 0 fires. */
int timer_backend_advance(uint64_t ms) { (void)ms; return 0; }

/* No-op on target: the clock is real and the k_timer expiry ISR already posts through
   the pulse mailbox, which pulse_pump drains onto sys/timer. Exists only so pumpEvents
   can call it unconditionally across both backends. */
int timer_backend_poll(void) { return 0; }
