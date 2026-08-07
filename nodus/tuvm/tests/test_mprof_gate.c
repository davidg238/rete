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

    printf("test_mprof_gate: PASSED\n");
    return 0;
}
