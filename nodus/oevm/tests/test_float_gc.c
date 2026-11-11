/* Unit test for W2a float-arena reclaim (src/gc.c).
   Build:
     gcc -std=c99 -Isrc tests/test_float_gc.c \
         src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/solidified_image.c \
         -lm -o /tmp/tfgc && /tmp/tfgc */
#include "../src/oevm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

extern oop    float_alloc(oop vtable);
extern oop    gc_allocate(oop vtable, size_t size);
extern void   gc_collect(void);
extern void   heap_init(void);
extern void   gc_root(oop *slot);
extern void (*gc_additional_roots_hook)(void);

extern size_t float_arena_slab_count(void);
extern size_t float_arena_free_count(void);
extern int    float_is_live(oop ptr);
extern size_t float_arena_slab_budget(void);

static int fake_vtable;
#define VT ((oop)&fake_vtable)
#define FLOAT_ARENA_SLAB_CELLS_T ((size_t)4096)

/* PlainVtable: a minimal vtable whose size=0 causes trace_object to walk the
   ObjectInstance payload word-by-word (the generic path), which is exactly the
   transitive-mark path we want to exercise. */
static struct Vtable PlainVtable;

/* File-static holder used by the gc_additional_roots_hook below. */
static oop holder_root;
static void keep_holder(void) { gc_root(&holder_root); }

/* (2f) Transitive float survival: a Float stored in a heap object's field is
   reached by trace_object -> mark -> float_mark and survives gc_collect even
   when no C local references it directly. The conservative C-stack scan is
   deleted (de-recursify Phase 3, fiber_derecursify_core_design.md 6d) — precise
   GC only, so ONLY the transitive path below keeps the float alive. */
static void test_float_via_heap_field(void) {
    /* Allocate holder with one extra oop slot beyond the struct Object header. */
    size_t holder_size = sizeof(struct ObjectInstance) + sizeof(oop);
    oop holder = gc_allocate((oop)&PlainVtable, holder_size);
    assert(holder != NULL);

    /* gc_allocate does not zero the payload — zero the slot explicitly to avoid
       spurious oop values being traced and confusing later assertions. */
    ((struct ObjectInstance *)holder)->slots[0] = NULL;

    /* Allocate the float we want to survive.  Store it ONLY in the holder's
       first payload slot; no C local retains a reference to it after this. */
    oop floatcell = float_alloc(VT);
    ((struct Float *)floatcell)->value = 2.718281828;
    ((struct ObjectInstance *)holder)->slots[0] = floatcell;
    floatcell = NULL;               /* drop the only C local reference */

    /* Allocate a second float that is NOT stored anywhere — it should be
       reclaimed to confirm the first survived for the right reason. */
    oop dead_float = float_alloc(VT);
    ((struct Float *)dead_float)->value = -99.0;
    dead_float = NULL;              /* immediately unreachable */

    /* Register the hook that marks holder as a GC root; holder transitively
       reaches the float via its slot[0] field. */
    holder_root = holder;
    gc_additional_roots_hook = keep_holder;

    gc_collect();

    /* Retrieve the survived float from the holder's slot. */
    oop survived = ((struct ObjectInstance *)holder)->slots[0];

    assert(float_is_live(survived) == 1);
    assert(((struct Float *)survived)->value == 2.718281828);

    /* The unreferenced float must have been reclaimed. */
    /* (dead_float pointer is NULL here; we cannot call float_is_live(NULL) —
       instead verify free count grew, proving at least one float was swept.) */
    assert(float_arena_free_count() > 0);

    gc_additional_roots_hook = NULL;
    holder_root = NULL;
    printf("test_float_gc: float via heap field (transitive mark) OK\n");
}

/* (2a) "A Float referenced only by a C local survives a collection (conservative
   C-stack pinning reaches the arena)" was deleted along with the conservative
   C-stack scan itself (de-recursify Phase 3, fiber_derecursify_core_design.md
   6d) — precise GC does not root bare C locals, so this premise no longer
   holds and there is nothing to revive it into (test_float_via_heap_field and
   test_mixed_liveness below already cover precise-root survival). Retired. */

