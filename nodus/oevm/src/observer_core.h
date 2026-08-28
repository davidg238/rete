/* Copyright (c) 2026 Ekorau LLC */
#ifndef OEVM_OBSERVER_CORE_H
#define OEVM_OBSERVER_CORE_H

/* rete#215 / rete#269 — THE WEDGE VERDICT, AND NOTHING ELSE.
 *
 * WHY THIS FILE EXISTS. The decision "is this node wedged?" lived inside
 * zephyr/src/observer.c, which appears in exactly one build list
 * (zephyr/CMakeLists.txt) and is excluded from footprint.sh by construction
 * (it #includes <zephyr/...>). So the function that rete#269 found rebooting a
 * HEALTHY node had never been compiled on the host, let alone executed there,
 * and every bug it ever had cost a board cycle to find. The decision is pure —
 * a function of (declared periods, time since each fired, mailbox depth,
 * drain counter, now) — so it belongs where a host test can reach it. This is
 * the same split reboot_note.h already makes: the portable rule in a header
 * under src/, the target placement left in zephyr/src/.
 *
 * WHAT IS NOT HERE, deliberately: the thread, the watchdog, printk, sys_reboot,
 * the trace ring and the forensics. Those are Zephyr and they stay in
 * observer.c. This header decides; it never acts.
 *
 * THE ONE THING A READER MUST NOT MISS (rete#269): the pulse condition judges
 * PROGRESS, not DEPTH. `pending` is a point sample of a transient and it
 * ALIASES against the workload — at cad 2000 under a silent uplink the mailbox
 * empties for ~1 s inside every 4.007 s tick while the observer samples every
 * 1001 ms, and a 3 ms beat walks the one sample that catches the empty window
 * out of it. `drained` is monotonic, so it integrates over the whole interval
 * instead of sampling an instant: if it moved, the VM ran the pump between
 * these two samples, whatever the mailbox happens to read right now. A VM
 * genuinely stuck in the interpreter never moves it. tests/test_observer_core.c
 * holds both halves of that as executable properties. */

#include <stdint.h>

/* == OEVM_MAX_TARGET_TIMERS, as defined by each backend's timer leaf. */
#define OBS_MAX_TIMERS 8

/* The ONE invented constant of the design (2026-08-16 design §3.3), shipped at 8.
 * Deliberately generous: the cost of a false reboot is far higher than the cost
 * of a slow detection, and the measured worst-case pushBuild is 132 ms against a
 * 2 s period. It is falsifiable BY CONSTRUCTION — a wrong SLACK produces a
 * reboot whose dump says exactly how overdue it thought the node was. */
#define OBS_WEDGE_SLACK 8u

/* A deadline longer than this cannot be judged at all: `since_ms` comes from
 * k_uptime_get_32(), which wraps at ~49.7 days, so a comparison against a larger
 * number is not merely imprecise, it is unsound — and it would fail toward a
 * FALSE TRIP, the one failure mode the design says would make this instrument
 * worse than nothing. Such a timer is skipped and said to be skipped. */
#define OBS_MAX_JUDGEABLE_MS 0x7FFFFFFFull

enum {
    OBS_COND_NONE = 0,
    OBS_COND_TIMER_OVERDUE,
    OBS_COND_PULSE_UNDRAINED
};

/* One observation. Every field is something the caller already had: the two
 * arrays come from timer_target_snapshot(), the counters from pulse_backend.h,
 * `now_ms` from the platform clock. */
struct ObsSample {
    uint32_t now_ms;
    int      n;                          /* timers declared; 0 is legal and meaningful */
    uint32_t period_ms[OBS_MAX_TIMERS];
    uint32_t since_ms[OBS_MAX_TIMERS];
    uint32_t pending;                    /* pulse_pending() */
    uint32_t drained;                    /* pulse_drained() — MONOTONIC */
};

/* Everything the verdict carries between samples, plus what the dump wants to
 * name afterwards. Zero-initialise it once and never touch it from outside;
 * `undrained_since_ms` is read by observer.c's trace ring, which is why it is
 * not private. */
struct ObsCore {
    uint32_t undrained_since_ms;   /* 0 == not currently undrained */
    uint32_t last_drained_seen;
    uint32_t margin_ms;            /* out: how far past the deadline */
    uint32_t deadline_ms;          /* out: which deadline was operative */
};

/* Answers OBS_COND_* and fills st->margin_ms / st->deadline_ms.
 *
 * Both conditions derive their deadline from state the app already declared;
 * nothing global is assumed. They are kept as TWO NAMED CONDITIONS rather than
 * one merged predicate because the dump reports which one fired, and neither
 * alone covers both wedge shapes.
 *
 * MUST be called at a regular cadence — it is a sampler, and the caller's
 * period is what `undrained_since_ms` is measured in. */
static inline int observer_core_verdict(struct ObsCore *st, const struct ObsSample *s) {
    /* The app declared no cadence: nothing to judge it against, and guessing one
     * is what the design forbids. NOTE THE HOLE THIS LEAVES (and it is a real
     * one): a node whose app declares no repeating timer is not judged healthy,
     * it is UNJUDGEABLE. rete#231 D1 closes that by having the kernel own a base
     * tick — but only once startTicking: has run, which on a bridge node sits
     * downstream of the socket bind. */
    if (s->n == 0) return OBS_COND_NONE;

    /* §3.1 — an armed repeating timer is overdue against ITS OWN period. Catches
     * the timer subsystem itself stopping: the ISR no longer firing at all. A
     * node with a ten-minute timer is judged against ten minutes; one with a 2 s
     * timer against 2 s. */
    uint32_t shortest = 0;
    for (int i = 0; i < s->n; i++) {
        uint64_t deadline = (uint64_t)s->period_ms[i] * OBS_WEDGE_SLACK;
        if (deadline > OBS_MAX_JUDGEABLE_MS) continue;   /* unjudgeable; see above */
        if (shortest == 0 || s->period_ms[i] < shortest) shortest = s->period_ms[i];
        if (s->since_ms[i] > (uint32_t)deadline) {
            st->deadline_ms = (uint32_t)deadline;
            st->margin_ms   = s->since_ms[i] - (uint32_t)deadline;
            return OBS_COND_TIMER_OVERDUE;
        }
    }
    if (shortest == 0) return OBS_COND_NONE;   /* every declared timer was unjudgeable */

    /* §3.2 — the ISR is firing and the mailbox is filling, but the VM is not
     * draining it: a VM stuck in the interpreter rather than blocked at the
     * mailbox. Judged against the SHORTEST declared period, because that is the
     * fastest cadence the app claims to service. */
    {
        uint32_t deadline    = shortest * OBS_WEDGE_SLACK;
        uint32_t now         = s->now_ms;
        uint32_t drained_now = s->drained;
        int      progressed  = (drained_now != st->last_drained_seen);
        st->last_drained_seen = drained_now;

        if (s->pending == 0 || progressed) {
            st->undrained_since_ms = 0;
        } else {
            if (st->undrained_since_ms == 0) st->undrained_since_ms = now ? now : 1u;
            uint32_t held = now - st->undrained_since_ms;
            if (held > deadline) {
                st->deadline_ms = deadline;
                st->margin_ms   = held - deadline;
                return OBS_COND_PULSE_UNDRAINED;
            }
        }
    }
    return OBS_COND_NONE;
}

#endif /* OEVM_OBSERVER_CORE_H */
