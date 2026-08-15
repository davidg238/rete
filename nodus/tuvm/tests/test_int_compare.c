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
extern oop send(oop receiver, oop selector, int argc, oop *args);
extern int is_truthy(oop);

/* Each call gets a fresh selector pointer: the primitive cache keys on symbol
   pointer identity (real selectors are interned, one stable pointer per name),
   so a reused address would mis-cache one operator's id for another. */
static oop cmp(intptr_t a, const char *sel, intptr_t b) {
    size_t n = strlen(sel);
    struct Symbol *s = malloc(sizeof *s + n + 1);
    s->base.vtable = SymbolVtable;
    s->length = n;
    memcpy(s->name, sel, n + 1);
    oop arg = TAG_SMI(b);
    return send(TAG_SMI(a), (oop)s, 1, &arg);
}

int main(void) {
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
