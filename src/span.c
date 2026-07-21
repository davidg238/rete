/* Job-latency span slots — see src/span.h for the contract and tuvm#61 for the why.
   No clock policy and no names live here: the clock is span_backend.h's job and the
   names are nsl's. This file is pure accumulation. */
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
   struct to 56 B under the u64's 8-byte alignment, costing an extra 64 B of DRAM
   on a board with 816 B spare (tuvm#53). One shared u32 costs 4 B and caps slots
   at 32, far above OEVM_SPAN_SLOTS. It also lets start==0 be a legal stamp rather
   than an in-band "not open" sentinel — which matters, because span_now() really
   does return 0 once per wrap. */
static uint32_t span_open_mask;

static int slot_ok(int slot) { return slot >= 0 && slot < OEVM_SPAN_SLOTS; }

/* Log-spaced decades. A comparison chain, not a division — this runs on the
   measured path, so it must not cost more than the thing being measured. */
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
    /* Not open: a stray end, or an end whose begin was skipped by an unwinding
       exception. Record nothing — a phantom sample is worse than a missing one,
       because it would move the percentiles by a duration nobody measured. */
    if ((span_open_mask & ((uint32_t)1 << slot)) == 0) return;
    span_open_mask &= ~((uint32_t)1 << slot);
    /* Unsigned 32-bit difference: correct across a counter wrap so long as the
       span is shorter than the wrap period (17.9 s on the WROOM-32). */
    span_record(slot, span_now() - spans[slot].start);
}

void span_record(int slot, uint32_t us) {
    struct SpanSlot *s;
    if (!slot_ok(slot)) return;
    s = &spans[slot];
    /* n == 0 seeds min, so no UINT32_MAX initialiser is needed and a statically
       zeroed array is already correct — including after span_reset_all. */
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