/* (2b) Dead Floats are reclaimed and reused; the slab count does not grow on a
   second wave that fits within the reclaimed cells. Nothing roots the wave
   (conservative C-stack scan is gone; no explicit root is registered either),
   so it is unreachable and reclaimed deterministically. */
__attribute__((noinline))
static void alloc_wave(size_t n) {       /* locals die when this returns */
    for (size_t i = 0; i < n; i++) { oop f = float_alloc(VT); ((struct Float *)f)->value = (double)i; }
}
static void test_reclaim_reuse(void) {
    size_t want = 3 * FLOAT_ARENA_SLAB_CELLS_T;   /* force ~3 slabs */
    alloc_wave(want);
    size_t slabs_after_wave = float_arena_slab_count();
    gc_collect();                        /* all of the wave is unreachable */
    assert(float_arena_free_count() > 0);
    size_t slabs_before_reuse = float_arena_slab_count();
    alloc_wave(want / 2);                /* fits in reclaimed cells */
    assert(float_arena_slab_count() == slabs_before_reuse);  /* reused, no growth */
    (void)slabs_after_wave;
    printf("test_float_gc: reclaim + reuse OK\n");
}

/* (2c) Mixed liveness: marked Floats survive, unmarked are reclaimed. */
#define KEEP 4
static oop kept[KEEP];
static void keep_roots(void) { for (int i = 0; i < KEEP; i++) gc_root(&kept[i]); }
static void test_mixed_liveness(void) {
    gc_additional_roots_hook = keep_roots;
    for (int i = 0; i < KEEP; i++) { kept[i] = float_alloc(VT); ((struct Float *)kept[i])->value = 100.0 + i; }
    oop dead = float_alloc(VT);          /* unreferenced */
    ((struct Float *)dead)->value = -1.0;
    gc_collect();
    for (int i = 0; i < KEEP; i++) {
        assert(float_is_live(kept[i]) == 1);
        assert(((struct Float *)kept[i])->value == 100.0 + i);
    }
    assert(float_is_live(dead) == 0);    /* reclaimed */
    gc_additional_roots_hook = NULL;
    printf("test_float_gc: mixed liveness OK\n");
}

/* Task 1: introspection hooks report a coherent picture of a fresh arena. */
static void test_hooks_basic(void) {
    size_t slabs0 = float_arena_slab_count();
    oop c = float_alloc(VT);
    assert(c != NULL);
    assert(((struct Float *)c)->base.vtable == VT);
    assert(float_arena_slab_count() >= 1);
    assert(float_arena_slab_count() > slabs0 || slabs0 >= 1);
    assert(float_arena_free_count() == 0);   /* nothing reclaimed yet */
    assert(float_is_live(c) == 1);           /* freshly handed out => in use */
    printf("test_float_gc: hooks basic OK\n");
}

/* (2d) Regression: stale markbit on a pristine bump cell must be cleared.
 *
 * Scenario: the conservative C-stack scan (or gc_root) sets the markbit
 * of a pristine cell (index >= float_slab_used) before it is handed out.
 * float_sweep() only visits [0, float_slab_used), so the bit is NEVER cleared
 * by the sweep. The next float_alloc() (bump path) hands out that cell; the
 * cell then goes dead; but the next gc_collect() sees the still-set bit and
 * treats the dead cell as live — a bounded leak.
 *
 * Fix: the bump branch of float_alloc() clears the cell's markbit before
 * returning. This test FAILS without that one-liner and PASSES with it.
 */
