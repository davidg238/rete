/* Copyright (c) 2026 Ekorau LLC */

/* Unit test for the wedge verdict (src/observer_core.h, rete#215 / rete#269).
 *
 * WHY THIS TEST EXISTS AT ALL. Until this file, `observer_verdict()` lived only
 * in zephyr/src/observer.c — one build list, excluded from footprint.sh because
 * it #includes <zephyr/...>. It had never been compiled on the host. rete#269
 * found it rebooting a HEALTHY node, and it cost a board cycle to find, as every
 * bug in it had. This runs the same decision on the host, in a few milliseconds.
 *
 * WHAT IT PROVES, and what it does NOT. It proves the PREDICATE, not the
 * mailbox: `pending` and `drained` are supplied by a model here, not by
 * pulse_zephyr.c. The mailbox itself is gated by test_coalesce.c on the host and
 * by tests/target/{pulse-coalesce,stale-handle} on a board. What no board gate
 * can do cheaply, and this can, is sweep the parameter space — property 3 runs
 * 30 015 workload/sampler combinations, which is more distinct cadences than the
 * bench has ever flown.
 *
 * THE HARNESS HAS TEETH, and property 1 is where that is demonstrated rather
 * than asserted: it runs the SAME timeline through the pre-rete#269 predicate
 * (kept below, in this file, and nowhere else) and requires it to false-fire. A
 * regression test written after the fix is worth nothing unless it can be shown
 * to fail against the code that had the bug.
 *
 * Build (header-only, no link set — same idiom as test_frame_abi/test_layout):
 *   gcc -std=c99 -Isrc tests/test_observer_core.c -o /tmp/toc && /tmp/toc
 *
 * Verifies:
 *   1. The rete#269 false trip: pending>0 at every sample while `drained`
 *      climbs. The old predicate fires at ~8 x cad; the new one never does.
 *   2. A real wedge still fires, at exactly period x OBS_WEDGE_SLACK — the fix
 *      did not disarm the detector (wedge-detect must stay red-capable).
 *   3. Aliasing sweep: over 30 015 (tick, empty-window, sampler) triples, the
 *      new predicate never fires while `drained` is advancing. The count of
 *      triples on which the OLD predicate false-fires is reported, not asserted.
 *   4. The timer-overdue condition fires against its own declared period.
 *   5. A period too large to judge soundly is SKIPPED, not guessed at.
 *   6. A node that declares no timer is unjudgeable — the rete#231 D1 hole,
 *      written down as an executable fact rather than a comment.
 *
 * rete#273 D1 (2026-09-17): §3.2 now judges FEED AGE against a DECLARED
 * `app_cad_ms`; the shortest-period inference is gone. `legacy_verdict` below is
 * the harness's own copy of BOTH old rules and is what lets property 8 show the
 * fast-poll false trip before the fix. */

#include "../src/observer_core.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

/* The pre-#273 §3.2 (as shipped 2026-08-27 .. 2026-09-17): the SHORTEST declared
 * period × slack, judged as "held undrained while pending". Kept verbatim so the
 * fast-poll false trip (property 8) is DEMONSTRATED, not asserted from memory. */
enum { LEGACY_COND_PULSE_UNDRAINED = 99 };
struct LegacyCore { uint32_t undrained_since_ms, last_drained_seen, margin_ms, deadline_ms; };
struct LegacySample { uint32_t now_ms; int n; uint32_t period_ms[OBS_MAX_TIMERS];
                      uint32_t since_ms[OBS_MAX_TIMERS]; uint32_t pending, drained; };

