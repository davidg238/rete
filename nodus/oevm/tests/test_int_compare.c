/* oevm/tests/test_int_compare.c — root-cause test for the
   integer-comparison-via-double defect.

   PRIM_lt/gt/le/ge converted integer operands to `double` before comparing.
   That loses precision for integers above 2^53 (they collapse to the same
   double) and, on the nRF52840's single-precision FPU (soft-float `double`),
   produced a wrong result for `i < 1000` that left an inlined `whileTrue:`
   loop spinning forever. Integer comparisons must be exact and never round-trip
   through double.

   Build (Model-B link: kernel_boot.c present, solidified_image.c absent):
     cd ~/workspace/oevm && gcc -std=c99 -Isrc tests/test_int_compare.c \
       src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/kernel_boot.c \
       -lm -o /tmp/ticmp && /tmp/ticmp */
#include "../src/oevm.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern oop SymbolVtable;
extern void heap_init(void);
extern oop send(oop receiver, oop selector, int argc, oop *args);
extern int is_truthy(oop);

/* rete#226/#275: symbol_new is THE Symbol constructor — it bakes prim_id from the
   name, and the send path reads that field. A hand-built Symbol carries an
   uninitialised prim_id and is never recognised as a primitive, which is how this
   test aborted with "Message '<' not understood". Never open-code a Symbol.
   (This comment used to justify the open-coding by the pointer-keyed primitive
   CACHE. That cache was deleted by #226; classification is a field now.) */
static oop cmp(intptr_t a, const char *sel, intptr_t b) {
    oop s = symbol_new(sel, strlen(sel));
    assert(s != NULL);
    oop arg = TAG_SMI(b);
    return send(TAG_SMI(a), s, 1, &arg);
}

int main(void) {
    heap_init();
    kernel_boot();

    /* Small values: still correct. The `5 < 5` case is exactly the loop-exit
       condition that hung on device — it MUST be false. */
    assert(is_truthy(cmp(3, "<", 5)));
    assert(!is_truthy(cmp(5, "<", 5)));
    assert(!is_truthy(cmp(7, "<", 5)));
    assert(is_truthy(cmp(5, ">", 3)));
    assert(is_truthy(cmp(5, "<=", 5)));
    assert(is_truthy(cmp(5, ">=", 5)));
    assert(!is_truthy(cmp(5, ">=", 6)));

    /* Negative / mixed signs. */
    assert(is_truthy(cmp(-5, "<", 3)));
    assert(is_truthy(cmp(-5, "<", -3)));
    assert(!is_truthy(cmp(-3, "<", -5)));

    /* Large integers that share a double representation: must NOT collapse.
       (intptr_t is 64-bit on the test host, so 2^53 fits a 63-bit SMI payload.)
       Under the old double round-trip, (double)2^53 == (double)(2^53+1), so
       `2^53 < 2^53+1` wrongly returned false. */
    intptr_t big = ((intptr_t)1 << 53);
    assert(is_truthy(cmp(big, "<", big + 1)));
    assert(!is_truthy(cmp(big + 1, "<", big)));
    assert(is_truthy(cmp(big + 1, ">", big)));
    assert(!is_truthy(cmp(big, ">", big + 1)));

    printf("test_int_compare: OK\n");
    return 0;
}
