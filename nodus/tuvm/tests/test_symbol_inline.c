/* rete#190 stage 2: Symbol name bytes are INLINE (src/oevm.h) — a C99 flexible
   array member replacing the `const char *name` that was oevm_malloc'd per
   decoded Symbol and never freed. That leak crashed three ESP32-C6 boards.
   Three passing gates prove only that nothing broke; this proves the intended
   thing happened, and guards the landmine the change introduced.

   Fails-before: this file cannot compile against the pre-fix VM at all —
   `((struct Symbol *)s)->name[i] = c` is a write through a `const char *` on
   the old struct, and `sizeof(struct Symbol) + len + 1` is not the old
   allocation shape. A compile error is a weak demonstration, so the real
   falsification for property 2 was done empirically: deleting the SymbolVtable
   arm from gc.c's trace_one makes property 2 below FAIL (see the commit body).

   Build (Model A link set — ENG_A in tools/run_c_tests.sh):
     bash tools/run_c_tests.sh   # runs this as test_symbol_inline

   Verifies:
     1. Building Symbols costs the C heap NOTHING. Every Symbol used to cost an
        oevm_malloc that was never freed; the C-allocation seam's own counters
        (stage 1) are the witness. `allocs` not moving across 200 Symbol builds
        is a stronger claim than "out_bytes came back down" — a leak moves
        allocs and leaves out_bytes high, but a malloc/free pair that merely
        churns still moves allocs, and inline bytes must not even do that.

     2. Inline name bytes are a GC LEAF. gc.c's trace_one has a SymbolVtable arm
        that marks only the vtable. Without it a Symbol falls through to the
        generic word-count fallback (trace_object), which would scan raw symbol
        TEXT as oops. This test plants that landmine deliberately: an
        unreachable decoy object, and a rooted Symbol whose name bytes are
        literally the decoy's address. With the leaf arm the decoy is swept;
        without it the fallback reads the name as a pointer, marks the decoy,
        and gc_is_live(decoy) stays true. The assertion is on reclamation, NOT
        on "the Symbol still reads back correctly" — this collector is
        non-moving and sweeps inside its own arena, so a swept object's bytes
        remain physically readable and unchanged. Reading back correct-looking
        bytes after a collection proves nothing about reachability, which is
        also why OEVM_GC_STRESS is blind to this whole class of bug. */
#include "../src/oevm.h"
#include "../src/oimg.h"
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>

extern oop  SymbolVtable;
extern oop  gc_allocate(oop vtable, size_t size);
extern void gc_collect(void);
extern void heap_init(void);
extern int  gc_is_live(oop);
extern void mark_object(oop);
extern void (*gc_additional_roots_hook)(void);
extern void init_solidified_vtables(void);   /* src/solidified_image.c — sets SymbolVtable */

/* Roots: the same precise-rooting idiom as test_heap_stats.c/test_coalesce.c.
   The GC does NOT scan the C stack (see vm.c:325 and gc.c's scan_vm_stack,
   which walks VM frames only), so an oop held in a C local is not a root at
   all. Property 2 would be a false green without this: an object that was
   never reachable proves nothing about how it is traced. */
#define MAX_ROOTS 8
static oop g_roots[MAX_ROOTS];
static void roots_hook(void) {
    for (int i = 0; i < MAX_ROOTS; i++)
        if (g_roots[i] != NULL) mark_object(g_roots[i]);
}

static oop make_symbol(const char *text, size_t len) {
    oop s = gc_allocate(SymbolVtable, sizeof(struct Symbol) + len + 1);
    assert(s != NULL);
    ((struct Symbol *)s)->length = len;
    memcpy(((struct Symbol *)s)->name, text, len);
    ((struct Symbol *)s)->name[len] = 0;
    return s;
}

/* A decoy payload big enough that its reclamation is unmistakable in
   live_bytes as well as in the alloc bitmap. */
#define DECOY_SZ 512

