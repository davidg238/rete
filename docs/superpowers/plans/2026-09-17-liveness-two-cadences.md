# Two liveness cadences (#273 D1 + D2) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the one conflated `cad` with two declared numbers — `appCad` (node-side, judged by the observer, never on the wire) and `telCad` + `misses` (published; porta derives the budget) — and land it as ONE manifest-moving change with its gates.

**Architecture:** On the node the observer's §3.2 rule becomes *feed age > appCad*, where the feed IS `pulse_drained()` (the kernel's event dispatch — amended D1, 2026-09-17) and `appCad` arrives through one new platform primitive. The legacy inference (`shortest period × OBS_WEDGE_SLACK`) is DELETED from §3.2; `OBS_WEDGE_SLACK` survives only in §3.1. On the wire `Liveness { misses, telCad }` replaces `{ cad, bud }`; porta computes `bud = telCad × misses`, refuses to judge a budget below its own sweep tick (D2.1), and reports the miss count in the reason (D2.2). Both ends reflash together; there is no compatibility window.

**Tech Stack:** C99 (`nodus/oevm/src`, host-testable header), Zephyr C (`nodus/oevm/zephyr/src`), nsl (`nodus/nsl/lib/kernel.nsl` + node apps), Go 1.26 (`porta`), CDDL/zcbor (`protocol/MANIFEST.cddl`), bash target harness (`nodus/oevm/tests/target`).

**Progress (2026-09-17):** Tasks 1–9 DONE and committed (`1315e1e`, `00c1470`, `365f8f9`, `47633bb`, `9b6332a`, `3b4331c`, `1e6f921`, `888500f`, + the promotion commit); whole tree green, five board gates green on `c6spare` with `liveness-fastpoll` RED on the pre-fix VM first. **Task 10 (the gateway landing) is WITH DAVID.** Deviations recorded in the commits: fastpoll's block is 3 s / 4 s (1.5 s did not reproduce the red) and it declares 2 × blockMs = 6000; `run.sh`'s C6 flash was broken since `151643f` and is fixed in `52eaedc`; the custos descriptor table was a reader the plan missed.

**Spec:** `docs/design/2026-09-14-liveness-two-cadences-design.md` (D1 as amended 2026-09-17 in commit `bb26b32`; D2, D2.1, D2.2, D2.3; §2 deletions; §4 falsification; §5 sequencing; §6 promotion). **D3 (`#stalled`) is NOT in this plan** — it needs the TLA+ `DoSweep` correction first and is its own plan.

## Global Constraints

- Branch: `281-nrf-porta-bridge` (current). Commit per task; `refs #273` in every message; end with the Co-Authored-By line.
- Go: `gofmt -l .` prints nothing before every commit; new Go files carry `// Copyright (c) 2026 Ekorau LLC`.
- nsl tests: inline `(* expect: *)` at the TOP; run one with `cd nodus/nsl && python3 tools/run-one.py <file.nsl> B` (ALWAYS pass `B`); the full suite is `python3 tools/run_tests_oevm.py` — READ THE AUX LINE, not only the count. Never two suites concurrently.
- `MANIFEST.cddl`: named sym keys only; KEY ORDER IS WIRE ORDER (sorted by encoded bytes); no quantified tagged list element (zcbor 0.9.1). `misses` precedes `telCad`.
- A new C prim touches FOUR places: `src/prim_names.def` (append), `src/dispatch.c` (enum append + parity guard), `src/prim_ids.h` (`#define`), `src/platform.c` (case). The id is arrived at by COUNTING entries, not by arithmetic.
- The nsl field for the tolerated-miss count is `missBudget` — `misses` is ALREADY a PortaBridge field (the reply-miss counter printed as `tick N misses M`). The WIRE key is `misses`.
- Board work runs on `c6spare` only (`class=disruptive`), through `nodus/oevm/tests/target/run.sh`. Budget ~5 min per scenario. `c6spare` has both cables and a live collector (verified 2026-09-17 11:12).
- Every `deadline=16000ms` claim in the existing gates must survive UNCHANGED: `appCad` defaults to `cad × 8` inside the bridge, so the number does not move, only the rule that produced it.

---

### Task 1: The observer rule — `feed age > appCad`, legacy §3.2 deleted (host, TDD)

**Files:**
- Modify: `nodus/oevm/src/observer_core.h:52-151`
- Modify: `nodus/oevm/tests/test_observer_core.c` (whole file: properties 1–7 rewritten around the new rule; the legacy copy stays as the reproduce-first witness)

**Interfaces:**
- Produces: `struct ObsSample { now_ms, n, period_ms[], since_ms[], drained, app_cad_ms }` (no `pending`); `struct ObsCore { fed_at_ms, last_drained_seen, margin_ms, deadline_ms }` (no `undrained_since_ms`); `OBS_COND_APP_STALE` replaces `OBS_COND_PULSE_UNDRAINED`. Task 2 and Task 8 depend on these names.

- [x] **Step 1: Rewrite the test first.** Replace `nodus/oevm/tests/test_observer_core.c` from the `#include` lines to the end of `main` with the following. The file's leading comment block (lines 1–~30) stays; append one paragraph to it: *"rete#273 D1 (2026-09-17): §3.2 now judges FEED AGE against a DECLARED `app_cad_ms`; the shortest-period inference is gone. `legacy_verdict` below is the harness's own copy of BOTH old rules and is what lets property 8 show the fast-poll false trip before the fix."*

```c
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
```

- [x] **Step 2: Run it and watch it fail to compile** (no `app_cad_ms`, no `fed_at_ms`, no `OBS_COND_APP_STALE`):

```bash
cd /home/david/workspace/rete/nodus/oevm && gcc -Wall -Wextra -std=c99 -Isrc tests/test_observer_core.c -o /tmp/claude-1000/test_observer_core && /tmp/claude-1000/test_observer_core
```
Expected: compile errors naming those three identifiers.

- [x] **Step 3: Rewrite the header.** In `nodus/oevm/src/observer_core.h` replace everything from `enum {` (line 52) to the closing `}` of `observer_core_verdict` (line 151) with:

```c
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
        if (progressed || st->fed_at_ms == 0) {
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
```

Also edit the header's top comment (lines 5–31): replace the paragraph beginning `THE ONE THING A READER MUST NOT MISS (rete#269)` with: *"THE ONE THING A READER MUST NOT MISS (rete#269, rete#273): §3.2 judges the AGE OF THE FEED against a DECLARED number. `drained` is monotonic and moves only when the interpreter runs the pump, so it integrates over the whole interval rather than sampling an instant; mailbox DEPTH is not read here at all any more, because a depth sample aliases against the workload (the 2026-08-27 false trip). tests/test_observer_core.c holds the properties."* Update the "a function of (declared periods, time since each fired, mailbox depth, drain counter, now)" phrase on line 13–14 to "(declared periods, time since each fired, the drain counter, the declared app cadence, now)".

- [x] **Step 4: Run the test; all nine properties pass.**

```bash
cd /home/david/workspace/rete/nodus/oevm && gcc -Wall -Wextra -std=c99 -Isrc tests/test_observer_core.c -o /tmp/claude-1000/test_observer_core && /tmp/claude-1000/test_observer_core
```
Expected: nine `OK` lines then `all properties OK`. Property 8 must print a non-zero legacy fire time with `deadline=800`.

- [x] **Step 5: Commit.**

```bash
cd /home/david/workspace/rete && git add nodus/oevm/src/observer_core.h nodus/oevm/tests/test_observer_core.c && git commit -m "fix(#273): §3.2 judges feed age against a DECLARED appCad; the shortest-period inference is deleted

Property 8 reproduces the fast-poll false trip on the old rule (800 ms deadline
against a 1.5 s reaction) and shows the declared rule quiet; property 9 shows a
run-once node with NO timer judged at its own number where the old rule never
judged it. OBS_WEDGE_SLACK now lives in §3.1 only.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 2: The primitive `platform appLiveness: ms` and the observer seam

**Files:**
- Modify: `nodus/oevm/src/prim_names.def` (append after `PRIM_NAME("\\\\")`)
- Modify: `nodus/oevm/src/dispatch.c:983-1006` (enum + parity guard)
- Modify: `nodus/oevm/src/prim_ids.h:556` (new `#define`)
- Modify: `nodus/oevm/src/platform.c` (new case beside `PRIM_pf_manifestPin` at ~1522; new accessor)
- Modify: `nodus/oevm/zephyr/src/observer.c:55-60, 100-106, 138-146, 311-316, 349-357`

**Interfaces:**
- Produces: nsl selector `appLiveness:` (one SMI arg, ms; answers the stored value as SMI); C accessor `uint32_t platform_app_cad_ms(void)`. Task 3 sends the selector from the kernel.

- [x] **Step 1: Count, then append the name.** 

```bash
cd /home/david/workspace/rete/nodus/oevm && grep -c '^PRIM_NAME(' src/prim_names.def
```
Expected: `157`. The new entry is therefore id **158**. Append to `src/prim_names.def` as the LAST line:

```c
PRIM_NAME("appLiveness:")
```

- [x] **Step 2: Append the enum member and move the parity guard.** In `src/dispatch.c`, after `PRIM_intMod,` (line 983) add:

```c
    /* rete#273 D1: `platform appLiveness: ms` — the 158th PRIM_NAMES entry (counted,
       not computed). Appended after intMod; no pre-existing id shifts.
       Platform-dispatched (mirrored in prim_ids.h). The app's declared cadence for
       the observer's §3.2; 0 withdraws it. */
    PRIM_pf_appLiveness,
```
and change the parity guard (line 1006) from `PRIM_intMod` to `PRIM_pf_appLiveness`:

