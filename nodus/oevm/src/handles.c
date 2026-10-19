/* M6 C-boundary handle table — see src/handles.h. Storage mirrors src/space.c's
   static-table idiom (fixed array, -D capacity, bare-frozen-string abort on full);
   marking mirrors space_gc_trace (iterate live slots, mark_object each). */
#include "handles.h"
#include <stdio.h>
#include <stdlib.h>


/* tuvm#13: `generation` advances on every free, so a reused slot issues handles that
   never compare equal to the ones outstanding before the reuse. */
struct HandleSlot { oop value; int live; unsigned generation; };
static struct HandleSlot handles[OEVM_HANDLE_TABLE_MAX];
/* tuvm#24: pin telemetry (see handles.h). */
static unsigned handles_live = 0;
static unsigned handles_high_water = 0;

/* Pack the slot's current generation with its index into one non-negative handle. */
static int handle_encode(int idx) {
    unsigned gen = handles[idx].generation & OEVM_HANDLE_GEN_MASK;
    return (int)((gen << OEVM_HANDLE_IDX_BITS) | (unsigned)idx);
}

int handle_new(oop o) {
    int i;
    for (i = 0; i < OEVM_HANDLE_TABLE_MAX; i++) {
        if (!handles[i].live) {
            handles[i].value = o;
            handles[i].live = 1;
            handles_live++;
            if (handles_live > handles_high_water) handles_high_water = handles_live;
            return handle_encode(i);
        }
    }
    fprintf(stderr, "handle table full\n");
    abort();
    return -1;   /* unreachable; silences -Wreturn-type */
}

/* Decode+validate a handle: negative is invalid; the index must be in range and live;
   the generation must match the slot's current one. Returns -1 on any mismatch. */
static int handle_resolve(int handle) {
    if (handle < 0) return -1;
    int idx = handle & OEVM_HANDLE_IDX_MASK;
    if (idx >= OEVM_HANDLE_TABLE_MAX) return -1;
    if (!handles[idx].live) return -1;
    unsigned gen = (unsigned)handle >> OEVM_HANDLE_IDX_BITS;
    if ((handles[idx].generation & OEVM_HANDLE_GEN_MASK) != gen) return -1;
    return idx;
}

oop handle_get(int handle) {
    int idx = handle_resolve(handle);
    return idx < 0 ? NULL : handles[idx].value;
}

void handle_free(int handle) {
    int idx = handle_resolve(handle);
    if (idx < 0) return;               /* out-of-range, already-free, or stale: no-op */
    handles[idx].live = 0;
    handles[idx].value = NULL;
    handles[idx].generation++;         /* invalidate every handle outstanding for this slot */
    handles_live--;
}

void handle_gc_trace(void) {
    int i;
    for (i = 0; i < OEVM_HANDLE_TABLE_MAX; i++)
        if (handles[i].live) mark_object(handles[i].value);
}

unsigned handle_live_count(void) { return handles_live; }
unsigned handle_high_water(void) { return handles_high_water; }
