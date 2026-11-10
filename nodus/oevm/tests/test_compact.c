/* rete#477: safe-point compaction (src/gc.c gc_compact / gc_safe_point).

   A small arena is filled, every other block is unrooted so the free space is
   scattered in holes no larger request fits, and then the slide must:
     1. leave every survivor reachable from its (rewritten) roots, with its fields
        intact and its interior references rewritten to the new addresses;
     2. never move a Stack (frames and vm_stack point into it), freeing the space
        below it as a block of its own;
     3. turn the scatter into one run, so the request that failed now succeeds;
     4. run from the safe point only when the last sweep asked for it, and only at
        interpreter depth 1;
     5. refuse for good once an arena address has been handed out as a hash.

   Build (Model A link set, small heap):
     gcc -std=c99 -Isrc -DOEVM_HEAP_SIZE=8192 tests/test_compact.c <ENG_A> -lm */
#include "../src/oevm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <setjmp.h>

extern void gc_collect(void);
extern void heap_init(void);
extern int  gc_is_live(oop);
extern oop (*oevm_oom_handler)(size_t);
extern void (*gc_additional_roots_hook)(void);
extern int vm_interp_depth;

#define NODE_SLOTS 4
#define MAX_KEEP 512
static oop g_head;                 /* chain of the kept nodes, through slots[1] */
static oop g_mid;                  /* a second root into the middle of the chain */
static oop g_stack;                /* a pinned block between the nodes and the fill */
static oop g_fill[MAX_KEEP];       /* rooted filler that exhausts the arena */
static int g_nfill = 0;
static oop g_drop[MAX_KEEP];       /* the blocks unrooted to make the holes */
static int g_ndrop = 0;

static void roots_hook(void) {
    gc_root(&g_head);
    gc_root(&g_mid);
    gc_root(&g_stack);
    for (int i = 0; i < g_nfill; i++) gc_root(&g_fill[i]);
    for (int i = 0; i < g_ndrop; i++) if (g_drop[i] != NULL) gc_root(&g_drop[i]);
}

/* Not NULL: in the Model A unit link VtableVtable is NULL, and trace_one would read a
   NULL-vtable block as a Vtable. Generic blocks need a vtable that matches nothing. */
static struct Vtable node_vt;
#define NODE_VT ((oop)&node_vt)

static jmp_buf oom_jmp;
static volatile int oom_fired = 0;
static oop test_oom(size_t size) { (void)size; oom_fired = 1; longjmp(oom_jmp, 1); }

static struct ObjectInstance *node_new(intptr_t id) {
    struct ObjectInstance *n = (struct ObjectInstance *)gc_allocate(NODE_VT,
        sizeof(struct ObjectInstance) + NODE_SLOTS * sizeof(oop));
    n->slots[0] = TAG_SMI(id);
    n->slots[1] = NULL;
    n->slots[2] = TAG_SMI(id * 7);
    n->slots[3] = NULL;
    return n;
}

static int try_alloc(size_t size, oop *out) {
    oom_fired = 0;
    if (setjmp(oom_jmp) == 0) { *out = gc_allocate(NODE_VT, size); return 1; }
    return 0;
}

/* Fill the arena: nodes 0..2n-1 (odd ones are the holes), the Stack, then rooted
   filler until the bump pointer is gone. */
static int build(int nodes) {
    heap_init();
    g_head = g_mid = g_stack = NULL;
    g_nfill = g_ndrop = 0;
    struct ObjectInstance *prev = NULL;
    for (int i = 0; i < nodes; i++) {
        struct ObjectInstance *n = node_new(i);
        if (i % 2 == 1) { g_drop[g_ndrop++] = (oop)n; continue; }
        if (prev) prev->slots[1] = (oop)n; else g_head = (oop)n;
        if (i == nodes / 2) g_mid = (oop)n;
        n->slots[3] = g_head;                  /* a back-reference to rewrite too */
        prev = n;
        if (i == nodes / 2) g_stack = stack_alloc(8);   /* pinned, mid-chain */
    }
    oom_fired = 0;
    if (setjmp(oom_jmp) == 0) {
        for (;;) {
            oop f = gc_allocate(NODE_VT, sizeof(struct ObjectInstance) + 2 * sizeof(oop));
            ((struct ObjectInstance *)f)->slots[0] = TAG_SMI(1000 + g_nfill);
            ((struct ObjectInstance *)f)->slots[1] = g_head;
            g_fill[g_nfill++] = f;
            assert(g_nfill < MAX_KEEP);
        }
    }
    for (int i = 0; i < g_ndrop; i++) g_drop[i] = NULL;   /* the holes */
    gc_collect();
    return g_nfill;
}

