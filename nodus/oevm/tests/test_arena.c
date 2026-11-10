/* Unit test for the fixed-arena general-object allocator (src/gc.c, W4).
   Build:
     gcc -std=c99 -Isrc tests/test_arena.c \
         src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/solidified_image.c \
         -lm -o /tmp/ta && /tmp/ta
   Verifies: distinct bump-allocated blocks; bitmap membership accepts live
   payload starts and rejects mid-object / interior / out-of-range pointers;
   exhaustion routes through the overridable OOM handler. */
#include "../src/oevm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <setjmp.h>

extern oop  gc_allocate(oop vtable, size_t size);
extern void gc_collect(void);
extern void heap_init(void);
extern int  gc_is_live(oop);
extern oop (*oevm_oom_handler)(size_t);
extern void (*gc_additional_roots_hook)(void);
extern void gc_root(oop *slot);

/* capture the default the library installed at startup */
static oop (*g_default_oom)(size_t);
static oop (*default_oom_ref(void))(size_t) { return g_default_oom; }

/* ---- membership is exercised through the public mark/sweep path ---- */
/* A root object whose payload word points either at the START of a target
   (should retain) or into its INTERIOR (should NOT retain). */
static oop g_root = NULL;
static void root_hook(void) { gc_root(&g_root); }

/* ---- exhaustion handler: flag + longjmp instead of abort ---- */
static jmp_buf oom_jmp;
static volatile int oom_fired = 0;
static oop test_oom(size_t size) { (void)size; oom_fired = 1; longjmp(oom_jmp, 1); }

/* Non-head chain object captured during test 3's exhaustion loop.
   File-scope so it survives the longjmp (C locals would be clobbered). */
static oop g_second = NULL;

int main(void) {
    g_default_oom = oevm_oom_handler;
    heap_init();

    /* 1. Distinct, writable bump allocations. */
    oop a = gc_allocate(NULL, sizeof(struct Object) + 2 * sizeof(oop));
    oop b = gc_allocate(NULL, sizeof(struct Object) + 2 * sizeof(oop));
    assert(a && b && a != b);

    /* 2. Membership precision: object-start validation, observed via gc_is_live.
       A root object's payload holds a START pointer to `b` (must retain b) and
       an INTERIOR pointer to `c` (must NOT retain c). `a` is referenced by
       nobody (must be swept). */
    oop c = gc_allocate(NULL, sizeof(struct Object) + 2 * sizeof(oop));
    oop *root_slots = (oop *)gc_allocate(NULL, sizeof(struct Object) + 2 * sizeof(oop));
    g_root = (oop)root_slots;
    /* payload starts right after the Object header (vtable word) */
    oop *payload = (oop *)((char *)root_slots + sizeof(struct Object));
    payload[0] = b;                                   /* start pointer  -> retains b */
    payload[1] = (oop)((char *)c + sizeof(oop));      /* interior ptr   -> must NOT retain c */

    gc_additional_roots_hook = root_hook;
    gc_collect();                                     /* a, c unreachable -> swept; b retained */
    gc_additional_roots_hook = NULL;

    assert(gc_is_live(b)  && "start pointer must retain b");
    assert(!gc_is_live(c) && "interior pointer must NOT retain c");
    assert(!gc_is_live(a) && "unreferenced object must be swept");

    /* 3. Exhaustion routes through the overridable handler (no abort).
       With collect-and-retry, the OOM handler is only reached when the live
       set itself exceeds the arena. Root every allocated block so collection
       cannot free them, ensuring we genuinely exhaust the arena.

       WHY the whole chain is retained across GC cycles:
       root_hook calls gc_root(g_root), which marks the head.  mark() then
       falls into the generic trace_object() path (vtable == NULL), which walks
       every payload word and calls mark() on each.  The first payload word of
       every chain node holds the previous g_root — i.e. the next link — so
       mark() recurses transitively through the entire chain.  No chain member
       can be swept.  g_second (the second link ever allocated) is pinned by
       this transitive reachability; gc_is_live(g_second) after OOM verifies it. */
    heap_init();
    oevm_oom_handler = test_oom;
    g_root = NULL;
    g_second = NULL;
    gc_additional_roots_hook = root_hook;
    oom_fired = 0;
    if (setjmp(oom_jmp) == 0) {
        /* Each allocation is linked into a root chain so GC cannot reclaim it.
           Capture g_second after the second allocation so the post-OOM assert
           can verify that a non-head node is kept alive by transitive marking. */
        for (;;) {
            oop obj = gc_allocate(NULL, 4096);
            ((oop *)obj)[0] = g_root;   /* chain: new head -> old root */
            g_root = obj;               /* root hook marks g_root (and traces chain) */
            if (g_second == NULL && g_root != NULL && ((oop *)g_root)[0] != NULL) {
                /* At least two links exist: g_root is the head, its payload[0]
                   is the second link. Capture it now for the post-OOM check. */
                g_second = ((oop *)g_root)[0];
            }
        }
        assert(0 && "unreachable");
    }
    gc_additional_roots_hook = NULL;
    assert(oom_fired);
    /* Verify transitive marking: the second chain link (not the head) must still
       be live — it was reachable only through g_root's payload word, so a
       non-recursive gc_root refactor that stops at the root would break this. */
    assert(g_second != NULL && "test 3 needs at least two allocations before OOM");
    assert(gc_is_live(g_second) && "transitive mark must retain mid-chain object");

    /* 4. Free-list reuse: after a collection frees a block, the next
       same-size allocation must REUSE it (address comes back) rather than
       always bumping. We allocate a uniquely-sized object, drop it, collect,
       then allocate the same size and expect the freed address back. */
    heap_init();                          /* fresh arena for a deterministic check */
    oevm_oom_handler = default_oom_ref(); /* restore non-longjmp behaviour */
    {
        size_t sz = sizeof(struct Object) + 5 * sizeof(oop); /* distinct size class */
        oop x = gc_allocate(NULL, sz);
        /* x is unreachable (no root) -> next collect frees it */
        g_root = NULL; /* g_root never set to x; x is unreachable */
        gc_additional_roots_hook = root_hook;   /* roots nothing */
        gc_collect();
        gc_additional_roots_hook = NULL;
        oop y = gc_allocate(NULL, sz);
        assert(y == x && "freed block of same size class must be reused");
    }

    /* 5. Repeated collection must not corrupt the free list.
       A block freed in one cycle must not be pushed onto its bin twice across
       cycles (which would self-cycle the bin and alias a reused address). */
    heap_init();
    oevm_oom_handler = default_oom_ref();
    {
        size_t sz = sizeof(struct Object) + 7 * sizeof(oop); /* distinct size class */
        oop dead = gc_allocate(NULL, sz);                    /* unreferenced */
        (void)dead;
        g_root = NULL;
        gc_additional_roots_hook = root_hook;                /* roots nothing */
        gc_collect();                                        /* frees `dead` (push once) */
        gc_collect();                                        /* must NOT push it again */
        gc_additional_roots_hook = NULL;
        oop y = gc_allocate(NULL, sz);                       /* reuses the freed slot */
        oop z = gc_allocate(NULL, sz);                       /* must be a DISTINCT block */
        assert(y != z && "repeated GC corrupted the free list (address aliased)");
    }

    printf("test_arena: OK\n");
    return 0;
}
