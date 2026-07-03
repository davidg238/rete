/* Unit test for free-block coalescing + cross-bin reuse in the arena allocator
   (src/gc.c). Regression for the 32-bit/embedded fragmentation OOM found on the
   nRF52840 running Perfstex arrayIterate: GC reclaimed ~57% of the arena, but it
   sat as small free blocks of one size class while the pending request was a
   DIFFERENT size class — and the exact-bin-only allocator could not reuse it,
   OOMing on top of a large *contiguous* run of dead blocks.

   Build (small heap so exhaustion is fast + deterministic). Use the standard
   C-unit-test link set (solidified_image.c, NOT kernel_boot.c — they both define
   Vtable_* and would clash):
     gcc -std=c99 -Isrc -DOEVM_HEAP_SIZE=8192 tests/test_coalesce.c \
         src/vm.c src/gc.c src/dispatch.c src/platform.c src/serial.c src/udp.c \
         src/mirror.c src/backend/posix/platform_posix.c \
         src/backend/posix/serial_posix.c src/backend/posix/udp_posix.c \
         src/oimg_loader.c src/solidified_image.c -lm -o /tmp/tc && /tmp/tc
   Also build with -m32 to cover the word size that originally failed.

   Verifies:
     1. coalescing: a contiguous run of freed small blocks is reusable by a
        LARGER size-class request that fits nowhere as a single freed block but
        fits within the coalesced run (this OOMed before the fix);
     2. split remainder: after carving the larger block, the leftover is itself
        reusable by a subsequent smaller allocation. */
#include "../src/oevm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <setjmp.h>

extern oop  gc_allocate(oop vtable, size_t size);
extern void gc_collect(void);
extern void heap_init(void);
extern int  gc_is_live(oop);
extern oop (*oevm_oom_handler)(size_t);
extern void (*gc_additional_roots_hook)(void);
extern void mark_object(oop);

/* Roots: the test pins a fill of blocks via this array so they truly exhaust the
   bump region (an unrooted fill would be reclaimed by gc_allocate's auto-collect
   and never exhaust). NULL slots are skipped → those blocks become collectable. */
#define MAX_BLOCKS 4096
static oop g_blocks[MAX_BLOCKS];
static int g_nblocks = 0;
static void roots_hook(void) {
    for (int i = 0; i < g_nblocks; i++)
        if (g_blocks[i] != NULL) mark_object(g_blocks[i]);
}

static jmp_buf oom_jmp;
static volatile int oom_fired = 0;
static oop test_oom(size_t size) { (void)size; oom_fired = 1; longjmp(oom_jmp, 1); }

#define A_SZ (sizeof(struct Object) + 2 * sizeof(oop))   /* small size class      */
#define B_SZ (sizeof(struct Object) + 8 * sizeof(oop))   /* larger size class     */

int main(void) {
    heap_init();
    oevm_oom_handler = test_oom;
    gc_additional_roots_hook = roots_hook;

    /* 1. Fill the arena with rooted, physically-adjacent small (A) blocks until
          the bump pointer is exhausted. Bump allocation is sequential, so
          g_blocks[k] and g_blocks[k+1] are adjacent in memory. */
    oom_fired = 0;
    if (setjmp(oom_jmp) == 0) {
        for (;;) {
            oop o = gc_allocate(NULL, A_SZ);     /* rooted via g_blocks → survives */
            g_blocks[g_nblocks++] = o;
            assert(g_nblocks < MAX_BLOCKS);
        }
    }
    assert(oom_fired && "fill should exhaust the small arena");
    assert(g_nblocks > 8 && "need a usable contiguous run to free");

    /* 2. Free a contiguous run in the middle (unroot it), then collect. The run
          is adjacent in memory → must coalesce into one large free block. */
    int lo = 2, hi = g_nblocks - 2;            /* keep ends live → interior run */
    oop run_lo_addr = g_blocks[lo];
    oop run_hi_addr = g_blocks[hi];            /* first still-live block above run */
    for (int i = lo; i < hi; i++) g_blocks[i] = NULL;
    gc_collect();

    /* 3. Request a LARGER size class (B). No bump room remains and no single
          freed A block is big enough; only the coalesced run can serve it.
          Before the fix this OOMs (longjmp → else branch). */
    oom_fired = 0;
    oop big = NULL, more = NULL;
    if (setjmp(oom_jmp) == 0) {
        big = gc_allocate(NULL, B_SZ);
        assert(big != NULL);
        /* served from inside the freed contiguous run */
        assert((char *)big >= (char *)run_lo_addr && (char *)big < (char *)run_hi_addr
               && "B must be carved from the coalesced freed run");
        /* 4. The split remainder is reusable by a later smaller allocation. */
        more = gc_allocate(NULL, A_SZ);
        assert(more != NULL);
        assert((char *)more >= (char *)run_lo_addr && (char *)more < (char *)run_hi_addr
               && "split remainder must remain reusable");
    } else {
        assert(0 && "OOM: coalescing/cross-bin reuse failed on a contiguous free run");
    }

    gc_additional_roots_hook = NULL;
    printf("test_coalesce: coalescing + cross-bin split reuse OK (%d-block run)\n", hi - lo);
    return 0;
}