static int legacy_verdict(struct LegacyCore *st, const struct LegacySample *s) {
    if (s->n == 0) return OBS_COND_NONE;
    uint32_t shortest = 0;
    for (int i = 0; i < s->n; i++) {
        uint64_t deadline = (uint64_t)s->period_ms[i] * OBS_WEDGE_SLACK;
        if (deadline > OBS_MAX_JUDGEABLE_MS) continue;
        if (shortest == 0 || s->period_ms[i] < shortest) shortest = s->period_ms[i];
        if (s->since_ms[i] > (uint32_t)deadline) {
            st->deadline_ms = (uint32_t)deadline;
            st->margin_ms   = s->since_ms[i] - (uint32_t)deadline;
            return OBS_COND_TIMER_OVERDUE;
        }
    }
    if (shortest == 0) return OBS_COND_NONE;
    uint32_t deadline    = shortest * OBS_WEDGE_SLACK;
    uint32_t now         = s->now_ms;
    int      progressed  = (s->drained != st->last_drained_seen);
    st->last_drained_seen = s->drained;
    if (s->pending == 0 || progressed) {
        st->undrained_since_ms = 0;
    } else {
        if (st->undrained_since_ms == 0) st->undrained_since_ms = now ? now : 1u;
        uint32_t held = now - st->undrained_since_ms;
        if (held > deadline) {
            st->deadline_ms = deadline;
            st->margin_ms   = held - deadline;
            return LEGACY_COND_PULSE_UNDRAINED;
        }
    }
    return OBS_COND_NONE;
}

/* A workload: the VM drains one pulse every tick_ms (or never, when drains == 0),
 * and a reaction BLOCKS for block_ms of every tick — during which `drained` does
 * not move. empty_ms is how long the mailbox reads empty inside each tick. */
struct Workload {
    uint32_t tick_ms;
    uint32_t empty_ms;
    uint32_t block_ms;
    int      drains;
};

static uint32_t pending_at(const struct Workload *w, uint32_t t) {
    if (!w->drains) return 1;
    uint32_t phase = t % w->tick_ms;
    return phase >= (w->tick_ms - w->empty_ms) ? 0u : 1u;
}

static uint32_t drained_at(const struct Workload *w, uint32_t t) {
    if (!w->drains) return 30u;                      /* frozen, as wedge-detect saw */
    uint32_t phase = t % w->tick_ms;
    /* the drain lands at the END of the blocking reaction */
    return (t / w->tick_ms) + (phase >= w->block_ms ? 1u : 0u);
}

/* Runs the NEW rule. period_ms is the one declared timer (0 = no timer at all,
 * the run-once shape); app_cad_ms is the declaration (0 = undeclared). Answers the
 * time of the first OBS_COND_APP_STALE, or 0. */
static uint32_t run_timeline(const struct Workload *w, uint32_t period_ms,
                             uint32_t app_cad_ms, uint32_t sampler_ms,
                             uint32_t horizon_ms, uint32_t *margin, uint32_t *deadline) {
    struct ObsCore st; memset(&st, 0, sizeof st);
    for (uint32_t t = sampler_ms; t <= horizon_ms; t += sampler_ms) {
        struct ObsSample s; memset(&s, 0, sizeof s);
        s.now_ms = t;
        if (period_ms) { s.n = 1; s.period_ms[0] = period_ms; s.since_ms[0] = t % period_ms; }
        s.drained    = drained_at(w, t);
        s.app_cad_ms = app_cad_ms;
        if (observer_core_verdict(&st, &s) == OBS_COND_APP_STALE) {
            if (margin)   *margin   = st.margin_ms;
            if (deadline) *deadline = st.deadline_ms;
            return t ? t : 1u;
        }
    }
    return 0;
}

/* Runs the LEGACY rule against the same workload. */
static uint32_t run_legacy(const struct Workload *w, uint32_t period_ms, uint32_t sampler_ms,
                           uint32_t horizon_ms, uint32_t *margin, uint32_t *deadline) {
    struct LegacyCore st; memset(&st, 0, sizeof st);
    for (uint32_t t = sampler_ms; t <= horizon_ms; t += sampler_ms) {
        struct LegacySample s; memset(&s, 0, sizeof s);
        s.now_ms = t;
        if (period_ms) { s.n = 1; s.period_ms[0] = period_ms; s.since_ms[0] = t % period_ms; }
        s.pending = pending_at(w, t);
        s.drained = drained_at(w, t);
        if (legacy_verdict(&st, &s) == LEGACY_COND_PULSE_UNDRAINED) {
            if (margin)   *margin   = st.margin_ms;
            if (deadline) *deadline = st.deadline_ms;
            return t ? t : 1u;
        }
    }
    return 0;
}

