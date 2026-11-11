/* Unit test for the W1 GC trigger + precise rooting (src/gc.c).
   Build:
     gcc -std=c99 -Isrc -DOEVM_HEAP_SIZE=65536 tests/test_gc_trigger.c \
         src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/solidified_image.c \
         -lm -o /tmp/tgct && /tmp/tgct
   Verifies: (1) an oop registered as a precise root (gc_additional_roots_hook /
   gc_root — the conservative C-stack scan this originally exercised was
   deleted in de-recursify Phase 3, fiber_derecursify_core_design.md 6d) survives
   gc_collect; (2) the exhaustion path collects + retries rather than aborting,
   and reclaims a bounded rolling root set within a small cap. */
#include "../src/oevm.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

extern oop  gc_allocate(oop vtable, size_t size);
extern void gc_collect(void);
extern void heap_init(void);
extern int  gc_is_live(oop);
extern void (*gc_additional_roots_hook)(void);
extern void gc_root(oop *slot);

/* A minimal vtable so allocated objects look like ordinary instances to the
   GC (payload is traced word-by-word; NULL/SMI words are ignored by mark). */
static struct Vtable PlainVtable;

/* Registered as a precise root (gc_additional_roots_hook / gc_root) across
   the collection — the conservative C-stack scan this used to rely on is
   deleted (de-recursify Phase 3, fiber_derecursify_core_design.md 6d); a bare
   C local is no longer sufficient to root an oop through gc_collect(). */
static oop g_precise_root_obj;
static void root_precise_obj(void) { gc_root(&g_precise_root_obj); }

__attribute__((noinline))
static int precise_root_survives(void) {
    oop obj = gc_allocate((oop)&PlainVtable, sizeof(struct ObjectInstance));
    g_precise_root_obj = obj;
    gc_additional_roots_hook = root_precise_obj;
    gc_collect();                 /* obj is rooted ONLY by the precise-root hook */
    gc_additional_roots_hook = NULL;
    int live = gc_is_live(obj);
    g_precise_root_obj = NULL;
    return live;
}

/* Rolling root set: only these survive each collection. */
#define ROLL 8
static oop roll[ROLL];
static void roll_roots(void) { for (int i = 0; i < ROLL; i++) gc_root(&roll[i]); }

__attribute__((noinline))
static void exhaustion_reclaims(void) {
    gc_additional_roots_hook = roll_roots;
    for (int i = 0; i < ROLL; i++) roll[i] = NULL;

    /* 64 KB cap holds far fewer than 20000 of these live at once; success
       requires the exhaustion path to collect + reclaim the dropped objects. */
    for (int i = 0; i < 20000; i++) {
        oop o = gc_allocate((oop)&PlainVtable, sizeof(struct ObjectInstance) + 4 * sizeof(oop));
        roll[i % ROLL] = o;            /* drop the object ROLL allocations ago */
    }
    /* All current roots must still be live after all that churn. */
    for (int i = 0; i < ROLL; i++) assert(gc_is_live(roll[i]));
    gc_additional_roots_hook = NULL;
}

/* --- debugger_hook global root test --- */
extern oop debugger_hook;

/* Allocate an object and stash it ONLY in the debugger_hook global; no C
   local survives back to the caller, so the global is the sole root. */
__attribute__((noinline))
static void stash_debugger_hook(void) {
    debugger_hook = gc_allocate((oop)&PlainVtable, sizeof(struct ObjectInstance));
}

/* Test that debugger_hook is covered by the precise root list in gc_collect.
   The conservative C-stack scan this test used to explicitly disable is gone
   (de-recursify Phase 3) — precise roots are ALL there is now. Before the fix,
   gc_collect does not mark debugger_hook, so the object is swept and
   gc_is_live returns 0. After the fix, gc_collect marks it and gc_is_live
   returns 1. */
__attribute__((noinline))
static int debugger_hook_global_survives(void) {
    stash_debugger_hook();
    /* At this point debugger_hook is the ONLY reference to the object. */
    gc_collect();
    int live = gc_is_live(debugger_hook);
    debugger_hook = NULL;          /* cleanup: don't leak into later tests */
    return live;
}

int main(void) {
    heap_init();
    PlainVtable.base.vtable = NULL;   /* VtableVtable is NULL pre-boot; fine */
    PlainVtable.size = 0;
    PlainVtable.delegate = NULL;

    assert(precise_root_survives() == 1);
    printf("test_gc_trigger: precise-root survival OK\n");

    exhaustion_reclaims();
    printf("test_gc_trigger: exhaustion collect+retry reclaims OK\n");

    assert(debugger_hook_global_survives() == 1);
    printf("test_gc_trigger: debugger_hook global root survives GC OK\n");

    printf("ALL TESTS PASSED\n");
    return 0;
}
