#include "../../timer_backend.h"
#include "../../pulse_backend.h"
#include "../../space.h"
#include "../../handles.h"
#include "../../oevm.h"
#include <zephyr/kernel.h>

/* Target timer backend (rung 8): a real k_timer per live registration. The expiry
   callback runs in ISR context, so it does exactly what a GPIO edge ISR does and nothing
   more — it calls pulse_post_isr (non-allocating, non-blocking) with the
   (chan_id, handle_idx) pair resolved at REGISTRATION time, in thread context.
   pulse_pump then resolves the handle and calls space_post_id in thread context, posting
   TAG_SMI(token) onto the reserved `sys/timer` channel — the identical path the host
   virtual clock posts on via space_post (backend/posix/timer_posix.c). pulse_backend.h's
   contract already sanctions this: "A GPIO/timer ISR calls pulse_post_isr".

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
    int handle_idx;     /* M6 handle pinning TAG_SMI(token); released on cancel/reap */
    int repeating;
    int live;
};
static struct TargetTimer target_timers[OEVM_MAX_TARGET_TIMERS];

/* ISR context. Posting to the pulse mailbox is the ONLY thing permitted here. */
static void timer_expiry(struct k_timer *t) {
    struct TargetTimer *e = CONTAINER_OF(t, struct TargetTimer, t);
    if (e->live) pulse_post_isr(e->chan_id, e->handle_idx);
}

/* Thread context. A fired one-shot leaves a dead slot still holding an M6 handle;
   reclaim it lazily at the next registration, because handle_free is NOT ISR-safe and so
   cannot run in timer_expiry. k_timer_status_get answers >0 iff the timer has fired since
   it was last asked (and resets the count), which for a stopped one-shot means "done". */
static void reap_oneshots(void) {
    for (int i = 0; i < OEVM_MAX_TARGET_TIMERS; i++) {
        struct TargetTimer *e = &target_timers[i];
        if (e->live && !e->repeating && k_timer_status_get(&e->t) > 0) {
            handle_free(e->handle_idx);
            e->live = 0;
        }
    }
}

static void timer_start(int token, uint64_t ms, int repeating) {
    reap_oneshots();
    for (int i = 0; i < OEVM_MAX_TARGET_TIMERS; i++) {
        struct TargetTimer *e = &target_timers[i];
        if (!e->live) {
            e->token      = token;
            e->chan_id    = space_intern("sys/timer", 9);   /* the reserved channel */
            e->handle_idx = handle_new(TAG_SMI(token));     /* pin the token oop via M6 */
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
            handle_free(e->handle_idx);   /* or every cancel leaks an M6 handle slot */
            e->live = 0;
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
