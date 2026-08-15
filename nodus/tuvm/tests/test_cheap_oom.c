/* rete#190 sub-project A: an allocation failure must be counted and survived,
   never dereferenced. Forces failure with --wrap=malloc, the test_float_oom
   precedent. Link set: Model A. */
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "oevm.h"

static int fail_allocs = 0;
void *__real_malloc(size_t n);
void *__wrap_malloc(size_t n) { return fail_allocs ? NULL : __real_malloc(n); }

int main(void) {
    struct OevmCHeapStats a = oevm_cheap_stats();

    fail_allocs = 1;
    void *p = oevm_malloc(64);
    fail_allocs = 0;

    assert(p == NULL);
    struct OevmCHeapStats b = oevm_cheap_stats();
    assert(b.fails == a.fails + 1);
    assert(b.allocs == a.allocs);        /* a failure is not an allocation */
    assert(b.out_bytes == a.out_bytes);  /* and moves nothing outstanding */

    printf("test_cheap_oom OK\n");
    return 0;
}