```c
typedef char prim_names_enum_parity_check[(N_PRIM_NAMES == PRIM_pf_appLiveness) ? 1 : -1];
```

- [x] **Step 3: Mirror the id.** In `src/prim_ids.h` before `#endif /* OEVM_PRIM_IDS_H */` add:

```c
/* rete#273 D1: `appLiveness:` — the app's DECLARED cadence for the observer's §3.2
   (feed age > appCad). Appended after manifestPin AND after dispatch.c's PRIM_intMod
   (157, not platform-dispatched, so not mirrored here); 158 was arrived at by
   COUNTING PRIM_NAMES (157 entries before this append), not by arithmetic.
   Platform-dispatched. Stores a uint32 the observer thread reads through
   platform_app_cad_ms(); 0 = undeclared = §3.2 not judged. Answers the stored
   value, so a caller can see what a clamp did. */
#define PRIM_pf_appLiveness       158
```

- [x] **Step 4: The case and the accessor.** In `src/platform.c`, immediately before `case PRIM_pf_manifestPin: {` add:

```c
    case PRIM_pf_appLiveness: {
        /* rete#273 D1 — `platform appLiveness: ms`. One SMI in, stored for the
           observer thread (zephyr/src/observer.c reads it every sample through
           platform_app_cad_ms()). Non-SMI or negative reads as 0 = undeclared;
           anything past OBS_MAX_JUDGEABLE_MS is also 0, because a deadline the
           32-bit uptime clock cannot compare against would fail toward a false
           trip. No allocation. */
        intptr_t n = IS_SMI(args[0]) ? UNTAG_SMI(args[0]) : 0;
        if (n < 0 || n > 0x7FFFFFFF) n = 0;
        s_app_cad_ms = (uint32_t)n;
        *out = TAG_SMI((intptr_t)s_app_cad_ms);
        return 1;
    }
```
and near the top of `platform.c`'s file-scope statics (search for the first `static` after the includes) add:

```c
/* rete#273 D1: the app's declared cadence for the observer (PRIM_pf_appLiveness).
   Read from the observer THREAD on Zephyr; a uint32 store/load is atomic on every
   target this runs on, and a torn read would only ever mis-time one sample. */
static uint32_t s_app_cad_ms = 0;
uint32_t platform_app_cad_ms(void) { return s_app_cad_ms; }
```

- [x] **Step 5: The observer seam.** In `nodus/oevm/zephyr/src/observer.c`:
  1. After the `int timer_target_snapshot(...)` extern (line 58) add:
     ```c
     /* rete#273 D1: the app's declared cadence, defined in src/platform.c beside the prim. */
     uint32_t platform_app_cad_ms(void);
     ```
  2. In `observer_verdict()` (lines 139–146) replace the body with:
     ```c
     struct ObsSample s;
     s.n = timer_target_snapshot(s.period_ms, s.since_ms, OBS_MAX_TIMERS);
     s.now_ms     = k_uptime_get_32();
     s.drained    = pulse_drained();
     s.app_cad_ms = platform_app_cad_ms();
     return observer_core_verdict(&g_core, &s);
     ```
  3. Line 105 `e->since = g_core.undrained_since_ms;` → `e->since = g_core.fed_at_ms;` and read the trace-ring comment above it: if it says "undrained since", change the words to "fed at".
  4. In `observer_dump()` (line 313) replace the ternary with:
     ```c
     cond == OBS_COND_TIMER_OVERDUE ? "timer-overdue" : "app-stale",
     ```
     and add `appCad=%ums ` to the format after `deadline=%ums ` with `(unsigned)platform_app_cad_ms()` as the matching argument, so the dump names the declaration beside the deadline that fired.
  5. Line 356 (synthetic trigger) `observer_dump(OBS_COND_PULSE_UNDRAINED);` → `observer_dump(OBS_COND_APP_STALE);`.
  6. Update the comment at lines 137–138 ("the app's declared cadences, the mailbox depth, and the monotonic drain counter") to "the app's declared timer periods, the monotonic drain counter, and the declared app cadence".

- [x] **Step 6: Host compile check of the C side (the parity guard is the test).**

```bash
cd /home/david/workspace/rete/nodus/nsl && python3 tools/run_tests_oevm.py 2>&1 | tail -6
```
Expected: `Passed: N / Failed: 0`, aux line shows `c-units: ok`. (nsl tests that send `liveness:budget:` still pass here — Task 3 changes them.) If `c-units` is red, the parity guard or the case is wrong; read the compiler line.

- [x] **Step 7: Zephyr compile check** — no host gate compiles `observer.c`. Replay the last board build's compile command:

```bash
cd /home/david/workspace/rete/nodus/oevm/zephyr && f=$(ls -t build-*/compile_commands.json 2>/dev/null | head -1); echo "$f"; python3 -c "
import json,sys,subprocess,shlex
cc=json.load(open('$f'))
for e in cc:
    if e['file'].endswith('zephyr/src/observer.c'):
        cmd=e['command'].split(' -o ')[0]+' -fsyntax-only'
        print(subprocess.run(cmd,shell=True,cwd=e['directory']).returncode); break
else: print('observer.c not in this build')"
```
Expected: `0`. If no build dir exists, this check moves to Task 8 Step 3 (the first board build).

- [x] **Step 8: Commit.**