int main(void) {
    heap_init();
    init_solidified_vtables();
    assert(SymbolVtable != NULL && "SymbolVtable must be initialised before Symbols exist");
    gc_additional_roots_hook = roots_hook;

    /* ---- property 1: 200 Symbols cost the C heap zero bytes ---- */
    struct OevmCHeapStats a = oevm_cheap_stats();
    for (int i = 0; i < 200; i++) {
        char nm[24];
        int n = snprintf(nm, sizeof nm, "leakCanary%d:", i);
        oop s = make_symbol(nm, (size_t)n);
        /* the bytes live INSIDE the object, not at the far end of a pointer */
        const char *got = symbol_name((struct Symbol *)s);
        assert(got == (const char *)s + offsetof(struct Symbol, name)
               && "symbol_name() must point into the object, not at a malloc'd buffer");
        assert(strcmp(got, nm) == 0);
        assert(((struct Symbol *)s)->length == (size_t)n);
    }
    struct OevmCHeapStats b = oevm_cheap_stats();
    assert(b.allocs == a.allocs && "200 Symbols must not cost one C-heap allocation");
    assert(b.out_bytes == a.out_bytes && "Symbol construction must leave no C-heap bytes outstanding");
    assert(b.out_high == a.out_high && "Symbol construction must not even transiently touch the C heap");
    assert(b.fails == a.fails);
    printf("test_symbol_inline: property 1 (200 Symbols cost the C heap 0 bytes) OK\n");

    /* ---- property 2: inline name bytes are a GC leaf ---- */
    /* Settle first, so the 200 dead canaries above are already reclaimed and
       cannot mask the one reclamation this property measures. */
    gc_collect();

    /* The decoy: reachable for now (rooted), so it survives the baseline. */
    oop decoy = gc_allocate(NULL, DECOY_SZ);
    assert(decoy != NULL);
    memset((void *)decoy, 0, DECOY_SZ);     /* no stale words that could mark anything */
    g_roots[0] = decoy;

    /* The trap: a Symbol whose name bytes ARE the decoy's address, repeated so
       at least one copy lands on a word boundary under any struct padding.
       If trace_one lacks its SymbolVtable arm, trace_object scans this text at
       word granularity and mark()s the decoy — which exact-start-validates in
       the alloc bitmap, so it really is retained. */
    char ptrtext[2 * sizeof(void *)];
    memcpy(ptrtext, &decoy, sizeof(void *));
    memcpy(ptrtext + sizeof(void *), &decoy, sizeof(void *));
    oop trap = make_symbol(ptrtext, sizeof ptrtext);
    g_roots[1] = trap;

    gc_collect();
    struct OevmHeapStats h1 = oevm_heap_stats();
    assert(gc_is_live(decoy) && "a rooted decoy must survive — otherwise the baseline is meaningless");
    assert(gc_is_live(trap)  && "the rooted trap Symbol must survive");

    /* Now drop the decoy's ONLY legitimate root. The sole remaining path to it
       is the trap Symbol's name text — which is not a path at all, unless the
       leaf arm is missing. */
    g_roots[0] = NULL;
    gc_collect();
    struct OevmHeapStats h2 = oevm_heap_stats();

    assert(!gc_is_live(decoy)
           && "UNREACHABLE decoy survived a collection: trace_one scanned a Symbol's "
              "name bytes as oops — the SymbolVtable leaf arm in gc.c is missing");
    assert(h1.live_bytes - h2.live_bytes >= DECOY_SZ
           && "the decoy's bytes must come off live_bytes; nothing else was garbage");

    /* The trap itself is still rooted and must be intact and correctly traced. */
    assert(gc_is_live(trap) && "the trap Symbol is still rooted and must survive");
    assert(((struct Symbol *)trap)->length == sizeof ptrtext);
    assert(memcmp(symbol_name((struct Symbol *)trap), ptrtext, sizeof ptrtext) == 0);
    assert(h2.live_bytes <= h2.high_water && h2.high_water <= h2.heap_size);
    printf("test_symbol_inline: property 2 (inline bytes are a GC leaf) OK\n");

    /* A fresh Symbol after the collection still behaves — the arena is intact. */
    oop after = make_symbol("afterCollect:", 13);
    g_roots[2] = after;
    assert(strcmp(symbol_name((struct Symbol *)after), "afterCollect:") == 0);

    gc_additional_roots_hook = NULL;
    printf("test_symbol_inline: OK\n");
    return 0;
}
