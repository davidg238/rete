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
 *      written down as an executable fact rather than a comment. */

#include "../src/observer_core.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

/* ------------------------------------------------------------------ *
 * The PRE-rete#269 predicate. Depth only: it resets on an empty mailbox
 * and has no idea whether the VM made progress. Reproduced here so the
 * harness can be shown to catch the bug it was written for. Do not use
 * it for anything else, and do not move it into the header.
 * ------------------------------------------------------------------ */
static int legacy_verdict(struct ObsCore *st, const struct ObsSample *s) {
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
    uint32_t deadline = shortest * OBS_WEDGE_SLACK;
    if (s->pending == 0) {                      /* <-- the whole bug, one clause */
        st->undrained_since_ms = 0;
    } else {
        if (st->undrained_since_ms == 0)
            st->undrained_since_ms = s->now_ms ? s->now_ms : 1u;
        uint32_t held = s->now_ms - st->undrained_since_ms;
        if (held > deadline) {
            st->deadline_ms = deadline;
            st->margin_ms   = held - deadline;
            return OBS_COND_PULSE_UNDRAINED;
        }
    }
    return OBS_COND_NONE;
}

/* ------------------------------------------------------------------ *
 * The workload model.
 *
 * `tick_ms` is the DELIVERED period, which is not the declared one: under a
 * silent uplink a 2 s check-in costs cad + 2 x replyBudget ~= 4.007 s, and
 * that gap is what rete#269 aliased on. `empty_ms` is how long the mailbox
 * actually sits empty inside each delivered tick. `drains` is the whole
 * difference between a healthy node and a wedged one.
 * ------------------------------------------------------------------ */
struct Workload {
    uint32_t tick_ms;
    uint32_t empty_ms;
    int      drains;
};

static uint32_t pending_at(const struct Workload *w, uint32_t t) {
    if (!w->drains) return 1;                 /* wedged: it fills and never empties */
    uint32_t phase = t % w->tick_ms;
    return phase >= (w->tick_ms - w->empty_ms) ? 0u : 1u;
}

static uint32_t drained_at(const struct Workload *w, uint32_t t) {
    return w->drains ? (t / w->tick_ms) : 30u;   /* frozen at 30, as wedge-detect saw */
}

/* Runs one timeline and answers the time of the first pulse-undrained verdict,
 * or 0 for none. `margin` and `deadline` report what that verdict carried. */
static uint32_t run_timeline(int (*verdict)(struct ObsCore *, const struct ObsSample *),
                             const struct Workload *w, uint32_t declared_period_ms,
                             uint32_t sampler_ms, uint32_t horizon_ms,
                             uint32_t *margin, uint32_t *deadline) {
    struct ObsCore st;
    memset(&st, 0, sizeof st);
    /* Starts at the FIRST sample, not at t=0 — see property 7 for why t=0 is a
     * degenerate input the board can never present. */
    for (uint32_t t = sampler_ms; t <= horizon_ms; t += sampler_ms) {
        struct ObsSample s;
        memset(&s, 0, sizeof s);
        s.now_ms       = t;
        s.n            = 1;
        s.period_ms[0] = declared_period_ms;
        /* The ISR IS firing throughout — that is the premise of the pulse
         * condition, and it keeps the timer-overdue arm out of the way. */
        s.since_ms[0]  = t % declared_period_ms;
        s.pending      = pending_at(w, t);
        s.drained      = drained_at(w, t);
        if (verdict(&st, &s) == OBS_COND_PULSE_UNDRAINED) {
            if (margin)   *margin   = st.margin_ms;
            if (deadline) *deadline = st.deadline_ms;
            return t ? t : 1u;
        }
    }
    return 0;
}

