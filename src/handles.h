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

/* Pin `o` and return a stable index in [0, OEVM_HANDLE_TABLE_MAX). Aborts
   "handle table full" when every slot is live. Reuses a freed slot before growing. */
int  handle_new(oop o);
/* Resolve a live index to its oop; NULL for an out-of-range or freed index. */
oop  handle_get(int idx);
/* Release the pin at `idx`; idempotent, ignores out-of-range/already-free. */
void handle_free(int idx);
/* Mark every live slot as a GC root (called from gc_collect's root scan). */
void handle_gc_trace(void);
/* tuvm#24 telemetry: currently-live pins, and the monotonic max ever observed.
   Maintained incrementally in handle_new/handle_free (no scan). Read by the
   sysStats prim (platform.c) for the sys/metrics record. */
unsigned handle_live_count(void);
unsigned handle_high_water(void);
#endif /* OEVM_HANDLES_H */
