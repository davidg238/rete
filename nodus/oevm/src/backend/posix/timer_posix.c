/* clock_gettime/CLOCK_MONOTONIC are POSIX, not C99, and the host builds pass -std=c99
   (which defines __STRICT_ANSI__ and hides them). Same reason the other posix backends
   declare a feature-test macro. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "../../timer_backend.h"
#include "../../space.h"
#include "../../oevm.h"
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

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

static int use_wallclock(void);
static uint64_t monotonic_ms(void);
static uint64_t wall_base_ms;

static int host_timer_add(int token, uint64_t ms, int repeating) {
    int i;
    /* In wall-clock mode host_clock_ms is only refreshed by timer_backend_poll, so a
       registration arriving between polls would compute due_ms off a stale base and fire
       early. Resync before deriving due_ms. */
    if (use_wallclock()) host_clock_ms = monotonic_ms() - wall_base_ms;
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
            return 1;
        }
    }
    if (host_n_timers < HOST_MAX_TIMERS) {
        host_timers[host_n_timers].token = token;
        host_timers[host_n_timers].due_ms = host_clock_ms + ms;
        host_timers[host_n_timers].interval_ms = ms;
        host_timers[host_n_timers].repeating = repeating;
        host_timers[host_n_timers].live = 1;
        host_n_timers++;
        return 1;
    }
    return 0;   /* full: the caller raises (#81) — never a silent drop */
}

int timer_backend_after(int token, uint64_t ms) { return host_timer_add(token, ms, 0); }
int timer_backend_every(int token, uint64_t ms) { return host_timer_add(token, ms, 1); }

void timer_backend_cancel(int token) {
    int i;
    for (i = 0; i < host_n_timers; i++)
        if (host_timers[i].live && host_timers[i].token == token) host_timers[i].live = 0;
}

/* Wall-clock mode (OEVM_TIMER_WALLCLOCK=1). The virtual clock above is the right host
   default: the suite drives it with advanceMillis: and asserts exact tick sequences
   (11_supervision/06_timers, 08_timer_zero_interval, 11_crash_timer_sweep), so it must
   stay reproducible. But a host binary running a REAL server loop
   (`[true] whileTrue: [platform pumpEvents. space drain]`) never calls advanceMillis: —
   under the virtual clock its timers are frozen forever, silently: no error, just a
   cadence that never ticks. That is what left ThermalHttpNode's sample cell nil and
   /thermal rendering 'no frame'. In this mode host_clock_ms tracks CLOCK_MONOTONIC and
   timer_backend_poll() drains due timers from pumpEvents, mirroring how the target's
   k_timer expiry reaches the same sys/timer channel. Opt-in only — nothing in the suite
   sets the env var, so default determinism is unchanged. */
static int wallclock_mode = -1;     /* -1 = not yet resolved */

static uint64_t monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

static int use_wallclock(void) {
    if (wallclock_mode < 0) {
        const char *e = getenv("OEVM_TIMER_WALLCLOCK");
        wallclock_mode = (e && *e && *e != '0') ? 1 : 0;
        if (wallclock_mode) wall_base_ms = monotonic_ms();
    }
    return wallclock_mode;
}

uint64_t timer_backend_millis(void) {
    if (use_wallclock()) return monotonic_ms() - wall_base_ms;
    return host_clock_ms;
}

/* Fire everything now due against the real clock. Same posting path and same repeater
   catch-up semantics as timer_backend_advance — only the clock source differs. */
int timer_backend_poll(void) {
    int i, fired = 0;
    if (!use_wallclock()) return 0;
    host_clock_ms = monotonic_ms() - wall_base_ms;
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
