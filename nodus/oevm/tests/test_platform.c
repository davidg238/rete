/* Unit test for the real indexable representation + platform capability object
   (src/dispatch.c, std-lib step 1). Build:
     gcc -std=c99 -Isrc tests/test_platform.c \
         src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/kernel_boot.c \
         -lm -o /tmp/tp && /tmp/tp
   (link set = image-VM set minus oevm_image_main.c; this file supplies main). */
#include "../src/oevm.h"
#include "../src/span.h"          /* tuvm#61: span_reset_all for the prim assertions */
#include "../src/space.h"         /* porta space-sync: space_make_string_pub for path args */
#include <stdio.h>
#include <stdlib.h>
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

/* Root the outer indexable across a collection (mirrors tests/test_arena.c),
   AND every interned selector — see sel() below. */
static oop g_outer = NULL;
#define SEL_MAX 32
static oop g_sels[SEL_MAX];
static int g_nsels = 0;
static void platform_roots(void) {
    mark_object(g_outer);
    for (int i = 0; i < g_nsels; i++) mark_object(g_sels[i]);
}

/* Build (and INTERN) a selector Symbol.

   rete#226/#275: symbol_new is THE Symbol constructor — it bakes prim_id from the
   name, and the send path reads that field rather than looking the name up. A
   hand-built Symbol carries an uninitialised prim_id and is never recognised as a
   primitive, which is how this test aborted with "Message 'basicSize' not
   understood". Never open-code a Symbol.

   Two consequences of using the real constructor, both handled here:
     - it allocates ON THE GC HEAP (the old malloc'd Symbols were off-heap and so
       could never be swept). The collector has no C-stack scan — roots come from
       gc_additional_roots_hook — so every selector is kept in g_sels and marked
       by platform_roots, or a collection inside a send could sweep the selector
       being sent. mark() ignores NULL and off-heap pointers, so this is safe
       before the first intern.
     - interning is now CORRECT rather than a hazard. The old comment here kept
       one allocation per call site because the pointer-keyed primitive CACHE
       would otherwise mis-key one operator's id onto another. #226 deleted that
       cache; real selectors are interned, one stable pointer per name, and this
       now matches them. */
static oop sel(const char *name) {
    for (int i = 0; i < g_nsels; i++)
        if (strcmp(((struct Symbol *)g_sels[i])->name, name) == 0) return g_sels[i];
    assert(g_nsels < SEL_MAX);
    oop s = symbol_new(name, strlen(name));
    assert(s != NULL);
    g_sels[g_nsels++] = s;
    return s;
}

int main(void) {
    heap_init();
    kernel_boot();
    /* rete#275: installed for the WHOLE test now, not just the Task 3 GC block —
       selectors are heap objects and must be rooted at every send. */
    gc_additional_roots_hook = platform_roots;

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

    /* --- porta space-sync (plan 2): spaceSeq / spaceStamp /
       spaceChangedSince:under:max:, ids 138-140. Driven through send()+sel()
       rather than the C functions — tests/test_space_seq.c already covers those
       — so what this proves is the OTHER half: the PRIM_NAMES wiring, the SMI
       tagging, and the shape of the answer. A miscount in PRIM_NAMES misroutes
       every platform prim after the insertion point, and this is where that
       shows up. --- */
    {
        oop path   = space_make_string_pub("obs/one", 7);
        oop prefix = space_make_string_pub("obs", 3);

        oop put_args[2] = { path, TAG_SMI(11) };
        send(pf, sel("spacePut:value:"), 2, put_args);

        oop seq_before = send(pf, sel("spaceSeq"), 0, NULL);
        assert(IS_SMI(seq_before) && UNTAG_SMI(seq_before) >= 1
               && "the write advanced the space's counter");

        oop stamped = send(pf, sel("spaceStamp"), 0, NULL);
        assert(UNTAG_SMI(stamped) == UNTAG_SMI(seq_before) + 1
               && "a stamp bumps the counter — it does not merely read it");

        oop scan_args[3] = { TAG_SMI(0), prefix, TAG_SMI(4) };
        oop rows = send(pf, sel("spaceChangedSince:under:max:"), 3, scan_args);
        assert(rows != NULL);
        oop nrows = send(rows, sel("basicSize"), 0, NULL);
        assert(UNTAG_SMI(nrows) == 1 && "exactly one obs cell has been written");

        oop row = send(rows, sel("basicAt:"), 1, (oop[]){ TAG_SMI(1) });
        oop rowsz = send(row, sel("basicSize"), 0, NULL);
        assert(UNTAG_SMI(rowsz) == 3 && "{path, seq, value}");
        oop rowseq = send(row, sel("basicAt:"), 1, (oop[]){ TAG_SMI(2) });
        oop rowval = send(row, sel("basicAt:"), 1, (oop[]){ TAG_SMI(3) });
        assert(UNTAG_SMI(rowseq) == UNTAG_SMI(seq_before) && "the write's own seq");
        assert(UNTAG_SMI(rowval) == 11 && "the value rides with the row — no second peek");

        /* An empty answer is an empty indexable, not nil: a caller loops over
           basicSize and must not have to special-case "nothing changed". */
        oop none_args[3] = { stamped, prefix, TAG_SMI(4) };
        oop none = send(pf, sel("spaceChangedSince:under:max:"), 3, none_args);
        assert(none != NULL && UNTAG_SMI(send(none, sel("basicSize"), 0, NULL)) == 0);

        printf("test_platform: space-sync prims OK\n");
    }

    printf("TASK2 OK\n");
    return 0;
}
