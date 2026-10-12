/* Context heap-object unit test: allocation + chaining + GC survival
 * (de-recursify deferred-lift Phase 1, Task 1). LIVE — the compiler emits heap
 * Contexts for escaping captures (2026-07-04 decision); this test exercises the allocation/trace path directly.
 *
 * Standalone, links the Model A (solidified_image.c) set the other C unit
 * tests use — see tests/test_stack.c for the link-set precedent.
 *
 * cc -Wall -std=c99 -Isrc src/vm.c src/gc.c src/dispatch.c src/platform.c
 *   src/serial.c src/udp.c src/mirror.c src/backend/posix sources
 *   src/oimg_loader.c src/solidified_image.c tests/test_context.c
 *   -o /tmp/test_context -lm
 */
#include "../src/oevm.h"
#include "../src/frame_abi.h"
#include <stdio.h>
#include <assert.h>

extern oop ContextVtable;
extern void init_solidified_vtables(void);
extern void gc_collect(void);
extern void gc_temp_root_push(oop);
extern void gc_temp_roots_clear(void);
oop context_alloc(uint32_t num_slots, oop outer);

int main(void) {
    init_solidified_vtables();
    assert(ContextVtable != NULL);

    /* An outer context with one slot holding a sentinel SMI. */
    oop outer = context_alloc(1, NULL);
    assert(outer != NULL);
    assert(((struct Object *)outer)->vtable == ContextVtable);
    ((struct Context *)outer)->slots[0] = TAG_SMI(1234);

    /* An inner context chained to outer, two slots. */
    oop inner = context_alloc(2, outer);
    struct Context *ic = (struct Context *)inner;
    assert(ic->num_slots == 2);
    assert(ic->outer == outer);
    assert(ic->slots[0] == NULL && ic->slots[1] == NULL);  /* nil-initialized */

    /* GC must trace outer + slots: keep only `inner` reachable via a temp root. */
    gc_temp_root_push(inner);
    gc_collect();
    /* After collection, outer (reachable only through inner->outer) survives and
       its slot value is intact. */
    assert(ic->outer == outer);
    assert(((struct Context *)outer)->slots[0] == TAG_SMI(1234));
    gc_temp_roots_clear();

    printf("CONTEXT OK\n");
    return 0;
}