int main(void) {
    /* ---- 1. the rete#269 silent-uplink workload stays quiet ------------- *
     * 4.007 s delivered ticks against a 2 s declaration, appCad at the bridge
     * default (2000 × 8). The feed moves every 4 s, well inside 16 s. */
    {
        struct Workload silent = { .tick_ms = 4007, .empty_ms = 0, .block_ms = 0, .drains = 1 };
        assert(run_timeline(&silent, 2000, 16000, 1001, 600000, NULL, NULL) == 0);
        printf("test_observer_core: property 1 (silent uplink, feed every 4 s against "
               "appCad 16 s: no trip in 600 s) OK\n");
    }

    /* ---- 2. a real wedge fires against the DECLARED number --------------- */
    {
        struct Workload wedged = { .tick_ms = 1000, .empty_ms = 0, .block_ms = 0, .drains = 0 };
        uint32_t margin = 0, deadline = 0;
        uint32_t fire = run_timeline(&wedged, 1000, 8000, 1001, 60000, &margin, &deadline);
        assert(fire != 0);
        assert(deadline == 8000u);                /* the declaration, not period × slack */
        assert(fire > 8000u && fire <= 8000u + 2u * 1001u);   /* latch + one sampler */
        assert(margin < 1001u);
        printf("test_observer_core: property 2 (a real wedge fires at %u ms against the "
               "declared appCad=%u, overdue=%u) OK\n", fire, deadline, margin);
    }

    /* ---- 3. the aliasing sweep: never a trip while the feed moves -------- */
    {
        unsigned long triples = 0;
        static const uint32_t samplers[] = { 997, 999, 1001, 1003, 1005 };
        static const uint32_t empties[]  = { 600, 1000, 1400 };
        for (uint32_t tick = 3000; tick <= 5000; tick++) {
            for (unsigned si = 0; si < sizeof samplers / sizeof samplers[0]; si++) {
                for (unsigned ei = 0; ei < sizeof empties / sizeof empties[0]; ei++) {
                    struct Workload w = { .tick_ms = tick, .empty_ms = empties[ei],
                                          .block_ms = 0, .drains = 1 };
                    triples++;
                    if (run_timeline(&w, 2000, 16000, samplers[si], 120000, NULL, NULL) != 0) {
                        printf("test_observer_core: FALSE TRIP at tick=%u empty=%u sampler=%u\n",
                               tick, empties[ei], samplers[si]);
                        assert(0 && "fired while the feed advanced");
                    }
                }
            }
        }
        printf("test_observer_core: property 3 (%lu triples swept, 0 false trips) OK\n", triples);
    }

    /* ---- 4. the timer-overdue arm is UNCHANGED --------------------------- */
    {
        struct ObsCore st; memset(&st, 0, sizeof st);
        struct ObsSample s; memset(&s, 0, sizeof s);
        s.now_ms = 100000; s.n = 1;
        s.period_ms[0] = 2000;
        s.since_ms[0]  = 2000u * OBS_WEDGE_SLACK + 5u;
        s.drained = 7; s.app_cad_ms = 16000;
        assert(observer_core_verdict(&st, &s) == OBS_COND_TIMER_OVERDUE);
        assert(st.deadline_ms == 16000u && st.margin_ms == 5u);
        printf("test_observer_core: property 4 (timer-overdue fires against its own "
               "period, deadline=%u overdue=%u) OK\n", st.deadline_ms, st.margin_ms);
    }

    /* ---- 5. an unjudgeable period is skipped; an unjudgeable appCad too -- */
    {
        struct ObsCore st; memset(&st, 0, sizeof st);
        struct ObsSample s; memset(&s, 0, sizeof s);
        s.now_ms = 500000; s.n = 1;
        s.period_ms[0] = 300000000u; s.since_ms[0] = 400000000u;
        s.drained = 0; s.app_cad_ms = 0;
        assert(observer_core_verdict(&st, &s) == OBS_COND_NONE);
        s.app_cad_ms = 0x80000000u;               /* past OBS_MAX_JUDGEABLE_MS */
        assert(observer_core_verdict(&st, &s) == OBS_COND_NONE);
        printf("test_observer_core: property 5 (an unjudgeable period or appCad is "
               "skipped, not guessed) OK\n");
    }

    /* ---- 6. undeclared appCad means §3.2 is NOT judged ------------------- *
     * With no declaration the feed is not judged at all, whatever the timers say.
     * This is the inference's deletion made executable: a frozen feed beside a
     * live 2 s timer answers NONE — and that is why every bridge declares. */
    {
        struct Workload wedged = { .tick_ms = 2000, .empty_ms = 0, .block_ms = 0, .drains = 0 };
        assert(run_timeline(&wedged, 2000, 0, 1001, 120000, NULL, NULL) == 0);
        struct ObsCore st; memset(&st, 0, sizeof st);
        struct ObsSample s; memset(&s, 0, sizeof s);
        s.now_ms = 900000; s.n = 0; s.drained = 0; s.app_cad_ms = 0;
        assert(observer_core_verdict(&st, &s) == OBS_COND_NONE);
        printf("test_observer_core: property 6 (no declaration => §3.2 unjudged; "
               "no timer and no declaration => NONE) OK\n");
    }

    /* ---- 7. the now == 0 degenerate input -------------------------------- */
    {
        struct ObsCore st; memset(&st, 0, sizeof st);
        struct ObsSample s; memset(&s, 0, sizeof s);
        s.now_ms = 0; s.n = 1; s.period_ms[0] = 2000; s.since_ms[0] = 0;
        s.drained = 0; s.app_cad_ms = 16000;
        assert(observer_core_verdict(&st, &s) == OBS_COND_NONE);   /* first sample latches */
        assert(st.fed_at_ms == 1u);
        printf("test_observer_core: property 7 (now==0 latches 1, no underflow) OK\n");
    }

    /* ---- 8. THE FAST-POLL FALSE TRIP (rete#273 D1, the defect) ----------- *
     * A 100 ms timer beside a 1.5 s blocking reaction. The legacy rule judges
     * 100 × 8 = 800 ms and reboots a healthy node; the declared rule, told the
     * app's longest reaction is inside 4 s, does not. Reproduce FIRST. */
    {
        struct Workload fastpoll = { .tick_ms = 2000, .empty_ms = 0, .block_ms = 1500, .drains = 1 };
        uint32_t margin = 0, deadline = 0;
        uint32_t legacy_fire = run_legacy(&fastpoll, 100, 101, 60000, &margin, &deadline);
        assert(legacy_fire != 0);
        assert(deadline == 800u);
        assert(run_timeline(&fastpoll, 100, 4000, 101, 60000, NULL, NULL) == 0);
        printf("test_observer_core: property 8 (fast-poll: legacy false-trips at %u ms on "
               "deadline=%u; declared appCad=4000 never trips in 60 s) OK\n",
               legacy_fire, deadline);
    }

    /* ---- 9. RUN-ONCE: no repeating timer, a declaration, a wedge ---------- *
     * n == 0 used to mean unjudgeable. With a declaration the feed is judged
     * anyway, so a node that registers nothing and then stops dispatching is
     * caught at ITS number. Today's rule answers NONE forever here. */
    {
        struct Workload stuck = { .tick_ms = 1000, .empty_ms = 0, .block_ms = 0, .drains = 0 };
        uint32_t margin = 0, deadline = 0;
        uint32_t fire = run_timeline(&stuck, 0, 4000, 1001, 60000, &margin, &deadline);
        assert(fire != 0 && deadline == 4000u);
        assert(run_legacy(&stuck, 0, 1001, 60000, NULL, NULL) == 0);
        printf("test_observer_core: property 9 (run-once: declared 4000 with NO timer "
               "fires at %u ms; the legacy rule never judges it) OK\n", fire);
    }

    printf("test_observer_core: all properties OK\n");
    return 0;
}
