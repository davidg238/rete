/* Unit test for the W1 GC trigger + conservative C-stack rooting (src/gc.c).
   Build:
     gcc -std=c99 -Isrc -DOEVM_HEAP_SIZE=65536 tests/test_gc_trigger.c \
         src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/solidified_image.c \
         -lm -o /tmp/tgct && /tmp/tgct
   Verifies: (1) an oop held only in a C local survives gc_collect via the
   conservative C-stack scan; (2) the exhaustion path collects + retries rather
   than aborting, and reclaims a bounded rolling root set within a small cap. */
#include "../src/oevm.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

extern oop  gc_allocate(oop vtable, size_t size);
extern void gc_collect(void);
extern void heap_init(void);
extern int  gc_is_live(oop);
extern void gc_set_stack_base(void *p);
extern void (*gc_additional_roots_hook)(void);
extern void mark_object(oop);

/* A minimal vtable so allocated objects look like ordinary instances to the
   GC (payload is traced word-by-word; NULL/SMI words are ignored by mark). */
static struct Vtable PlainVtable;

/* Held only in a C local across the collection. Marked noinline so the local
   genuinely lives in this frame (not folded into main). Used after the collect
   so the compiler must preserve it across the opaque gc_collect() call. */
__attribute__((noinline))
static int local_only_survives(void) {
    oop obj = gc_allocate((oop)&PlainVtable, sizeof(struct ObjectInstance));
    gc_collect();                 /* obj is rooted ONLY by this C local */
    return gc_is_live(obj);       /* use obj after the collect */
}

/* Rolling root set: only these survive each collection. */
#define ROLL 8
static oop roll[ROLL];
static void roll_roots(void) { for (int i = 0; i < ROLL; i++) mark_object(roll[i]); }

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
extern void *vm_c_stack_base;  /* declared in gc.c; we clear it to disable stack scan */

/* Allocate an object and stash it ONLY in the debugger_hook global; no C
   local survives back to the caller, so the global is the sole root. */
__attribute__((noinline))
static void stash_debugger_hook(void) {
    debugger_hook = gc_allocate((oop)&PlainVtable, sizeof(struct ObjectInstance));
}

/* Test that debugger_hook is covered by the precise root list in gc_collect.
   We deliberately disable the conservative C-stack scan (vm_c_stack_base=NULL)
   so that ONLY precise roots protect the object.  Before the fix, gc_collect
   does not mark debugger_hook, so the object is swept and gc_is_live returns 0.
   After the fix, gc_collect marks it and gc_is_live returns 1. */
__attribute__((noinline))
static int debugger_hook_global_survives(void) {
    void *saved_stack_base = vm_c_stack_base;
    /* Disable conservative scan: only precise roots protect oops. */
    vm_c_stack_base = NULL;
    stash_debugger_hook();
    /* At this point debugger_hook is the ONLY reference to the object. */
    gc_collect();
    int live = gc_is_live(debugger_hook);
    debugger_hook = NULL;          /* cleanup: don't leak into later tests */
    vm_c_stack_base = saved_stack_base;  /* restore for subsequent tests */
    return live;
}

/* --- Symbol_accessViolation global root test --- */
extern oop Symbol_accessViolation;

/* Allocate an object and stash it ONLY in Symbol_accessViolation; no C local
   survives back to the caller, so the global is the sole root. */
__attribute__((noinline))
static void stash_accessviolation(void) {
    Symbol_accessViolation = gc_allocate((oop)&PlainVtable, sizeof(struct ObjectInstance));
}

/* Test that Symbol_accessViolation is covered by the precise root list in
   gc_collect.  We deliberately disable the conservative C-stack scan
   (vm_c_stack_base=NULL) so that ONLY precise roots protect the object.
   Before the fix, gc_collect does not mark Symbol_accessViolation, so the
   object is swept and gc_is_live returns 0.  After the fix, gc_collect marks
   it and gc_is_live returns 1. */
__attribute__((noinline))
static int accessviolation_global_survives(void) {
    void *saved_stack_base = vm_c_stack_base;
    /* Disable conservative scan: only precise roots protect oops. */
    vm_c_stack_base = NULL;
    stash_accessviolation();
    /* At this point Symbol_accessViolation is the ONLY reference to the object. */
    gc_collect();
    int live = gc_is_live(Symbol_accessViolation);
    Symbol_accessViolation = NULL;  /* cleanup: don't leak into later tests */
    vm_c_stack_base = saved_stack_base;  /* restore for subsequent tests */
    return live;
}

int main(void) {
    int base_anchor;
    gc_set_stack_base(&base_anchor);
    heap_init();
    PlainVtable.base.vtable = NULL;   /* VtableVtable is NULL pre-boot; fine */
    PlainVtable.size = 0;
    PlainVtable.delegate = NULL;

    assert(local_only_survives() == 1);
    printf("test_gc_trigger: conservative-scan survival OK\n");

    exhaustion_reclaims();
    printf("test_gc_trigger: exhaustion collect+retry reclaims OK\n");

    assert(debugger_hook_global_survives() == 1);
    printf("test_gc_trigger: debugger_hook global root survives GC OK\n");

    assert(accessviolation_global_survives() == 1);
    printf("test_gc_trigger: Symbol_accessViolation global root survives GC OK\n");

    printf("ALL TESTS PASSED\n");
    return 0;
}
