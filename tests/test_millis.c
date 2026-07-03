/* oevm/tests/test_millis.c — host TDD for the in-language millis clock.
   Build (Model-B link: kernel_boot.c present, solidified_image.c absent):
     cd ~/workspace/oevm && gcc -std=c99 -Isrc tests/test_millis.c \
       src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/kernel_boot.c \
       -lm -o /tmp/tmillis && /tmp/tmillis
   Verifies: (1) oevm_millis() is monotonic non-decreasing across a busy spin;
   (2) the `millis` selector dispatches through send() to a SmallInteger >= 0. */
#include "../src/oevm.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

extern uint64_t (*oevm_millis)(void);
extern oop SymbolVtable;
extern oop send(oop receiver, oop selector, int argc, oop *args);

int main(void) {
    /* (1) hook is monotonic */
    uint64_t a = oevm_millis();
    volatile unsigned long spin = 0;
    for (unsigned long i = 0; i < 50000000UL; i++) spin += i;
    uint64_t b = oevm_millis();
    assert(b >= a);

    /* (2) `millis` dispatches via send() to a non-negative SmallInteger */
    kernel_boot();
    struct Symbol s;
    s.base.vtable = SymbolVtable;
    s.name = "millis";
    s.length = 6;
    oop r = send(TAG_SMI(0), (oop)&s, 0, NULL);
    assert(IS_SMI(r));
    assert(UNTAG_SMI(r) >= 0);

    printf("test_millis: OK\n");
    return 0;
}