static void check_survivors(int nodes, oop head_before, oop stack_before) {
    int i = 0;
    for (oop o = g_head; o != NULL; o = ((struct ObjectInstance *)o)->slots[1], i += 2) {
        struct ObjectInstance *n = (struct ObjectInstance *)o;
        assert(gc_is_live(o));
        assert(n->slots[0] == TAG_SMI(i) && n->slots[2] == TAG_SMI(i * 7));
        assert(n->slots[3] == g_head && "interior reference rewritten");
    }
    assert(i == nodes + (nodes % 2) && "every kept node still on the chain");
    assert(((struct ObjectInstance *)g_mid)->slots[0] == TAG_SMI(nodes / 2) && "second root rewritten");
    for (int k = 0; k < g_nfill; k++) {
        struct ObjectInstance *f = (struct ObjectInstance *)g_fill[k];
        assert(gc_is_live(g_fill[k]));
        assert(f->slots[0] == TAG_SMI(1000 + k) && f->slots[1] == g_head);
    }
    assert(g_head == head_before && "the first block has nowhere lower to go");
    assert(g_stack == stack_before && "a Stack never moves");
    assert(((struct Stack *)g_stack)->capacity == 8);
}

int main(void) {
    oevm_oom_handler = test_oom;
    gc_additional_roots_hook = roots_hook;
    static struct Vtable fake_stack_vt;           /* Model A sets no StackVtable */
    if (StackVtable == NULL) StackVtable = (oop)&fake_stack_vt;
    const int nodes = 40;
    const size_t big = 12 * sizeof(struct ObjectInstance) + 12 * sizeof(oop);  /* > one hole */
    oop got;

    /* 1-3. The slide itself. */
    build(nodes);
    oop head0 = g_head, stack0 = g_stack, fill0 = g_fill[0];
    assert(!try_alloc(big, &got) && "the holes are scattered: no one fits the request");
    assert(oevm_heap_stats().largest_free_block < big);
    assert(gc_compact() == 1);
    check_survivors(nodes, head0, stack0);
    assert(g_fill[0] < fill0 && "the fill slid down into the holes");
    assert(oevm_heap_stats().compactions == 1);
    assert(oevm_heap_stats().largest_free_block >= big);
    assert(try_alloc(big, &got) && "the request fits after the slide");

    /* 4. The trigger: the failed request above made the sweep ask for a slide. */
    build(nodes);
    head0 = g_head; stack0 = g_stack;
    assert(!try_alloc(big, &got));               /* records the request, sweeps */
    vm_interp_depth = 2;
    gc_safe_point();
    assert(oevm_heap_stats().compactions == 0 && "never inside a nested activation");
    vm_interp_depth = 1;
    gc_safe_point();
    assert(oevm_heap_stats().compactions == 1 && "the sweep's request is honoured at depth 1");
    check_survivors(nodes, head0, stack0);
    gc_safe_point();
    assert(oevm_heap_stats().compactions == 1 && "one slide per request, not one per wait");

    /* 5. An identity hash stops compaction for good. */
    build(nodes);
    gc_note_identity_hash(g_mid);
    assert(gc_compact() == 0);
    assert(oevm_heap_stats().compact_skips == 1 && oevm_heap_stats().compactions == 0);

    gc_additional_roots_hook = NULL;
    printf("test_compact: slide, pin, trigger and identity-hash refusal OK (%d nodes, %d fill)\n",
           nodes, g_nfill);
    return 0;
}