static void test_pristine_markbit_cleared(void) {
    /* Conservative scan is gone; only explicit gc_root(&) below is in play. */

    /* Drain the free-list: prior tests may have left reclaimed cells there.
       float_alloc draws from the free-list before the bump cursor, so we must
       exhaust it before we can deterministically predict the next bump address. */
    gc_collect();  /* no roots => everything dead => free-list rebuilt; do NOT clear it */
    while (float_arena_free_count() > 0) float_alloc(VT);  /* consume all free-list cells */

    /* Now the free-list is empty.  The next two float_alloc() calls will use the
       bump cursor. Alloc A first to learn the current bump position. */
    oop A = float_alloc(VT);
    ((struct Float *)A)->value = 1.0;

    /* Step 2: compute the address of the next-to-be-bumped pristine cell.
       Because A was bump-allocated and free-list is empty, the next bump cell
       is immediately after A.  We verify B == pristine after the alloc. */
    oop pristine = (oop)(((struct Float *)A) + 1);

    /* Step 3: mark the pristine cell as if the conservative scan saw its address.
       This sets its markbit even though float_slab_used hasn't reached it yet. */
    gc_root(&pristine);

    /* Step 4: bump-allocate — free-list is empty, so this MUST use the bump path
       and return that same pristine cell. */
    oop B = float_alloc(VT);
    assert(B == pristine && "float_alloc must return the pre-marked pristine cell");
    ((struct Float *)B)->value = 42.0;

    /* Step 5: drop B (no roots) and collect.  Without the fix, the stale bit
       makes float_sweep() skip B — it stays off the free-list (leaked).
       With the fix, the bump branch clears the bit, so sweep reclaims B. */
    gc_collect();

    assert(float_is_live(B) == 0 && "pristine cell must be reclaimed after drop+collect");

    printf("test_float_gc: pristine markbit cleared on bump OK\n");
}

/* Acceptance: under sustained churn with a bounded live-set, the slab count
   stops growing once steady state is reached (the float analogue of the W1
   gc_stress RSS bound). Without reclaim this would grow without limit. */
__attribute__((noinline))
static void churn_round(size_t n) {       /* all allocations die at return */
    volatile double sink = 0.0;
    for (size_t i = 0; i < n; i++) {
        oop f = float_alloc(VT);
        ((struct Float *)f)->value = (double)i;
        sink += ((struct Float *)f)->value;
    }
    (void)sink;
}
static void test_bounded_churn(void) {
    /* Warm up to steady state, then measure that slabs stop growing. */
    for (int r = 0; r < 3; r++) { churn_round(50000); gc_collect(); }
    size_t slabs_steady = float_arena_slab_count();
    for (int r = 0; r < 10; r++) { churn_round(50000); gc_collect(); }
    assert(float_arena_slab_count() == slabs_steady);  /* bounded: no growth */
    printf("test_float_gc: bounded churn OK (steady at %zu slabs)\n", slabs_steady);
}

/* W2a.1: the float arena self-triggers a collection at its soft cap and stays
   bounded WITHOUT any explicit gc_collect call. Without the trigger, the slab
   count grows without limit. Conservative scan is gone, so the dropped floats
   are deterministically unreachable (precise roots don't cover them). */
__attribute__((noinline))
static void churn_floats_no_collect(size_t n) {   /* locals die at return */
    volatile double sink = 0.0;
    for (size_t i = 0; i < n; i++) {
        oop f = float_alloc(VT);
        ((struct Float *)f)->value = (double)i;
        sink += ((struct Float *)f)->value;
    }
    (void)sink;
}
static void test_arena_self_triggers_bound(void) {
    size_t budget = float_arena_slab_budget();
    /* Allocate FAR more than budget*4096 cells, all dropped. With the trigger the
       arena collects-and-reuses at the cap; without it, it grows ~linearly. */
    churn_floats_no_collect((budget + 8) * FLOAT_ARENA_SLAB_CELLS_T * 4);
    assert(float_arena_slab_count() <= budget + 2);
    printf("test_float_gc: arena self-triggers + bounded OK (%zu/%zu slabs)\n",
           float_arena_slab_count(), budget);
}

int main(void) {
    heap_init();

    PlainVtable.base.vtable = NULL;
    PlainVtable.size = 0;
    PlainVtable.delegate = NULL;

    test_hooks_basic();

    test_reclaim_reuse();
    test_mixed_liveness();
    test_pristine_markbit_cleared();
    test_bounded_churn();
    test_float_via_heap_field();
    test_arena_self_triggers_bound();

    printf("ALL TESTS PASSED\n");
    return 0;
}
