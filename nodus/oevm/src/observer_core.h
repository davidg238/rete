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
 * a function of (declared periods, time since each fired, the drain
 * counter, the declared app cadence, now) — so it belongs where a host test can reach it. This is
 * the same split reboot_note.h already makes: the portable rule in a header
 * under src/, the target placement left in zephyr/src/.
 *
 * WHAT IS NOT HERE, deliberately: the thread, the watchdog, printk, sys_reboot,
 * the trace ring and the forensics. Those are Zephyr and they stay in
 * observer.c. This header decides; it never acts.
 *
 * THE ONE THING A READER MUST NOT MISS (rete#269, rete#273): §3.2 judges the
 * AGE OF THE FEED against a DECLARED number. `drained` is monotonic and moves
 * only when the interpreter runs the pump, so it integrates over the whole
 * interval rather than sampling an instant; mailbox DEPTH is not read here at
 * all any more, because a depth sample aliases against the workload (the
 * 2026-08-27 false trip). tests/test_observer_core.c holds the properties. */

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
    OBS_COND_APP_STALE
};

/* One observation. The two arrays come from timer_target_snapshot(), `drained`
 * from pulse_backend.h, `now_ms` from the platform clock, and `app_cad_ms` from
 * the app's declaration through PRIM_pf_appLiveness (0 = undeclared). */
struct ObsSample {
    uint32_t now_ms;
    int      n;                          /* timers declared; 0 is legal and meaningful */
    uint32_t period_ms[OBS_MAX_TIMERS];
    uint32_t since_ms[OBS_MAX_TIMERS];
    uint32_t drained;                    /* pulse_drained() — MONOTONIC. THE FEED. */
    uint32_t app_cad_ms;                 /* rete#273 D1: the declared app cadence */
    int      held;                       /* rete#311: the VM is blocked in the debug read */
};

/* Everything the verdict carries between samples, plus what the dump wants to
 * name afterwards. Zero-initialise it once and never touch it from outside;
 * `fed_at_ms` is read by observer.c's trace ring, which is why it is not private. */
struct ObsCore {
    uint32_t fed_at_ms;            /* when `drained` last moved; 0 == not yet armed */
    uint32_t last_drained_seen;
    uint32_t margin_ms;            /* out: how far past the deadline */
    uint32_t deadline_ms;          /* out: which deadline was operative */
};

/* Answers OBS_COND_* and fills st->margin_ms / st->deadline_ms.
 *
 * TWO NAMED CONDITIONS, because the dump reports which one fired and neither
 * alone covers both wedge shapes:
 *
 *   §3.1 TIMER_OVERDUE — an armed repeating timer is overdue against ITS OWN
 *        period × OBS_WEDGE_SLACK. The ISR stopped. Unchanged by rete#273.
 *
 *   §3.2 APP_STALE — the FEED has not moved for longer than the app DECLARED it
 *        may go without dispatching (rete#273 D1, amended 2026-09-17). The feed
 *        is pulse_drained(): it moves only when the interpreter runs the pump, so
 *        it proves exactly one sentence — "the VM executed this node's nsl event
 *        dispatch within appCad" — and claims nothing about application progress.
 *        The old rule here inferred a deadline from the SHORTEST declared timer
 *        period × slack, which reboots a healthy node running a 100 ms sensor
 *        poll beside a 1.5 s e-paper refresh (800 ms deadline against a 1500 ms
 *        reaction), and judged nothing at all on a node with no repeating timer.
 *        Both are the same defect: a number nobody declared. UNDECLARED (0) means
 *        §3.2 IS NOT JUDGED — not "judged against a default" — which is why the
 *        bridge always declares (cad × 8 unless the app says otherwise).
 *
 * MUST be called at a regular cadence — it is a sampler, and the caller's
 * period is the resolution `fed_at_ms` is measured in. */
static inline int observer_core_verdict(struct ObsCore *st, const struct ObsSample *s) {
    /* §3.1 */
    for (int i = 0; i < s->n; i++) {
        uint64_t deadline = (uint64_t)s->period_ms[i] * OBS_WEDGE_SLACK;
        if (deadline > OBS_MAX_JUDGEABLE_MS) continue;   /* unjudgeable; see above */
        if (s->since_ms[i] > (uint32_t)deadline) {
            st->deadline_ms = (uint32_t)deadline;
            st->margin_ms   = s->since_ms[i] - (uint32_t)deadline;
            return OBS_COND_TIMER_OVERDUE;
        }
    }

    /* §3.2 */
    if (s->app_cad_ms == 0 || s->app_cad_ms > OBS_MAX_JUDGEABLE_MS) return OBS_COND_NONE;
    {
        uint32_t now        = s->now_ms ? s->now_ms : 1u;   /* 0 is the sentinel */
        int      progressed = (s->drained != st->last_drained_seen);
        st->last_drained_seen = s->drained;
        /* rete#311 — a HELD VM is not a wedged one. The debugger holds the VM by
           blocking it in the debug read (at a breakpoint, or in the boot-time command
           loop), and from here that is indistinguishable from a wedge: the feed stops
           and the ISR keeps firing. So the hold is told to the sampler, and while it
           lasts the feed age is re-latched every sample — the clock restarts from the
           RELEASE, so a board held longer than appCad is not rebooted on the first
           sample after `dbg:continue`. §3.1 above is deliberately NOT held off: a
           repeating timer whose ISR has stopped is a wedge whoever holds the VM. */
        if (progressed || s->held || st->fed_at_ms == 0) {
            st->fed_at_ms = now;
            return OBS_COND_NONE;
        }
        uint32_t held = now - st->fed_at_ms;
        if (held > s->app_cad_ms) {
            st->deadline_ms = s->app_cad_ms;
            st->margin_ms   = held - s->app_cad_ms;
            return OBS_COND_APP_STALE;
        }
    }
    return OBS_COND_NONE;
}

#endif /* OEVM_OBSERVER_CORE_H */
