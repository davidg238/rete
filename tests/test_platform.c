/* Unit test for the real indexable representation + platform capability object
   (src/dispatch.c, std-lib step 1). Build:
     gcc -std=c99 -Isrc tests/test_platform.c \
         src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/kernel_boot.c \
         -lm -o /tmp/tp && /tmp/tp
   (link set = image-VM set minus oevm_image_main.c; this file supplies main). */
#include "../src/oevm.h"
#include "../src/span.h"          /* tuvm#61: span_reset_all for the prim assertions */
#include <stdio.h>
#include <string.h>
#include <assert.h>

extern oop  send(oop receiver, oop selector, int argc, oop *args);
extern void kernel_boot(void);
extern void heap_init(void);
extern int  is_truthy(oop);
extern oop  SymbolVtable;

/* Task 1 reaches the allocator directly; Task 2 replaces this with `array:`. */
extern oop  indexable_new(intptr_t n);

extern void gc_collect(void);
extern int  gc_is_live(oop ptr);
extern void mark_object(oop ptr);
extern void (*gc_additional_roots_hook)(void);

/* Root the outer indexable across a collection (mirrors tests/test_arena.c). */
static oop g_outer = NULL;
static void platform_roots(void) { mark_object(g_outer); }

/* Build a throwaway selector Symbol with the given name. send() classifies
   selectors by symbol_name() string, so a local Symbol works as long as its
   vtable is SymbolVtable. */
static oop sel(const char *name) {
    static struct Symbol pool[32];
    static int n = 0;
    struct Symbol *s = &pool[n++];
    s->base.vtable = SymbolVtable;
    s->name = name;
    s->length = strlen(name);
    return (oop)s;
}

