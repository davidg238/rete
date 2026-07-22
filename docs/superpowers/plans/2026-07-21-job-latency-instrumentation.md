# Job Latency Instrumentation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Measure and publish segmented end-to-end latency (p50/p95 decades, exact max, sample count, failure count) for the two job shapes `ThermalHttpNode` runs — timer→thermal→cell and request→format→response.

**Architecture:** A fixed static array of nameless span slots in C (`src/span.c`), each holding `start`/`n`/`min`/`max`/`sum` plus six log-spaced decade buckets. Four new platform prims open, close, feed and read a slot by SMI index. nsl owns every name via a `Spans` class in `kernel.nsl`, builds a `#LatencySpan` record per slot onto a `sys/latency` cell, and reduces buckets to percentile decades. Zero allocation on any measured path.

**Tech Stack:** C99 (`src/`, `tests/`), nsl (`~/workspace/nsl-tuvm/lib/kernel.nsl`, `~/workspace/nsl-tests/`), Zephyr 4.2 (ESP32 WROOM-32 target), Python test harnesses.

**Design of record:** `~/workspace/tuvm/docs/design/2026-07-21-job-latency-instrumentation-design.md` — read it before Task 1. Section references below (§2.2, §4.1 …) point into it.

## Global Constraints

- **Prim ids are append-only.** Shifting an existing id silently misroutes every prim after the insertion point. New ids are 118–121, appended after `PRIM_pf_sensorArmEvent` (117). Ids are the **1-based position in `PRIM_NAMES`, counted not computed** — the standing rule stated throughout `src/prim_ids.h`.
- **The compile-time parity guard at `src/dispatch.c:811` must be updated** in the same commit that adds the names, or the build fails (by design).
- **SMI is 31-bit** (`TAG_SMI` is `val << 1 | 1`, `src/oevm.h:62`) — max 1,073,741,823 on a 32-bit target. No value crossing the prim seam may exceed it. This is why `sum` is split lo/hi at 1e9.
- **Zero allocation on a measured path.** No `malloc`, no `gc_allocate`, no nsl object creation inside a span. GC is collect-when-full; a growing structure pegs heap high-water to the arena ceiling.
- **C never learns a slot's name.** Slots are integers `0..OEVM_SPAN_SLOTS-1`. Names live in nsl. This is the rule `src/timer_backend.h` already states for the timer seam.
- **`OEVM_SPAN_SLOTS` defaults to 8.** DRAM cost is 8 × 48 B + a 4 B open-mask = **388 B**, against #53's 816 B spare on the WROOM-32.
- **Percentiles are reported as bucket decades, never interpolated** (§4.1), and **no p95 is reported below n ≥ 200** (§4.2).
- **nsl suite runs Path B (`.oimg`) only** — the standing rule. Use `~/workspace/nsl-tuvm/tools/run-one.py <file.nsl>` for the inner loop; `run_tests_oevm.py` ignores argv and globs the whole 2–3 minute suite.
- **Device target is `/dev/ttyUSB1`** — the ESP32 WROOM-32 with the AMG8833. `/dev/ttyACM0` and `/dev/ttyUSB0` are two ports of the *same* ESP32-C6 and are not this plan's target.

## File Structure

| file | status | responsibility |
|---|---|---|
| `src/span_backend.h` | create | the one-function clock seam: `uint32_t span_now(void)` |
| `src/backend/posix/span_posix.c` | create | host clock; honours the virtual clock so the suite stays deterministic |
| `src/backend/zephyr/span_zephyr.c` | create | `k_cycle_get_32()` → µs |
| `src/span.h` | create | slot API consumed by `platform.c`, `http_core.c` and tests |
| `src/span.c` | create | the slot array, bucketing, and accumulate logic — no clock policy, no names |
| `tests/test_span.c` | create | C unit test for `src/span.c` |
| `src/prim_ids.h` | modify | mirror ids 118–121 |
| `src/dispatch.c` | modify | enum entries, `PRIM_NAMES` entries, parity guard |
| `src/platform.c` | modify | the four prim cases |
| `src/http_core.c` | modify | C-owned `httpQueue` and `egress` spans |
| `tools/run_c_tests.sh` | modify | register `test_span` |
| `zephyr/CMakeLists.txt` | modify | compile `span_zephyr.c` and `span.c` |
| `~/workspace/nsl-tuvm/tools/run_tests_oevm.py` | modify | add `span_posix.c` to `OEVM_BACKEND` |
| `~/workspace/nsl-tuvm/lib/kernel.nsl` | modify | the `Spans` class + `platform Spans` accessor |
| `~/workspace/nsl-tests/19_latency/*.nsl` | create | nsl coverage |
| `zephyr/ThermalHttpNode.nsl` | modify | app spans + the `/stats` latency table |
| `zephyr/tools/latency_harness.py` | create | client-side RTT harness (500 requests) |

---

### Task 1: The span slot array in C

**Files:**
- Create: `src/span_backend.h`, `src/backend/posix/span_posix.c`, `src/span.h`, `src/span.c`
- Test: `tests/test_span.c`
- Modify: `tools/run_c_tests.sh:79` (add after the `test_http` line)

**Interfaces:**
- Consumes: nothing (this is the base layer).
- Produces: `span_now()`, `span_begin(int)`, `span_end(int)`, `span_record(int, uint32_t)`, `span_stats(int, uint32_t*)`, `span_reset_all(void)`, `SPAN_STATS_FIELDS` (== 11), `OEVM_SPAN_SLOTS` (default 8).

- [ ] **Step 1: Write the failing test**

Create `tests/test_span.c`:

```c
/* tuvm/tests/test_span.c — host TDD for the job-latency span slots (#61).
   Build (backend-only link, the test_http precedent — span.c has no VM deps):
     cd ~/workspace/tuvm && gcc -std=c99 -Isrc tests/test_span.c \
       src/span.c src/backend/posix/span_posix.c -lm -o /tmp/tspan && /tmp/tspan
   Verifies the accumulate logic exhaustively WITHOUT a clock (span_record feeds
   samples directly), so nothing here can be timing-flaky. */
#include "../src/span.h"
#include "../src/span_backend.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static uint32_t st[SPAN_STATS_FIELDS];

int main(void) {
    /* (1) A virgin slot reads all zeroes. */
    span_reset_all();
    span_stats(0, st);
    for (int i = 0; i < SPAN_STATS_FIELDS; i++) assert(st[i] == 0);

    /* (2) Bucket edges are exact. Each pair is (microseconds, expected bucket).
       The edges are the whole point: 99->b0 but 100->b1, 999->b1 but 1000->b2. */
    {
        struct { uint32_t us; int b; } cases[] = {
            {0,0}, {99,0}, {100,1}, {999,1}, {1000,2}, {9999,2},
            {10000,3}, {99999,3}, {100000,4}, {999999,4},
            {1000000,5}, {4000000000U,5},
        };
        for (unsigned c = 0; c < sizeof(cases)/sizeof(cases[0]); c++) {
            span_reset_all();
            span_record(1, cases[c].us);
            span_stats(1, st);
            assert(st[0] == 1 && "one sample recorded");
            for (int b = 0; b < 6; b++) {
                uint32_t want = (b == cases[c].b) ? 1u : 0u;
                assert(st[5 + b] == want && "sample landed in exactly one bucket");
            }
        }
    }

    /* (3) n/min/max/sum accumulate across samples; min is NOT stuck at the
       zero-initialised value (the classic uninitialised-min bug). */
    span_reset_all();
    span_record(2, 500);
    span_record(2, 20);
    span_record(2, 9000);
    span_stats(2, st);
    assert(st[0] == 3       && "n");
    assert(st[1] == 20      && "min took the smallest, not the first or the zero init");
    assert(st[2] == 9000    && "max");
    assert(st[3] == 9520    && "sumUsLo");
    assert(st[4] == 0       && "sumUsHi");

    /* (4) The 1e9 lo/hi split. 3 samples of 400e6 us = 1.2e9 us, which is past
       the 31-bit SMI ceiling of 1073741823 — the overflow this split exists to
       avoid. lo must stay below 1e9 so it is always SMI-safe. */
    span_reset_all();
    span_record(3, 400000000U);
    span_record(3, 400000000U);
    span_record(3, 400000000U);
    span_stats(3, st);
    assert(st[4] == 1         && "sumUsHi = 1200000000 / 1e9");
    assert(st[3] == 200000000U && "sumUsLo = 1200000000 %% 1e9");
    assert(st[3] < 1000000000U && "lo is always SMI-safe");

    /* (5) Slots are independent — writing slot 4 does not disturb slot 5. */
    span_reset_all();
    span_record(4, 700);
    span_stats(5, st);
    assert(st[0] == 0 && "slot 5 untouched");

    /* (6) An out-of-range slot is ignored, not a crash and not a wild write.
       nsl computes slot indices, so a bad one must be inert. */
    span_reset_all();
    span_record(OEVM_SPAN_SLOTS, 123);
    span_record(-1, 123);
    span_stats(OEVM_SPAN_SLOTS, st);
    for (int i = 0; i < SPAN_STATS_FIELDS; i++) assert(st[i] == 0);

    /* (7) begin/end records exactly one sample against the real clock. The
       DURATION is not asserted (that would be timing-flaky); only that a sample
       appeared and is internally consistent. */
    span_reset_all();
    span_begin(6);
    span_end(6);
    span_stats(6, st);
    assert(st[0] == 1 && "begin/end produced one sample");
    assert(st[1] <= st[2] && "min <= max");

    /* (8) An UNOPENED end records nothing — the open-mask guard. Without it a
       stray spanEnd: would record garbage from a zero start. */
    span_reset_all();
    span_end(7);
    span_stats(7, st);
    assert(st[0] == 0 && "end without begin records nothing");

    /* (9) An abandoned span self-heals: begin, never end, begin again, end ->
       exactly one sample. This is the exception-unwind case from the design. */
    span_reset_all();
    span_begin(7);
    span_begin(7);
    span_end(7);
    span_stats(7, st);
    assert(st[0] == 1 && "re-begin overwrites the abandoned start");

    /* (10) span_now is monotonic non-decreasing across a spin (test_millis
       precedent). */
    {
        uint32_t a = span_now();
        volatile unsigned long spin = 0;
        for (unsigned long i = 0; i < 5000000UL; i++) spin += i;
        uint32_t b = span_now();
        assert(b - a < 1000000000U && "a plausible elapsed microsecond delta");
    }

    printf("test_span: OK\n");
    return 0;
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
cd ~/workspace/tuvm && gcc -std=c99 -Isrc tests/test_span.c \
  src/span.c src/backend/posix/span_posix.c -lm -o /tmp/tspan
```

