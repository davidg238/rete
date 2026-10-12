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
    /* Indices reflect the Task 9a compaction (fiber/NLR rows removed). */
    assert(builtin_registry[3] == IntegerVtable);
    assert(builtin_registry[4] == FloatVtable);
    assert(builtin_registry[5] == MethodContextVtable);
    assert(builtin_registry[6] == SymbolVtable);
    assert(builtin_registry[7] == VtableVtable);

    /* Singletons: nil is the VM's NULL; true/false are interned symbols. */
    assert(oimg_singleton[0] == NULL);      /* nil == NULL in this VM */
    assert(oimg_singleton[1] != NULL);      /* true */
    assert(oimg_singleton[2] != NULL);      /* false */
    assert(builtin_registry[1] == oimg_singleton[1]);
    assert(builtin_registry[2] == oimg_singleton[2]);

    /* Kernel symbols are interned and registered at their (compacted) indices.
       (The NLR-only rows executeCleanup/resumeWith:/unwindTo:value: were removed
       in Task 9a; sender/value/== remain.) */
    assert(builtin_registry[12] != NULL);   /* sender */
    assert(strcmp(symbol_name((struct Symbol *)builtin_registry[12]), "sender") == 0);
    assert(builtin_registry[13] != NULL);   /* value */
    assert(strcmp(symbol_name((struct Symbol *)builtin_registry[13]), "value") == 0);
    assert(builtin_registry[11] != NULL);   /* == */
    assert(strcmp(symbol_name((struct Symbol *)builtin_registry[11]), "==") == 0);
    assert(strcmp(symbol_name((struct Symbol *)builtin_registry[14]), "debug:") == 0);
    assert(builtin_registry[15] == LargeIntegerVtable);

    /* Global kernel symbol vars are wired to the registered symbols. */
    extern oop Symbol_value;
    assert(Symbol_value == builtin_registry[13]);

    /* Version constant present (bumped 4->5 in Task 9a). */
    extern const uint32_t BUILTIN_REGISTRY_VERSION;
    assert(BUILTIN_REGISTRY_VERSION == 6);   /* rete#37 L1: accessViolation: deleted, rows renumbered */

    printf("kernel_boot PASS\n");
    return 0;
}
