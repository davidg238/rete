#ifndef OEVM_HANDLES_H
#define OEVM_HANDLES_H
/* M6 C-boundary handle table (G3 decision doc §M6): when C code (the ISR→channel
   pulse seam, timer callbacks, drivers) must hold a reference to a VM object across
   a boundary the GC cannot see (an ISR slot, a k_msgq item), it holds a small stable
   INDEX into this table, never a raw oop. Live slots are marked as GC roots every
   collection (handle_gc_trace, called from gc.c beside space_gc_trace), so the
   referenced object survives; and C never sees a heap address (strengthens §M4).
   Fixed capacity, -D-overridable (embedded-arena style); exhaustion is a Tier-2
   crash with the FROZEN string "handle table full". */
#include "oevm.h"

#ifndef OEVM_HANDLE_TABLE_MAX
#define OEVM_HANDLE_TABLE_MAX 32
#endif

/* tuvm#13 — a handle is NOT a bare slot index but a (generation, index) pair packed
   into one non-negative int (JNI-global-ref style). Low OEVM_HANDLE_IDX_BITS bits are
   the slot index; the remaining bits (below the sign bit, kept clear so -1 stays an
   invalid sentinel) are the slot's generation, bumped on every free. A handle enqueued
   for a slot that is freed and REUSED before the consumer resolves it therefore carries
   a stale generation and reads NULL, instead of silently resolving to the new occupant
   (the ABA window). The default 8 index bits cover up to 256 slots and leave 23
   generation bits; a slot must be reused 2^23 times before its generation wraps and the
   window theoretically reopens. Override with -DOEVM_HANDLE_IDX_BITS to raise the cap. */
#ifndef OEVM_HANDLE_IDX_BITS
#define OEVM_HANDLE_IDX_BITS 8
#endif
#define OEVM_HANDLE_IDX_MASK ((1 << OEVM_HANDLE_IDX_BITS) - 1)
#define OEVM_HANDLE_GEN_MASK ((unsigned)((1U << (31 - OEVM_HANDLE_IDX_BITS)) - 1))
#if OEVM_HANDLE_TABLE_MAX > (OEVM_HANDLE_IDX_MASK + 1)
#error "OEVM_HANDLE_TABLE_MAX exceeds OEVM_HANDLE_IDX_BITS capacity — raise OEVM_HANDLE_IDX_BITS"
#endif

/* Pin `o` and return a non-negative generation-tagged handle for a stable slot in
   [0, OEVM_HANDLE_TABLE_MAX). Aborts "handle table full" when every slot is live.
   Reuses a freed slot before growing; the reused slot's generation has advanced, so
   the new handle never compares equal to a stale one. Treat the value as opaque. */
int  handle_new(oop o);
/* Resolve a live, current handle to its oop; NULL for an out-of-range, freed, or
   stale (generation-mismatched) handle. */
oop  handle_get(int handle);
/* Release the pin named by `handle`; idempotent, ignores out-of-range/already-free/
   stale handles (a stale handle must never free the slot's current occupant). */
void handle_free(int handle);
/* Mark every live slot as a GC root (called from gc_collect's root scan). */
void handle_gc_trace(void);
/* tuvm#24 telemetry: currently-live pins, and the monotonic max ever observed.
   Maintained incrementally in handle_new/handle_free (no scan). Read by the
   sysStats prim (platform.c) for the sys/metrics record. */
unsigned handle_live_count(void);
unsigned handle_high_water(void);
#endif /* OEVM_HANDLES_H */