Expected: FAIL — `fatal error: ../src/span.h: No such file or directory`.

- [ ] **Step 3: Write the clock seam header**

Create `src/span_backend.h`:

```c
#ifndef OEVM_SPAN_BACKEND_H
#define OEVM_SPAN_BACKEND_H
/* The microsecond clock seam for job-latency spans (#61). Exactly one impl is
   linked per OEVM_TARGET, the timer_backend.h precedent:
     backend/posix/span_posix.c   — CLOCK_MONOTONIC in wall-clock mode; under the
                                    DEFAULT virtual clock it answers
                                    timer_backend_millis() * 1000 so the nsl suite
                                    stays bit-reproducible.
     backend/zephyr/span_zephyr.c — k_cycle_get_32(), 62.5 ns on the C6 and 4.2 ns
                                    on the WROOM-32.

   THIS IS THE THIRD CLOCK IN THE TREE (tuvm#52). The other two:
     oevm_millis()           — the in-language `millis` hook (src/vm.c)
     timer_backend_millis()  — the timer seam's clock, 1 ms granular on target
   span_now() is the ONLY one that is 32-bit and therefore wrap-limited: it wraps
   every 268 s on the C6 and every 17.9 s on the WROOM-32. That is safe here and
   ONLY here, because spans use unsigned 32-bit DIFFERENCES, which are correct
   across a wrap provided the interval is shorter than the wrap period. Never use
   span_now() as an absolute timestamp. */
#include <stdint.h>

uint32_t span_now(void);   /* free-running microseconds; use ONLY via u32 deltas */
#endif /* OEVM_SPAN_BACKEND_H */
```

- [ ] **Step 4: Write the slot API header**

Create `src/span.h`:

```c
#ifndef OEVM_SPAN_H
#define OEVM_SPAN_H
/* Job-latency span slots (#61). A fixed array of accumulators — count, min, max,
   sum, and six log-spaced decade buckets — with ZERO allocation, so any context
   (prim, ISR-adjacent thread code, HTTP egress) may bump one freely. This is the
   mechanic review/metrics-spec.md specifies for all node telemetry.

   SLOTS ARE NAMELESS. C never learns that slot 3 means "acquire"; the name table
   lives in nsl (kernel.nsl's Spans class), exactly as timer_backend.h keeps the
   channel and message in nsl and carries only a token in C. */
#include <stdint.h>

#ifndef OEVM_SPAN_SLOTS
#define OEVM_SPAN_SLOTS 8          /* 8 * 48 B + a 4 B open-mask = 388 B of DRAM */
#endif

/* span_stats field order — mirrored by the prim and by nsl's Spans class.
   sum is SPLIT because an SMI is 31-bit (max 1073741823) and a microsecond sum
   passes that after ~17.9 minutes of accumulated span time:
     sum = sumUsHi * 1000000000 + sumUsLo,  with sumUsLo always < 1e9. */
enum {
    SPAN_STAT_N = 0, SPAN_STAT_MIN, SPAN_STAT_MAX,
    SPAN_STAT_SUM_LO, SPAN_STAT_SUM_HI,
    SPAN_STAT_B0, SPAN_STAT_B1, SPAN_STAT_B2,
    SPAN_STAT_B3, SPAN_STAT_B4, SPAN_STAT_B5,
    SPAN_STATS_FIELDS
};

void span_begin(int slot);                  /* stamp the start; re-begin is legal */
void span_end(int slot);                    /* accumulate now-start; no-op if not open */
void span_record(int slot, uint32_t us);    /* feed a sample directly (test seam + C callers) */
void span_stats(int slot, uint32_t *out);   /* fill SPAN_STATS_FIELDS values; zeroes a bad slot */
void span_reset_all(void);                  /* zero every slot — tests only */
#endif /* OEVM_SPAN_H */
```

- [ ] **Step 5: Write the slot implementation**

Create `src/span.c`:

```c
/* Job-latency span slots — see src/span.h for the contract and #61 for the why. */
#include "span.h"
#include "span_backend.h"
#include <string.h>

struct SpanSlot {
    uint32_t start;      /* span_now() at span_begin; meaningful only while open */
    uint32_t n;
    uint32_t min, max;   /* microseconds */
    uint64_t sum;        /* microseconds */
    uint32_t b[6];       /* decade buckets, edges in bucket_of below */
};                       /* 48 B: 4 u32 = 16, u64 sum at 16, b[6] at 24..48 */

static struct SpanSlot spans[OEVM_SPAN_SLOTS];
/* Open-ness is a bitmask, NOT a per-slot flag: a uint8_t field would push the
   struct to 56 B under the u64's 8-byte alignment, costing 64 B of DRAM on a
   board with 816 B spare (#53). One shared u32 costs 4 B and caps slots at 32,
   which is far above OEVM_SPAN_SLOTS. It also lets start==0 be a legal stamp
   rather than an in-band "not open" sentinel. */
static uint32_t span_open_mask;

static int slot_ok(int slot) { return slot >= 0 && slot < OEVM_SPAN_SLOTS; }

/* Log-spaced decades. A comparison chain, not a division — this runs on the
   measured path. */
static int bucket_of(uint32_t us) {
    if (us < 100)       return 0;   /* < 100 us */
    if (us < 1000)      return 1;   /* < 1 ms   */
    if (us < 10000)     return 2;   /* < 10 ms  */
    if (us < 100000)    return 3;   /* < 100 ms */
    if (us < 1000000)   return 4;   /* < 1 s    */
    return 5;                       /* >= 1 s   */
}

void span_begin(int slot) {
    if (!slot_ok(slot)) return;
    spans[slot].start = span_now();
    span_open_mask |= (uint32_t)1 << slot;
}

void span_end(int slot) {
    if (!slot_ok(slot)) return;
    /* Not open: a stray end, or an end whose begin was skipped. Record nothing —
       a phantom sample would be worse than a missing one, because it would move
       the percentiles with a duration nobody measured. */
    if ((span_open_mask & ((uint32_t)1 << slot)) == 0) return;
    span_open_mask &= ~((uint32_t)1 << slot);
    /* Unsigned 32-bit difference: correct across a counter wrap provided the span
       is shorter than the wrap period (17.9 s on the WROOM-32). */
    span_record(slot, span_now() - spans[slot].start);
}

void span_record(int slot, uint32_t us) {
    struct SpanSlot *s;
    if (!slot_ok(slot)) return;
    s = &spans[slot];
    /* n == 0 seeds min, so no UINT32_MAX initialiser is needed and a
       statically-zeroed array is already correct. */
    if (s->n == 0 || us < s->min) s->min = us;
    if (us > s->max) s->max = us;
    s->n++;
    s->sum += us;
    s->b[bucket_of(us)]++;
}

void span_stats(int slot, uint32_t *out) {
    const struct SpanSlot *s;
    int i;
    if (out == NULL) return;
    if (!slot_ok(slot)) {
        for (i = 0; i < SPAN_STATS_FIELDS; i++) out[i] = 0;
        return;
    }
    s = &spans[slot];
    out[SPAN_STAT_N]      = s->n;
    out[SPAN_STAT_MIN]    = s->min;
    out[SPAN_STAT_MAX]    = s->max;
    out[SPAN_STAT_SUM_LO] = (uint32_t)(s->sum % 1000000000ULL);
    out[SPAN_STAT_SUM_HI] = (uint32_t)(s->sum / 1000000000ULL);
    for (i = 0; i < 6; i++) out[SPAN_STAT_B0 + i] = s->b[i];
}

void span_reset_all(void) {
    memset(spans, 0, sizeof(spans));
    span_open_mask = 0;
}
```

- [ ] **Step 6: Write the posix clock backend**

Create `src/backend/posix/span_posix.c`:

```c
/* Host impl of the span_backend.h microsecond clock seam (#61).

   Under the DEFAULT virtual clock the host has no real time at all — nothing moves
   but the nsl-level advanceMillis: prim — so answering CLOCK_MONOTONIC here would
   make every nsl span test depend on how fast the machine ran, which is exactly the
   determinism timer_posix.c exists to protect. Instead we scale the virtual
   millisecond clock to microseconds. Tests that need sub-millisecond resolution use
   spanRecord:us: to feed samples directly and involve no clock at all. */
#include "../../span_backend.h"
#include "../../timer_backend.h"
#include <stdlib.h>
#include <time.h>

static int wallclock_mode(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *e = getenv("OEVM_TIMER_WALLCLOCK");
        cached = (e && *e && *e != '0') ? 1 : 0;
    }
    return cached;
}

uint32_t span_now(void) {
    if (!wallclock_mode()) {
        /* Virtual clock: deterministic, advances only via advanceMillis:. */
        return (uint32_t)(timer_backend_millis() * 1000ULL);
    }
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        /* Truncated to 32 bits deliberately — see span_backend.h: only DELTAS are
           meaningful, and a u32 delta is correct across the wrap. */
        return (uint32_t)((uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL);
    }
}
```

- [ ] **Step 7: Run the test to verify it passes**

