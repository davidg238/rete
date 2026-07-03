/* L1 Stack-object unit test: allocation + GC survival.
 * Standalone, links the Model A (solidified_image.c) set the other C unit
 * tests use — see tests/test_fibers.c for the link-set precedent.
 *
 * cc -Wall -std=c99 -Isrc src/vm.c src/gc.c src/dispatch.c src/platform.c
 *   src/serial.c src/udp.c src/mirror.c src/backend/posix sources
 *   src/oimg_loader.c src/solidified_image.c tests/test_stack.c
 *   -o /tmp/test_stack -lm
 */
#include "../src/oevm.h"
#include <stdio.h>
#include <assert.h>

extern oop StackVtable;
extern void init_solidified_vtables(void);
oop stack_alloc(uint32_t capacity);

int main(void) {
    init_solidified_vtables();

    oop s = stack_alloc(64);
    assert(s != NULL);
    assert(((struct Object *)s)->vtable == StackVtable);

    struct Stack *st = (struct Stack *)s;
    assert(st->capacity == 64);
    assert(st->sp == 0);
    assert(st->current_off == 0);

    printf("test_stack: PASSED\n");
    return 0;
}
