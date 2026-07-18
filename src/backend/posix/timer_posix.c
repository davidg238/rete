#include "../../timer_backend.h"
#include "../../space.h"
#include "../../oevm.h"
#include <stdint.h>

/* The host virtual-clock timer facility (Milestone D Task 6; lifted VERBATIM out of
   src/platform.c in rung 8, when the seam gained a real on-target backend — see
   src/backend/zephyr/timer_zephyr.c). A deterministic event source: the host test rig
   never reads wall time. `timer_backend_advance` is the ONLY thing that moves the clock,
   driven exclusively by the nsl-level `advanceMillis:` prim, so timer firing order is
   fully reproducible across runs and across Path A/B. Each live HostTimer is a
   (token, due_ms, interval_ms, repeating, live) slot in a small fixed table
   (HOST_MAX_TIMERS is a host-rig cap, not a VM limit). `host_timer_add` reuses a dead
   slot before growing the table, so after:/every: churn (register, fire/cancel, register
   again) doesn't leak slots. The C side carries DATA ONLY (a token, a millisecond count)
   — which channel, what message, and cancel bookkeeping all live in nsl's `Timers` class
   (lib/kernel.nsl); this file posts just TAG_SMI(token) onto the single reserved
   `sys/timer` channel via space_post. */
#define HOST_MAX_TIMERS 32
struct HostTimer { int token; uint64_t due_ms; uint64_t interval_ms; int repeating; int live; };
static struct HostTimer host_timers[HOST_MAX_TIMERS];
static int host_n_timers = 0;
static uint64_t host_clock_ms = 0;

static void host_timer_add(int token, uint64_t ms, int repeating) {
    int i;
    /* A repeating timer with a 0ms interval would never let due_ms advance past
       host_clock_ms in timer_backend_advance's fire loop below, hanging the process on
       the very next advanceMillis: call. Floor the interval to 1ms for repeaters only —
       a one-shot (`repeating` false) with ms==0 is harmless: it fires once on the next
       advance and deactivates, no loop involved, so its ms is left untouched. */
    if (repeating && ms == 0) ms = 1;
    for (i = 0; i < host_n_timers; i++) {
        if (!host_timers[i].live) {
            host_timers[i].token = token;
            host_timers[i].due_ms = host_clock_ms + ms;
            host_timers[i].interval_ms = ms;
            host_timers[i].repeating = repeating;
            host_timers[i].live = 1;
            return;
        }
    }
    if (host_n_timers < HOST_MAX_TIMERS) {
        host_timers[host_n_timers].token = token;
        host_timers[host_n_timers].due_ms = host_clock_ms + ms;
        host_timers[host_n_timers].interval_ms = ms;
        host_timers[host_n_timers].repeating = repeating;
        host_timers[host_n_timers].live = 1;
        host_n_timers++;
    }
    /* else: silently drop — host test rig; on-target uses the real timer facility */
}

void timer_backend_after(int token, uint64_t ms) { host_timer_add(token, ms, 0); }
void timer_backend_every(int token, uint64_t ms) { host_timer_add(token, ms, 1); }

void timer_backend_cancel(int token) {
    int i;
    for (i = 0; i < host_n_timers; i++)
        if (host_timers[i].live && host_timers[i].token == token) host_timers[i].live = 0;
}

uint64_t timer_backend_millis(void) { return host_clock_ms; }

/* Advance the virtual clock by `ms`; post each now-due timer's token onto sys/timer
   (9 = strlen("sys/timer"), mirroring the String-path prims' length-carrying convention).
   A repeater whose interval is smaller than the advance can fire more than once per call
   (the inner while loop keeps posting and rescheduling until it is no longer due); a
   one-shot fires at most once and then deactivates. Answers the total number of fires,
   for observability. */
int timer_backend_advance(uint64_t ms) {
    int i, fired = 0;
    host_clock_ms += ms;
    for (i = 0; i < host_n_timers; i++) {
        struct HostTimer *t = &host_timers[i];
        while (t->live && t->due_ms <= host_clock_ms) {
            oop tok = TAG_SMI(t->token);
            space_post("sys/timer", 9, tok);
            fired++;
            if (t->repeating) t->due_ms += t->interval_ms; else { t->live = 0; break; }
        }
    }
    return fired;
}