```bash
cd ~/workspace/tuvm && gcc -std=c99 -Isrc tests/test_span.c \
  src/span.c src/backend/posix/span_posix.c src/backend/posix/timer_posix.c \
  -lm -o /tmp/tspan && /tmp/tspan
```

Expected: `test_span: OK`

> Note: `timer_posix.c` joins the link line because `span_posix.c` calls `timer_backend_millis()` for the virtual clock. If `timer_posix.c` drags in VM symbols the link will fail — in that case, and only then, replace the virtual-clock branch with a file-static counter that `span_reset_all` does not touch, and record the deviation in the task's commit message.

- [ ] **Step 8: Register the test in the aggregate runner**

In `tools/run_c_tests.sh`, immediately after the `run test_http` line (line 79):

```bash
# #61: job-latency span slots (src/span.c + the posix microsecond clock seam).
# Backend-only link like test_http above — span.c has no VM dependencies at all,
# which is the point: any context may bump a slot.
run test_span   "src/span.c src/backend/posix/span_posix.c src/backend/posix/timer_posix.c"
```

- [ ] **Step 9: Run the full C suite**

```bash
cd ~/workspace/tuvm && ./tools/run_c_tests.sh
```

Expected: every line `PASS` or `SKIP`; `test_span` shows `PASS`; exit status 0.

- [ ] **Step 10: Commit**

```bash
cd ~/workspace/tuvm
git add src/span.h src/span.c src/span_backend.h src/backend/posix/span_posix.c \
        tests/test_span.c tools/run_c_tests.sh
git commit -m "feat(#61): job-latency span slots + the microsecond clock seam

A fixed 8-slot static array (388 B) of count/min/max/sum plus six
log-spaced decade buckets, zero allocation, nameless by design — the
name table lives in nsl, the rule timer_backend.h already states.

sum is split lo/hi at 1e9 because an SMI is 31-bit and a microsecond
sum passes 1073741823 after ~17.9 min of accumulated span time.

span_now() is the tree's THIRD clock (#52) and the only 32-bit one; it
is wrap-safe only because spans use u32 differences. Documented on the
seam header.

Tests feed samples via span_record so bucket edges are asserted exactly
with no clock involved and nothing can be timing-flaky."
```

---

### Task 2: The four prims

**Files:**
- Modify: `src/dispatch.c` (enum after `PRIM_pf_sensorArmEvent`, `PRIM_NAMES` tail, parity guard at line 811)
- Modify: `src/prim_ids.h` (append ids 118–121)
- Modify: `src/platform.c` (four cases + the `span.h` include)
- Test: `tests/test_platform.c` (extend)

**Interfaces:**
- Consumes: `span_begin/span_end/span_record/span_stats` from Task 1.
- Produces: prims `spanBegin:` (118), `spanEnd:` (119), `spanStats:` (120), `spanRecord:us:` (121), reachable from nsl as `platform spanBegin: 3` etc. `spanStats:` answers a raw indexable of 11 SMIs, read with `basicAt:` (1-based), field order per `SPAN_STAT_*`.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_platform.c`, inside `main`, before the final `printf`:

```c
    /* #61: the span prims. spanStats: must answer an 11-slot indexable of SMIs in
       SPAN_STAT_* order, and spanRecord:us: must land the sample in the right
       bucket THROUGH the prim seam (span.c's own bucketing is covered by
       tests/test_span.c; what is new here is SMI tag/untag across the boundary). */
    {
        oop args[2];
        oop out;
        span_reset_all();

        args[0] = TAG_SMI(2);
        args[1] = TAG_SMI(1500);              /* 1.5 ms -> bucket 2 (< 10 ms) */
        assert(platform_dispatch(PRIM_pf_spanRecord, 2, args, &out) == 1
               && "spanRecord:us: dispatched");

        args[0] = TAG_SMI(2);
        assert(platform_dispatch(PRIM_pf_spanStats, 1, args, &out) == 1
               && "spanStats: dispatched");
        {
            struct ObjectInstance *a = (struct ObjectInstance *)out;
            assert(a->slots[1] == TAG_SMI(1)    && "n");
            assert(a->slots[2] == TAG_SMI(1500) && "minUs");
            assert(a->slots[3] == TAG_SMI(1500) && "maxUs");
            assert(a->slots[4] == TAG_SMI(1500) && "sumUsLo");
            assert(a->slots[5] == TAG_SMI(0)    && "sumUsHi");
            assert(a->slots[8] == TAG_SMI(1)    && "bucket 2 got the sample");
            assert(a->slots[6] == TAG_SMI(0)    && "bucket 0 empty");
        }

        /* begin/end reaches span.c through the seam — duration not asserted. */
        args[0] = TAG_SMI(3);
        assert(platform_dispatch(PRIM_pf_spanBegin, 1, args, &out) == 1);
        assert(platform_dispatch(PRIM_pf_spanEnd, 1, args, &out) == 1);
        args[0] = TAG_SMI(3);
        assert(platform_dispatch(PRIM_pf_spanStats, 1, args, &out) == 1);
        assert(((struct ObjectInstance *)out)->slots[1] == TAG_SMI(1)
               && "begin/end through the prim seam recorded one sample");

        printf("test_platform: span prims PASS\n");
    }
```

Add near the top of `tests/test_platform.c`, with the other includes:

```c
#include "../src/span.h"
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
cd ~/workspace/tuvm && ./tools/run_c_tests.sh 2>&1 | grep test_platform
```

Expected: `test_platform  FAIL`, with `'PRIM_pf_spanRecord' undeclared`.

- [ ] **Step 3: Append the enum entries in dispatch.c**

In `src/dispatch.c`, immediately after `PRIM_pf_sensorArmEvent,` and before the closing `};` of the enum:

```c
    /* #61 job latency: the span seam. spanBegin:/spanEnd: open and close a
       nameless slot; spanStats: snapshots one; spanRecord:us: feeds a sample
       directly (the deterministic test seam — see the design §8, and §2.2 for why
       it is not gated behind a test define). Appended after sensorArmEvent — no
       pre-existing id shifts. Counted (not computed) to be the 118th-121st
       PRIM_NAMES entries. Platform-dispatched (mirrored in prim_ids.h). */
    PRIM_pf_spanBegin,
    PRIM_pf_spanEnd,
    PRIM_pf_spanStats,
    PRIM_pf_spanRecord,
```

- [ ] **Step 4: Append the names and update the parity guard**

In `src/dispatch.c`, append to the end of the `PRIM_NAMES[]` initialiser (after `"sensorArmEventPost:on:threshold:"` or whatever the final existing entry is — read it, do not assume):

```c
    "spanBegin:", "spanEnd:", "spanStats:", "spanRecord:us:",
```

Then change the parity guard (currently at `src/dispatch.c:811`) from:

```c
typedef char prim_names_enum_parity_check[(N_PRIM_NAMES == PRIM_pf_sensorArmEvent) ? 1 : -1];
```

to:

```c
typedef char prim_names_enum_parity_check[(N_PRIM_NAMES == PRIM_pf_spanRecord) ? 1 : -1];
```

- [ ] **Step 5: Mirror the ids in prim_ids.h**

Append to `src/prim_ids.h`, before the closing `#endif`:

```c
/* #61 job latency: the span seam — spanBegin:/spanEnd:/spanStats:/spanRecord:us:.
   Appended after sensorArmEvent; no pre-existing id shifts. 118-121 were arrived
   at by COUNTING PRIM_NAMES (117 entries before this append), not by 117+1
   arithmetic, per the standing rule above. */
#define PRIM_pf_spanBegin  118
#define PRIM_pf_spanEnd    119
#define PRIM_pf_spanStats  120
#define PRIM_pf_spanRecord 121
```

- [ ] **Step 6: Verify the counted ids are right before writing any prim case**

```bash
cd ~/workspace/tuvm && python3 -c "
import re
s = open('src/dispatch.c').read()
body = s.split('static const char *const PRIM_NAMES[] = {',1)[1].split('};',1)[0]
names = re.findall(r'\"((?:[^\"\\\\]|\\\\.)*)\"', body)
for want, nm in ((118,'spanBegin:'),(119,'spanEnd:'),(120,'spanStats:'),(121,'spanRecord:us:')):
    got = names.index(nm) + 1
    print(('OK  ' if got == want else 'BAD '), nm, 'is id', got, '(expected', str(want)+')')
print('total names:', len(names))
"
```

Expected: four `OK` lines and `total names: 121`. **If any line says BAD, fix `prim_ids.h` to the counted value — the count wins, never the arithmetic.**

- [ ] **Step 7: Add the prim cases in platform.c**

Add to the includes at the top of `src/platform.c`:

```c
#include "span.h"
```

Add these cases to the `platform_dispatch` switch, after `case PRIM_pf_sensorArmEvent:`'s block:

```c
    /* --- #61 job latency: the span seam. Slots are integers; the name table
       lives in nsl (kernel.nsl's Spans class), so C never learns what slot 3
       means — the timer-seam convention. All four answer the platform capability
       except spanStats:, which answers its own snapshot.
       Bad slot indices are inert (span.c guards), so a miscomputed index in nsl
       loses samples rather than corrupting memory. */
    case PRIM_pf_spanBegin:
        span_begin((int)UNTAG_SMI(args[0]));
        *out = platform_object();
        return 1;
    case PRIM_pf_spanEnd:
        span_end((int)UNTAG_SMI(args[0]));
        *out = platform_object();
        return 1;
    case PRIM_pf_spanRecord:
        /* args mirror the keyword selector left-to-right: spanRecord: slot us: n
           -> args[0]=slot, args[1]=microseconds. */
        span_record((int)UNTAG_SMI(args[0]), (uint32_t)UNTAG_SMI(args[1]));
        *out = platform_object();
        return 1;
    case PRIM_pf_spanStats: {
        /* Same GC-safety shape as heapStats/sysStats above: every scalar is copied
           into a C local BEFORE the single allocating call (indexable_new), and the
           stores are SMIs (immediates, no allocation), so no heap oop is held across
           an allocation and no gc_temp_root pin is required.
           Every field is SMI-safe by construction: counts and microsecond
           magnitudes are far below 2^30, and sum is pre-split at 1e9 by span.c
           precisely so sumUsLo cannot exceed it (src/span.h). */
        uint32_t st[SPAN_STATS_FIELDS];
        int i;
        oop r;
        span_stats((int)UNTAG_SMI(args[0]), st);
        r = indexable_new(SPAN_STATS_FIELDS);
        for (i = 0; i < SPAN_STATS_FIELDS; i++)
            ((struct ObjectInstance *)r)->slots[i + 1] = TAG_SMI((intptr_t)st[i]);
        *out = r;
        return 1;
    }
```

