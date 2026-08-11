/* rete#190 stage 2: Symbol name bytes are INLINE (src/oevm.h) — a C99 flexible
   array member replacing the `const char *name` that was oevm_malloc'd per
   decoded Symbol and never freed. That leak crashed three ESP32-C6 boards.
   Three passing gates prove only that nothing broke; this proves the intended
   thing happened, and guards the landmine the change introduced.

   Build (Model A link set — ENG_A in tools/run_c_tests.sh):
     bash tools/run_c_tests.sh   # runs this as test_symbol_inline

   Verifies:
     1. THE LEAK IS GONE, measured at the site that leaked. The test drives the
        real `platform asSymbol:` primitive (PRIM_pf_asSymbol, src/platform.c)
        200 times through platform_dispatch and reads the stage-1 C-allocation
        seam's counters across the loop. This is the wire-decode path: every
        CBOR tag-39 Symbol a node decodes comes through here, which is why the
        leak was one malloc per decoded Symbol.

        Fails-before is MEASURED, not argued. Built against 703547e (the commit
        before the fix, extracted with `git archive` — its PRIM_pf_asSymbol does
        `char *nm = oevm_malloc(len + 1)` and never frees it), the same 200
        iterations report:
            allocs +200, out_bytes +2890, out_high +2890
        out_bytes is the OUTSTANDING count: 2890 bytes still held at exit,
        never freed — the leak itself, not mere allocator churn. At HEAD all
        three deltas are +0, and the test PRINTS them, so a regression reports
        its own size rather than just failing.

        Note what this does NOT do: it does not merely build Symbols with
        gc_allocate here in the test. The arena is a static BSS array
        (gc.c's oevm_heap_store), so a test-local gc_allocate loop cannot reach
        malloc no matter what the VM does — such a loop's cheap-stats
        assertions hold for any VM in which this file compiles, and would stay
        green if someone reintroduced an oevm_malloc (an interning table, a
        case-folded copy) into PRIM_pf_asSymbol. Only calling the prim guards
        the prim.

     1b. Shape, for the record: symbol_name() points INTO the object. This is
        compile-time force only — a flexible array member cannot be anywhere
        else, and the read-backs below re-read this test's own memcpy. Kept as
        documentation of the intended layout, not as runtime evidence.

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
        also why OEVM_GC_STRESS is blind to this whole class of bug.
        Falsified by hand: short-circuiting trace_one's SymbolVtable arm makes
        property 2 abort, while every other test in run_c_tests.sh stays green. */
#include "../src/oevm.h"
#include "../src/oimg.h"
#include "../src/platform.h"
#include "../src/prim_ids.h"
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>

extern oop  SymbolVtable;
extern oop  StringVtable;
extern oop  gc_allocate(oop vtable, size_t size);
extern void gc_collect(void);
extern void heap_init(void);
extern int  gc_is_live(oop);
extern void mark_object(oop);
extern void gc_temp_root_push(oop o);
extern int  gc_temp_roots_count;
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

#define SYMBOL_BUILDS 200