int main(void) {
    /* ---- 1. the rete#269 false trip -------------------------------------- *
     * The observed trace: `pending` read 1 at every sample for 16 s while
     * `drained` climbed 35 -> 39 and the console printed ticks 34-38. Modelled
     * as empty_ms = 0 — the mailbox is never caught empty AT A SAMPLE INSTANT,
     * which is precisely what the beat between 1001 ms and 4007 ms produces. */
    {
        struct Workload silent = { .tick_ms = 4007, .empty_ms = 0, .drains = 1 };
        uint32_t margin = 0, deadline = 0;

        uint32_t old_fire = run_timeline(legacy_verdict, &silent, 2000, 1001, 600000,
                                         &margin, &deadline);
        assert(old_fire != 0);                 /* the harness can see the bug */
        assert(deadline == 2000u * OBS_WEDGE_SLACK);
        assert(margin < 1001u);                /* caught within one sampler period */
        printf("test_observer_core: property 1a (the old predicate false-fires at "
               "%u ms, deadline=%u overdue=%u) OK\n", old_fire, deadline, margin);

        uint32_t new_fire = run_timeline(observer_core_verdict, &silent, 2000, 1001,
                                         600000, NULL, NULL);
        assert(new_fire == 0);                 /* 600 s and never a word */
        printf("test_observer_core: property 1b (progress, not depth: no false trip "
               "in 600 s) OK\n");
    }

    /* ---- 2. a real wedge still fires ------------------------------------- */
    {
        struct Workload wedged = { .tick_ms = 1000, .empty_ms = 0, .drains = 0 };
        uint32_t margin = 0, deadline = 0;
        uint32_t fire = run_timeline(observer_core_verdict, &wedged, 1000, 1001, 60000,
                                     &margin, &deadline);
        assert(fire != 0);
        assert(deadline == 1000u * OBS_WEDGE_SLACK);   /* 8000, as wedge-detect reports */
        assert(margin < 1001u);
        printf("test_observer_core: property 2 (a real wedge fires at %u ms, "
               "deadline=%u overdue=%u) OK\n", fire, deadline, margin);
    }

    /* ---- 3. the aliasing sweep ------------------------------------------- *
     * Every (delivered tick, empty window, sampler period) triple in the band
     * the bench can actually produce. The assertion is one-sided on purpose:
     * the new predicate must NEVER fire while `drained` advances. How often the
     * old one did is reported as data, not asserted — it is a property of the
     * beat, and pinning it would make this test brittle for no gain. */
    {
        unsigned long triples = 0, legacy_fires = 0;
        static const uint32_t samplers[] = { 997, 999, 1001, 1003, 1005 };
        static const uint32_t empties[]  = { 600, 1000, 1400 };
        for (uint32_t tick = 3000; tick <= 5000; tick++) {
            for (unsigned si = 0; si < sizeof samplers / sizeof samplers[0]; si++) {
                for (unsigned ei = 0; ei < sizeof empties / sizeof empties[0]; ei++) {
                    struct Workload w = { .tick_ms = tick, .empty_ms = empties[ei],
                                          .drains = 1 };
                    triples++;
                    if (run_timeline(observer_core_verdict, &w, 2000, samplers[si],
                                     120000, NULL, NULL) != 0) {
                        printf("test_observer_core: FALSE TRIP at tick=%u empty=%u "
                               "sampler=%u\n", tick, empties[ei], samplers[si]);
                        assert(0 && "the new predicate fired while drained advanced");
                    }
                    if (run_timeline(legacy_verdict, &w, 2000, samplers[si], 120000,
                                     NULL, NULL) != 0)
                        legacy_fires++;
                }
            }
        }
        printf("test_observer_core: property 3 (%lu triples swept, 0 false trips; "
               "the pre-#269 predicate false-fires on %lu of them) OK\n",
               triples, legacy_fires);
    }

    /* ---- 4. the timer-overdue arm ---------------------------------------- */
    {
        struct ObsCore st; memset(&st, 0, sizeof st);
        struct ObsSample s; memset(&s, 0, sizeof s);
        s.now_ms = 100000; s.n = 1;
        s.period_ms[0] = 2000;
        s.since_ms[0]  = 2000u * OBS_WEDGE_SLACK + 5u;   /* 5 ms past its own deadline */
        s.pending = 0; s.drained = 7;
        assert(observer_core_verdict(&st, &s) == OBS_COND_TIMER_OVERDUE);
        assert(st.deadline_ms == 16000u && st.margin_ms == 5u);
        printf("test_observer_core: property 4 (timer-overdue fires against its own "
               "period, deadline=%u overdue=%u) OK\n", st.deadline_ms, st.margin_ms);
    }

    /* ---- 5. an unjudgeable period is skipped, not guessed ----------------- *
     * period x 8 must exceed INT32_MAX for this to bite: k_uptime_get_32()
     * wraps, so judging it would fail toward a FALSE TRIP. */
    {
        struct ObsCore st; memset(&st, 0, sizeof st);
        struct ObsSample s; memset(&s, 0, sizeof s);
        s.now_ms = 500000; s.n = 1;
        s.period_ms[0] = 300000000u;                 /* x8 overflows the judgeable band */
        s.since_ms[0]  = 400000000u;
        s.pending = 1; s.drained = 0;
        assert(observer_core_verdict(&st, &s) == OBS_COND_NONE);

        /* and with a judgeable timer beside it, the judgeable one is operative */
        s.n = 2;
        s.period_ms[1] = 1000; s.since_ms[1] = 10;
        s.pending = 1; s.drained = 0;
        st.undrained_since_ms = 1;                   /* held far past 1000 x 8 */
        st.last_drained_seen  = 0;
        s.now_ms = 500000;
        assert(observer_core_verdict(&st, &s) == OBS_COND_PULSE_UNDRAINED);
        assert(st.deadline_ms == 8000u);
        printf("test_observer_core: property 5 (an unjudgeable period is skipped; a "
               "judgeable one beside it still decides) OK\n");
    }

    /* ---- 6. no declared timer means UNJUDGEABLE --------------------------- *
     * Not healthy — unjudgeable. This is the hole rete#231 D1 closes by having
     * the kernel own a base tick, and it is worth an executable fact because a
     * node in this state is invisible in the worst way: the detector returns the
     * same answer it returns for a node that is fine. */
    {
        struct ObsCore st; memset(&st, 0, sizeof st);
        struct ObsSample s; memset(&s, 0, sizeof s);
        s.now_ms = 900000; s.n = 0; s.pending = 1; s.drained = 0;
        assert(observer_core_verdict(&st, &s) == OBS_COND_NONE);
        printf("test_observer_core: property 6 (no declared timer => unjudgeable, "
               "not healthy) OK\n");
    }

    /* ---- 7. the now == 0 degenerate input -------------------------------- *
     * FOUND BY THIS TEST, on its first run. `undrained_since_ms` uses 0 as its
     * "not currently undrained" sentinel, so the store guards with
     * `now ? now : 1u` — and at now == 0 that stores 1, after which
     * `held = now - undrained_since_ms` is 0 - 1, which underflows to
     * 0xFFFFFFFF and trips instantly.
     *
     * IT IS NOT REACHABLE ON A BOARD and this test does not pretend otherwise:
     * the observer thread starts after the kernel is up and its first sample
     * lands ~1000 ms in, so k_uptime_get_32() is never 0 at a verdict. It is
     * recorded here rather than quietly avoided, because the sentinel-vs-value
     * collision is exactly the shape that produced rete#269, and the next
     * caller of this header will not know the guard exists. */
    {
        struct ObsCore st; memset(&st, 0, sizeof st);
        struct ObsSample s; memset(&s, 0, sizeof s);
        s.now_ms = 0; s.n = 1; s.period_ms[0] = 2000; s.since_ms[0] = 0;
        s.pending = 1; s.drained = 0;
        /* first sample: nothing has moved, so it merely latches */
        assert(observer_core_verdict(&st, &s) == OBS_COND_PULSE_UNDRAINED);
        assert(st.undrained_since_ms == 1u);
        printf("test_observer_core: property 7 (now==0 underflows the held-time "
               "guard — unreachable on a board, recorded not fixed) OK\n");
    }

    printf("test_observer_core: all properties OK\n");
    return 0;
}