- [ ] **Step 8: Add span.c to the C test link sets**

In `tools/run_c_tests.sh`, add `src/span.c` and `src/backend/posix/span_posix.c` to `ENG_COMMON` (line 24–29), since `platform.c` now references them:

```bash
ENG_COMMON="src/platform.c src/serial.c src/udp.c src/http.c src/http_core.c src/mirror.c src/space.c src/handles.c src/span.c \
  src/backend/posix/platform_posix.c src/backend/posix/serial_posix.c src/backend/posix/udp_posix.c \
  src/backend/posix/http_posix.c \
  src/backend/posix/pulse_posix.c \
  src/backend/posix/timer_posix.c \
  src/backend/posix/span_posix.c \
  src/backend/posix/sensor_posix.c"
```

- [ ] **Step 9: Run the full C suite**

```bash
cd ~/workspace/tuvm && ./tools/run_c_tests.sh
```

Expected: all `PASS`/`SKIP`, exit 0, and `test_platform: span prims PASS` visible in the output of a direct run.

- [ ] **Step 10: Commit**

```bash
cd ~/workspace/tuvm
git add src/dispatch.c src/prim_ids.h src/platform.c tests/test_platform.c tools/run_c_tests.sh
git commit -m "feat(#61): span prims 118-121 (spanBegin:/End:/Stats:/Record:us:)

Appended after sensorArmEvent, ids counted from PRIM_NAMES not computed,
parity guard advanced to PRIM_pf_spanRecord.

spanStats: answers an 11-slot indexable of SMIs in SPAN_STAT_* order,
the heapStats/sysStats shape: scalars into C locals before the single
allocating call, SMI stores only, no gc_temp_root pin needed."
```

---

### Task 3: The Zephyr clock backend

**Files:**
- Create: `src/backend/zephyr/span_zephyr.c`
- Modify: `zephyr/CMakeLists.txt:31-39` (the backend source list) and the shared-source list that carries `src/space.c` etc.

**Interfaces:**
- Consumes: `span_backend.h` from Task 1.
- Produces: a linkable `span_now()` on target. No nsl-visible change.

- [ ] **Step 1: Write the Zephyr backend**

Create `src/backend/zephyr/span_zephyr.c`:

```c
/* Zephyr impl of the span_backend.h microsecond clock seam (#61).

   k_cycle_get_32() is a register read of the platform cycle counter:
     ESP32-C6   16 MHz -> 62.5 ns resolution, wraps every 268 s
     ESP32 WROOM 240 MHz -> 4.2 ns resolution, wraps every 17.9 s
   Both are far finer than k_uptime_get()'s 1 ms, which is why this seam exists —
   a VM-compute segment is hundreds of microseconds and 1 ms cannot see it.

   The wrap is handled by the CALLER: src/span.c subtracts two u32 samples, which
   is correct across a wrap so long as the span is shorter than the wrap period.
   CONFIG_TIMING_FUNCTIONS is not enabled on either build, so Zephyr's timing_*
   API is unavailable; this needs no new Kconfig. */
#include "../../span_backend.h"
#include <zephyr/kernel.h>

uint32_t span_now(void) {
    return k_cyc_to_us_floor32(k_cycle_get_32());
}
```

- [ ] **Step 2: Wire it into the Zephyr build**

In `zephyr/CMakeLists.txt`, add to the backend source list (beside `timer_zephyr.c` at line 35):

```cmake
  ${TUVM_SRC}/src/backend/zephyr/span_zephyr.c
```

And add `${TUVM_SRC}/src/span.c` to the same target's shared source list — find where `${TUVM_SRC}/src/space.c` is listed and add it directly after, so the slot array links on target.

- [ ] **Step 3: Build for the WROOM-32 and confirm it links**

```bash
cd ~/workspace/tuvm/zephyr && rm -rf build-esp32-span && \
  west build -b esp32_devkitc/esp32/procpu -d build-esp32-span \
    -- -DEXTRA_CONF_FILE=prj_esp32.conf 2>&1 | tail -30
```

Expected: a successful link ending in a memory report. **Record the `dram0_0_seg` line verbatim** — it is the baseline half of the #53 delta this plan owes. If the link fails on DRAM overflow, drop `OEVM_SPAN_SLOTS` to 6 via `-DOEVM_SPAN_SLOTS=6` and record that this happened; do not proceed with 8.

> If `west build` is not on PATH, source the ESP32 west workspace first — see `~/workspace/tuvm/zephyr/README.md`. The ESP32 tree is `~/zephyr-esp32`, NOT `~/ncs` (that one is for nRF).

- [ ] **Step 4: Commit**

```bash
cd ~/workspace/tuvm
git add src/backend/zephyr/span_zephyr.c zephyr/CMakeLists.txt
git commit -m "feat(#61): Zephyr microsecond clock via k_cycle_get_32

62.5 ns on the C6, 4.2 ns on the WROOM-32 — against k_uptime_get()'s
1 ms, which cannot resolve a VM-compute segment. Wrap is the caller's
problem and span.c handles it with u32 differences.

dram0 after this change: <paste the memory report line here>"
```

---

### Task 4: The `Spans` class in nsl

**Files:**
- Modify: `~/workspace/nsl-tuvm/lib/kernel.nsl` (a new class beside `HeapStats`, plus a `public Spans` accessor beside `public HeapStats`)
- Modify: `~/workspace/nsl-tuvm/tools/run_tests_oevm.py:29-36` (`OEVM_BACKEND` list)
- Create: `~/workspace/nsl-tests/19_latency/01_span_slots.nsl`

**Interfaces:**
- Consumes: prims 118–121 from Task 2.
- Produces: `platform Spans` → the class; `Spans new` → an instance; `register: aSymbol` → SMI slot; `begin: slot`, `end: slot`, `recordSlot: slot us: n`, `statsAt: slot` → an `Array`-read indexable, `nameAt: slot` → Symbol, `slotCount` → SMI, `noteFailure: slot`, `failuresAt: slot`.

- [ ] **Step 1: Add span_posix.c to the nsl suite's backend list**

In `~/workspace/nsl-tuvm/tools/run_tests_oevm.py`, extend `OEVM_BACKEND` (line 29) with:

```python
                 f"src/backend/{OEVM_TARGET}/span_{OEVM_TARGET}.c"]
```

and add `"src/span.c"` wherever the non-backend VM sources are listed (search for `src/space.c` in that file and add `src/span.c` beside it).

- [ ] **Step 2: Write the failing nsl test**

Create `~/workspace/nsl-tests/19_latency/01_span_slots.nsl`:

```
(*
expect:
false
true
true
true
true
true
true
true
done
*)
(* tuvm#61: the span slot seam end-to-end — prims 118-121 through the kernel's
   Spans class. Values are FED, not timed (spanRecord:us:), so bucket edges are
   asserted exactly and nothing is timing-dependent; the clock plumbing is a
   separate test (02_span_clock). Slots are nameless in C and named here, so this
   also proves the name<->slot table round-trips. Both-path A==B gate. *)
class SpanSlotsTest packageUsing: manifest = () (
public main: platform args: args = (
  | spans acquire compute s |
  spans::   platform Spans new.
  acquire:: spans register: #acquire.
  compute:: spans register: #vmCompute.

  (* Registration hands out distinct slots and remembers the names. *)
  (acquire = compute) printString println.                  (* false *)
  ((spans nameAt: acquire) = #acquire) printString println.  (* true  *)

  (* A virgin slot is all zeroes. *)
  s:: spans statsAt: acquire.
  ((s basicAt: 1) = 0) printString println.                  (* true  *)

  (* Exact bucket edges: 99 us -> b0, 100 us -> b1. b0 is field 6. *)
  spans recordSlot: acquire us: 99.
  spans recordSlot: acquire us: 100.
  s:: spans statsAt: acquire.
  ((s basicAt: 1) = 2) printString println.                  (* n = 2      *)
  ((s basicAt: 6) = 1) printString println.                  (* b0 = 1     *)
  ((s basicAt: 7) = 1) printString println.                  (* b1 = 1     *)
  ((s basicAt: 2) = 99) printString println.                 (* min = 99   *)
  ((s basicAt: 3) = 100) printString println.                (* max = 100  *)

  'done' println.
)
) : (
)
```

> The first expected line is `false` — `acquire` and `compute` must be *distinct* slots. The remaining seven are `true`.

- [ ] **Step 3: Run the test to verify it fails**

```bash
cd ~/workspace/nsl-tuvm && python3 tools/run-one.py ~/workspace/nsl-tests/19_latency/01_span_slots.nsl
```

Expected: FAIL — `Spans` does not resolve (a DNU or a compile error naming `Spans`).

- [ ] **Step 4: Add the `Spans` class to kernel.nsl**

In `~/workspace/nsl-tuvm/lib/kernel.nsl`, immediately after the `HeapStats` class's closing `) : ( public fromList: a = ( ^ self new setFromList: a ) )`:

