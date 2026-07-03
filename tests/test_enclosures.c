#include "../src/oevm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* Global variables and function declarations */
extern oop IntegerVtable;
extern oop VtableVtable;
extern oop SymbolVtable;
extern oop BytecodeClosureVtable;

extern void (*gc_additional_roots_hook)(void);
extern void mark_object(oop ptr);
extern void gc_collect(void);
extern oop gc_allocate(oop vtable, size_t size);
extern oop send(oop receiver, oop selector, int argc, oop *args);

static oop Symbol_outerMethod = NULL;
static oop Symbol_shadowedMethod = NULL;
static oop Symbol_runInner = NULL;
static oop Symbol_runShadowedImplicit = NULL;
static oop Symbol_runShadowedExplicit = NULL;
static oop Symbol_outer = NULL;

static oop OuterVtable = NULL;
static oop SuperInnerVtable = NULL;
static oop InnerVtable = NULL;

static oop inner_instance_root = NULL;

static void test_roots_callback(void) {
    mark_object(Symbol_outerMethod);
    mark_object(Symbol_shadowedMethod);
    mark_object(Symbol_runInner);
    mark_object(Symbol_runShadowedImplicit);
    mark_object(Symbol_runShadowedExplicit);
    mark_object(Symbol_outer);
    mark_object(OuterVtable);
    mark_object(SuperInnerVtable);
    mark_object(InnerVtable);
    mark_object(inner_instance_root);
}

static oop create_symbol(const char *name) {
    oop sym = gc_allocate(SymbolVtable, sizeof(struct Symbol));
    ((struct Symbol *)sym)->name = name;
    ((struct Symbol *)sym)->length = strlen(name);
    return sym;
}

static oop create_test_vtable(oop delegate, int is_nested) {
    oop vt = gc_allocate(VtableVtable, sizeof(struct Vtable));
    struct Vtable *v = (struct Vtable *)vt;
    v->delegate = delegate;
    v->capacity = 16;
    v->size = 0;
    v->bindings = malloc(v->capacity * sizeof(struct Binding));
    v->is_nested = is_nested;
    return vt;
}

static void add_test_method(oop vtable, oop symbol, oop method) {
    struct Vtable *v = (struct Vtable *)vtable;
    if (v->size >= v->capacity) {
        v->capacity *= 2;
        v->bindings = realloc(v->bindings, v->capacity * sizeof(struct Binding));
    }
    v->bindings[v->size].symbol = symbol;
    v->bindings[v->size].method = method;
    v->size++;
}

