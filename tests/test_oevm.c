#include "../src/oevm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>



/* External globals and functions */
extern oop gc_allocate(oop vtable, size_t size);
extern void gc_collect(void);
extern oop send(oop receiver, oop selector, int argc, oop *args);
extern oop run_interpreter(oop closure, oop receiver, int argc, oop *args);

extern oop Symbol_value;
extern oop IntegerVtable;
extern oop FloatVtable;
extern oop MethodContextVtable;
extern oop SymbolVtable;
extern oop VtableVtable;
extern oop PrimitiveClosureVtable;
extern oop BytecodeClosureVtable;
extern struct MethodContext *current_frame;

extern void (*gc_additional_roots_hook)(void);
extern void mark_object(oop ptr);

/* Symbols used in the tests */
static oop Symbol_double = NULL;

static void test_roots_callback(void) {
    mark_object(Symbol_double);
}

/* Helper to allocate symbols */
static oop create_symbol(const char *name) {
    oop sym = gc_allocate(SymbolVtable, sizeof(struct Symbol));
    ((struct Symbol *)sym)->name = name;
    ((struct Symbol *)sym)->length = strlen(name);
    return sym;
}

/* Helper to allocate and initialize Vtables */
static oop create_vtable(oop delegate) {
    oop vt = gc_allocate(VtableVtable, sizeof(struct Vtable));
    struct Vtable *v = (struct Vtable *)vt;
    v->delegate = delegate;
    v->capacity = 16;
    v->size = 0;
    v->bindings = malloc(v->capacity * sizeof(struct Binding));
    v->is_nested = 0;
    return vt;
}

/* Bind a symbol selector to a method closure in a Vtable */
static void add_method(oop vtable, oop symbol, oop method) {
    struct Vtable *v = (struct Vtable *)vtable;
    if (v->size >= v->capacity) {
        v->capacity *= 2;
        v->bindings = realloc(v->bindings, v->capacity * sizeof(struct Binding));
    }
    v->bindings[v->size].symbol = symbol;
    v->bindings[v->size].method = method;
    v->size++;
}

/* 
 * Primitive: Integer >> double
 * Returns receiver * 2
 */
static oop primitive_integer_double(oop receiver, int argc, oop *args) {
    (void)argc; (void)args;
    assert(IS_SMI(receiver));
    intptr_t val = UNTAG_SMI(receiver);
    return TAG_SMI(val * 2);
}

/* Primitive to execute bytecode closures */
static oop primitive_closure_value(oop receiver, int argc, oop *args) {
    (void)argc; (void)args;
    return run_interpreter(receiver, receiver, 0, NULL);
}

/* Setup the test VM environment */
static void setup_test_environment(void) {
    printf("Setting up OEVM test environment...\n");

    // Root meta-vtables
    VtableVtable = create_vtable(NULL);
    SymbolVtable = create_vtable(NULL);

    // Set GC additional roots hook
    gc_additional_roots_hook = test_roots_callback;

    // Dynamic symbols
    Symbol_value = create_symbol("value");
    Symbol_double = create_symbol("double");

    // Standard Vtables
    IntegerVtable = create_vtable(NULL);
    FloatVtable = create_vtable(NULL);
    MethodContextVtable = create_vtable(NULL);
    PrimitiveClosureVtable = create_vtable(NULL);
    BytecodeClosureVtable = create_vtable(NULL);

    // Bind Integer >> double to primitive_integer_double
    oop double_cl = gc_allocate(PrimitiveClosureVtable, sizeof(struct Closure));
    ((struct Closure *)double_cl)->home_ctx = NULL;
    ((struct Closure *)double_cl)->home_generation = 0;
    ((struct Closure *)double_cl)->entry_point = (void *)primitive_integer_double;
    add_method(IntegerVtable, Symbol_double, double_cl);
}

/* Test 1: Smi Pointer Tagging, Untagging, and Vtable routing */
static void test_pointer_tagging(void) {
    printf("Running pointer tagging and routing test...\n");
    oop five = TAG_SMI(5);
    assert(IS_SMI(five));
    assert(UNTAG_SMI(five) == 5);

    oop res = send(five, Symbol_double, 0, NULL);
    assert(IS_SMI(res));
    assert(UNTAG_SMI(res) == 10);
    printf("Pointer tagging test passed successfully!\n");
}

/* Test 2 (Monomorphic Inline Cache) removed: send_ic()/struct InlineCache were
   deleted in the de-recursify refactor, Phase 2 (fiber_derecursify_core_design.md;
   oevm.h:244-246), before this fork. No surviving feature to test here. */

/* Test 3: Garbage collection sweep phase */
static void test_garbage_collector(void) {
    printf("Running Garbage Collector test...\n");
    // Allocate dummy object
    oop dummy = gc_allocate(IntegerVtable, 16);
    assert(dummy != NULL);

    // Run collector: since dummy isn't reachable via registers/stack, it gets swept
    gc_collect();
    printf("Garbage Collector test passed successfully!\n");
}


int main(void) {
    setup_test_environment();
    test_pointer_tagging();
    test_garbage_collector();

    printf("\nAll OEVM integration tests PASSED successfully!\n");
    return 0;
}