```
  class Spans = ( | names fails | ) (
    (* tuvm#61: named access to the C span slots (prims 118-121). The C side is
       NAMELESS — slots are integers 0..N-1 — exactly as the timer seam carries
       only a token and keeps the channel and message in nsl. This class owns the
       name<->slot table and the per-slot failure counts.

       register: is called ONCE per name at setup and answers the slot index; the
       caller HOLDS that index and passes it to begin:/end:. Nothing on a measured
       path may do a name lookup, because a lookup is a send and an allocation and
       this code runs inside the thing it is measuring.

       State is lazily initialised in register: rather than in a class-side `new`
       override. `ThermalView new` is the precedent — a plain `new` with instance
       variables starting nil — and it avoids depending on `super new` resolving
       in class-side context, which is not a pattern this kernel uses anywhere. *)
    ensureTables = (
      names isNil ifTrue: [
        names:: OrderedCollection new.
        fails:: OrderedCollection new ].
      ^ self )

    public register: aName = (
      | i n |
      self ensureTables.
      (* Idempotent: registering the same name twice answers the same slot, so a
         re-entered setup path cannot silently consume the slot budget. *)
      i:: 1. n:: names size.
      [ i <= n ] whileTrue: [
        ((names at: i) = aName) ifTrue: [ ^ i - 1 ].
        i:: i + 1 ].
      names add: aName.
      fails add: 0.
      ^ names size - 1 )

    public slotCount = ( ^ names size )
    public nameAt: slot = ( ^ names at: slot + 1 )

    public begin: slot = ( platform spanBegin: slot. ^ self )
    public end:   slot = ( platform spanEnd: slot. ^ self )
    public recordSlot: slot us: n = ( platform spanRecord: slot us: n. ^ self )
    public statsAt: slot = ( ^ platform spanStats: slot )

    (* A failed job produces no span sample, so without this a failure would
       silently IMPROVE the percentiles. Counted in nsl because a failure is an
       app-level concept the C seam cannot detect (design 2.2). *)
    public noteFailure: slot = (
      fails at: slot + 1 put: (fails at: slot + 1) + 1. ^ self )
    public failuresAt: slot = ( ^ fails at: slot + 1 )
  ) : (
  )
```

- [ ] **Step 5: Expose it on the platform capability**

In `~/workspace/nsl-tuvm/lib/kernel.nsl`, beside `public HeapStats = ( ^ HeapStats )` (around line 2063):

```
  (* tuvm#61: the span-slot capability. Answers the CLASS (the `^ Cbor new`
     comment above explains why a class accessor is spelled this way — sending an
     instance-side selector to a class object silently NEWs it and drops the
     message). Callers do `platform Spans new`. *)
  public Spans = ( ^ Spans )
```

- [ ] **Step 6: Run the test to verify it passes**

```bash
cd ~/workspace/nsl-tuvm && python3 tools/run-one.py ~/workspace/nsl-tests/19_latency/01_span_slots.nsl
```

Expected: PASS. A SKIP proves nothing — it means the runner returned before compiling. If you see SKIP, investigate why before continuing.

- [ ] **Step 7: Commit both repos**

```bash
cd ~/workspace/nsl-tuvm
git add lib/kernel.nsl tools/run_tests_oevm.py
git commit -m "feat(#61): Spans class — named access to the nameless C span slots

register: is called once at setup and answers a slot index the caller
holds; nothing on a measured path does a name lookup, because a lookup
is a send and an allocation inside the thing being measured.

Per-slot failure counts live here too: a failed job produces no span
sample, so uncounted failures would silently improve the percentiles."

cd ~/workspace/nsl-tests
git add 19_latency/01_span_slots.nsl
git commit -m "test(#61): span slot seam end-to-end through prims 118-121

Samples are fed with spanRecord:us: rather than timed, so bucket edges
are asserted exactly (99 -> b0, 100 -> b1) and nothing is
timing-dependent."
```

---

### Task 5: The clock-driven span test

**Files:**
- Create: `~/workspace/nsl-tests/19_latency/02_span_clock.nsl`

**Interfaces:**
- Consumes: `Spans` from Task 4, `platform advanceMillis:`.
- Produces: nothing new — this closes the coverage gap Task 4 deliberately left.

- [ ] **Step 1: Write the failing test**

Create `~/workspace/nsl-tests/19_latency/02_span_clock.nsl`:

```
(*
expect:
true
true
true
true
done
*)
(* tuvm#61: the begin/end clock path, which 01_span_slots deliberately does not
   touch (it feeds samples instead). Determinism comes from the VIRTUAL clock:
   span_posix.c scales timer_backend_millis() to microseconds under the default
   virtual clock, so advanceMillis: moves the span clock by an exact amount and
   the resulting bucket is not a race. 5 ms -> bucket 2 (< 10 ms), field 8.
   An unclosed span must record NOTHING and self-heal on the next begin — the
   exception-unwind case. Both-path A==B gate. *)
class SpanClockTest packageUsing: manifest = () (
public main: platform args: args = (
  | spans job s |
  spans:: platform Spans new.
  job::   spans register: #job.

  spans begin: job.
  platform advanceMillis: 5.
  spans end: job.
  s:: spans statsAt: job.
  ((s basicAt: 1) = 1) printString println.       (* one sample        *)
  ((s basicAt: 9) = 1) printString println.       (* b2, the 1-10ms decade *)

  (* An end with no begin records nothing. *)
  spans end: job.
  s:: spans statsAt: job.
  ((s basicAt: 1) = 1) printString println.       (* still one sample  *)

  (* An abandoned span self-heals: begin, abandon, begin again, end -> exactly
     one further sample, not two and not zero. *)
  spans begin: job.
  spans begin: job.
  platform advanceMillis: 5.
  spans end: job.
  s:: spans statsAt: job.
  ((s basicAt: 1) = 2) printString println.       (* two samples total *)

  'done' println.
)
) : (
)
```

- [ ] **Step 2: Run to verify it fails, then passes**

```bash
cd ~/workspace/nsl-tuvm && python3 tools/run-one.py ~/workspace/nsl-tests/19_latency/02_span_clock.nsl
```

If it fails on the bucket assertion, the virtual-clock branch of `span_posix.c` is not wired — re-read Task 1 Step 6. No implementation change should be needed for this test; it exists to prove Task 1 and Task 4 compose.

- [ ] **Step 3: Commit**

```bash
cd ~/workspace/nsl-tests
git add 19_latency/02_span_clock.nsl
git commit -m "test(#61): begin/end clock path under the virtual clock

advanceMillis: moves the span clock by an exact amount, so the bucket
is deterministic rather than a race. Covers the unclosed-span and
abandoned-span cases from the design."
```

---

### Task 6: The percentile-decade reducer

**Files:**
- Modify: `~/workspace/nsl-tuvm/lib/kernel.nsl` (extend `Spans`)
- Create: `~/workspace/nsl-tests/19_latency/03_decades.nsl`

**Interfaces:**
- Consumes: `Spans>>statsAt:` from Task 4.
- Produces: `Spans>>decadeAt: slot percentile: p` → a Symbol naming the decade (`#lt100us`, `#lt1ms`, `#lt10ms`, `#lt100ms`, `#lt1s`, `#ge1s`) or `#insufficient` when the §4.2 gate is unmet; `Spans>>p50At:`, `Spans>>p95At:`; `Spans>>minSamplesForP95` → 200.

- [ ] **Step 1: Write the failing test**

Create `~/workspace/nsl-tests/19_latency/03_decades.nsl`:

```
(*
expect:
true
true
true
true
true
true
done
*)
(* tuvm#61 design 4.1/4.2: percentiles are the DECADE a sample count falls in,
   never a number interpolated inside a bucket — a log-scaled bucket's mass is not
   uniform, so an interpolated figure would carry precision the data does not
   contain. And no p95 at all below n >= 200, because at small n a p95 is just the
   k-th sample and one retransmit relocates it.

   Histograms here are hand-built with recordSlot:us: so every case is exact.
   Both-path A==B gate. *)
class DecadesTest packageUsing: manifest = () (
  fill: spans slot: slot count: k us: us = (
    | i |
    i:: 1. [ i <= k ] whileTrue: [ spans recordSlot: slot us: us. i:: i + 1 ].
    ^ self )

public main: platform args: args = (
  | spans a b c |
  spans:: platform Spans new.
  a:: spans register: #tooFew.
  b:: spans register: #oneBucket.
  c:: spans register: #tailed.

  (* (1) Below the gate, p95 is refused outright. 199 samples is one short. *)
  self fill: spans slot: a count: 199 us: 500.
  ((spans p95At: a) = #insufficient) printString println.

  (* (2) At exactly 200 it is reported. This is the boundary, both sides. *)
  spans recordSlot: a us: 500.
  ((spans p95At: a) = #lt1ms) printString println.

  (* (3) All mass in ONE bucket: p50 and p95 are the same decade. An
     interpolating reducer would have invented two different numbers here. *)
  self fill: spans slot: b count: 300 us: 2000.
  ((spans p50At: b) = #lt10ms) printString println.
  ((spans p95At: b) = #lt10ms) printString println.

  (* (4) A real tail: 290 fast samples and 10 slow ones (3.3%). p50 sits in the
     fast decade, p95 must NOT be dragged to the slow one -- 3.3% is under 5%. *)
  self fill: spans slot: c count: 290 us: 300.
  self fill: spans slot: c count: 10  us: 2000000.
  ((spans p50At: c) = #lt1ms) printString println.
  ((spans p95At: c) = #lt1ms) printString println.

  'done' println.
)
) : (
)
```

- [ ] **Step 2: Run to verify it fails**

```bash
cd ~/workspace/nsl-tuvm && python3 tools/run-one.py ~/workspace/nsl-tests/19_latency/03_decades.nsl
```

Expected: FAIL — `p95At:` does not understand.

- [ ] **Step 3: Add the reducer to `Spans`**

