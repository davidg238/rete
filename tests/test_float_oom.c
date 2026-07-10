/* Regression for tuvm#15: float-arena OOM under a small embedded malloc pool.
 *
 * float_alloc only ran a reclaiming collection once the arena reached the slab
 * BUDGET (OEVM_FLOAT_ARENA_SLAB_BUDGET). On a device whose malloc pool cannot
 * hold BUDGET slabs (the nRF52840-DK's 64 KB newlib pool vs. a ~50 KB budget),
 * the budget trigger never fired: float_alloc grew slab-by-slab until malloc
 * failed mid-ramp and aborted "VM Float Arena Error: Out of Memory" — even
 * though the existing slabs were full of dead cells a collection would reclaim
 * (float-heavy loops keep a tiny live set). floatMath therefore OOM'd on the DK.
 *
 * The fix makes reclaim demand-driven: on a malloc failure, run one collection
 * and, if it freed any cells, serve from the refilled free-list instead of
 * growing. The arena then self-bounds to whatever the pool actually allows.
 *
 * This test models the small pool with -Wl,--wrap=malloc capping float-slab
 * allocations to POOL_SLABS (< BUDGET, so the budget trigger never fires), then
 * churns far more floats than the pool could hold without reclaim. Pre-fix this
 * aborts; post-fix it completes with the arena bounded to the pool and a small
 * live set intact (proving reclaim is precise, not a blanket wipe).
 *
 * Build (see tools/run_c_tests.sh):
 *   -DOEVM_FLOAT_ARENA_SLAB_CELLS=512 -DOEVM_FLOAT_ARENA_SLAB_BUDGET=8
 *   -Wl,--wrap=malloc  + the Model-A engine link set.
 */
#include "../src/oevm.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

extern oop    float_alloc(oop vtable);
extern void   gc_collect(void);
extern void   heap_init(void);
extern void   mark_object(oop);
extern void (*gc_additional_roots_hook)(void);
extern size_t float_arena_slab_count(void);

static int fake_vtable;
#define VT ((oop)&fake_vtable)

/* --wrap=malloc: model a newlib pool that admits only POOL_SLABS float slabs.
 * A float slab (512 cells) is the only large allocation in this link set; small
 * allocations pass straight through. The cap is ARMED only after the live set is
 * in place, so unrelated startup allocations never consume a slab budget slot. */
#define POOL_SLABS 3
extern void *__real_malloc(size_t);
static int    cap_armed = 0;
static size_t big_allocs = 0;   /* slab-sized allocations admitted since arming */
void *__wrap_malloc(size_t n) {
    if (cap_armed && n >= 4096) {
        if (big_allocs >= POOL_SLABS) return NULL;   /* pool exhausted */
        big_allocs++;
    }
    return __real_malloc(n);
}

/* A small permanent live set: every collection must reclaim the dead churn while
 * keeping these alive, so reclaim is proven precise rather than a blanket sweep. */
#define KEEP 4
static oop kept[KEEP];
static void keep_roots(void) { for (int i = 0; i < KEEP; i++) mark_object(kept[i]); }

int main(void) {
    heap_init();
    gc_additional_roots_hook = keep_roots;

    for (int i = 0; i < KEEP; i++) {
        kept[i] = float_alloc(VT);
        ((struct Float *)kept[i])->value = 1000.0 + i;
    }

    /* Arm the pool cap now: POOL_SLABS *additional* slabs may be allocated before
       malloc starts failing. */
    cap_armed = 1;
    big_allocs = 0;
    size_t base_slabs = float_arena_slab_count();

    /* Churn far more floats than the capped pool could ever hold without reclaim.
       Each is immediately dead (no C/VM root retains it). Pre-fix: aborts at the
       (POOL_SLABS+1)-th slab. Post-fix: float_alloc collects on the malloc failure,
       reclaims the churn, and keeps serving cells. */
    size_t churn = (size_t)POOL_SLABS * 512 * 50;   /* 50x the pool capacity */
    for (size_t i = 0; i < churn; i++) {
        oop f = float_alloc(VT);
        ((struct Float *)f)->value = (double)i;      /* immediately unreachable */
    }

    /* Reached here => no OOM abort. The arena self-bounded to the pool... */
    assert(float_arena_slab_count() <= base_slabs + POOL_SLABS);
    /* ...and the live set survived every reclaim intact. */
    for (int i = 0; i < KEEP; i++)
        assert(((struct Float *)kept[i])->value == 1000.0 + (double)i);

    gc_additional_roots_hook = NULL;
    printf("test_float_oom: OK (reclaim-on-pressure; slabs=%zu <= pool=%zu)\n",
           float_arena_slab_count(), base_slabs + POOL_SLABS);
    return 0;
}
