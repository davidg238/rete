#ifndef OEVM_SPAN_H
#define OEVM_SPAN_H
/* Job-latency span slots (#61). A fixed array of accumulators — count, min, max,
   sum, and six log-spaced decade buckets — with ZERO allocation, so any context
   (prim, ISR-adjacent thread code, HTTP egress) may bump one freely. This is the
   mechanic review/metrics-spec.md specifies for all node telemetry: "collection is
   a fixed C array of counters/gauges ... zero allocation on any hot path".

   SLOTS ARE NAMELESS. C never learns that slot 3 means "acquire"; the name table
   lives in nsl (kernel.nsl's Spans class), exactly as timer_backend.h keeps the
   channel and the message in nsl and carries only a token in C. The two exceptions
   are the C-OWNED slots below, and they are pinned constants rather than names. */
#include <stdint.h>

#ifndef OEVM_SPAN_SLOTS
#define OEVM_SPAN_SLOTS 8          /* 8 * 48 B + a 4 B open-mask = 388 B of DRAM */
#endif

/* span_stats field order — mirrored by the spanStats: prim and by nsl's Spans class.
   sum is SPLIT because an SMI is 31-bit (TAG_SMI is `val << 1 | 1`, so the max is
   1073741823 on a 32-bit target) and a microsecond sum passes that after only
   ~17.9 minutes of accumulated span time:
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

/* The two C-OWNED slots (#61 design §5) — the one place the nameless rule above is
   bent, and it is bent by pinning two constants rather than by teaching C a name.
   Everything else in the range is nsl's to allocate.

   They exist because these two intervals are stamped inside the core functions that
   own the work, so a node cannot ship an uninstrumented HTTP path by forgetting a
   bracket — which means C must know the index without asking nsl. Pinned at the TOP
   of the range so nsl's sequential register: allocation, which hands out 0 upward,
   never collides. */
#define SPAN_SLOT_HTTP_QUEUE (OEVM_SPAN_SLOTS - 2)   /* accept -> take           */
#define SPAN_SLOT_EGRESS     (OEVM_SPAN_SLOTS - 1)   /* the writes in http_chunk */
#endif /* OEVM_SPAN_H */