Add these methods inside the `Spans` class in `~/workspace/nsl-tuvm/lib/kernel.nsl`:

```
    (* --- design 4.1/4.2: percentiles as DECADES, gated on sample count --- *)

    (* A p95 computed from too few samples is noise wearing a precise-looking
       label: at n = 20 it is simply the 19th value, and one unlucky retransmit
       relocates it. 200 is the floor below which nothing is reported. *)
    public minSamplesForP95 = ( ^ 200 )

    decadeNames = ( ^ #(#lt100us #lt1ms #lt10ms #lt100ms #lt1s #ge1s) )

    (* Walk the cumulative histogram and answer the decade in which the
       p-th percentile sample falls. NO interpolation inside the bucket — see the
       design 4.1 for why inventing a figure there is the easiest thing for a
       critic to attack. *)
    public decadeAt: slot percentile: p = (
      | s n target seen i |
      s:: self statsAt: slot.
      n:: s basicAt: 1.
      (n < self minSamplesForP95) ifTrue: [ ^ #insufficient ].
      (* The target is the 1-based rank of the percentile sample, rounded up so
         that p95 of 200 samples is the 190th, not the 189.99th. *)
      target:: ((n * p) + 99) // 100.
      seen:: 0. i:: 1.
      [ i <= 6 ] whileTrue: [
        seen:: seen + (s basicAt: 5 + i).     (* buckets are fields 6..11 *)
        (seen >= target) ifTrue: [ ^ self decadeNames at: i ].
        i:: i + 1 ].
      ^ #ge1s )

    public p50At: slot = ( ^ self decadeAt: slot percentile: 50 )
    public p95At: slot = ( ^ self decadeAt: slot percentile: 95 )
```

- [ ] **Step 4: Run to verify it passes**

```bash
cd ~/workspace/nsl-tuvm && python3 tools/run-one.py ~/workspace/nsl-tests/19_latency/03_decades.nsl
```

Expected: PASS.

- [ ] **Step 5: Run the whole nsl suite for regressions**

```bash
cd ~/workspace/nsl-tuvm && python3 tools/run_tests_oevm.py 2>&1 | tail -20
```

Expected: no new failures against the pre-task baseline. Takes 2–3 minutes. Never run two nsl suites at once — they share `/tmp`.

- [ ] **Step 6: Commit**

```bash
cd ~/workspace/nsl-tuvm
git add lib/kernel.nsl
git commit -m "feat(#61): percentile decades with an n>=200 gate

A percentile is the decade it lands in, never a number interpolated
inside a log-scaled bucket whose mass is not uniform. Below n=200 a p95
is refused outright and answers #insufficient."

cd ~/workspace/nsl-tests
git add 19_latency/03_decades.nsl
git commit -m "test(#61): decade reducer, the n=199/200 boundary, one-bucket histogram

The one-bucket case is the one an interpolating reducer would have
invented two different numbers for."
```

---

### Task 7: The `#LatencySpan` record and the `sys/latency` cell

**Files:**
- Modify: `~/workspace/nsl-tuvm/lib/kernel.nsl` (extend `Spans`)
- Create: `~/workspace/nsl-tests/19_latency/04_record.nsl`

**Interfaces:**
- Consumes: `Spans>>statsAt:`, `Spans>>nameAt:`, `Spans>>failuresAt:` (Task 4); `Record`, `Map` (existing kernel classes).
- Produces: `Spans>>recordAt: slot` → a `#LatencySpan` Record with fields `span, n, fails, minUs, maxUs, sumUsLo, sumUsHi, b0..b5`.

- [ ] **Step 1: Write the failing test**

Create `~/workspace/nsl-tests/19_latency/04_record.nsl`:

```
(*
expect:
true
true
true
true
true
true
done
*)
(* tuvm#61 design 7: one #LatencySpan record PER SLOT on a sys/latency cell —
   NOT a 12th #Metrics field. porta docs/PROTOCOL-NSL-RECORDS.md 4.4 forbids
   appending there and names this issue as the reason: bucket counts are many
   fields at once. Adding a JOB here adds a record, not a field, which is exactly
   the property #Metrics lacks.

   Publishing is the app's act on a user-requested cadence (G9) — this test IS the
   publisher; no periodic publisher exists. Both-path A==B gate. *)
class LatencyRecordTest packageUsing: manifest = () (
public main: platform args: args = (
  | spans slot r cell got |
  spans:: platform Spans new.
  slot::  spans register: #acquire.
  spans recordSlot: slot us: 1500.
  spans recordSlot: slot us: 2500.
  spans noteFailure: slot.

  r:: spans recordAt: slot.
  (r tag = #LatencySpan) printString println.
  ((r at: #span  ifAbsent: [ nil ]) = #acquire) printString println.
  ((r at: #n     ifAbsent: [ -1 ]) = 2) printString println.
  ((r at: #fails ifAbsent: [ -1 ]) = 1) printString println.
  ((r at: #b2    ifAbsent: [ -1 ]) = 2) printString println.

  (* Round-trips through the cell unchanged — a reader peeks by name,
     non-destructively (4.4 kind C). *)
  cell:: platform space channel: 'sys/latency'.
  cell put: '' value: r.
  got:: cell peek: '' ifAbsent: [ nil ].
  (got = r) printString println.

  'done' println.
)
) : (
)
```

- [ ] **Step 2: Run to verify it fails**

```bash
cd ~/workspace/nsl-tuvm && python3 tools/run-one.py ~/workspace/nsl-tests/19_latency/04_record.nsl
```

Expected: FAIL — `recordAt:` does not understand.

- [ ] **Step 3: Add the record builder to `Spans`**

Add inside the `Spans` class in `~/workspace/nsl-tuvm/lib/kernel.nsl`:

```
    (* One record PER SLOT, published onto sys/latency. Deliberately NOT an
       extension of #Metrics: porta docs/PROTOCOL-NSL-RECORDS.md 4.4 forbids a
       12th field there and names tuvm#61 as the reason, because bucket counts are
       many fields at once. The consequence worth preserving is that adding a JOB
       adds a RECORD, not a field, so this schema never grows.

       Field names are deliberately SHORT: 4.4 observes that symbol field names
       dominate the encoded size, which is how #Metrics reached ~185-230 B on
       eleven fields with names like largestFreeBlock. *)
    public recordAt: slot = (
      | s pairs |
      s:: self statsAt: slot.
      pairs:: OrderedCollection new.
      pairs add: #span    -> (self nameAt: slot).
      pairs add: #n       -> (s basicAt: 1).
      pairs add: #fails   -> (self failuresAt: slot).
      pairs add: #minUs   -> (s basicAt: 2).
      pairs add: #maxUs   -> (s basicAt: 3).
      pairs add: #sumUsLo -> (s basicAt: 4).
      pairs add: #sumUsHi -> (s basicAt: 5).
      pairs add: #b0      -> (s basicAt: 6).
      pairs add: #b1      -> (s basicAt: 7).
      pairs add: #b2      -> (s basicAt: 8).
      pairs add: #b3      -> (s basicAt: 9).
      pairs add: #b4      -> (s basicAt: 10).
      pairs add: #b5      -> (s basicAt: 11).
      ^ Record tag: #LatencySpan fields: (Map fromPairs: pairs) )
```

- [ ] **Step 4: Run to verify it passes, then commit**

```bash
cd ~/workspace/nsl-tuvm && python3 tools/run-one.py ~/workspace/nsl-tests/19_latency/04_record.nsl
```

Expected: PASS. Then commit `lib/kernel.nsl` and `19_latency/04_record.nsl` in their respective repos.

---

### Task 8: C-owned spans on the HTTP path

**Files:**
- Modify: `src/http_core.c:129` (`http_accept_request`), `:151` (`http_take_request`), `:190` (`http_chunk`)
- Modify: `tests/test_http.c` (extend)

**Interfaces:**
- Consumes: `span_record` from Task 1.
- Produces: two C-owned slots whose indices are **fixed by convention** — `SPAN_SLOT_HTTP_QUEUE` = 6, `SPAN_SLOT_EGRESS` = 7, declared in `src/span.h`. nsl must register its own names in an order that leaves those two free; Task 9 does so explicitly.

**Why fixed indices:** these two spans are stamped inside prims that own the work, so no app can ship an uninstrumented HTTP path. That means C must know which slot to use without asking nsl — the one place the nameless rule is bent, and it is bent by pinning two constants rather than by teaching C a name.

- [ ] **Step 1: Add the two reserved slot constants**

In `src/span.h`, before the closing `#endif`:

```c
/* The two C-OWNED slots (#61 design 5). Everything else is nsl's to allocate.
   These are stamped inside the prims that own the work, so an app cannot ship an
   uninstrumented HTTP path by forgetting a bracket — which is why C needs to know
   the index without asking nsl. They are pinned at the TOP of the range so nsl's
   sequential register: allocation (which starts at 0) never collides. */
#define SPAN_SLOT_HTTP_QUEUE (OEVM_SPAN_SLOTS - 2)   /* accept -> take */
#define SPAN_SLOT_EGRESS     (OEVM_SPAN_SLOTS - 1)   /* the write() inside http_chunk */
```

- [ ] **Step 2: Write the failing test**

Append to `tests/test_http.c`, in `main` before the final `printf`:

```c
    /* #61: the C-owned HTTP spans. A request that is accepted and then taken must
       leave exactly one httpQueue sample; a chunk write must leave exactly one
       egress sample. These are stamped in C precisely so no app can omit them. */
    {
        uint32_t st[SPAN_STATS_FIELDS];
        int id;
        char m[8], p[64];
        span_reset_all();
        http_backend_reset();

        http_accept_request(1, "GET", 3, "/thermal", 8);
        span_stats(SPAN_SLOT_HTTP_QUEUE, st);
        assert(st[SPAN_STAT_N] == 0 && "no sample until the request is TAKEN");

        http_take_request(&id, m, sizeof m, p, sizeof p);
        span_stats(SPAN_SLOT_HTTP_QUEUE, st);
        assert(st[SPAN_STAT_N] == 1 && "accept->take recorded one queue sample");

        printf("test_http: span instrumentation PASS\n");
    }
```