static void setup_enclosure_environment(void) {
    // 1. Initialize symbols
    Symbol_outerMethod = create_symbol("outerMethod");
    Symbol_shadowedMethod = create_symbol("shadowedMethod");
    Symbol_runInner = create_symbol("runInner");
    Symbol_runShadowedImplicit = create_symbol("runShadowedImplicit");
    Symbol_runShadowedExplicit = create_symbol("runShadowedExplicit");
    Symbol_outer = create_symbol("outer");

    // 2. Set up Vtables
    OuterVtable = create_test_vtable(NULL, 0); // Not nested
    SuperInnerVtable = create_test_vtable(NULL, 0); // Not nested
    InnerVtable = create_test_vtable(SuperInnerVtable, 1); // Nested!

    // 3. Compile Outer >> outerMethod returning 42
    struct CompiledMethod *cm_outer_method = malloc(sizeof(struct CompiledMethod));
    cm_outer_method->num_registers = 4;
    cm_outer_method->num_arguments = 0;
    cm_outer_method->instruction_count = 2;
    cm_outer_method->constant_count = 1;
    uint32_t *insts_outer = malloc(2 * sizeof(uint32_t));
    insts_outer[0] = 0x00000300; // OP_LD_CONST dest=3, src1=0 (SMI 42)
    insts_outer[1] = 0x00000304; // OP_RET R3
    cm_outer_method->instructions = insts_outer;
    oop *consts_outer = malloc(1 * sizeof(oop));
    consts_outer[0] = TAG_SMI(42);
    cm_outer_method->constants = consts_outer;

    oop cl_outer_method = gc_allocate(BytecodeClosureVtable, sizeof(struct Closure));
    ((struct Closure *)cl_outer_method)->home_ctx = NULL;
    ((struct Closure *)cl_outer_method)->home_generation = 0;
    ((struct Closure *)cl_outer_method)->entry_point = cm_outer_method;
    add_test_method(OuterVtable, Symbol_outerMethod, cl_outer_method);

    // 4. Compile Outer >> shadowedMethod returning 200
    struct CompiledMethod *cm_outer_shadowed = malloc(sizeof(struct CompiledMethod));
    cm_outer_shadowed->num_registers = 4;
    cm_outer_shadowed->num_arguments = 0;
    cm_outer_shadowed->instruction_count = 2;
    cm_outer_shadowed->constant_count = 1;
    uint32_t *insts_outer_shadowed = malloc(2 * sizeof(uint32_t));
    insts_outer_shadowed[0] = 0x00000300; // OP_LD_CONST dest=3, src1=0 (SMI 200)
    insts_outer_shadowed[1] = 0x00000304; // OP_RET R3
    cm_outer_shadowed->instructions = insts_outer_shadowed;
    oop *consts_outer_shadowed = malloc(1 * sizeof(oop));
    consts_outer_shadowed[0] = TAG_SMI(200);
    cm_outer_shadowed->constants = consts_outer_shadowed;

    oop cl_outer_shadowed = gc_allocate(BytecodeClosureVtable, sizeof(struct Closure));
    ((struct Closure *)cl_outer_shadowed)->home_ctx = NULL;
    ((struct Closure *)cl_outer_shadowed)->home_generation = 0;
    ((struct Closure *)cl_outer_shadowed)->entry_point = cm_outer_shadowed;
    add_test_method(OuterVtable, Symbol_shadowedMethod, cl_outer_shadowed);

    // 5. Compile SuperInner >> shadowedMethod returning 100
    struct CompiledMethod *cm_super_shadowed = malloc(sizeof(struct CompiledMethod));
    cm_super_shadowed->num_registers = 4;
    cm_super_shadowed->num_arguments = 0;
    cm_super_shadowed->instruction_count = 2;
    cm_super_shadowed->constant_count = 1;
    uint32_t *insts_super_shadowed = malloc(2 * sizeof(uint32_t));
    insts_super_shadowed[0] = 0x00000300; // OP_LD_CONST dest=3, src1=0 (SMI 100)
    insts_super_shadowed[1] = 0x00000304; // OP_RET R3
    cm_super_shadowed->instructions = insts_super_shadowed;
    oop *consts_super_shadowed = malloc(1 * sizeof(oop));
    consts_super_shadowed[0] = TAG_SMI(100);
    cm_super_shadowed->constants = consts_super_shadowed;

    oop cl_super_shadowed = gc_allocate(BytecodeClosureVtable, sizeof(struct Closure));
    ((struct Closure *)cl_super_shadowed)->home_ctx = NULL;
    ((struct Closure *)cl_super_shadowed)->home_generation = 0;
    ((struct Closure *)cl_super_shadowed)->entry_point = cm_super_shadowed;
    add_test_method(SuperInnerVtable, Symbol_shadowedMethod, cl_super_shadowed);

    // 6. Compile Inner >> runInner using OP_SEND_IMPLICIT for outerMethod
    struct CompiledMethod *cm_run_inner = malloc(sizeof(struct CompiledMethod));
    cm_run_inner->num_registers = 4;
    cm_run_inner->num_arguments = 0;
    cm_run_inner->instruction_count = 3;
    cm_run_inner->constant_count = 1;
    uint32_t *insts_run_inner = malloc(3 * sizeof(uint32_t));
    insts_run_inner[0] = 0x00000301; // OP_LD_REG dest=3, src1=0 (R3 = self)
    insts_run_inner[1] = 0x0300000F; // OP_SEND_IMPLICIT dest=0, src1=0 ("outerMethod"), src2=3
    insts_run_inner[2] = 0x00000304; // OP_RET R3
    cm_run_inner->instructions = insts_run_inner;
    oop *consts_run_inner = malloc(1 * sizeof(oop));
    consts_run_inner[0] = Symbol_outerMethod;
    cm_run_inner->constants = consts_run_inner;

    oop cl_run_inner = gc_allocate(BytecodeClosureVtable, sizeof(struct Closure));
    ((struct Closure *)cl_run_inner)->home_ctx = NULL;
    ((struct Closure *)cl_run_inner)->home_generation = 0;
    ((struct Closure *)cl_run_inner)->entry_point = cm_run_inner;
    add_test_method(InnerVtable, Symbol_runInner, cl_run_inner);

    // 7. Compile Inner >> runShadowedImplicit using OP_SEND_IMPLICIT for shadowedMethod
    struct CompiledMethod *cm_run_shadowed_imp = malloc(sizeof(struct CompiledMethod));
    cm_run_shadowed_imp->num_registers = 4;
    cm_run_shadowed_imp->num_arguments = 0;
    cm_run_shadowed_imp->instruction_count = 3;
    cm_run_shadowed_imp->constant_count = 1;
    uint32_t *insts_shadowed_imp = malloc(3 * sizeof(uint32_t));
    insts_shadowed_imp[0] = 0x00000301; // OP_LD_REG dest=3, src1=0 (R3 = self)
    insts_shadowed_imp[1] = 0x0300000F; // OP_SEND_IMPLICIT dest=0, src1=0 ("shadowedMethod"), src2=3
    insts_shadowed_imp[2] = 0x00000304; // OP_RET R3
    cm_run_shadowed_imp->instructions = insts_shadowed_imp;
    oop *consts_shadowed_imp = malloc(1 * sizeof(oop));
    consts_shadowed_imp[0] = Symbol_shadowedMethod;
    cm_run_shadowed_imp->constants = consts_shadowed_imp;

    oop cl_run_shadowed_imp = gc_allocate(BytecodeClosureVtable, sizeof(struct Closure));
    ((struct Closure *)cl_run_shadowed_imp)->home_ctx = NULL;
    ((struct Closure *)cl_run_shadowed_imp)->home_generation = 0;
    ((struct Closure *)cl_run_shadowed_imp)->entry_point = cm_run_shadowed_imp;
    add_test_method(InnerVtable, Symbol_runShadowedImplicit, cl_run_shadowed_imp);

    // 8. Compile Inner >> runShadowedExplicit using OP_SEND for shadowedMethod to self
    struct CompiledMethod *cm_run_shadowed_exp = malloc(sizeof(struct CompiledMethod));
    cm_run_shadowed_exp->num_registers = 4;
    cm_run_shadowed_exp->num_arguments = 0;
    cm_run_shadowed_exp->instruction_count = 3;
    cm_run_shadowed_exp->constant_count = 1;
    uint32_t *insts_shadowed_exp = malloc(3 * sizeof(uint32_t));
    insts_shadowed_exp[0] = 0x00000301; // OP_LD_REG dest=3, src1=0 (R3 = self)
    insts_shadowed_exp[1] = 0x03000002; // OP_SEND dest=0, src1=0 ("shadowedMethod"), src2=3
    insts_shadowed_exp[2] = 0x00000304; // OP_RET R3
    cm_run_shadowed_exp->instructions = insts_shadowed_exp;
    oop *consts_shadowed_exp = malloc(1 * sizeof(oop));
    consts_shadowed_exp[0] = Symbol_shadowedMethod;
    cm_run_shadowed_exp->constants = consts_shadowed_exp;

    oop cl_run_shadowed_exp = gc_allocate(BytecodeClosureVtable, sizeof(struct Closure));
    ((struct Closure *)cl_run_shadowed_exp)->home_ctx = NULL;
    ((struct Closure *)cl_run_shadowed_exp)->home_generation = 0;
    ((struct Closure *)cl_run_shadowed_exp)->entry_point = cm_run_shadowed_exp;
    add_test_method(InnerVtable, Symbol_runShadowedExplicit, cl_run_shadowed_exp);
}