int main(void) {
    heap_init();
    init_solidified_vtables();
    assert(SymbolVtable != NULL && "SymbolVtable must be initialised before Symbols exist");
    assert(StringVtable != NULL);
    gc_additional_roots_hook = roots_hook;

    /* ---- property 1: the REAL seam. 200 asSymbol: prim calls, C-heap cost 0 ---- */
    struct OevmCHeapStats a = oevm_cheap_stats();
    for (int i = 0; i < SYMBOL_BUILDS; i++) {
        char txt[24];
        int n = snprintf(txt, sizeof txt, "leakCanary%d:", i);

        /* The String receiver the prim reads its bytes from. */
        oop s = gc_allocate(StringVtable, sizeof(struct String) + (size_t)n);
        assert(s != NULL);
        ((struct String *)s)->length = (size_t)n;
        memcpy(((struct String *)s)->bytes, txt, (size_t)n);

        /* A C local is not a GC root (no conservative C-stack scan), and the
           prim allocates. Root the receiver across the call — same idiom as
           tests/test_cbor.c's test_cbor_as_symbol. */
        int saved_roots = gc_temp_roots_count;
        gc_temp_root_push(s);
        oop out = NULL;
        int ok = platform_dispatch(PRIM_pf_asSymbol, 1, &s, &out);
        gc_temp_roots_count = saved_roots;

        assert(ok && "PRIM_pf_asSymbol must service the dispatch");
        assert(out != NULL && "asSymbol: must not answer nil");
        assert(obj_vtable(out) == SymbolVtable);
        struct Symbol *sym = (struct Symbol *)out;
        assert(sym->length == (size_t)n);
        assert(memcmp(sym->name, txt, (size_t)n) == 0);
        assert(sym->name[n] == 0 && "the prim must NUL-terminate: symbol_name() is fed to strcmp");
    }
    struct OevmCHeapStats b = oevm_cheap_stats();
    /* Measured at 703547e (pre-fix): allocs 0 -> 200, out_bytes/out_high 0 -> 2890.
       (2690 is the name-bytes subtotal alone; each malloc was len + 1, so the 200
       NUL terminators bring it to 2890 — the figure the pre-fix run actually printed.) */
    assert(b.allocs == a.allocs
           && "asSymbol: must not cost one C-heap allocation — that malloc WAS the leak");
    assert(b.out_bytes == a.out_bytes
           && "asSymbol: must leave no C-heap bytes outstanding");
    assert(b.out_high == a.out_high
           && "asSymbol: must not even transiently touch the C heap");
    assert(b.fails == a.fails);
    printf("test_symbol_inline: property 1 (%d asSymbol: prim calls: allocs +%zu, "
           "out_bytes +%zu, out_high +%zu) OK\n",
           SYMBOL_BUILDS, b.allocs - a.allocs, b.out_bytes - a.out_bytes,
           b.out_high - a.out_high);

    /* ---- property 1b: layout, for the record (compile-time force only) ---- */
    {
        oop s = make_symbol("inlineShape:", 12);
        const char *got = symbol_name((struct Symbol *)s);
        /* True for any flexible array member — documents the intended layout;
           the strcmp re-reads this test's own memcpy. Neither is evidence
           about the VM at runtime. */
        assert(got == (const char *)s + offsetof(struct Symbol, name));
        assert(strcmp(got, "inlineShape:") == 0);
    }

    /* ---- property 2: inline name bytes are a GC leaf ---- */
    /* Settle first, so the dead Strings/Symbols above are already reclaimed and
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
    /* Written as an addition, not a subtraction: live_bytes is size_t
       (oevm.h), so `h1 - h2 >= DECOY_SZ` would UNDERFLOW to near-SIZE_MAX and
       pass if live_bytes ever rose between the snapshots. Fail-open. */
    assert(h1.live_bytes >= h2.live_bytes + DECOY_SZ
           && "the decoy's bytes must come off live_bytes; nothing else was garbage");

    /* The trap itself is still rooted; gc_is_live is the reachability claim.
       (The length/memcmp read-backs below are this test's own memcpy — kept as
       a corruption tripwire, not as evidence of survival.) */
    assert(gc_is_live(trap) && "the trap Symbol is still rooted and must survive");
    assert(((struct Symbol *)trap)->length == sizeof ptrtext);
    assert(memcmp(symbol_name((struct Symbol *)trap), ptrtext, sizeof ptrtext) == 0);
    assert(h2.live_bytes <= h2.high_water && h2.high_water <= h2.heap_size);
    printf("test_symbol_inline: property 2 (inline bytes are a GC leaf) OK\n");

    gc_additional_roots_hook = NULL;
    printf("test_symbol_inline: OK\n");
    return 0;
}
