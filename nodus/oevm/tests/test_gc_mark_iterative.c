/* GH#4: the mark phase must NOT recurse proportionally to object-graph depth.
   This test builds a deep reachable chain, then runs gc_collect() on a pthread
   with a small (256 KB) stack — modeling Zephyr's small fixed thread stacks.
   Under the OLD recursive mark, marking the chain recurses ~DEPTH frames deep
   and overflows the 256 KB stack → the process crashes (SIGSEGV). Under the
   iterative mark, C-stack use is O(1) and the whole chain survives.

   Build (NO -O so the old recursion is not tail-call-optimized away):
     gcc -std=c99 -Isrc -DOEVM_HEAP_SIZE=33554432 tests/test_gc_mark_iterative.c \
         src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/solidified_image.c \
         -lm -lpthread -o /tmp/tgmi && /tmp/tgmi */
#include "../src/oevm.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <pthread.h>

extern oop  gc_allocate(oop vtable, size_t size);
extern void gc_collect(void);
extern void heap_init(void);
extern int  gc_is_live(oop);
extern void gc_root(oop *slot);
extern void (*gc_additional_roots_hook)(void);

/* Plain instance vtable: the GC traces the payload word-by-word; the vtable
   word points at this static (not a heap object) so mark() skips it, and each
   node's slots[0] is the only traced child. */
static struct Vtable PlainVtable;

#define DEPTH 500000           /* 500k * 32 B/node = 16 MB < 32 MB arena */

static oop chain_head;         /* newest node; reaches the whole chain */
static oop chain_tail;         /* first/deepest node; survives only via the chain */

/* Precise root: marks just the chain head (transitively reaches all DEPTH nodes). */
static void root_chain(void) { gc_root(&chain_head); }

static volatile int head_live;
static volatile int tail_live;

/* Runs on a 256 KB pthread stack. Old recursive mark overflows it here. */
static void *collect_on_small_stack(void *arg) {
    (void)arg;
    gc_collect();
    head_live = gc_is_live(chain_head);
    tail_live = gc_is_live(chain_tail);
    return NULL;
}

static void test_deep_chain_no_recursion(void) {
    heap_init();
    /* Conservative C-stack scan (vm_c_stack_base) deleted in de-recursify Phase 3
       (fiber_derecursify_core_design.md 6d); precise roots only via
       gc_additional_roots_hook below — deterministic either way. */
    chain_head = NULL;
    for (size_t i = 0; i < DEPTH; i++) {
        oop n = gc_allocate((oop)&PlainVtable, sizeof(struct Object) + sizeof(oop));
        ((struct ObjectInstance *)n)->slots[0] = chain_head;
        chain_head = n;
        if (i == 0) chain_tail = n;      /* deepest node */
    }
    gc_additional_roots_hook = root_chain;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    pthread_t th;
    int rc = pthread_create(&th, &attr, collect_on_small_stack, NULL);
    assert(rc == 0);
    pthread_join(th, NULL);
    pthread_attr_destroy(&attr);

    gc_additional_roots_hook = NULL;
    assert(head_live);                   /* whole chain reachable from the root */
    assert(tail_live);                   /* deepest node traced too */
    printf("test_gc_mark_iterative: deep chain (%d) survived on 256 KB stack OK\n", DEPTH);
}

/* GH#4: when the worklist overflows, the heap-rescan recovery must still mark
   every reachable object (no false sweep) AND unreachable objects must still be
   reclaimed. Built with -DOEVM_MARK_STACK_SIZE=4 so a >4-object reachable graph
   forces overflow. */
#define REACH 64
static oop reach_nodes[REACH];     /* roots of a reachable graph (chain) */
static oop dead_node;              /* allocated, never rooted */

static void root_reach(void) { gc_root(&reach_nodes[0]); }  /* head reaches all REACH */

static void test_overflow_recovery(void) {
    heap_init();
    /* Build a REACH-long reachable chain: reach_nodes[i].slots[0] = reach_nodes[i+1]. */
    for (size_t i = 0; i < REACH; i++) {
        reach_nodes[i] = gc_allocate((oop)&PlainVtable, sizeof(struct Object) + sizeof(oop));
    }
    for (size_t i = 0; i + 1 < REACH; i++) {
        ((struct ObjectInstance *)reach_nodes[i])->slots[0] = reach_nodes[i + 1];
    }
    ((struct ObjectInstance *)reach_nodes[REACH - 1])->slots[0] = NULL;
    /* An unreachable object (rooted by nothing). */
    dead_node = gc_allocate((oop)&PlainVtable, sizeof(struct Object) + sizeof(oop));
    ((struct ObjectInstance *)dead_node)->slots[0] = NULL;

    gc_additional_roots_hook = root_reach;
    gc_collect();                  /* worklist (size 4) overflows; recovery rescans */
    gc_additional_roots_hook = NULL;

    for (size_t i = 0; i < REACH; i++) {
        assert(gc_is_live(reach_nodes[i]));   /* recovery re-found every reachable node */
    }
    assert(!gc_is_live(dead_node));           /* unreachable still swept (not falsely kept) */
    printf("test_gc_mark_iterative: overflow recovery kept %d reachable, swept dead OK\n", REACH);
}

int main(void) {
    test_deep_chain_no_recursion();
    test_overflow_recovery();
    printf("ALL TESTS PASSED\n");
    return 0;
}
