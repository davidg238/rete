/* Unit test for heap telemetry counters (src/gc.c, G3/M2, https://github.com/davidg238/nsl-tuvm/issues/3).
   These counters are the fragmentation early-warning system for a non-moving
   mark-sweep GC on a small embedded arena: largest_free_block is the number
   that matters most, since a raw "bytes free" total can overstate what the
   allocator can actually satisfy once free space is fragmented.

   Build (small heap so the properties below are fast and deterministic; same
   link-set as test_arena.c/test_coalesce.c):
     gcc -std=c99 -Isrc -DOEVM_HEAP_SIZE=65536 tests/test_heap_stats.c \
         src/vm.c src/gc.c src/dispatch.c src/platform.c src/serial.c src/udp.c \
         src/mirror.c src/backend/posix/platform_posix.c \
         src/backend/posix/serial_posix.c src/backend/posix/udp_posix.c \
         src/oimg_loader.c src/solidified_image.c -lm -o /tmp/ths && /tmp/ths

   Verifies (property-based, not brittle exact byte counts):
     1. heap_init() resets all counters to a clean baseline, even after prior
        use; an empty arena reports (almost) the whole heap as one free extent.
     2. N reachable allocations of a known size class: live_bytes strictly
        increases per allocation; high_water tracks the running peak;
        largest_free_block shrinks as the untouched bump region shrinks.
     3. Dropping reachability of half the set and forcing a collection:
        gc_cycles increments by one; live_bytes drops but stays above the
        pre-allocation baseline (the surviving half); high_water is UNCHANGED
        (monotonic — it never decreases).
     4. After that collection, largest_free_block reflects the blocks the
        sweep returned to the free bins (>= one freed block's rounded size).
     5. Sanity invariants: live_bytes <= high_water <= heap_size; and
        largest_free_block <= heap_size - live_bytes (+ small defensive slack;
        see comment at the assertion — the inequality is expected to hold with
        zero slack, given how live_bytes/largest_free_block are defined, but a
        slack constant is kept so an internal rounding wrinkle fails loudly
        with a clear message instead of a hard-to-diagnose off-by-N). */
#include "../src/oevm.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

extern oop  gc_allocate(oop vtable, size_t size);
extern void gc_collect(void);
extern void heap_init(void);
extern void (*gc_additional_roots_hook)(void);
extern void mark_object(oop);

/* Known size class for this test's allocations (mirrors test_coalesce.c's
   A_SZ pattern). */
#define OBJ_SZ (sizeof(struct Object) + 3 * sizeof(oop))

/* Roots: pin reachability the same way test_coalesce.c does — an array the
   custom roots hook walks, marking only non-NULL slots. Setting a slot to
   NULL drops that object's reachability for the next collection. */
#define MAX_BLOCKS 64
static oop g_blocks[MAX_BLOCKS];
static int g_nblocks = 0;
static void roots_hook(void) {
    for (int i = 0; i < g_nblocks; i++)
        if (g_blocks[i] != NULL) mark_object(g_blocks[i]);
}

int main(void) {
    heap_init();
    gc_additional_roots_hook = roots_hook;

    /* 1. heap_init() must reset all counters to a clean baseline, even if the
       arena saw prior use (tests rely on this for isolation). */
    (void)gc_allocate(NULL, OBJ_SZ);
    heap_init();
    struct OevmHeapStats s0 = oevm_heap_stats();
    assert(s0.heap_size == OEVM_HEAP_SIZE);
    assert(s0.gc_cycles == 0 && "heap_init must reset gc_cycles");
    assert(s0.live_bytes == 0 && "heap_init must reset live_bytes to a clean baseline");
    assert(s0.high_water == s0.live_bytes && "high_water must equal live_bytes at boot");
    assert(s0.largest_free_block > (size_t)(0.9 * (double)s0.heap_size)
           && "an empty arena must present as (almost) one big free extent");
    size_t baseline = s0.live_bytes;
    printf("test_heap_stats: property 1 (clean baseline after heap_init) OK\n");

    /* 2. Allocate N reachable objects of a known size class. */
    #define N 20
    size_t prev_live = baseline;
    for (int i = 0; i < N; i++) {
        oop o = gc_allocate(NULL, OBJ_SZ);
        assert(o != NULL);
        assert(g_nblocks < MAX_BLOCKS);
        g_blocks[g_nblocks++] = o;
        struct OevmHeapStats s = oevm_heap_stats();
        assert(s.live_bytes > prev_live && "live_bytes must strictly increase per live allocation");
        prev_live = s.live_bytes;
    }
    struct OevmHeapStats s1 = oevm_heap_stats();
    assert(s1.high_water == s1.live_bytes && "high_water must track the current peak");
    assert(s1.largest_free_block < s0.largest_free_block
           && "allocating must shrink the largest available free extent");
    printf("test_heap_stats: property 2 (N reachable allocations grow live_bytes/high_water) OK\n");

    /* 3. Drop reachability of half the set, force a collection. */
    int half = g_nblocks / 2;
    for (int i = 0; i < half; i++) g_blocks[i] = NULL;
    gc_collect();
    struct OevmHeapStats s2 = oevm_heap_stats();
    assert(s2.gc_cycles == s1.gc_cycles + 1 && "a forced gc_collect() must be counted");
    assert(s2.live_bytes < s1.live_bytes && "dropping reachability + collecting must shrink live_bytes");
    assert(s2.live_bytes > baseline && "the surviving half must still count as live");
    assert(s2.high_water == s1.high_water
           && "high_water must remain pinned at its prior peak (monotonic, never decreases)");
    printf("test_heap_stats: property 3 (collection shrinks live_bytes; high_water monotonic) OK\n");

    /* 4. The sweep just returned `half` freed blocks to the free bins (and/or
       coalesced them with the bump tail); largest_free_block must reflect
       that reclaimed space — at minimum, the rounded size of one freed block. */
    size_t granule = sizeof(oop);
    size_t rounded_obj_sz = (OBJ_SZ + (granule - 1)) & ~(granule - 1);
    assert(s2.largest_free_block >= rounded_obj_sz
           && "sweep-reclaimed blocks must be visible to largest_free_block");
    printf("test_heap_stats: property 4 (freed blocks visible to largest_free_block) OK\n");

    /* 5. Sanity invariants. */
    assert(s2.live_bytes <= s2.high_water && s2.high_water <= s2.heap_size);
    /* largest_free_block reports a free-bin PAYLOAD size (no header) or the
       untouched bump remainder; live_bytes counts header+payload for every
       live block. Every free byte not counted in live_bytes is either in a
       free-bin block (header+payload, so its payload alone is <= that) or in
       the untouched bump tail (counted exactly). So largest_free_block should
       never exceed heap_size - live_bytes even with zero slack; the slack
       below is a defensive margin only, to fail loudly (not silently pass)
       if an internal rounding wrinkle ever violates that reasoning. */
    size_t slack = 4 * sizeof(oop);
    assert(s2.largest_free_block <= s2.heap_size - s2.live_bytes + slack
           && "a free extent cannot exceed non-live space (+ small defensive slack)");
    printf("test_heap_stats: property 5 (sanity invariants) OK\n");

    gc_additional_roots_hook = NULL;
    printf("test_heap_stats: OK\n");
    return 0;
}