int main(void) {
    printf("Setting up OEVM Enclosures and Nested Classes test environment...\n");
    
    // Set up standard roots callback
    gc_additional_roots_hook = test_roots_callback;

    // Boot VM Vtables (from solidified code stub or manual initialization)
    extern void init_solidified_vtables(void);
    init_solidified_vtables();

    setup_enclosure_environment();

    // 1. Instantiate Outer (0 slots)
    oop outer_inst = gc_allocate(OuterVtable, sizeof(struct ObjectInstance));
    printf("Created Outer instance at %p\n", (void *)outer_inst);

    // 2. Instantiate Inner (1 slot for 'outer' reference)
    oop inner_inst = gc_allocate(InnerVtable, sizeof(struct ObjectInstance) + 1 * sizeof(oop));
    struct ObjectInstance *inner_obj = (struct ObjectInstance *)inner_inst;
    inner_obj->slots[0] = outer_inst; // Establish enclosure!
    printf("Created Inner instance at %p, slot[0] (outer) = %p\n", (void *)inner_inst, (void *)inner_obj->slots[0]);

    // Keep it alive during GC verification
    inner_instance_root = inner_inst;

    // 3. Test reflection primitive 'outer'
    printf("Testing 'outer' reflection primitive...\n");
    oop resolved_outer = send(inner_inst, Symbol_outer, 0, NULL);
    printf("Resolved outer via reflection: %p (expected %p)\n", (void *)resolved_outer, (void *)outer_inst);
    assert(resolved_outer == outer_inst);
    printf("Reflection 'outer' lookup passed!\n");

    // 4. Run Inner >> runInner to test implicit lexical lookup of outerMethod
    printf("Testing implicit lexical lookup (Inner >> runInner)...\n");
    oop res_inner = send(inner_inst, Symbol_runInner, 0, NULL);
    printf("Result of runInner: %ld (expected 42)\n", (long)UNTAG_SMI(res_inner));
    assert(UNTAG_SMI(res_inner) == 42);
    printf("Implicit lexical lookup passed!\n");

    // 5. Test precedence: Implicit send of shadowedMethod resolves to lexical scope (Outer -> 200)
    printf("Testing implicit send precedence (shadowedMethod -> Outer -> 200)...\n");
    oop res_shadowed_imp = send(inner_inst, Symbol_runShadowedImplicit, 0, NULL);
    printf("Result of runShadowedImplicit: %ld (expected 200)\n", (long)UNTAG_SMI(res_shadowed_imp));
    assert(UNTAG_SMI(res_shadowed_imp) == 200);
    printf("Implicit send precedence passed!\n");

    // 6. Test precedence: Explicit send of shadowedMethod resolves to inheritance of self (SuperInner -> 100)
    printf("Testing explicit send precedence (shadowedMethod -> SuperInner -> 100)...\n");
    oop res_shadowed_exp = send(inner_inst, Symbol_runShadowedExplicit, 0, NULL);
    printf("Result of runShadowedExplicit: %ld (expected 100)\n", (long)UNTAG_SMI(res_shadowed_exp));
    assert(UNTAG_SMI(res_shadowed_exp) == 100);
    printf("Explicit send precedence passed!\n");

    // 7. Verify GC works cleanly and traces through slot 0 (outer) correctly
    printf("Running Garbage Collector to verify references...\n");
    gc_collect();
    printf("GC completed successfully!\n");

    // Verify Inner and Outer instances are still alive and intact
    assert(inner_inst->vtable == InnerVtable);
    assert(inner_obj->slots[0] == outer_inst);
    assert(outer_inst->vtable == OuterVtable);

    // Run again after GC to confirm stability
    oop res_post_gc = send(inner_inst, Symbol_runInner, 0, NULL);
    assert(UNTAG_SMI(res_post_gc) == 42);
    printf("Post-GC execution stability confirmed!\n");

    printf("\nAll OEVM Nested Class & Lexical Enclosure tests PASSED successfully!\n");
    return 0;
}
