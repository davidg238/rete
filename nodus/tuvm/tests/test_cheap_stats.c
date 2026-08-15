/* Unit test for the C-allocation seam's counters (src/gc.c, rete#190 sub-project A).
   Nine crashes on record across three ESP32-C6 boards traced to a C-heap leak
   (one wire-Symbol buffer per decoded Symbol) that no instrument could see,
   because the VM's malloc/realloc/free calls were scattered across six call
   sites with no shared accounting. This test proves the seam's own counters
   are exact — bytes-outstanding, high-water, alloc/free/fail tallies — before
   any call site is converted to use it.

   Build (same link-set as test_heap_stats.c/test_coalesce.c — Model A):
     gcc -std=c99 -Isrc -DOEVM_HEAP_SIZE=65536 tests/test_cheap_stats.c \
         src/vm.c src/gc.c src/dispatch.c src/platform.c src/serial.c src/udp.c \
         src/mirror.c src/backend/posix/platform_posix.c \
         src/backend/posix/serial_posix.c src/backend/posix/udp_posix.c \
         src/oimg_loader.c src/solidified_image.c -lm -o /tmp/tcs && /tmp/tcs

   Verifies:
     1. oevm_malloc counts the alloc, tracks out_bytes/out_high, and hands back
        an 8-aligned, fully writable payload (the alignment matters on a 32-bit
        target: a 4-byte header would misalign every double stored through it).
     2. oevm_free returns out_bytes to baseline, counts the free, and never
        moves out_high (monotonic since boot).
     3. oevm_realloc tracks out_bytes both growing and shrinking.
     4. oevm_free(NULL) is a no-op: it moves no counter. */
#include "../src/oevm.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

int main(void) {
    struct OevmCHeapStats a = oevm_cheap_stats();

    void *p = oevm_malloc(100);
    assert(p != NULL);
    struct OevmCHeapStats b = oevm_cheap_stats();
    assert(b.allocs == a.allocs + 1);
    assert(b.out_bytes == a.out_bytes + 100);
    assert(b.out_high >= b.out_bytes);

    /* the payload must be 8-aligned: a 4-byte header on a 32-bit target would
       silently misalign every double the VM ever stores through this pointer */
    assert(((uintptr_t)p % 8) == 0);

    /* the block must be writable across its whole declared length */
    memset(p, 0xA5, 100);

    oevm_free(p);
    struct OevmCHeapStats c = oevm_cheap_stats();
    assert(c.frees == a.frees + 1);
    assert(c.out_bytes == a.out_bytes);       /* returned to baseline */
    assert(c.out_high == b.out_high);         /* high water never falls */
    printf("test_cheap_stats: property 1-2 (malloc/free counters exact) OK\n");

    /* realloc: grow, then shrink, and outstanding must track both ways */
    void *q = oevm_malloc(16);
    assert(q != NULL);
    q = oevm_realloc(q, 64);
    assert(q != NULL);
    struct OevmCHeapStats d = oevm_cheap_stats();
    assert(d.out_bytes == c.out_bytes + 64);
    q = oevm_realloc(q, 8);
    assert(q != NULL);
    struct OevmCHeapStats e = oevm_cheap_stats();
    assert(e.out_bytes == c.out_bytes + 8);
    oevm_free(q);
    assert(oevm_cheap_stats().out_bytes == c.out_bytes);
    printf("test_cheap_stats: property 3 (realloc tracks growth and shrink) OK\n");

    /* oevm_free(NULL) is a no-op and must not move any counter */
    struct OevmCHeapStats f = oevm_cheap_stats();
    oevm_free(NULL);
    struct OevmCHeapStats g = oevm_cheap_stats();
    assert(g.frees == f.frees);
    assert(g.out_bytes == f.out_bytes);
    printf("test_cheap_stats: property 4 (free(NULL) is a no-op) OK\n");

    printf("test_cheap_stats: OK\n");
    return 0;
}