int main(void) {
    heap_init();
    kernel_boot();

    /* Allocate a 3-element indexable directly. */
    oop arr = indexable_new(3);

    /* basicSize == 3 */
    oop sz = send(arr, sel("basicSize"), 0, NULL);
    assert(IS_SMI(sz) && UNTAG_SMI(sz) == 3);

    /* basicAt:put: then basicAt: round-trips (1-based). */
    oop putargs[2] = { TAG_SMI(1), TAG_SMI(42) };
    oop stored = send(arr, sel("basicAt:put:"), 2, putargs);
    assert(IS_SMI(stored) && UNTAG_SMI(stored) == 42);

    oop idx1 = TAG_SMI(1);
    oop got = send(arr, sel("basicAt:"), 1, &idx1);
    assert(IS_SMI(got) && UNTAG_SMI(got) == 42);

    /* fresh slots are nil (NULL) */
    oop idx2 = TAG_SMI(2);
    oop got2 = send(arr, sel("basicAt:"), 1, &idx2);
    assert(got2 == NULL);

    /* --- Task 2: platform capability object --- */
    oop pf = platform_object();
    assert(pf != NULL);

    /* array: allocates an indexable equivalent to indexable_new */
    oop three = TAG_SMI(3);
    oop arr2 = send(pf, sel("array:"), 1, &three);
    oop sz2 = send(arr2, sel("basicSize"), 0, NULL);
    assert(IS_SMI(sz2) && UNTAG_SMI(sz2) == 3);

    /* object:identicalTo: — true for same oop, false for different */
    oop sameargs[2] = { arr2, arr2 };
    oop t = send(pf, sel("object:identicalTo:"), 2, sameargs);
    assert(is_truthy(t));
    oop diffargs[2] = { arr2, three };
    oop f = send(pf, sel("object:identicalTo:"), 2, diffargs);
    assert(!is_truthy(f));

    /* objHashcode: — an SMI hashes to itself; identical oops hash equal */
    oop h1 = send(pf, sel("objHashcode:"), 1, &three);
    assert(IS_SMI(h1) && UNTAG_SMI(h1) == 3);
    oop ha[1] = { arr2 };
    oop hb[1] = { arr2 };
    oop ah = send(pf, sel("objHashcode:"), 1, ha);
    oop bh = send(pf, sel("objHashcode:"), 1, hb);
    assert(ah == bh);

    /* print: / printNewline: — exercise the path (output asserted by harness) */
    oop msg = TAG_SMI(7);
    send(pf, sel("print:"), 1, &msg);
    send(pf, sel("printNewline:"), 0, NULL);

    /* --- Task 3: GC correctness --- */
    gc_additional_roots_hook = platform_roots;

    /* Outer indexable holds an inner indexable in slot 1. Only the outer is
       rooted; the inner must survive because the outer's slot is traced. */
    oop two = TAG_SMI(2);
    g_outer = send(pf, sel("array:"), 1, &two);
    oop one2 = TAG_SMI(1);
    oop inner = send(pf, sel("array:"), 1, &one2);
    oop put_inner[2] = { TAG_SMI(1), inner };
    send(g_outer, sel("basicAt:put:"), 2, put_inner);

    gc_collect();

    /* inner survived (reached only through the outer's element slot) */
    assert(gc_is_live(inner));
    /* outer survived and its length (a tagged SMI) is intact */
    oop outer_sz = send(g_outer, sel("basicSize"), 0, NULL);
    assert(IS_SMI(outer_sz) && UNTAG_SMI(outer_sz) == 2);
    /* the stored reference is still the same inner object */
    oop idx1b = TAG_SMI(1);
    oop fetched = send(g_outer, sel("basicAt:"), 1, &idx1b);
    assert(fetched == inner);

    gc_additional_roots_hook = NULL;

    /* --- tuvm#61: the span prims (118-121) through the FULL send path.
       Deliberately routed via send()+sel() rather than platform_dispatch(): that
       exercises selector_prim_id's name lookup, so if PRIM_NAMES and the enum ever
       drift apart these assertions fail rather than silently calling the wrong
       prim. src/span.c's own arithmetic is covered by tests/test_span.c; what is
       new here is the SMI tag/untag across the capability boundary. */
    {
        oop rec_args[2];
        oop slot2 = TAG_SMI(2);
        oop stats, n, minUs, maxUs, sumLo, sumHi, b2, b0;

        span_reset_all();

        rec_args[0] = TAG_SMI(2);
        rec_args[1] = TAG_SMI(1500);        /* 1.5 ms -> bucket 2 (< 10 ms) */
        send(pf, sel("spanRecord:us:"), 2, rec_args);

        stats = send(pf, sel("spanStats:"), 1, &slot2);
        n     = send(stats, sel("basicAt:"), 1, (oop[]){ TAG_SMI(1) });
        minUs = send(stats, sel("basicAt:"), 1, (oop[]){ TAG_SMI(2) });
        maxUs = send(stats, sel("basicAt:"), 1, (oop[]){ TAG_SMI(3) });
        sumLo = send(stats, sel("basicAt:"), 1, (oop[]){ TAG_SMI(4) });
        sumHi = send(stats, sel("basicAt:"), 1, (oop[]){ TAG_SMI(5) });
        b0    = send(stats, sel("basicAt:"), 1, (oop[]){ TAG_SMI(6) });
        b2    = send(stats, sel("basicAt:"), 1, (oop[]){ TAG_SMI(8) });

        assert(UNTAG_SMI(n)     == 1    && "n");
        assert(UNTAG_SMI(minUs) == 1500 && "minUs");
        assert(UNTAG_SMI(maxUs) == 1500 && "maxUs");
        assert(UNTAG_SMI(sumLo) == 1500 && "sumUsLo");
        assert(UNTAG_SMI(sumHi) == 0    && "sumUsHi");
        assert(UNTAG_SMI(b2)    == 1    && "the 1-10 ms decade got the sample");
        assert(UNTAG_SMI(b0)    == 0    && "the <100 us decade did not");

        /* begin/end reaches span.c through the seam. The duration is not asserted
           here — under the default virtual clock it is zero, and asserting a
           duration is test_span's job, where the clock is controllable. */
        {
            oop slot3 = TAG_SMI(3);
            send(pf, sel("spanBegin:"), 1, &slot3);
            send(pf, sel("spanEnd:"), 1, &slot3);
            stats = send(pf, sel("spanStats:"), 1, &slot3);
            n = send(stats, sel("basicAt:"), 1, (oop[]){ TAG_SMI(1) });
            assert(UNTAG_SMI(n) == 1
                   && "begin/end through the prim seam recorded one sample");
        }

        printf("test_platform: span prims OK\n");
    }

    printf("TASK2 OK\n");
    return 0;
}