Add `#include "../src/span.h"` to the test's includes.

- [ ] **Step 3: Run to verify it fails**

```bash
cd ~/workspace/tuvm && ./tools/run_c_tests.sh 2>&1 | grep test_http
```

Expected: `test_http FAIL` — `SPAN_SLOT_HTTP_QUEUE` undeclared or `span_stats` unresolved.

- [ ] **Step 4: Instrument `http_core.c`**

Add to the includes at the top of `src/http_core.c`:

```c
#include "span.h"
#include "span_backend.h"
```

In the request slot struct used by `http_accept_request`/`http_take_request` (read the file — it is the in-flight array `fd_for_id` indexes), add a field:

```c
    uint32_t accepted_us;   /* #61: span_now() at accept; closed at take */
```

In `http_accept_request`, where the slot is populated:

```c
    slot->accepted_us = span_now();   /* #61: opens the httpQueue span */
```

In `http_take_request`, immediately after the slot is selected and before it is handed out:

```c
    /* #61: close the httpQueue span. Recorded directly rather than via
       span_begin/span_end because the interval is stored per-REQUEST, not per-slot
       — several requests can be in flight, and one shared slot start would have
       them overwrite each other. */
    span_record(SPAN_SLOT_HTTP_QUEUE, span_now() - slot->accepted_us);
```

In `http_chunk`, bracket only the write:

```c
    {
        uint32_t t0 = span_now();
        /* ... the existing write() call, unchanged ... */
        span_record(SPAN_SLOT_EGRESS, span_now() - t0);
    }
```

- [ ] **Step 5: Add span.c to the test_http link set**

In `tools/run_c_tests.sh`, change line 79:

```bash
run test_http   "src/http_core.c src/backend/posix/http_posix.c src/span.c src/backend/posix/span_posix.c src/backend/posix/timer_posix.c"
```

- [ ] **Step 6: Run the full C suite, then commit**

```bash
cd ~/workspace/tuvm && ./tools/run_c_tests.sh
```

Expected: all `PASS`/`SKIP`, exit 0.

```bash
git add src/span.h src/http_core.c tests/test_http.c tools/run_c_tests.sh
git commit -m "feat(#61): C-owned httpQueue and egress spans

Stamped inside the prims that own the work, so an app cannot ship an
uninstrumented HTTP path by forgetting a bracket. That is why these two
slots are pinned constants at the top of the range rather than
registered by name — the one place the nameless rule is bent.

httpQueue uses a per-REQUEST timestamp rather than the shared slot
start, because several requests can be in flight and one shared start
would have them overwrite each other."
```

---

### Task 9: Instrument `ThermalHttpNode` and render the `/stats` table

**Files:**
- Modify: `zephyr/ThermalHttpNode.nsl`

**Interfaces:**
- Consumes: `platform Spans`, `Spans>>register:/begin:/end:/p50At:/p95At:/statsAt:/nameAt:` (Tasks 4, 6).
- Produces: an instrumented node whose `/stats` page carries the latency table. No new nsl API.

- [ ] **Step 1: Register the app slots in `main:args:`**

In `zephyr/ThermalHttpNode.nsl`'s `main: platform args: args`, after `timers::` is set up:

```
    (* tuvm#61: six app-named slots. Registration order matters only in that it
       must stay clear of the two C-owned slots, which are pinned at the TOP of
       the range (src/span.h) precisely so this sequential allocation cannot
       collide with them. *)
    spans::   platform Spans new.
    sAcquire:: spans register: #acquire.
    sCompute:: spans register: #vmCompute.
    sPublish:: spans register: #publish.
    sJob::     spans register: #jobThermal.
    sRender::  spans register: #render.
```

Add `spans sAcquire sCompute sPublish sJob sRender` to the method's temporaries, and pass `spans` plus the slot indices into `ThermalNode` via its constructor (the Milestone-C capabilities-as-constructor-args rule — do **not** reach for a global).

- [ ] **Step 2: Bracket the thermal job**

Rewrite `ThermalNode>>sampleOnce` (currently at `zephyr/ThermalHttpNode.nsl:100`):

```
    sampleOnce = (
      | f oc i |
      spans begin: sJob.
      spans begin: sAcquire.
      f:: seam sensorFrame.
      spans end: sAcquire.
      (f == nil) ifTrue: [
        (* A failed fetch produces no sample, so it must be COUNTED or it
           silently improves the percentiles (design 4.2). The outer span is
           abandoned deliberately; span.c self-heals on the next begin. *)
        spans noteFailure: sJob.
        ^ self ].
      spans begin: sCompute.
      oc:: seam OrderedCollection new.
      i:: 1. [ i <= 64 ] whileTrue: [
        oc add: (f basicAt: i). i:: i + 1 ].
      spans end: sCompute.
      spans begin: sPublish.
      sample put: '' value: (seam List fromCollection: oc).
      spans end: sPublish.
      spans end: sJob.
      ^ self )
```

- [ ] **Step 3: Bracket the render**

In `main:args:`, wrap the `/thermal` route block:

```
    http route: '/thermal' do: [ :id :plat |
        spans begin: sRender.
        tview renderFrame: ((space channel: 'dev/thermal0/sample') peek) toId: id on: plat.
        spans end: sRender ].
```

- [ ] **Step 4: Add the latency table to `/stats`**

Add these methods to `ThermalHttpNode`, and call `self latencyTableOn: platform toId: id spans: spans` from `statsPageOn:toId:` just before the closing `</tbody></table>` chunk:

```
  latencyTableOn: platform toId: id spans: spans = (
    | i |
    platform httpChunkId: id data:
      '<h2>job latency</h2>',
      '<p>Percentiles are the bucket DECADE a sample falls in, never a number ',
      'interpolated inside it &mdash; a log-scaled bucket''s mass is not uniform. ',
      'max is exact. No p95 is shown below n&nbsp;&ge;&nbsp;200.</p>',
      '<table style=''border-collapse:collapse;width:100%''>',
      '<thead><tr><th>span</th><th>n</th><th>fails</th><th>p50</th><th>p95</th>',
      '<th>max&nbsp;&micro;s</th><th>histogram</th></tr></thead><tbody>'.
    i:: 0. [ i < spans slotCount ] whileTrue: [
      self latencyRow: i toId: id on: platform spans: spans.
      i:: i + 1 ].
    platform httpChunkId: id data: '</tbody></table>'.
    ^ self )

  latencyRow: slot toId: id on: platform spans: spans = (
    | s b j |
    s:: spans statsAt: slot.
    b:: ''. j:: 6. [ j <= 11 ] whileTrue: [
      b:: b , (s basicAt: j) printString , ' '. j:: j + 1 ].
    platform httpChunkId: id data:
      '<tr><td>' , (spans nameAt: slot) printString ,
      '</td><td>' , (s basicAt: 1) printString ,
      '</td><td>' , (spans failuresAt: slot) printString ,
      '</td><td>' , (spans p50At: slot) printString ,
      '</td><td>' , (spans p95At: slot) printString ,
      '</td><td>' , (s basicAt: 3) printString ,
      '</td><td>' , b , '</td></tr>'.
    ^ self )
```

> The histogram column is deliberate: it lets a human on the bench see the shape that produced the summary, which is the whole reason the node ships raw buckets rather than a computed number.

- [ ] **Step 5: Compile the node to `.oimg` and verify it builds**

```bash
cd ~/workspace/nsl-tuvm && python3 tools/ns_compile_oevm.py \
  ~/workspace/tuvm/zephyr/ThermalHttpNode.nsl -o ~/workspace/tuvm/zephyr/ThermalHttpNode.oimg
```

Expected: a fresh `.oimg` with no compile error. **Do not use `-g`** — never build a solidified image with debug symbols.

- [ ] **Step 6: Commit**

```bash
cd ~/workspace/tuvm
git add zephyr/ThermalHttpNode.nsl zephyr/ThermalHttpNode.oimg zephyr/ThermalHttpNode.stmap
git commit -m "feat(#61): instrument ThermalHttpNode and render the latency table

Five app-named spans plus the two C-owned ones. A nil sensor frame is
counted as a failure rather than silently skipped, because a failure
that produces no sample would otherwise improve the percentiles.

/stats renders p50/p95 as decades with the raw histogram beside them,
so a human on the bench sees the shape that produced the summary."
```

---

### Task 10: The client RTT harness

**Files:**
- Create: `zephyr/tools/latency_harness.py`

**Interfaces:**
- Consumes: a running node's HTTP endpoint.
- Produces: client-observed RTT percentiles for the `request→format→response` path, which is the only way to get the network column (design §3 — the node cannot see ingress wire time).

- [ ] **Step 1: Write the harness**

Create `zephyr/tools/latency_harness.py`:

