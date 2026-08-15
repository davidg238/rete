/* Copyright (c) 2026 Ekorau LLC */
/* #165: the runtime gate is a shipping control, not a spike seam. Counting must
   be OFF until mprof_start(), and mprof_start() must zero a table left over from
   a previous window — a window that inherits the last one's counts reports a
   sum that never happened. */
#include <assert.h>
#include <stdio.h>
#include "oevm.h"

extern void mprof_bump_for_test(const void *m);   /* test-only shim, Step 3 */
extern unsigned mprof_used_for_test(void);
extern unsigned long mprof_bytes_for_test(unsigned heap);
extern unsigned long mprof_noframe_bytes_for_test(unsigned heap);

int main(void) {
    static char m1, m2;

    /* 1. Counting is off by default. */
    assert(!mprof_is_running());
    mprof_bump_for_test(&m1);
    assert(mprof_used_for_test() == 0);

    /* 2. start() enables it. */
    mprof_start();
    assert(mprof_is_running());
    mprof_bump_for_test(&m1);
    mprof_bump_for_test(&m2);
    assert(mprof_used_for_test() == 2);

    /* 3. stop() disables it, and further activations are not counted. */
    mprof_stop();
    assert(!mprof_is_running());
    mprof_bump_for_test(&m1);
    assert(mprof_used_for_test() == 2);

    /* 4. A second start() ZEROES the table rather than accumulating. */
    mprof_start();
    assert(mprof_used_for_test() == 0);
    mprof_bump_for_test(&m1);
    assert(mprof_used_for_test() == 1);
    mprof_stop();

    /* 5. Allocation attribution is OFF outside a window, like activations. */
    mprof_start();
    assert(mprof_bytes_for_test(MPROF_HEAP_ARENA) == 0);
    mprof_stop();
    mprof_alloc(MPROF_HEAP_ARENA, 64);
    assert(mprof_bytes_for_test(MPROF_HEAP_ARENA) == 0);

    /* 6. With no current frame there is nothing to attribute to, so the bytes
          land in the no-frame bucket rather than being dropped or crashing.
          This runs with current_frame == NULL, which is exactly the boot and
          prim-context case the design's risk section names. */
    mprof_start();
    mprof_alloc(MPROF_HEAP_ARENA, 64);
    mprof_alloc(MPROF_HEAP_CHEAP, 32);
    assert(mprof_noframe_bytes_for_test(MPROF_HEAP_ARENA) == 64);
    assert(mprof_noframe_bytes_for_test(MPROF_HEAP_CHEAP) == 32);
    assert(mprof_bytes_for_test(MPROF_HEAP_ARENA) == 0);

    /* 7. The two heaps do not bleed into each other. */
    assert(mprof_noframe_bytes_for_test(MPROF_HEAP_CHEAP) == 32);

    /* 8. start() zeroes the byte columns too — a window that inherited the
          previous one's bytes would report a sum that never happened. */
    mprof_start();
    assert(mprof_noframe_bytes_for_test(MPROF_HEAP_ARENA) == 0);
    assert(mprof_noframe_bytes_for_test(MPROF_HEAP_CHEAP) == 0);
    mprof_stop();

    printf("test_mprof_gate: PASSED\n");
    return 0;
}
