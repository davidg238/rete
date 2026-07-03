/* oevm/tests/test_kernel_boot.c
 *
 * Task 11 RED/GREEN test: after kernel_boot() the firmware builtin registry,
 * the global vtable pointers, and the singleton table are all populated.
 *
 * Model-B-style link: kernel_boot.c present, solidified_image.c absent.
 */
#include "../src/oevm.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

int main(void) {
    kernel_boot();

    /* Vtable builtins resolve to the assigned global vtable pointers. */
    assert(builtin_registry[3] == IntegerVtable);
    assert(builtin_registry[4] == FloatVtable);
    assert(builtin_registry[5] == MethodContextVtable);
    assert(builtin_registry[7] == SymbolVtable);
    assert(builtin_registry[8] == VtableVtable);

    /* Singletons: nil is the VM's NULL; true/false are interned symbols. */
    assert(oimg_singleton[0] == NULL);      /* nil == NULL in this VM */
    assert(oimg_singleton[1] != NULL);      /* true */
    assert(oimg_singleton[2] != NULL);      /* false */
    assert(builtin_registry[1] == oimg_singleton[1]);
    assert(builtin_registry[2] == oimg_singleton[2]);

    /* At least one kernel symbol is interned and registered at its index. */
    assert(builtin_registry[19] != NULL);   /* unwindTo:value: */
    assert(strcmp(symbol_name((struct Symbol *)builtin_registry[19]), "unwindTo:value:") == 0);
    assert(builtin_registry[20] != NULL);   /* value */
    assert(strcmp(symbol_name((struct Symbol *)builtin_registry[20]), "value") == 0);
    assert(builtin_registry[15] != NULL);   /* == */
    assert(strcmp(symbol_name((struct Symbol *)builtin_registry[15]), "==") == 0);

    /* Global kernel symbol vars are wired to the registered symbols. */
    extern oop Symbol_value, Symbol_unwindTo_value;
    assert(Symbol_value == builtin_registry[20]);
    assert(Symbol_unwindTo_value == builtin_registry[19]);

    /* Version constant present. */
    extern const uint32_t BUILTIN_REGISTRY_VERSION;
    assert(BUILTIN_REGISTRY_VERSION == 2);

    printf("kernel_boot PASS\n");
    return 0;
}
