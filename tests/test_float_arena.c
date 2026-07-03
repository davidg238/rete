/* Unit test for the float bump arena (src/gc.c, P0 Phase 1).
   Build:
     gcc -std=c99 -Isrc tests/test_float_arena.c \
         src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/solidified_image.c \
         -lm -o /tmp/tfa && /tmp/tfa
   The arena hands out struct Float cells by pointer-bump instead of malloc.
   We verify: distinct writable cells, vtable tagging, and correct behaviour
   across a slab boundary (the riskiest path). A fake vtable pointer suffices —
   float_alloc only stores it into base.vtable. */
#include "../src/oevm.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

extern oop float_alloc(oop vtable);

/* Must exceed the slab size so we cross at least one slab boundary. */
#define N 10000

int main(void) {
    int fake_vtable; /* opaque, distinct pointer */
    oop vt = (oop)&fake_vtable;

    static oop cells[N];

    /* Allocate N cells, tagging each with the vtable and a unique value. */
    for (int i = 0; i < N; i++) {
        oop c = float_alloc(vt);
        assert(c != NULL);
        assert(((struct Float *)c)->base.vtable == vt);
        ((struct Float *)c)->value = (double)i + 0.5;
        cells[i] = c;
    }

    /* All cells must be distinct (no aliasing across or within slabs). */
    for (int i = 1; i < N; i++) {
        assert(cells[i] != cells[i - 1]);
    }

    /* Values written before any slab boundary survive later allocations:
       the arena never moves or overwrites a live cell. */
    for (int i = 0; i < N; i++) {
        assert(((struct Float *)cells[i])->value == (double)i + 0.5);
        assert(((struct Float *)cells[i])->base.vtable == vt);
    }

    printf("test_float_arena: OK (%d cells across slab boundaries)\n", N);
    return 0;
}