```bash
cd /home/david/workspace/rete && git add nodus/oevm/src/prim_names.def nodus/oevm/src/dispatch.c nodus/oevm/src/prim_ids.h nodus/oevm/src/platform.c nodus/oevm/zephyr/src/observer.c && git commit -m "feat(#273): platform appLiveness: — the declared appCad reaches the observer (prim 158)

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 3: The kernel — `liveness:misses:`, `appLiveness:`, `{misses, telCad}` on the wire

**Files:**
- Modify: `nodus/nsl/lib/kernel.nsl:1685-1705` (fields), `:2495-2549` (`adoptCad:bud:` → `adoptCad:misses:`), `:2575-2580` (`startTicking:` declares), `:2600-2638` (`applyLivenessGoal`, `publishLiveness`), `:~4082` (Kernel forwarder)
- Modify: `nodus/tests/22_spacesync/17_liveness_declared.nsl`, `18_liveness_grid.nsl`, `19_liveness_goal.nsl`, `20_liveness_size.nsl`, `21_liveness_goal_stream.nsl`, `22_kernel_tick.nsl`, `23_liveness_cad_rearm.nsl`
- Create: `nodus/tests/22_spacesync/24_liveness_app_cad.nsl`

**Interfaces:**
- Produces (PortaBridge): `liveness: aCadMs misses: aMisses`, `appLiveness: aMs`, accessors `cadMs`, `missBudget`, `budMs` (= `cadMs * missBudget`, derived), `appCadMs` (effective: the declaration, or `cadMs * 8`). Wire record `Liveness { misses, telCad }`. Kernel: `appLiveness: aMs` forwarder.
- Consumes: `platform appLiveness:` from Task 2.

- [x] **Step 1: Rewrite the tests first.** 

`17_liveness_declared.nsl` — expect block becomes:
```
tag true
telCad 30000
misses 8
published once true
seq advanced true
```
and the two print lines become:
```
    ('telCad ' , (rec at: #telCad ifAbsent: [ -1 ]) printString) println.
    ('misses ' , (rec at: #misses ifAbsent: [ -1 ]) printString) println.
```

`18_liveness_grid.nsl` — replace the header's three rules with:
```
Three rules, each of which the gateway must never have to know in advance:
  misses >= 1          — a budget below one cadence promises a silence the node
                         cannot keep even when perfectly healthy
  bud = telCad x misses — STRUCTURAL since rete#273 D2: there is no bud on the
                         wire to snap, so the grid cannot be violated
  bud <= 24 h          — CLAMPED on misses and published, never rejected (D7)
```
expect block:
```
plain 2000 8 16000
floor 2000 1 2000
ceiling 30000 2880 86400000
cadceiling 86400000 1 86400000
republished true
idempotent true
```
`show:bridge:` prints `b cadMs`, `b missBudget`, `b budMs`; the four cases become `b liveness: 2000 misses: 8`, `b liveness: 2000 misses: 0`, `b liveness: 30000 misses: 604800000`, `b liveness: 999999999 misses: 8` (labelled `cadceiling`); the republish/idempotent pair uses `b liveness: 2000 misses: 8`.

`19_liveness_goal.nsl` — expect:
```
absent leaves it alone true
adopted 2000 23 46000
clamped 2000 43200 86400000
```
the goal writes become `platform Record tag: #Liveness with: #misses -> n with: #telCad -> 2000` with n = 23 then 604800; the print lines add `missBudget` between `cadMs` and `budMs`; `b liveness: 2000 budget: 16000` → `b liveness: 2000 misses: 8`.

`20_liveness_size.nsl` — the record becomes `with: #misses -> 86400000 with: #telCad -> 86400000` (worst legal: a 1 ms telCad permits misses up to 86400000). Leave the expected number as `liveness 37` for now; Step 4 measures it.

`21_liveness_goal_stream.nsl`, `22_kernel_tick.nsl`, `23_liveness_cad_rearm.nsl` — every `liveness: 2000 budget: 16000` → `liveness: 2000 misses: 8`. In 23 the goal record becomes `with: #misses -> 3 with: #telCad -> 5000`, the expect line `adopted 5000 15000` stays (5000 × 3), and the header sentence "Budget re-snaps to the new grid: 12000 on a 5000 grid adopts as 15000" becomes "The budget is telCad × misses by construction: 5000 × 3 = 15000".

Create `24_liveness_app_cad.nsl`:
```
(*
rete#273 D1 — appCad is DECLARED by the app, defaults to telCad x 8, and is clamped.
The observer reads it through prim 158; on the host nothing judges it, so what this
proves is the kernel's half: the default, the override, the clamp, and that the
declaration survives a cadence re-arm.
expect:
default 16000
declared 4000
clamped 86400000
withdrawn 16000
rearmed 40000
*)
class LivenessAppCad packageUsing: manifest = () (
  bridgeOn: platform = (
    | b |
    b:: platform PortaBridge socket: nil space: platform space id: 'test' peer: '::1' port: 1
        spans: platform Spans new.
    b liveness: 2000 misses: 8.
    ^ b )

  public main: platform args: args = (
    | b |
    b:: self bridgeOn: platform.
    ('default ' , b appCadMs printString) println.
    b appLiveness: 4000.
    ('declared ' , b appCadMs printString) println.
    b appLiveness: 999999999.
    ('clamped ' , b appCadMs printString) println.
    b appLiveness: 0.
    ('withdrawn ' , b appCadMs printString) println.
    b liveness: 5000 misses: 8.
    ('rearmed ' , b appCadMs printString) println.
  )
) : (
)
```

- [x] **Step 2: Run 17 and 24 to see them fail.**

```bash
cd /home/david/workspace/rete/nodus/nsl && python3 tools/run-one.py ../tests/22_spacesync/17_liveness_declared.nsl B; python3 tools/run-one.py ../tests/22_spacesync/24_liveness_app_cad.nsl B
```
Expected: 17 prints `telCad -1`; 24 fails on `liveness:misses:` (does not understand).

- [x] **Step 3: The kernel edits.** In `nodus/nsl/lib/kernel.nsl`:

  1. Line 1705: `cadMs ::= 30000. budMs ::= 240000. livenessPublished ::= false.` → 
     ```
     cadMs ::= 30000. missBudget ::= 8. appCadDeclared ::= 0. livenessPublished ::= false.
     ```
     and rewrite the comment above it (lines 1690–1704) to:
     ```
     (* rete#231 D2 / rete#273 D2 — the liveness contract. `cadMs` is telCad: how
        often this node INTENDS to speak. `missBudget` is how many consecutive
        check-ins it tolerates losing to ordinary mesh loss before its silence
        means anything — c6-bell misses roughly half of its at 30 s, and the node
        is the only party that knows its own loss rate. porta derives the budget
        as telCad x misses; there is no `bud` on the wire and so no grid to snap
        (D2: an incoherent declaration is unrepresentable). 8 is the one default at
        every period (D2.3): the false-trip rate is ~p^misses and the period cancels.

        `appCadDeclared` is rete#273 D1's other number — how long the app's event
        dispatch may go without running before the OBSERVER reboots it. 0 means
        the app declared nothing and the bridge declares cadMs x 8 on its behalf
        (the same 16 s every gate already reads at cad 2000). It NEVER crosses
        the wire.

        `livenessPublished` is the obs/apps/main rule (#25): written once per boot,
        because a cell that never changes must not be allowed to crowd out the one
        that does under pushFrame's tail-trim. A declaration or an accepted goal
        clears it, which is the only way the value can move. *)
     ```
  2. Lines 2495–2549 (`public cadMs`/`budMs`, `livenessCeilingMs`, `adoptCad:bud:`, `liveness:budget:`) become:
     ```
     public cadMs      = ( ^ cadMs )
     public missBudget = ( ^ missBudget )
     (* DERIVED, never stored and never on the wire (rete#273 D2). *)
     public budMs      = ( ^ cadMs * missBudget )
     (* rete#273 D1: the EFFECTIVE app cadence — the declaration, or the bridge's
        default of telCad x 8 when the app said nothing. This is the number the
        observer holds. *)
     public appCadMs   = ( appCadDeclared = 0 ifTrue: [ ^ cadMs * 8 ]. ^ appCadDeclared )

     (* rete#231 D7 — the one invented constant in the design, and it is a ceiling on
        the DECLARATION rather than a cadence: a node speaking once a month is free to
        try, it simply cannot promise a month of silence and still be considered
        healthy. 24 h is the coarsest granularity at which a fleet is still a fleet.
        Since rete#273 D2 it bounds the PRODUCT telCad x misses, clamped on misses. *)
     livenessCeilingMs = ( ^ 86400000 )

     (* rete#231 D2/D7, rete#273 D2 — the ONE place a liveness pair is adopted,
        whether it came from the app (here) or from porta (applyLivenessGoal).

        CLAMPED, NEVER REJECTED. Every path out of this method adopts something and
        publishes it, so `goal/liveness` versus `obs/liveness` always answers "did the
        node accept?" — a silent refusal would read exactly like acceptance. *)
     adoptCad: aCadMs misses: aMisses = (
       | cad n ceiling cadMoved |
       ceiling:: self livenessCeilingMs.
       cad:: aCadMs.
       cad < 1 ifTrue: [ cad:: 30000 ].
       cad > ceiling ifTrue: [ cad:: ceiling ].
       n:: aMisses.
       n < 1 ifTrue: [ n:: 1 ].
       n > (ceiling // cad) ifTrue: [ n:: ceiling // cad ].
       (cad = cadMs and: [ n = missBudget ]) ifTrue: [ ^ self ].
       cadMoved:: (cad = cadMs) not.
       cadMs:: cad.
       missBudget:: n.
       (* rete#240.2 — an adopted cadence is a KEPT cadence: re-arm the base tick
          at the new period, and re-declare the app cadence, whose default rides
          on it. Guarded on tickToken so a pre-startTicking: declaration just sets
          the period startTicking: will use. *)
       (cadMoved and: [ (tickToken == nil) not ]) ifTrue: [
         tickTimers cancel: tickToken.
         tickToken:: tickTimers every: cadMs post: #go on: tickChannel.
         self declareAppCad ].
       livenessPublished:: false.
       ^ self )

     (* The app's declaration of how often it will speak and how many misses it
        tolerates. A SETTER and not a constructor argument, on purpose (see
        startTicking:). *)
     public liveness: aCadMs misses: aMisses = ( ^ self adoptCad: aCadMs misses: aMisses )

     (* rete#273 D1 — the app's declaration of how long its event dispatch may go
        without running. 0 withdraws it and the bridge's default (telCad x 8) is
        back in force. Clamped to the same 24 h ceiling, published to the OBSERVER
        only — this number never rides the wire. Takes effect at once if the tick
        is already armed, otherwise at startTicking:. *)
     public appLiveness: aMs = (
       | ms |
       ms:: aMs.
       ms < 0 ifTrue: [ ms:: 0 ].
       ms > self livenessCeilingMs ifTrue: [ ms:: self livenessCeilingMs ].
       appCadDeclared:: ms.
       (tickToken == nil) ifFalse: [ self declareAppCad ].
       ^ self )

     (* The one send that reaches the observer. Prints the number and whether it
        was chosen or defaulted (rete#273 D1.1: a dump that cannot tell "the app
        chose 16 s" from "nobody chose anything" invites reading an inference as
        a decision). *)
     declareAppCad = (
       platform appLiveness: self appCadMs.
       ^ self )
     ```
  3. In `startTicking:on:` (line 2575) after `tickToken:: aTimers every: cadMs post: #go on: aChannel.` add `self declareAppCad.`
  4. `applyLivenessGoal` (2600–2623): `bud:: rec at: #bud` → `n:: rec at: #misses`, `cad:: rec at: #cad` → `cad:: rec at: #telCad`, temp list `| rec n cad |`, last line `^ self adoptCad: cad misses: n`.
  5. `publishLiveness` (2634–2638): the record becomes
     ```
     space put: 'obs/liveness' value: (Record tag: #Liveness
       with: #misses -> missBudget
       with: #telCad -> cadMs).
     ```
  6. Kernel forwarder, after `public nodeId = ( ^ platform nodeId )` (~4082):
     ```
     (* rete#273 D1: an app WITHOUT a bridge (the wedge probes) declares its own
        app cadence straight to the observer. Bridged apps go through
        PortaBridge>>appLiveness:, which also owns the default. *)
     public appLiveness: aMs = ( platform appLiveness: aMs. ^ self )
     ```
  7. Grep the kernel for every other reader of `budMs`/`bud`/`#bud`/`#cad` and fix each: `rg -n "budMs|#bud|#cad\b|adoptCad: |liveness:.*budget:" nodus/nsl/lib/kernel.nsl`. Expected after the edit: only the lines written above.

- [x] **Step 4: Run the seven tests one by one; then measure 20.**

```bash
cd /home/david/workspace/rete/nodus/nsl && for t in 17_liveness_declared 18_liveness_grid 19_liveness_goal 21_liveness_goal_stream 22_kernel_tick 23_liveness_cad_rearm 24_liveness_app_cad; do python3 tools/run-one.py ../tests/22_spacesync/$t.nsl B 2>&1 | tail -2; done; python3 tools/run-one.py ../tests/22_spacesync/20_liveness_size.nsl B 2>&1 | grep -i "liveness"
```
Expected: six PASS lines; 20 prints `liveness <N>` where N is the measured size. Pin N in 20's expect block AND note it for Task 9 (RECORDS). N should be near 43 (two 6-char keys at 9 B each, two 5-byte uints, 11 B tag+name, 3 B framing); if it is far from that, the record is malformed — look before pinning.

- [x] **Step 5: Full nsl suite.**

```bash
cd /home/david/workspace/rete/nodus/nsl && python3 tools/run_tests_oevm.py 2>&1 | tail -6
```
Expected: `Passed: 280 / Failed: 0` (279 + test 24) and a green aux line EXCEPT `tutorial-porta`, which is expected RED until Task 6 changes porta and the tutorial page — note the failing line and move on.

- [x] **Step 6: Commit.**

```bash
cd /home/david/workspace/rete && git add nodus/nsl/lib/kernel.nsl nodus/tests/22_spacesync/ && git commit -m "feat(#273): the bridge declares telCad+misses on the wire and appCad to the observer

Liveness { misses, telCad } replaces { cad, bud }; bud is telCad x misses by
construction, so the grid snap and the bud < cad guard are deleted. appCad
defaults to telCad x 8 unless the app declares, and reaches the observer
through prim 158 at startTicking: and at every cadence re-arm.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 4: Every node site — `misses:` replaces `budget:`; the wedge probe declares

**Files:**
- Modify: the twelve `liveness: … budget: …` sites listed by `rg -n "liveness: .*budget:" nodus --glob '*.nsl'` (nine `self cadence * 8` sites, `RearmProbeC6.nsl:46,69`)
- Modify: `nodus/oevm/zephyr/WedgeProbeC6.nsl:62` (declare)

- [x] **Step 1: List them, then edit each.**

```bash
cd /home/david/workspace/rete && rg -n "liveness: .*budget:" nodus --glob '*.nsl' | grep -v "^nodus/tests/"
```
Expected: eleven lines (nine `self cadence * 8` + RearmProbe's two). Edit:
  - `PortaBridgeNodeC6.nsl:100`, `PortaBridgeNodeNrf.nsl:107`, `PortaBridgeNode.nsl:74` → `bridge liveness: self cadence misses: 16.` and replace the comment above each with:
    ```
    (* rete#273 D2: this node's own promise about its radio — telCad and how many
       consecutive misses it tolerates. 16 and not the fleet default of 8 because
       porta refuses to judge a budget below its own 30 s sweep tick (D2.1), and
       2 s x 8 = 16 s is; 2 s x 16 = 32 s is judged. The 2 s is bench turnaround,
       not a claim about the radio. appCad stays at the bridge default (x 8). *)
    ```
  - `RefusedProbeC6.nsl:80`, `ReplayProbeC6.nsl:106`, `ProfileCostNodeC6.nsl:174`, `SilentProbeC6.nsl:93`, `TimerlessNodeC6.nsl:113`, `ShipsBellAlarmNodeC6.nsl:173` → `bridge liveness: self cadence misses: 8.` and shorten each comment to `(* rete#273 D2: telCad and the fleet-default miss allowance. *)`.
  - `RearmProbeC6.nsl:46` → `bridge liveness: 2000 misses: 8.`; `:69` → `bridge liveness: 5000 misses: 8.`
  - `WedgeProbeC6.nsl`: after `timers every: 1000 post: #t on: tickCh.` (line 62) add:
    ```
    (* rete#273 D1: no bridge here, so the probe declares its own app cadence —
       1000 x 8, the same 8 s deadline wedge-detect has always reported. Without
       this the §3.2 rule is not judged at all and the wedge below is invisible. *)
    plat appLiveness: 8000.
    ```
    and in the header comment replace `That is COND_PULSE_UNDRAINED` with `That is OBS_COND_APP_STALE (rete#273: the feed stops moving past the declared 8 s)`.

- [x] **Step 2: Nothing left behind.**

```bash
cd /home/david/workspace/rete && rg -n "budget:|cadence \* 8|budMs|PULSE_UNDRAINED|pulse-undrained" nodus --glob '!*.md' --glob '!nodus/oevm/tests/target/test_observe.py' | grep -v "kernel.nsl.*public budMs\|tests/22_spacesync"
```
Expected: no output. (`test_observe.py:192,205` keeps `pulse-undrained` — it is a synthetic log of a 2026-08-17 event and the assertion is that the runner NAMES the condition it finds.)

- [x] **Step 3: Host node compiles.** The host `PortaBridgeNode.nsl` is what `porta_sync_host.sh` and the tutorial build; a quick bake proves the sites parse:

```bash
cd /home/david/workspace/rete/nodus/oevm && bash tests/porta_sync_host.sh 2>&1 | tail -5
```
Expected: FAIL at the `obs/liveness` grep (the script still greps `#cad: 2000`) — but AFTER a successful bake and boot. A failure before the node boots is a parse error in a site: read it.

- [x] **Step 4: Commit.**

```bash
cd /home/david/workspace/rete && git add nodus/oevm/zephyr/*.nsl && git commit -m "refactor(#273): every node declares liveness: telCad misses:; the wedge probe declares its app cadence

The ten hand-written cadence * 8 sites are gone. Bench bridge nodes declare
misses: 16 so a 2 s telCad clears porta's 30 s sweep tick (D2.1).

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 5: The manifest — `{misses, telCad}` and #285's four verdicts

**Files:**
- Modify: `protocol/MANIFEST.cddl:56-101` (field syms), `:188-193` (liveness rule), `:352-353` (gw-verdict)

- [x] **Step 1: Edit.** In the field-key block (sorted alphabetically): delete `sym-bud` (line 69) and `sym-cad` (line 71) IF nothing else uses them — check first:

```bash
cd /home/david/workspace/rete && grep -n "sym-bud\|sym-cad\b" protocol/MANIFEST.cddl
```
If only the definitions and the liveness rule appear, delete the two definitions. Add, in alphabetical position:
```
sym-misses      = #6.39("misses")
sym-telCad      = #6.39("telCad")
```
Replace lines 190–193 with:
```
liveness = #6.27([sym-Liveness, {
  sym-misses => uint,
  sym-telCad => uint,
}])
```
Replace line 353 with (#285):
```
gw-verdict = #6.39("live") / #6.39("silent") / #6.39("neverSeen") / #6.39("unknown")
```

- [x] **Step 2: Lint and gate.**

```bash
cd /home/david/workspace/rete && python3 protocol/tools/manifest_lint.py protocol/MANIFEST.cddl && cd nodus/nsl && python3 -m pytest -q tools/test_manifest_gate.py ../../protocol/tools 2>&1 | tail -3
```
Expected: lint clean; pytest green. `test_pin_manifest.py` computes the hash from the file, so it follows the edit.

- [x] **Step 3: Record the new hash** (it is the `obs/build.man` pin every node built from here on carries, and the served-hash side on the gateway):

```bash
cd /home/david/workspace/rete && sha256sum protocol/MANIFEST.cddl
```
Write the first 16 hex chars into the commit message.

- [x] **Step 4: Commit.**

```bash
cd /home/david/workspace/rete && git add protocol/MANIFEST.cddl && git commit -m "protocol(#273 #285): Liveness { misses, telCad }; gw-verdict names all four verdicts

MANIFEST.cddl sha256 now <first16>… — every node built from here pins it, and
the gateway must be restarted with this file (Task 10) before those nodes are
accepted.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 6: porta — derive the budget, refuse below the tick, count the misses

**Files:**
- Modify: `porta/internal/space/store.go:655-690` (`DeclaredBudget` → `DeclaredLiveness`)
- Modify: `porta/internal/space/sweep.go:48-55, 162-205`
- Modify: `porta/internal/portacli/serve.go:61-63`
- Modify tests: `porta/internal/space/store_test.go:760-828`, `porta/internal/space/sweep_test.go` (helper `declare` + every caller), `porta/internal/space/runs_test.go` (callers of `declare`), `porta/internal/portacli/serve_test.go:318-322`, `porta/internal/cval/cval_test.go:116-134`
- Modify: `nodus/oevm/tests/porta_sync_host.sh:153-160, 226-244`; `docs/tutorials/03-porta.md:196`

**Interfaces:**
- Produces: `type Declaration struct { TelCadMs, Misses int64 }`, `func (d Declaration) BudgetMs() int64`, `func (s *Store) DeclaredLiveness(nodeID string) (Declaration, bool, error)`; `LivenessChange{NodeID, Verdict, SinceMs, BudgetMs, TelCadMs, Misses}`.

- [x] **Step 1: Tests first — the store reader.** In `store_test.go` rename `TestDeclaredBudgetReadsTheNodesOwnRecord` → `TestDeclaredLivenessReadsTheNodesOwnRecord`, encode `{misses: 8, telCad: 2000}` (keys in that order), and assert:
```go
	d, ok, err := st.DeclaredLiveness("n1")
	if err != nil || !ok || d.TelCadMs != 2_000 || d.Misses != 8 || d.BudgetMs() != 16_000 {
		t.Fatalf("DeclaredLiveness = (%+v, %v, %v), want ({2000 8}, true, nil)", d, ok, err)
	}
```
In `TestDeclaredBudgetSurvivesAGarbledDeclaration` (rename `…Liveness…`) the cases become:
```go
		{"not a record", cval.Symbol("live")},
		{"wrong tag", cval.Record{Tag: "Health", Fields: cval.Map{{K: cval.Symbol("misses"), V: int64(1)}, {K: cval.Symbol("telCad"), V: int64(1)}}}},
		{"no misses", cval.Record{Tag: "Liveness", Fields: cval.Map{{K: cval.Symbol("telCad"), V: int64(2_000)}}}},
		{"no telCad", cval.Record{Tag: "Liveness", Fields: cval.Map{{K: cval.Symbol("misses"), V: int64(8)}}}},
		{"telCad not an int", cval.Record{Tag: "Liveness", Fields: cval.Map{{K: cval.Symbol("misses"), V: int64(8)}, {K: cval.Symbol("telCad"), V: "soon"}}}},
		{"misses not positive", cval.Record{Tag: "Liveness", Fields: cval.Map{{K: cval.Symbol("misses"), V: int64(0)}, {K: cval.Symbol("telCad"), V: int64(2_000)}}}},
```
and the call `st.DeclaredBudget("n1")` → `st.DeclaredLiveness("n1")`.

- [x] **Step 2: Tests first — the sweep.** In `sweep_test.go` the helper becomes:
```go
// declare writes what a node would have pushed at obs/liveness: its telCad and
// the number of consecutive misses it tolerates (rete#273 D2). porta derives
// the budget; nothing here says 16000.
func declare(t *testing.T, st *Store, node string, telCadMs, misses int64) {
	t.Helper()
	rec, err := cval.Encode(cval.Record{Tag: "Liveness", Fields: cval.Map{
		{K: cval.Symbol("misses"), V: misses},
		{K: cval.Symbol("telCad"), V: telCadMs},
	}})
	// ... Ingest unchanged
}
```
Then convert every caller: `rg -n "declare\(t, st" porta/internal/space/` — each `(cad, bud)` becomes `(cad, bud/cad)`, and where `bud < 30_000` raise misses so the product clears the tick (2_000 × 16 = 32_000) and move every boundary constant with it (in `TestSweepJudgesAgainstTheDeclaredBudget`: `16_000` → `32_000`, three places, and the `BudgetMs != 16_000` check → `32_000`). Add two tests:

```go
// D2.1: porta refuses to promise what its sweep cannot deliver. A budget below
// SweepTick is #unknown with a reason naming all three numbers, not a quiet 30 s.
func TestSweepRefusesABudgetBelowItsOwnTick(t *testing.T) {
	var clock int64 = 10_000
	st := storeAtClock(t, &clock)
	listening(t, st)
	checkIn(t, st, &clock, "n1", 0)
	declare(t, st, "n1", 2_000, 8) // 16 s < 30 s
	clock += 1_000
	changes, err := st.Sweep()
	if err != nil {
		t.Fatalf("Sweep: %v", err)
	}
	if len(changes) != 1 || changes[0].Verdict != "unknown" {
		t.Fatalf("changes = %+v, want one #unknown", changes)
	}
	if changes[0].TelCadMs != 2_000 || changes[0].Misses != 8 || changes[0].BudgetMs != 16_000 {
		t.Fatalf("change = %+v, want telCad 2000 misses 8 budget 16000", changes[0])
	}
	reason := lastReason(t, st, "n1")
	for _, want := range []string{"2000", "8", "16000", "30000", "sweep tick"} {
		if !strings.Contains(reason, want) {
			t.Fatalf("reason %q does not name %q", reason, want)
		}
	}
}

// D2.2: the miss count rides the REASON, not a verdict.
func TestSweepReasonCountsTheMisses(t *testing.T) {
	var clock int64 = 10_000
	st := storeAtClock(t, &clock)
	listening(t, st)
	checkIn(t, st, &clock, "n1", 0)
	declare(t, st, "n1", 10_000, 4) // 40 s budget
	clock += 45_000
	if _, err := st.Sweep(); err != nil {
		t.Fatalf("Sweep: %v", err)
	}
	if got := livenessOf(t, st, "n1"); got != "silent" {
		t.Fatalf("verdict = %q, want silent", got)
	}
	if reason := lastReason(t, st, "n1"); !strings.Contains(reason, "4.5 missed check-ins of 10000 ms, 4 tolerated") {
		t.Fatalf("reason = %q", reason)
	}
}
```
with the helper (add beside `livenessRows`):
```go
func lastReason(t *testing.T, st *Store, node string) string {
	t.Helper()
	var r string
	if err := st.DB().QueryRow(
		`SELECT reason FROM events WHERE node_id = ? AND path = ? ORDER BY id DESC LIMIT 1`,
		node, PathLiveness).Scan(&r); err != nil {
		t.Fatalf("last reason: %v", err)
	}
	return r
}
```
(If the events table's id column is not `id`, read `schema.go` and use its rowid name.)

`serve_test.go:318-322` and `runs_test.go`: same key change, and any `16_000` budget that must be judged becomes misses 16.

`cval_test.go:116-134`: the golden becomes `{misses: 604800, telCad: 2000}` (the host gate's clamp case, Step 6); set `want` to `""` for now — Step 4 prints the real bytes.

- [x] **Step 3: Run; expect compile failures naming `DeclaredLiveness`, `TelCadMs`, `Misses`.**

```bash
cd /home/david/workspace/rete/porta && go test ./internal/space/ ./internal/portacli/ ./internal/cval/ 2>&1 | head -20
```

- [x] **Step 4: Implement.** In `store.go` replace `DeclaredBudget` (and its comment) with:

```go
// Declaration is what a node published at obs/liveness (rete#231 D2, shape per
// rete#273 D2): how often it intends to speak and how many consecutive misses
// it tolerates. The budget is DERIVED, never declared — a node cannot publish a
// budget shorter than its own cadence because there is no field to put it in.
type Declaration struct {
	TelCadMs int64
	Misses   int64
}

// BudgetMs is the longest silence the node called healthy.
func (d Declaration) BudgetMs() int64 { return d.TelCadMs * d.Misses }

// DeclaredLiveness reads the node's own record. `ok` is false when there is no
// record or porta cannot read it: a wrong tag, a missing or non-positive field,
// or bytes that are not a closed cval encoding. None of those is returned as an
// error, because the caller is a sweep over every node and one garbled record
// must not cost the fleet its verdicts. No verdict is the honest answer; a
// default would be porta inventing the number this design exists to stop
// inventing.
func (s *Store) DeclaredLiveness(nodeID string) (Declaration, bool, error) {
	var raw []byte
	err := s.db.QueryRow(
		`SELECT value_cbor FROM cells WHERE node_id = ? AND path = ?`,
		nodeID, PathDeclared).Scan(&raw)
	if err == sql.ErrNoRows {
		return Declaration{}, false, nil
	}
	if err != nil {
		return Declaration{}, false, err
	}
	v, err := cval.Decode(raw)
	if err != nil {
		return Declaration{}, false, nil
	}
	rec, ok := v.(cval.Record)
	if !ok || rec.Tag != "Liveness" {
		return Declaration{}, false, nil
	}
	var d Declaration
	for _, p := range rec.Fields {
		k, ok := p.K.(cval.Symbol)
		if !ok {
			continue
		}
		n, isInt := p.V.(int64)
		switch k {
		case "telCad":
			if !isInt || n <= 0 {
				return Declaration{}, false, nil
			}
			d.TelCadMs = n
		case "misses":
			if !isInt || n <= 0 {
				return Declaration{}, false, nil
			}
			d.Misses = n
		}
	}
	if d.TelCadMs == 0 || d.Misses == 0 {
		return Declaration{}, false, nil
	}
	return d, true, nil
}
```

In `sweep.go`: `LivenessChange` gains
```go
	TelCadMs int64 // the cadence the node declared; 0 when it declared none
	Misses   int64 // the consecutive misses it tolerates; 0 when it declared none
```
and `verdictFor` becomes (signature: `(want []byte, verdict string, since int64, d Declaration, reason string, err error)`; update `Sweep` to build the change from `d`):

```go
func (s *Store) verdictFor(nodeID string, lastSeen sql.NullInt64, now int64) (want []byte, verdict string, since int64, d Declaration, reason string, err error) {
	if !lastSeen.Valid {
		return livenessNeverSeen, "neverSeen", 0, Declaration{},
			"neverSeen: no exchange has ever arrived from this node", nil
	}
	since = now - lastSeen.Int64
	d, ok, err := s.DeclaredLiveness(nodeID)
	if err != nil {
		return nil, "", 0, Declaration{}, "", fmt.Errorf("declared liveness: %w", err)
	}
	if !ok {
		return livenessUnknown, "unknown", since, Declaration{},
			fmt.Sprintf("unknown: %d ms since the last exchange, and this node has declared no cadence", since), nil
	}
	bud := d.BudgetMs()
	// rete#273 D2.1: porta refuses to promise what its sweep cannot deliver.
	// A budget below one tick would be detected at the tick, not at the
	// budget; saying so is worth more than quietly enforcing 30 s.
	if tick := SweepTick.Milliseconds(); bud < tick {
		return livenessUnknown, "unknown", since, d,
			fmt.Sprintf("unknown: telCad %d ms x misses %d = %d ms is below the sweep tick %d ms; porta cannot judge at that resolution — raise misses or telCad", d.TelCadMs, d.Misses, bud, tick), nil
	}
	missed := float64(since) / float64(d.TelCadMs)
	if since <= bud {
		return livenessLive, "live", since, d,
			fmt.Sprintf("live: %d ms since the last exchange, %.1f missed check-ins of %d ms, %d tolerated", since, missed, d.TelCadMs, d.Misses), nil
	}
	deaf, err := s.WasDeaf(now-since, now)
	if err != nil {
		return nil, "", 0, Declaration{}, "", fmt.Errorf("deaf intervals: %w", err)
	}
	if deaf {
		return livenessUnknown, "unknown", since, d,
			fmt.Sprintf("unknown: %d ms since the last exchange, budget %d ms, and porta was not listening for part of it", since, bud), nil
	}
	return livenessSilent, "silent", since, d,
		fmt.Sprintf("silent: %d ms since the last exchange — %.1f missed check-ins of %d ms, %d tolerated", since, missed, d.TelCadMs, d.Misses), nil
}
```
In `Sweep`: `want, verdict, since, d, reason, err := s.verdictFor(...)` and
```go
		changes = append(changes, LivenessChange{
			NodeID: n.id, Verdict: verdict, SinceMs: since,
			BudgetMs: d.BudgetMs(), TelCadMs: d.TelCadMs, Misses: d.Misses,
		})
```
Update the `Sweep` doc comment's "THE RULE IS ONE LINE" paragraph: `bud` is now `telCad × misses`, both from the node; porta still contributes no number, and D2.1 is the one case it declines.

`serve.go:61-63` log line:
```go
				logf("porta: %s is #%s — %d ms since its last exchange, budget %d ms (telCad %d x misses %d)",
					c.NodeID, c.Verdict, c.SinceMs, c.BudgetMs, c.TelCadMs, c.Misses)
```
Any other reader of `DeclaredBudget` or `BudgetMs` in `porta/`: `rg -n "DeclaredBudget|\.BudgetMs\b" porta` and fix.

- [x] **Step 5: Run; pin the golden.**

```bash
cd /home/david/workspace/rete/porta && gofmt -l . ; go test ./... 2>&1 | tail -15
```
Expected: everything green except `TestGoalLivenessGoldenForTheHostGate`, whose failure prints `goal/liveness golden = <hex>`. Paste that hex into `want` AND into `GOAL_LIVENESS_HEX` in `porta_sync_host.sh`. Re-run: all green.

- [x] **Step 6: The host gate and the tutorial page.** In `nodus/oevm/tests/porta_sync_host.sh`:
  - line 155 grep `"#cad: 2000"` → `"#telCad: 2000"`; line 158 grep `"#bud: 16000"` → `"#misses: 16"` and its message → `"obs/liveness does not carry the declared 16-miss allowance"`.
  - lines 226–244: the comment becomes `# constant is Liveness { misses: 604800, telCad: 2000 } — a week of misses, which the node CLAMPS to the 24 h ceiling (43200 x 2 s) and publishes; the goal keeps 604800. The diff IS the answer.`; the two `grep "46000"` → `grep "#misses: 43200"`; `grep "45000"` → `grep "604800"`; the echo → `"  goal/liveness misses 604800 -> obs/liveness misses 43200: the node clamped, and the diff IS the answer"`.
  - `docs/tutorials/03-porta.md:196` → `  obs/liveness   seq=6   boot=1  #Liveness{#misses: 16, #telCad: 2000}` and read the surrounding prose for `bud`/`budget`/`16 s`: rewrite to say the node declares its cadence and a miss allowance and porta derives the 32 s budget.

```bash
cd /home/david/workspace/rete/nodus/oevm && bash tests/porta_sync_host.sh 2>&1 | tail -4
```
Expected: `PORTA SYNC OK`.

- [x] **Step 7: Commit.**

```bash
cd /home/david/workspace/rete && gofmt -l porta && git add porta nodus/oevm/tests/porta_sync_host.sh docs/tutorials/03-porta.md && git commit -m "feat(#273): porta derives bud = telCad x misses, refuses below its tick (D2.1), counts misses in the reason (D2.2)

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 7: The whole tree green

- [x] **Step 1: The three gates, in this order (never two nsl suites at once; porta_sync_host is standalone).**

```bash
cd /home/david/workspace/rete/nodus/nsl && python3 tools/run_tests_oevm.py 2>&1 | tail -6
cd /home/david/workspace/rete/porta && go test ./... 2>&1 | tail -3
cd /home/david/workspace/rete/nodus/oevm && bash tests/porta_sync_host.sh 2>&1 | tail -2
```
Expected: `Passed: 280 / Failed: 0` with EVERY aux entry green including `tutorial-porta` and `porta-cli`; `ok` for every Go package; `PORTA SYNC OK`. A SKIP on the aux line is a finding — read it.

- [x] **Step 2: CLAUDE.md gate count.** Update the `279 passed` line under `## Gates` to the new count and date.

- [x] **Step 3: Commit.**

```bash
cd /home/david/workspace/rete && git add CLAUDE.md && git commit -m "docs: gate count after #273's tests

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 8: The board — reproduce first, then the two new gates and the three old ones

**Files:**
- Create: `nodus/oevm/zephyr/FastPollProbeC6.nsl`, `nodus/oevm/tests/target/liveness-fastpoll.env`
- Create: `nodus/oevm/zephyr/RunOnceProbeC6.nsl`, `nodus/oevm/tests/target/liveness-runonce.env`

**Ordering note.** The fast-poll probe must be RUN ON TODAY'S VM FIRST so the false trip is evidence, not memory. Today's VM does not know `appLiveness:`, so Step 1 writes the probe WITHOUT its declaration line, runs it against the pre-Task-1 commit, and records the red; Step 4 adds the line and runs it on the new build.

- [x] **Step 1: The fast-poll probe (no declaration yet).** Create `nodus/oevm/zephyr/FastPollProbeC6.nsl`:

```
(*
expect:
TAP fastpoll done
*)
(* rete#273 D1 — THE FAST-POLL FALSE TRIP, and the gate that must go red before
   the fix and green after it.

   A 100 ms timer beside a reaction that blocks ~1.5 s once a second. Every tick
   of the fast timer is serviced; the slow reaction is ordinary work (an e-paper
   refresh, a sensor conversion). Under the pre-#273 §3.2 the observer judged the
   SHORTEST declared period x 8 = 800 ms of "undrained" time and rebooted this
   healthy node inside its first blocking reaction. Under the declared rule the
   app says its dispatch may go 4 s without running, and nothing fires.

   BOOT 1 IS THE TEST. If it survives 20 slow ticks (~30 s, i.e. past a dozen of
   the old rule's 800 ms deadlines) with NO panic note pending, it reports
   pass=2. If the observer fired, boot 2 finds the note and reports not ok —
   which is exactly what today's build must do. *)
class FastPollProbeC6 packageUsing: manifest = (
  | plat space timers fastCh slowCh fast ::= 0. slow ::= 0.
    tapPass ::= 0. tapFail ::= 0. |
) (

  slowTicksToPass = ( ^ 20 )
  blockMs         = ( ^ 1500 )

  public main: platform args: args = (
    | note |
    plat:: platform.
    space:: plat space.
    timers:: plat timers.
    space whenever: 'sys/timer' do: [ :e | timers dispatch: e ].

    note:: plat panicNote.
    (note == nil) ifFalse: [ self reportFalseTrip: note ].

    fastCh:: space channel: 'probe/fast'.
    slowCh:: space channel: 'probe/slow'.
    fastCh whenever: '' do: [ :m | fast:: fast + 1 ].
    slowCh whenever: '' do: [ :m | self slowTick ].
    timers every: 100 post: #f on: fastCh.
    timers every: 2000 post: #s on: slowCh.
    (* DECLARATION GOES HERE in Step 4. *)

    [ true ] whileTrue: [ plat pumpEvents. space drain ].
    ^ self )

  (* Blocks the reactor for blockMs — the feed does not move while this runs. *)
  slowTick = (
    | until |
    slow:: slow + 1.
    until:: plat monotonicMillis + self blockMs.
    [ plat monotonicMillis < until ] whileTrue: [ ].
    ('fastpoll tick ' , slow printString , ' fast ' , fast printString) out.
    (slow = self slowTicksToPass) ifTrue: [ self reportSurvived ].
    ^ self )

  reportSurvived = (
    self tapPlan: 2.
    self tap: 1 named: 'no reboot note' ok: true detail: 'boot 1 reached tick 20'.
    self tap: 2 named: 'fast timer serviced' ok: (fast > 100)
         detail: ('fast=' , fast printString).
    self tapDone.
    ^ self )

  reportFalseTrip: note = (
    self tapPlan: 2.
    self tap: 1 named: 'no reboot note' ok: false
         detail: ('THE OBSERVER FIRED ON A HEALTHY NODE: ' , (note basicAt: 4)).
    self tap: 2 named: 'fast timer serviced' ok: false detail: 'boot 2'.
    self tapDone.
    plat panicNoteAck: 0.
    ^ self )

  scenarioName = ( ^ 'fastpoll' )
  tapPlan: n = ( ('TAP ' , self scenarioName , ' plan=' , n printString) out. ^ self )
  tap: idx named: nm ok: passed detail: d = (
    passed
      ifTrue:  [ tapPass:: tapPass + 1.
                 ('TAP ' , self scenarioName , ' ok ' , idx printString , ' ' , nm , ' ' , d) out ]
      ifFalse: [ tapFail:: tapFail + 1.
                 ('TAP ' , self scenarioName , ' not ok ' , idx printString , ' ' , nm , ' ' , d) out ].
    ^ self )
  tapDone = (
    ('TAP ' , self scenarioName , ' done pass=' , tapPass printString , ' fail=' , tapFail printString) out.
    ^ self )
) : (
)
```

Create `nodus/oevm/tests/target/liveness-fastpoll.env`:
```
# nodus/oevm/tests/target/liveness-fastpoll.env — rete#273 D1's reproduce-first gate
#
# A 100 ms timer beside a 1.5 s blocking reaction. RED on the pre-#273 build (the
# observer judges 100 x 8 = 800 ms and reboots the node inside its first slow tick:
# boot 2 reports `not ok 1 no reboot note`, cond=pulse-undrained deadline=800ms in
# the archive). GREEN on the declared rule (appCad 4000: 20 slow ticks, pass=2, no
# reboot through the observation window).
#
# The red was RUN AND ARCHIVED on <date> before the fix landed — see the archive
# entry named in this file's commit. A gate that starts by reproducing the defect
# is the only kind that can tell "fixed" from "never broken".
#
# No radio, no bridge: nothing here needs CONF_EXTRA. class=disruptive because the
# pre-fix build reboots the board.
SCENARIO=liveness-fastpoll
TAP_NAME=fastpoll
NODE=FastPollProbeC6.nsl
BOARD_ID=esp32c6
CLASS=disruptive
TIMEOUT_S=180
OBSERVE_S=120
OBSERVE_PROGRESS='^.*fastpoll tick [0-9]+'
```

- [x] **Step 2: Run it on TODAY'S VM.** Check out the pre-Task-1 VM sources into the working tree for the build only:

```bash
cd /home/david/workspace/rete && git stash -u -q 2>/dev/null; PRE=$(git log --format=%h --grep='fix(#273): §3.2' -1)^; echo "pre-fix commit: $PRE"; git checkout -q $PRE -- nodus/oevm/src nodus/oevm/zephyr/src nodus/nsl/lib/kernel.nsl && git stash pop -q 2>/dev/null; git status --short | head
```
(The probe and env are untracked, so they survive the checkout.) Then:

```bash
cd /home/david/workspace/rete/nodus/oevm/tests/target && ./run.sh liveness-fastpoll 2>&1 | tail -25
```
Expected: **FAIL** — the archive shows `oevm: WEDGE cond=pulse-undrained ... deadline=800ms` within the first two seconds, a reboot, and boot 2's `TAP fastpoll not ok 1 no reboot note`. Record the archive path. If it PASSES on the old build the probe does not reproduce the defect: raise `blockMs` toward 3000 and re-run before touching anything else.

Restore the tree: `cd /home/david/workspace/rete && git checkout -q HEAD -- nodus/oevm/src nodus/oevm/zephyr/src nodus/nsl/lib/kernel.nsl && git status --short` (expected: only the two new files).

- [x] **Step 3: The run-once probe.** Create `nodus/oevm/zephyr/RunOnceProbeC6.nsl`:

```
(*
expect:
TAP runonce done
*)
(* rete#273 D1 — A NODE WITH NO REPEATING TIMER IS JUDGED, at its own number.

   Before #273, `timer_target_snapshot` reporting n == 0 meant OBS_COND_NONE
   forever: a run-once node (MagTagDeepSleepNode's shape) that wedged inside its
   wake window burned its battery with nothing watching. Now the app declares
   appCad and the feed is judged whether or not a timer exists.

   BOOT 1 declares 4000 ms, prints one line, and then never returns to the pump
   — the wedge-inside-the-wake-window. The observer must fire at ~4 s with
   cond=app-stale deadline=4000ms (read it in the archive), write the note and
   reboot. BOOT 2 finds the note, reports, and idles WITHOUT declaring — an idle
   node that declared 4 s and then dispatched nothing would be rebooted again,
   and correctly so. The vacuous outcome on today's build is TIMEOUT: no deadline
   is ever formed, so nothing reboots and no TAP arrives. *)
class RunOnceProbeC6 packageUsing: manifest = (
  | plat space tapPass ::= 0. tapFail ::= 0. |
) (

  public main: platform args: args = (
    | note |
    plat:: platform.
    space:: plat space.
    note:: plat panicNote.
    (note == nil)
      ifTrue:  [ self work ]
      ifFalse: [ self reportNote: note ].
    [ true ] whileTrue: [ plat pumpEvents. space drain ].
    ^ self )

  work = (
    plat appLiveness: 4000.
    'runonce: declared 4000 ms, now working forever — nothing below this line is expected' out.
    [ true ] whileTrue: [ ].
    ^ self )

  reportNote: note = (
    | reason |
    self tapPlan: 2.
    self tap: 1 named: 'a deadline was formed with no timer' ok: true
         detail: ('detail=' , (note basicAt: 4)).
    reason:: (plat panic fields) at: #reason ifAbsent: [ #none ].
    self tap: 2 named: 'reason is watchdog' ok: (reason = #watchdog)
         detail: ('reason=' , reason printString).
    self tapDone.
    plat panicNoteAck: 0.
    ^ self )

  scenarioName = ( ^ 'runonce' )
  tapPlan: n = ( ('TAP ' , self scenarioName , ' plan=' , n printString) out. ^ self )
  tap: idx named: nm ok: passed detail: d = (
    passed
      ifTrue:  [ tapPass:: tapPass + 1.
                 ('TAP ' , self scenarioName , ' ok ' , idx printString , ' ' , nm , ' ' , d) out ]
      ifFalse: [ tapFail:: tapFail + 1.
                 ('TAP ' , self scenarioName , ' not ok ' , idx printString , ' ' , nm , ' ' , d) out ].
    ^ self )
  tapDone = (
    ('TAP ' , self scenarioName , ' done pass=' , tapPass printString , ' fail=' , tapFail printString) out.
    ^ self )
) : (
)
```

Create `nodus/oevm/tests/target/liveness-runonce.env`:
```
# nodus/oevm/tests/target/liveness-runonce.env — rete#273 D1: no timer, still judged
#
# The node registers NO repeating timer, declares appCad 4000 ms, and stops
# dispatching. PASS: the observer fires at ~4 s (cond=app-stale deadline=4000ms in
# the archive — READ IT, the TAP cannot see the console), reboots, and boot 2
# reports pass=2. The vacuous outcome on any build without the declared rule is
# TIMEOUT with no TAP, which is the red this row exists to make visible: it asserts
# a deadline was FORMED, not merely that nothing rebooted.
#
# Boot 2 idles without a progress line by design (it declares nothing and dispatches
# nothing), hence OBSERVE_PROGRESS=none.
#
# No radio, no bridge. class=disruptive: it reboots the board on purpose.
SCENARIO=liveness-runonce
TAP_NAME=runonce
NODE=RunOnceProbeC6.nsl
BOARD_ID=esp32c6
CLASS=disruptive
TIMEOUT_S=120
OBSERVE_S=60
OBSERVE_PROGRESS=none
```

- [x] **Step 4: Declare in the fast-poll probe.** Replace `(* DECLARATION GOES HERE in Step 4. *)` with:
```
    (* rete#273 D1: the app's longest legitimate blocking reaction is blockMs; 4 s
       is comfortably past it and comfortably under the 16 s a bridge would default
       to. This ONE line is the difference between the red and the green. *)
    plat appLiveness: 4000.
```

- [x] **Step 5: Run the five scenarios on the new build** (~25 min of board time; `run.sh` takes names):

```bash
cd /home/david/workspace/rete/nodus/oevm/tests/target && for s in liveness-fastpoll liveness-runonce liveness-heartbeat liveness-noradio wedge-detect; do echo "=== $s"; ./run.sh $s 2>&1 | tail -8; done
```
Expected, per scenario:
  - `liveness-fastpoll`: PASS, `pass=2 fail=0`, ≥20 `fastpoll tick` lines, no `WEDGE` line in the archive.
  - `liveness-runonce`: PASS, `pass=2 fail=0`; archive shows `WEDGE cond=app-stale ... deadline=4000ms appCad=4000ms`.
  - `liveness-heartbeat`, `liveness-noradio`: PASS **unchanged** — `deadline=16000ms` (now `appCad=16000ms` beside it, the bridge default), 18 s of silence, `pass=2 fail=0`. The cond reads `app-stale`.
  - `wedge-detect`: PASS, `deadline=8000ms appCad=8000ms`, `reason=#watchdog`.
Any red: STOP, read the archive, do not edit the gate to fit.

- [x] **Step 6: Record the evidence in the env headers** (the fast-poll red's archive path and date, the run-once dump line) and in `CLAUDE.md`'s scenario table — two new rows after `liveness-noradio`:

```
| `liveness-fastpoll` | disruptive | **#273 D1's reproduce-first gate: a 100 ms timer beside a 1.5 s blocking reaction.** RED on the pre-#273 build (deadline=800ms, false reboot inside the first slow tick — archived <date>), GREEN with the declared appCad 4000 |
| `liveness-runonce` | disruptive | **a node with NO repeating timer is judged at its declared number** (#273 D1). deadline=4000ms from the declaration alone; the vacuous outcome on the old rule is TIMEOUT |
```
and update the `liveness-heartbeat` / `liveness-noradio` / `wedge-detect` rows' `deadline=` phrases to say the number now comes from the declared appCad (bridge default cad × 8; the probe declares 8000).

- [x] **Step 7: Commit.**

```bash
cd /home/david/workspace/rete && git add nodus/oevm/zephyr/FastPollProbeC6.nsl nodus/oevm/zephyr/RunOnceProbeC6.nsl nodus/oevm/tests/target/liveness-fastpoll.env nodus/oevm/tests/target/liveness-runonce.env CLAUDE.md && git commit -m "test(#273): liveness-fastpoll (red before, green after) and liveness-runonce on a board

fastpoll false-rebooted on the pre-#273 build (deadline=800ms, archive <path>)
and holds 20 slow ticks on the declared rule; runonce forms deadline=4000ms
with no timer in the image. heartbeat, noradio and wedge-detect unchanged.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 9: Promotion — the mainline documents absorb the decision

**Files:**
- Modify: `protocol/PROTOCOL-NSL-RECORDS.md:405` (the `obs/liveness` row) + a new `gw/liveness` row in the same table
- Modify: `protocol/PROTOCOL-NSL.md:477` (the declare row) + a §8 log entry
- Modify: `porta/docs/ARCHITECTURE.md:227-260` (§3.5)
- Modify: `review/tests/CLAIMS.md` (two new gate rows — this is the one `review/` edit the promotion rule licenses: adding claims, not moving anything)
- Modify: `docs/design/2026-08-21-liveness-contract-design.md` (§604 banner)
- Modify: `docs/design/2026-08-31-liveness-timers.html` (§7 derivation list, §8 fork → decided)
- Modify: `docs/design/2026-09-14-liveness-two-cadences-design.md` (close-out: "BUILT <date>", D1 notes the feed IS `pulse_drained()`)
- Modify: `CLAUDE.md` focus block

- [x] **Step 1: RECORDS.** Replace row 405 with:

```
| `obs/liveness` | R | `Liveness { misses: int, telCad: int }` — **the node's declaration of how often it will speak and how many consecutive misses it tolerates** (rete#231 D2, shape per rete#273 D2). porta derives the budget `telCad × misses` and judges `now - last_seen > budget`, one line with no constants in it. Two numbers and not one because the bench proved one is not enough — `c6-bell` runs a 30 s cadence and misses roughly half its check-ins to ordinary mesh loss, so `misses` is the node's allowance for its own measured loss rate, and the node is the only party that knows it; **8 at every period** (the false-trip rate is ~p^misses and the period cancels — D2.3). A budget shorter than the cadence is **unrepresentable**, which is what deleted the grid snap. Clamped to 24 h on `misses`, published, never rejected. porta writes `#unknown` naming all three numbers when `telCad × misses` is below its 30 s sweep tick (D2.1) — bench nodes at 2 s declare `misses: 16` for that reason. Written **once per boot**. **<N> B encoded, MEASURED <date>** at the worst legal values (`nodus/tests/22_spacesync/20_liveness_size.nsl`). The node's OTHER liveness number, `appCad`, never rides the wire (rete#273 D1) |
```
and add, in the gateway-authored rows (find the table that holds `gw/**` paths; if none, add this row after `obs/liveness` with a note):
```
| `gw/liveness` | R | one of `#live` / `#silent` / `#neverSeen` / `#unknown` — porta's verdict about the node, gateway-authored, never on the wire down. `#unknown` carries a `reason` in the event log: no declaration, a gateway outage inside the window, or (rete#273 D2.1) a declared budget below the sweep tick. `#silent`'s reason counts the misses (`"silent: … — 4.5 missed check-ins of 10000 ms, 4 tolerated"`, D2.2): a ladder in the reason, not a verdict |
```
Also fix the paragraph at ~419 (`obs/liveness replaced an inference`) if it names `bud`.

- [x] **Step 2: PROTOCOL-NSL.** Row 477 → `` | `liveness` (declare) | `obs/liveness` | cell | `{misses:, telCad:}` — the node's own cadence and miss allowance; porta judges `now - last_seen > telCad × misses` and infers nothing (rete#231, rete#273 D2) | ``. Append to the §8 log:
```
- **2026-09-17 — rete#273 BUILT.** `Liveness { cad, bud }` → `{ misses, telCad }`; the budget is derived. `gw/liveness` may read `#neverSeen` and `#unknown` (rete#285 — the manifest now says so). The node's app-liveness deadline (`appCad`) is declared to the observer and never crosses the wire. Manifest sha256 <first16>….
```

- [x] **Step 3: ARCHITECTURE §3.5.** Add after the four-verdict table:
```
The budget is `telCad × misses`, both declared by the node at `obs/liveness` (rete#273 D2);
porta contributes no number. Two consequences an operator will meet: **a budget below the
30 s sweep tick reads `#unknown`** with a reason naming all three numbers (D2.1 — porta will
not promise a resolution it cannot deliver; raise `misses` or `telCad`), and **`#silent`'s
reason counts the misses** (`4.5 missed check-ins of 10000 ms, 4 tolerated`, D2.2) — a
ladder in the reason, deliberately not a fifth verdict until an operator is shown to act
differently on the first miss than on the fifth.
```

- [x] **Step 4: CLAIMS.md** — two rows in the target-gate table, in the style of the `liveness-noradio` row already there: `liveness-fastpoll` (claim: a 100 ms timer beside a 1.5 s reaction is not a wedge; evidence: red on the pre-#273 build, green after; archive path) and `liveness-runonce` (claim: a node with no repeating timer is judged at its declared number; evidence: `deadline=4000ms` with n == 0).

- [x] **Step 5: The two older records.** In `2026-08-21-liveness-contract-design.md` find the §604 addendum (grep `wedge deadline`) and put above it: `> **SUPERSEDED 2026-09-14 by \`2026-09-14-liveness-two-cadences-design.md\` D1 — the wedge deadline is never published; built 2026-09-17.**`. In `2026-08-31-liveness-timers.html` §8, replace the "fork" framing with one sentence pointing at the decision record and "BUILT 2026-09-17", and in §7's derivation list mark `observer_core.h:131 (shortest × 8)` as DELETED and add `appCad (declared; bridge default cad × 8)`. Render check: `python3 -m http.server 8088 --directory docs/design` is already how the page is browsed; open it and confirm §7/§8 read.

- [x] **Step 6: The decision record's close-out.** Under the title block of `2026-09-14-liveness-two-cadences-design.md` change `**Decided, NOT built.**` to `**D1 and D2 BUILT 2026-09-17 (commits <a>..<b>); D3 (#stalled) is still decided-not-built and has its own plan.**`. In D1 add one sentence after "One feed, one caller, no app-side call.": *"In the build the feed IS `pulse_drained()` — no new counter was added; the prim carries only the declaration."*

- [x] **Step 7: CLAUDE.md focus block.** Rewrite the `Decided 2026-09-14 … NOT built` bullet's opening to `**BUILT 2026-09-17 (D1+D2), do not re-litigate:**`, delete the "NOT built" phrasing, and add the bench truth from this session: `c6bench` on the gateway runs `TimerlessNodeC6` (18.7 days, boot 2026-08-30 01:42Z) and is silent to porta by construction, not by fault; `c6spare` has both cables again.

- [x] **Step 8: Docmap and commit.**

```bash
cd /home/david/workspace/rete && tools/docmap.sh >/dev/null 2>&1; git add -A protocol/*.md porta/docs/ARCHITECTURE.md review/tests/CLAIMS.md docs/design CLAUDE.md review/DOCMAP.md && git commit -m "docs(#273): promote D1/D2 to RECORDS, PROTOCOL-NSL, ARCHITECTURE, CLAIMS; the older records banner-superseded

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 10: The landing — gateway deploy and the first real node (WITH DAVID)

This is the one task that changes the running system. It is the staged rollout's steps 2 and 3. Every command below is proposed, not run, until David is at the bench.

- [ ] **Step 1: Build porta, ship it, restart BY PID** (read `~/.claude/projects/-home-david-workspace-rete/memory/porta-gateway-operational-state.md` FIRST — the pkill self-kill has struck five times). The restart carries `--manifest` pointing at the new `MANIFEST.cddl`, which persists the new hash into the store.
- [ ] **Step 2: Confirm the served hash**: `curl -s http://gw85224-01:6971/manifest | sha256sum` equals the Task 5 hash; `porta get --db … c6bench gw/liveness` still reads `#silent` (the node has not been reflashed yet).
- [ ] **Step 3: Reflash `c6bench` on the gateway** with `PortaBridgeNodeC6` + the Thread overlay. The board has only its CP2102N on the gateway, which is flash AND console; the harness is single-host, so this is either `esptool` on the gateway with a locally built image scp'd over, or the board comes to the workstation for the flash. David's call.
- [ ] **Step 4: Verify the wire.** Within a minute: `obs/liveness` reads `#Liveness{#misses: 16, #telCad: 2000}`, `obs/build.man` carries the new pin, `gw/liveness` goes `#live`. Then the physical falsification: unplug the board, expect `#silent` within ~32–62 s with a reason counting ≥16 misses; replug, expect `#live`.
- [ ] **Step 5: Record** the outcome in the design record's close-out and the status page (`/conclude` does the page).

---

## Self-review

- **Spec coverage.** D1 (Task 1–3, 8), D1.1 clamp + default-printed (Task 3 `appLiveness:` clamp; the dump prints `appCad=` beside `deadline=` — the "default vs declared" flag is the bridge's `appCadDeclared = 0`, visible only in nsl: acceptable, the observer cannot know who chose; NOTED as a deliberate simplification), D2 (Task 3, 5, 6), D2.1 (Task 6 refusal + bench `misses: 16`), D2.2 (Task 6 reason), D2.3 (default 8 in the kernel), §2 deletions (Task 1 deletes the inference, Task 3 deletes the snap and guard, Task 4 deletes the ten sites), §4 gates (Task 1 property 8/9 on the host; Task 8 the five board runs; `liveness-stalled` is D3 — out of scope), §5 sequencing (Task 5 + Task 10), §6 promotion (Task 9). #285 folded into Task 5. **Gap, deliberate:** D3 and the TLA `DoSweep` correction — separate plan.
- **Placeholders.** `<N>`, `<date>`, `<first16>`, `<path>` are values the executor MEASURES in the named step and must fill before committing; none is a TBD.
- **Type consistency.** `app_cad_ms` / `fed_at_ms` / `OBS_COND_APP_STALE` (Task 1) are what Task 2's observer.c and Task 8's env headers name. `DeclaredLiveness` / `Declaration{TelCadMs, Misses}` / `LivenessChange{TelCadMs, Misses}` (Task 6) match `serve.go`'s log line. nsl: `liveness:misses:`, `appLiveness:`, `missBudget`, `budMs` (derived), `appCadMs` (effective) are used identically in Tasks 3, 4 and 8.
