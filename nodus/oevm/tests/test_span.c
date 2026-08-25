/* oevm/tests/test_span.c — host TDD for the job-latency span slots (#61).
   Build (backend-only link, the test_http precedent — span.c has no VM deps):
     cd ~/workspace/tuvm && gcc -std=c99 -Isrc tests/test_span.c \
       src/span.c src/backend/posix/span_posix.c -lm -o /tmp/tspan && /tmp/tspan

   Verifies the accumulate logic exhaustively WITHOUT a real clock (span_record
   feeds samples directly), so nothing here can be timing-flaky.

   The timer_backend_millis STUB below is why the link set stays this small.
   span_posix.c calls it for the virtual clock, and linking the real
   src/backend/posix/timer_posix.c instead would pull in space_post and from there
   the entire VM — defeating the point of a backend-only unit test. The stub is not
   merely a link fix: because the test CONTROLS it, the virtual-clock scaling that
   the nsl suite depends on becomes exactly assertable here (case 10). The real
   timer_posix.c pairing is covered end-to-end by
   nodus/tests/19_latency/02_span_clock.nsl. */
#include "../src/span.h"
#include "../src/span_backend.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

/* The virtual clock span_posix.c reads. Advanced only by this test, exactly as
   advanceMillis: advances it in the nsl suite. */
static uint64_t stub_ms;
uint64_t timer_backend_millis(void) { return stub_ms; }

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
            assert(st[SPAN_STAT_N] == 1 && "one sample recorded");
            for (int b = 0; b < 6; b++) {
                uint32_t want = (b == cases[c].b) ? 1u : 0u;
                assert(st[SPAN_STAT_B0 + b] == want
                       && "sample landed in exactly one bucket");
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
    assert(st[SPAN_STAT_N]      == 3    && "n");
    assert(st[SPAN_STAT_MIN]    == 20   && "min took the smallest, not the first or the zero init");
    assert(st[SPAN_STAT_MAX]    == 9000 && "max");
    assert(st[SPAN_STAT_SUM_LO] == 9520 && "sumUsLo");
    assert(st[SPAN_STAT_SUM_HI] == 0    && "sumUsHi");

    /* (4) The 1e9 lo/hi split. 3 samples of 400e6 us = 1.2e9 us, which is past
       the 31-bit SMI ceiling of 1073741823 — the overflow this split exists to
       avoid. lo must stay below 1e9 so it is always SMI-safe. */
    span_reset_all();
    span_record(3, 400000000U);
    span_record(3, 400000000U);
    span_record(3, 400000000U);
    span_stats(3, st);
    assert(st[SPAN_STAT_SUM_HI] == 1          && "sumUsHi = 1200000000 / 1e9");
    assert(st[SPAN_STAT_SUM_LO] == 200000000U && "sumUsLo = 1200000000 mod 1e9");
    assert(st[SPAN_STAT_SUM_LO] < 1000000000U && "lo is always SMI-safe");

    /* (5) Slots are independent — writing slot 4 does not disturb slot 5. */
    span_reset_all();
    span_record(4, 700);
    span_stats(5, st);
    assert(st[SPAN_STAT_N] == 0 && "slot 5 untouched");

    /* (6) An out-of-range slot is ignored, not a crash and not a wild write.
       nsl computes slot indices, so a bad one must be inert. */
    span_reset_all();
    span_record(OEVM_SPAN_SLOTS, 123);
    span_record(-1, 123);
    span_stats(OEVM_SPAN_SLOTS, st);
    for (int i = 0; i < SPAN_STATS_FIELDS; i++) assert(st[i] == 0);

    /* (7) begin/end records exactly one sample, and because the clock is the
       controllable virtual one, the DURATION is asserted exactly rather than
       waved at: 5 virtual ms must arrive as 5000 us, landing in bucket 2. */
    span_reset_all();
    stub_ms = 1000;
    span_begin(6);
    stub_ms = 1005;
    span_end(6);
    span_stats(6, st);
    assert(st[SPAN_STAT_N] == 1        && "begin/end produced one sample");
    assert(st[SPAN_STAT_MAX] == 5000   && "5 virtual ms arrived as 5000 us");
    assert(st[SPAN_STAT_B2] == 1       && "5 ms landed in the 1-10 ms decade");

    /* (8) An UNOPENED end records nothing — the open-mask guard. Without it a
       stray spanEnd: would record garbage from a zero start. */
    span_reset_all();
    span_end(7);
    span_stats(7, st);
    assert(st[SPAN_STAT_N] == 0 && "end without begin records nothing");

    /* (9) An abandoned span self-heals: begin, never end, begin again, end ->
       exactly one sample, and its duration is measured from the SECOND begin.
       This is the exception-unwind case from the design: the first begin's start
       must be discarded entirely, not blended in. */
    span_reset_all();
    stub_ms = 2000;
    span_begin(7);
    stub_ms = 2900;          /* the abandoned span would have been 900 ms */
    span_begin(7);
    stub_ms = 2902;
    span_end(7);
    span_stats(7, st);
    assert(st[SPAN_STAT_N] == 1      && "re-begin overwrites the abandoned start");
    assert(st[SPAN_STAT_MAX] == 2000 && "measured from the SECOND begin, not the first");

    /* (10) span_stamp tracks the virtual clock exactly under the default
       (non-wall-clock) mode. This is the property the whole nsl suite's
       determinism rests on: advanceMillis: must move the span clock by a known
       amount, or every bucket assertion over there becomes a race. */
    stub_ms = 0;
    assert(span_stamp() == 0 && "virtual clock at zero");
    stub_ms = 7;
    assert(span_stamp() == 7000 && "virtual milliseconds scale to microseconds");
    stub_ms = 12;
    assert(span_elapsed_us(7000) == 5000 && "a 5 ms advance is a 5000 us delta");

    /* (11) The elapsed helper is wrap-correct in the backend's OWN unit. On the
       host that unit is the microsecond and the counter wraps at 2^32, so a stamp
       taken 1000 us before the wrap and read 200 us after it must answer 1200 —
       not 4294966096. This is the property the Zephyr backend violated by
       converting cycles to microseconds BEFORE subtracting (span_backend.h). */
    stub_ms = 0;
    {
        /* stub_ms is 0, so span_stamp() reads 0 — i.e. the counter has just
           wrapped. A stamp taken 1000 us earlier is 2^32-1000. */
        uint32_t before_wrap = (uint32_t)0 - 1000u;
        assert(span_elapsed_us(before_wrap) == 1000u
               && "a stamp from before the wrap yields a small positive delta");
    }

    printf("test_span: OK\n");
    return 0;
}
