#include "../src/oevm.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

/* Global declarations from solidified image */
extern void init_solidified_vtables(void);
extern const struct Vtable Vtable_VtableVtable;
extern const struct Vtable Vtable_CounterVtable;
extern oop CounterVtable;
extern const struct Symbol Symbol_increment;

/* Roots from gc.c */
extern uintptr_t flash_rom_start;
extern uintptr_t flash_rom_end;
extern void (*gc_additional_roots_hook)(void);
extern void gc_root(oop *slot);
extern void gc_collect(void);

static void test_roots_callback(void) {
    gc_root(&CounterVtable);
    gc_root(&(oop)&Symbol_increment);
}

int main(void) {
    printf("Initializing solidified VM environment...\n");
    init_solidified_vtables();

    // Configure simulated Flash ROM range
    uintptr_t addr1 = (uintptr_t)&Vtable_VtableVtable;
    uintptr_t addr2 = (uintptr_t)&Vtable_CounterVtable;
    if (addr1 < addr2) {
        flash_rom_start = addr1;
        flash_rom_end = addr2 + sizeof(struct Vtable);
    } else {
        flash_rom_start = addr2;
        flash_rom_end = addr1 + sizeof(struct Vtable);
    }
    printf("Simulated Flash ROM: %p - %p\n", (void *)flash_rom_start, (void *)flash_rom_end);

    // Register GC additional roots hook
    gc_additional_roots_hook = test_roots_callback;

    // 1. Allocate dynamic object instance of Counter (Counter has 1 slot)
    extern oop gc_allocate(oop vtable, size_t size);
    oop counter = gc_allocate(CounterVtable, sizeof(struct ObjectInstance) + 1 * sizeof(oop));
    assert(counter != NULL);
    assert(counter->vtable == CounterVtable);
    
    struct ObjectInstance *inst = (struct ObjectInstance *)counter;
    inst->slots[0] = TAG_SMI(10); // Start count at 10

    printf("Created dynamic Counter instance at %p, slot[0] = %ld\n", 
           (void *)counter, (long)UNTAG_SMI(inst->slots[0]));

    // Verify selector/method routing using the solidified binding
    oop selector = (oop)&Symbol_increment;
    printf("Sending 'increment' to Counter instance...\n");
    
    extern oop send(oop receiver, oop selector, int argc, oop *args);
    oop result = send(counter, selector, 0, NULL);

    printf("Result: %ld\n", (long)UNTAG_SMI(result));
    assert(IS_SMI(result));
    assert(UNTAG_SMI(result) == 11);
    assert(UNTAG_SMI(inst->slots[0]) == 11);

    printf("Increment method executed successfully from Flash ROM!\n");

    // 2. Run Garbage Collector to verify Flash objects are not freed or corrupted
    printf("Running garbage collector sweep...\n");
    gc_collect();

    // Verify object still alive and has correct vtable pointer
    assert(counter->vtable == CounterVtable);
    assert(UNTAG_SMI(inst->slots[0]) == 11);
    printf("Garbage collection with Flash boundaries passed successfully!\n");

    printf("\nAll OEVM Execute-in-Place (XIP) Solidification tests PASSED!\n");
    return 0;
}
