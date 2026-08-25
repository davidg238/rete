/* Copyright (c) 2026 Ekorau LLC */
/* Allocation attribution reaches the table from BOTH heaps. This test runs with
   no VM frame, so every byte must land in the no-frame bucket -- which is the
   point: it proves the hook fires, and that a frameless allocation is COUNTED
   rather than dropped or dereferenced. Per-method attribution needs a running
   VM and is proven by the target run, not here; standing up a real frame in a C
   unit test would test the harness rather than the hook.

   Design: docs/design/2026-08-12-allocation-attribution-design.md, D2/D3. */
#include <assert.h>
#include <stdio.h>
#include "oevm.h"

extern unsigned long mprof_noframe_bytes_for_test(unsigned heap);
/* gc_allocate is not in oevm.h; vm.c:49 declares it the same way. Without this
   the implicit declaration returns int, which TRUNCATES the pointer on a 64-bit
   host -- it happens to pass, which is the worst kind of passing. */
extern oop gc_allocate(oop vtable, size_t size);

int main(void) {
    void *p;
    oop o;

    mprof_start();

    /* C heap: oevm_malloc's PAYLOAD size is what gets attributed, not the
       payload plus the union CHeapBlock header. */
    p = oevm_malloc(100);
    assert(p != NULL);
    assert(mprof_noframe_bytes_for_test(MPROF_HEAP_CHEAP) == 100);

    /* realloc attributes its NEW size as a fresh allocation. D2 is cumulative:
       the column means "bytes this method caused to be allocated", never a
       live-set delta. */
    p = oevm_realloc(p, 250);
    assert(p != NULL);
    assert(mprof_noframe_bytes_for_test(MPROF_HEAP_CHEAP) == 350);

    /* A free attributes nothing -- cumulative, not outstanding. This is the
       limitation D2 accepts, pinned here so a later change to it is loud. */
    oevm_free(p);
    assert(mprof_noframe_bytes_for_test(MPROF_HEAP_CHEAP) == 350);

    /* Arena: a gc_allocate moves the arena column and NOT the C-heap one. The
       assertion is `> 0` rather than an exact figure because heap_stats_on_alloc
       receives sizeof(struct HeapHeader) + ROUNDED payload -- an internal detail
       this test must not pin. */
    assert(mprof_noframe_bytes_for_test(MPROF_HEAP_ARENA) == 0);
    o = gc_allocate(NULL, 64);
    assert(o != NULL);
    assert(mprof_noframe_bytes_for_test(MPROF_HEAP_ARENA) > 0);
    assert(mprof_noframe_bytes_for_test(MPROF_HEAP_CHEAP) == 350);

    mprof_stop();
    printf("test_mprof_alloc: PASSED\n");
    return 0;
}