```python
#!/usr/bin/env python3
"""Client-side RTT harness for tuvm#61.

The node cannot measure ingress wire time: http_accept_request is the earliest
on-node moment, and everything before it happened where the node has no clock.
So the network column is derived as (client RTT - node service time), which needs
no clock synchronisation because each half is measured with a single clock and only
DURATIONS are compared.

500 requests is chosen to clear the design's n >= 200 p95 gate with margin. It is
fixed here rather than tuned after seeing a result.
"""
import argparse
import statistics
import time
import urllib.request

DECADES = [(100, "<100us"), (1_000, "<1ms"), (10_000, "<10ms"),
           (100_000, "<100ms"), (1_000_000, "<1s"), (float("inf"), ">=1s")]


def decade(us):
    for edge, name in DECADES:
        if us < edge:
            return name
    return ">=1s"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("url", help="e.g. http://192.168.1.50:18080/thermal")
    ap.add_argument("-n", type=int, default=500,
                    help="request count (default 500, clears the n>=200 p95 gate)")
    args = ap.parse_args()

    samples, failures = [], 0
    for _ in range(args.n):
        t0 = time.perf_counter()
        try:
            with urllib.request.urlopen(args.url, timeout=10) as r:
                r.read()
        except Exception:
            failures += 1
            continue
        samples.append((time.perf_counter() - t0) * 1e6)

    n = len(samples)
    print(f"url        {args.url}")
    print(f"n          {n} ok, {failures} failed")
    if n == 0:
        print("no successful samples — nothing to report")
        return
    samples.sort()
    p50 = samples[max(0, (n * 50 + 99) // 100 - 1)]
    print(f"p50        {p50:.0f} us   ({decade(p50)})")
    if n >= 200:
        p95 = samples[max(0, (n * 95 + 99) // 100 - 1)]
        print(f"p95        {p95:.0f} us   ({decade(p95)})")
    else:
        print(f"p95        insufficient (n={n} < 200) — the design refuses this")
    print(f"max        {max(samples):.0f} us   (exact)")
    print(f"mean       {statistics.mean(samples):.0f} us   (secondary, never the headline)")


if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Smoke-test it against a host build**

Run `ThermalHttpNode` on the host (posix, wall-clock mode) and point the harness at it:

```bash
cd ~/workspace/tuvm && OEVM_TIMER_WALLCLOCK=1 ./oevm_runner zephyr/ThermalHttpNode.oimg &
sleep 2
python3 zephyr/tools/latency_harness.py http://127.0.0.1:18080/hello -n 250
```

Expected: `n 250 ok, 0 failed`, a p50 line, a p95 line (250 ≥ 200), and an exact max. Kill the background runner afterwards.

- [ ] **Step 3: Commit**

```bash
cd ~/workspace/tuvm
git add zephyr/tools/latency_harness.py
git commit -m "feat(#61): client-side RTT harness for the network column

The node cannot see ingress wire time, so network is derived as
(client RTT - node service time). No clock sync needed: each half is
measured with one clock and only durations are compared.

500 requests by default, fixed to clear the n>=200 p95 gate rather
than tuned after seeing a result."
```

---

### Task 11: Device run on the WROOM-32 and the published table

**Files:**
- Modify: `review/program-sequence.md`, `review/tests/CLAIMS.md`, `review/metrics-spec.md`
- Modify: `~/workspace/porta/docs/PROTOCOL-NSL-RECORDS.md` §4.4
- Modify: `src/timer_backend.h` (the #52 footgun note)

**Interfaces:**
- Consumes: everything above.
- Produces: the measured table — the actual deliverable of #61.

- [ ] **Step 1: Flash the WROOM-32**

```bash
cd ~/workspace/tuvm/zephyr && west flash -d build-esp32-span --esp-device /dev/ttyUSB1
```

**`/dev/ttyUSB1` is the WROOM-32.** `/dev/ttyACM0` and `/dev/ttyUSB0` are two ports of the same ESP32-C6 and flashing them will overwrite the Thread ping node. Confirm before flashing by resetting the port and reading the ROM banner — the WROOM prints `ets Jun 8 2016`, the C6 prints `ESP-ROM:esp32c6-20220919`.

- [ ] **Step 2: Record the DRAM delta**

From the `west build` memory report, record `dram0_0_seg` and compute the delta against #53's baseline of `195792 B / 192 KB  99.58%`. Expected increase: ~388 B for `spans[8]` plus whatever the new code costs in `.data`/`.bss`.

If the build did not link, this is the finding: report it on #53 with the numbers, drop `OEVM_SPAN_SLOTS`, and note the reduction in the CLAIMS entry.

- [ ] **Step 3: Let both paths clear the n ≥ 200 gate**

Read the node's IP from the console, then:

```bash
# The thermal path self-clears: 2 Hz x 5 min = 600 samples. Just wait.
sleep 300
# The HTTP path needs the harness.
python3 ~/workspace/tuvm/zephyr/tools/latency_harness.py http://<node-ip>:18080/thermal -n 500
curl -s http://<node-ip>:18080/stats
```

- [ ] **Step 4: Assemble the published table**

For each of the seven instrumented spans record: `n`, `fails`, p50 decade, p95 decade, exact max, and the six bucket counts. Add the two derived columns the design calls for and **label them derived, not measured**:

- **render-compute** = `render − egress` (§5 — `httpChunk:` formats in nsl and writes in C, so the nsl bracket necessarily contains the write)
- **network** = client RTT − node service time (§3)

- [ ] **Step 5: Measure the encoded record sizes**

Encode one `#LatencySpan` and one `#Metrics` through the CBOR path and record the byte counts. §4.4 flags that `#Metrics`'s ~185–230 B is an *estimate nobody has measured*, and this work builds the encoder path for a sibling record — so measuring both discharges a standing debt for the cost of one run.

- [ ] **Step 6: Promote to the documents of record**

Per the promotion rule, #61 is not closed until all five land:

1. **`review/program-sequence.md`** — the rung and the measured table.
2. **`review/tests/CLAIMS.md`** — each latency claim with its witness, its **sample count**, and percentiles stated as decades.
3. **`review/metrics-spec.md`** — the catalog gains a latency group, **and** the line under *Deliberately NOT collected* reading *"histograms (porta can bucket; nodes can't afford the slots)"* must be amended: it is now false, and a spec that contradicts the code is worse than no spec. Say what the slot budget was that made it affordable.
4. **`~/workspace/porta/docs/PROTOCOL-NSL-RECORDS.md` §4.4** — `#LatencySpan` added to the `sys/**` table, plus both measured encoded sizes from Step 5.
5. **`src/timer_backend.h` and `src/span_backend.h`** — #52's footgun note gains a third clock. There are now `oevm_millis`, `timer_backend_millis` and `span_now`; the seam headers must say which is which and that only `span_now` is wrap-limited.

- [ ] **Step 7: Commit and close**

```bash
cd ~/workspace/tuvm
git add review/ src/timer_backend.h src/span_backend.h
git commit -m "docs(#61): publish the measured job-latency table

<paste the table>

dram0 after instrumentation: <n> B (<delta> B over the #53 baseline).
Encoded record sizes measured, discharging the estimate PROTOCOL-NSL-RECORDS 4.4
flagged: #Metrics <n> B, #LatencySpan <n> B.

metrics-spec.md's 'histograms: nodes can't afford the slots' line
amended — it is now false."
```

---

### Task 12 (optional, sequenced last): the `tickQueue` span

**Files:**
- Modify: `src/pulse_backend.h`, `src/backend/posix/pulse_posix.c`, `src/backend/zephyr/pulse_zephyr.c`
- Modify: `tests/test_pulse.c`

**Why it is last and separable:** this is the only span that requires touching the ISR mailbox — the seam #34/#55 just stabilised, and the one `#62` notes has no automated coverage on the Zephyr side. Everything above ships and publishes a complete table without it. If it looks risky when you get here, file the deferral rather than forcing it.

**The semantic decision, made in advance:** a coalesced post keeps the **earliest** stamp, so `tickQueue` measures the worst-case queue delay for that wakeup. Keeping the latest would understate exactly the condition the span exists to detect.

- [ ] **Step 1: Add a stamp to the mailbox entry**

Add `uint32_t posted_us` to the mailbox entry struct in both pulse backends, set from `span_now()` in `pulse_post_isr`. On the coalesce path, **do not overwrite** an existing stamp.

Cost: 4 B × `OEVM_PULSE_MAILBOX_DEPTH` (16) = **64 B of DRAM**. Check this against the Task 11 Step 2 headroom before writing the code; if the margin is under ~150 B, stop and file the deferral instead.

- [ ] **Step 2: Close the span in `pulse_pump`**

Where `pulse_pump` calls `space_post_id`, add:

```c
span_record(SPAN_SLOT_TICK_QUEUE, span_now() - entry->posted_us);
```

with `SPAN_SLOT_TICK_QUEUE` added to `src/span.h` beside the other two C-owned constants (and `OEVM_SPAN_SLOTS` raised to 9 if the DRAM allows — otherwise take a slot back from the app-named set).

- [ ] **Step 3: Extend `tests/test_pulse.c`**

Assert that a post followed by a pump leaves exactly one `tickQueue` sample, and that **two coalesced posts followed by one pump leave exactly one sample whose duration derives from the FIRST post** — the semantic decision above, which is the whole reason this task is not mechanical.

- [ ] **Step 4: Run the C suite and commit**

```bash
cd ~/workspace/tuvm && ./tools/run_c_tests.sh
```

---

## Self-Review Notes

**Spec coverage:** §1 clock → Tasks 1, 3. §2 seam → Tasks 1, 2, 4. §3 client RTT → Task 10. §4 buckets/decades/n≥200 → Tasks 1, 6. §5 instrumented spans → Tasks 8, 9, 12. §6 DRAM budget → Tasks 3, 11. §7 record + `/stats` → Tasks 7, 9. §8 validation → Tasks 1, 5, 6, 11. §9 close-out → Task 11 Step 6. §10 sequencing → task order.

**Known deviation from the design, to reconcile at close-out:** the design states 384 B for eight slots; the implementation adds a 4 B open-mask, so the true figure is **388 B**. The mask exists because a per-slot `uint8_t open` flag would push the struct from 48 B to 56 B under the `u64`'s alignment — 64 B of DRAM on a board with 816 B spare. Task 11 should correct the design doc's figure rather than leave the two disagreeing.

**Deliberate coverage gap:** `span_zephyr.c` has no host test — it cannot have one, since `k_cycle_get_32()` does not exist off-target. It is covered by the Task 11 device run, which is the same arrangement `#62` complains about for the pulse shadow ring. If Task 12 is skipped, say so explicitly in the close-out rather than letting the table imply full coverage.
