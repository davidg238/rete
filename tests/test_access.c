#include "../src/oevm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

extern oop IntegerVtable;
extern oop VtableVtable;
extern oop SymbolVtable;
extern oop BytecodeClosureVtable;

extern void (*gc_additional_roots_hook)(void);
extern void mark_object(oop ptr);
extern void gc_collect(void);
extern oop gc_allocate(oop vtable, size_t size);
extern oop send(oop receiver, oop selector, int argc, oop *args);

static oop Symbol_protectedMethod = NULL;
static oop Symbol_privateMethod = NULL;
static oop Symbol_runProtectedExplicit = NULL;
static oop Symbol_runPrivateOnSelf = NULL;
static oop Symbol_runProtectedOnOther = NULL;
static oop Symbol_accessViolation = NULL;

static oop BaseVtable = NULL;
static oop SubVtable = NULL;
static oop HelperVtable = NULL;

static oop base_instance_root = NULL;
static oop sub_instance_root = NULL;
static oop helper_instance_root = NULL;

static void test_roots_callback(void) {
    mark_object(Symbol_protectedMethod);
    mark_object(Symbol_privateMethod);
    mark_object(Symbol_runProtectedExplicit);
    mark_object(Symbol_runPrivateOnSelf);
    mark_object(Symbol_runProtectedOnOther);
    mark_object(Symbol_accessViolation);
    mark_object(BaseVtable);
    mark_object(SubVtable);
    mark_object(HelperVtable);
    mark_object(base_instance_root);
    mark_object(sub_instance_root);
    mark_object(helper_instance_root);
}

static oop create_symbol(const char *name) {
    oop sym = gc_allocate(SymbolVtable, sizeof(struct Symbol));
    ((struct Symbol *)sym)->name = name;
    ((struct Symbol *)sym)->length = strlen(name);
    return sym;
}

static oop create_test_vtable(oop delegate) {
    oop vt = gc_allocate(VtableVtable, sizeof(struct Vtable));
    struct Vtable *v = (struct Vtable *)vt;
    v->delegate = delegate;
    v->capacity = 16;
    v->size = 0;
    v->bindings = malloc(v->capacity * sizeof(struct Binding));
    v->is_nested = 0;
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

static void setup_access_environment(void) {
    // Initialize symbols
    Symbol_protectedMethod = create_symbol("protectedMethod");
    Symbol_privateMethod = create_symbol("privateMethod");
    Symbol_runProtectedExplicit = create_symbol("runProtectedExplicit");
    Symbol_runPrivateOnSelf = create_symbol("runPrivateOnSelf");
    Symbol_runProtectedOnOther = create_symbol("runProtectedOnOther");
    Symbol_accessViolation = create_symbol("accessViolation:");

    // Vtables
    BaseVtable = create_test_vtable(NULL);
    SubVtable = create_test_vtable(BaseVtable);
    HelperVtable = create_test_vtable(NULL);

    // 1. Base >> protectedMethod (access = PROTECTED): returns 42
    struct CompiledMethod *cm_prot = malloc(sizeof(struct CompiledMethod));
    cm_prot->num_registers = 4;
    cm_prot->num_arguments = 0;
    cm_prot->instruction_count = 2;
    cm_prot->constant_count = 1;
    cm_prot->access_modifier = ACCESS_PROTECTED;
    uint32_t *insts_prot = malloc(2 * sizeof(uint32_t));
    insts_prot[0] = 0x00000300; // OP_LD_CONST dest=3, src1=0 (42)
    insts_prot[1] = 0x00000304; // OP_RET R3
    cm_prot->instructions = insts_prot;
    oop *consts_prot = malloc(1 * sizeof(oop));
    consts_prot[0] = TAG_SMI(42);
    cm_prot->constants = consts_prot;

    oop cl_prot = gc_allocate(BytecodeClosureVtable, sizeof(struct Closure));
    ((struct Closure *)cl_prot)->home_ctx = NULL;
    ((struct Closure *)cl_prot)->home_generation = 0;
    ((struct Closure *)cl_prot)->entry_point = cm_prot;
    add_test_method(BaseVtable, Symbol_protectedMethod, cl_prot);

    // 2. Base >> privateMethod (access = PRIVATE): returns 77
    struct CompiledMethod *cm_priv = malloc(sizeof(struct CompiledMethod));
    cm_priv->num_registers = 4;
    cm_priv->num_arguments = 0;
    cm_priv->instruction_count = 2;
    cm_priv->constant_count = 1;
    cm_priv->access_modifier = ACCESS_PRIVATE;
    uint32_t *insts_priv = malloc(2 * sizeof(uint32_t));
    insts_priv[0] = 0x00000300; // OP_LD_CONST dest=3, src1=0 (77)
    insts_priv[1] = 0x00000304; // OP_RET R3
    cm_priv->instructions = insts_priv;
    oop *consts_priv = malloc(1 * sizeof(oop));
    consts_priv[0] = TAG_SMI(77);
    cm_priv->constants = consts_priv;

    oop cl_priv = gc_allocate(BytecodeClosureVtable, sizeof(struct Closure));
    ((struct Closure *)cl_priv)->home_ctx = NULL;
    ((struct Closure *)cl_priv)->home_generation = 0;
    ((struct Closure *)cl_priv)->entry_point = cm_priv;
    add_test_method(BaseVtable, Symbol_privateMethod, cl_priv);

    // 3. Base >> runProtectedExplicit (access = PUBLIC): sends protectedMethod to self
    struct CompiledMethod *cm_run_prot = malloc(sizeof(struct CompiledMethod));
    cm_run_prot->num_registers = 4;
    cm_run_prot->num_arguments = 0;
    cm_run_prot->instruction_count = 3;
    cm_run_prot->constant_count = 1;
    cm_run_prot->access_modifier = ACCESS_PUBLIC;
    uint32_t *insts_run_prot = malloc(3 * sizeof(uint32_t));
    insts_run_prot[0] = 0x00000301; // OP_LD_REG dest=3, src1=0 (R3 = self)
    insts_run_prot[1] = 0x03000002; // OP_SEND dest=0, src1=0 ("protectedMethod"), src2=3
    insts_run_prot[2] = 0x00000304; // OP_RET R3
    cm_run_prot->instructions = insts_run_prot;
    oop *consts_run_prot = malloc(1 * sizeof(oop));
    consts_run_prot[0] = Symbol_protectedMethod;
    cm_run_prot->constants = consts_run_prot;

    oop cl_run_prot = gc_allocate(BytecodeClosureVtable, sizeof(struct Closure));
    ((struct Closure *)cl_run_prot)->home_ctx = NULL;
    ((struct Closure *)cl_run_prot)->home_generation = 0;
    ((struct Closure *)cl_run_prot)->entry_point = cm_run_prot;
    add_test_method(BaseVtable, Symbol_runProtectedExplicit, cl_run_prot);

    // 4. Base >> runPrivateOnSelf (access = PUBLIC): sends privateMethod to self
    struct CompiledMethod *cm_run_priv = malloc(sizeof(struct CompiledMethod));
    cm_run_priv->num_registers = 4;
    cm_run_priv->num_arguments = 0;
    cm_run_priv->instruction_count = 3;
    cm_run_priv->constant_count = 1;
    cm_run_priv->access_modifier = ACCESS_PUBLIC;
    uint32_t *insts_run_priv = malloc(3 * sizeof(uint32_t));
    insts_run_priv[0] = 0x00000301; // OP_LD_REG dest=3, src1=0 (R3 = self)
    insts_run_priv[1] = 0x03000002; // OP_SEND dest=0, src1=0 ("privateMethod"), src2=3
    insts_run_priv[2] = 0x00000304; // OP_RET R3
    cm_run_priv->instructions = insts_run_priv;
    oop *consts_run_priv = malloc(1 * sizeof(oop));
    consts_run_priv[0] = Symbol_privateMethod;
    cm_run_priv->constants = consts_run_priv;

    oop cl_run_priv = gc_allocate(BytecodeClosureVtable, sizeof(struct Closure));
    ((struct Closure *)cl_run_priv)->home_ctx = NULL;
    ((struct Closure *)cl_run_priv)->home_generation = 0;
    ((struct Closure *)cl_run_priv)->entry_point = cm_run_priv;
    add_test_method(BaseVtable, Symbol_runPrivateOnSelf, cl_run_priv);

    // 5. Base >> accessViolation: (access = PUBLIC): returns 999
    struct CompiledMethod *cm_viol = malloc(sizeof(struct CompiledMethod));
    cm_viol->num_registers = 4;
    cm_viol->num_arguments = 1;
    cm_viol->instruction_count = 2;
    cm_viol->constant_count = 1;
    cm_viol->access_modifier = ACCESS_PUBLIC;
    uint32_t *insts_viol = malloc(2 * sizeof(uint32_t));
    insts_viol[0] = 0x00000300; // OP_LD_CONST dest=3, src1=0 (999)
    insts_viol[1] = 0x00000304; // OP_RET R3
    cm_viol->instructions = insts_viol;
    oop *consts_viol = malloc(1 * sizeof(oop));
    consts_viol[0] = TAG_SMI(999);
    cm_viol->constants = consts_viol;

    oop cl_viol = gc_allocate(BytecodeClosureVtable, sizeof(struct Closure));
    ((struct Closure *)cl_viol)->home_ctx = NULL;
    ((struct Closure *)cl_viol)->home_generation = 0;
    ((struct Closure *)cl_viol)->entry_point = cm_viol;
    add_test_method(BaseVtable, Symbol_accessViolation, cl_viol);

    // 6. Sub >> runPrivateOnSelf (access = PUBLIC): sends privateMethod to self
    struct CompiledMethod *cm_sub_run_priv = malloc(sizeof(struct CompiledMethod));
    cm_sub_run_priv->num_registers = 4;
    cm_sub_run_priv->num_arguments = 0;
    cm_sub_run_priv->instruction_count = 3;
    cm_sub_run_priv->constant_count = 1;
    cm_sub_run_priv->access_modifier = ACCESS_PUBLIC;
    uint32_t *insts_sub_run_priv = malloc(3 * sizeof(uint32_t));
    insts_sub_run_priv[0] = 0x00000301; // OP_LD_REG dest=3, src1=0 (R3 = self)
    insts_sub_run_priv[1] = 0x03000002; // OP_SEND dest=0, src1=0 ("privateMethod"), src2=3
    insts_sub_run_priv[2] = 0x00000304; // OP_RET R3
    cm_sub_run_priv->instructions = insts_sub_run_priv;
    oop *consts_sub_run_priv = malloc(1 * sizeof(oop));
    consts_sub_run_priv[0] = Symbol_privateMethod;
    cm_sub_run_priv->constants = consts_sub_run_priv;

    oop cl_sub_run_priv = gc_allocate(BytecodeClosureVtable, sizeof(struct Closure));
    ((struct Closure *)cl_sub_run_priv)->home_ctx = NULL;
    ((struct Closure *)cl_sub_run_priv)->home_generation = 0;
    ((struct Closure *)cl_sub_run_priv)->entry_point = cm_sub_run_priv;
    add_test_method(SubVtable, Symbol_runPrivateOnSelf, cl_sub_run_priv);

    // 7. Helper >> runProtectedOnOther (access = PUBLIC): takes 1 arg (target base) and sends protectedMethod
    struct CompiledMethod *cm_help_run = malloc(sizeof(struct CompiledMethod));
    cm_help_run->num_registers = 4;
    cm_help_run->num_arguments = 1; // target in R1
    cm_help_run->instruction_count = 3;
    cm_help_run->constant_count = 1;
    cm_help_run->access_modifier = ACCESS_PUBLIC;
    uint32_t *insts_help_run = malloc(3 * sizeof(uint32_t));
    insts_help_run[0] = 0x00010301; // OP_LD_REG dest=3, src1=1 (R3 = R1)
    insts_help_run[1] = 0x03000002; // OP_SEND dest=0, src1=0 ("protectedMethod"), src2=3
    insts_help_run[2] = 0x00000304; // OP_RET R3
    cm_help_run->instructions = insts_help_run;
    oop *consts_help_run = malloc(1 * sizeof(oop));
    consts_help_run[0] = Symbol_protectedMethod;
    cm_help_run->constants = consts_help_run;

    oop cl_help_run = gc_allocate(BytecodeClosureVtable, sizeof(struct Closure));
    ((struct Closure *)cl_help_run)->home_ctx = NULL;
    ((struct Closure *)cl_help_run)->home_generation = 0;
    ((struct Closure *)cl_help_run)->entry_point = cm_help_run;
    add_test_method(HelperVtable, Symbol_runProtectedOnOther, cl_help_run);
}

int main(void) {
    printf("Setting up OEVM Access Control and Capabilities test environment...\n");

    gc_additional_roots_hook = test_roots_callback;

    // Boot solidified vtables
    extern void init_solidified_vtables(void);
    init_solidified_vtables();

    setup_access_environment();

    // Instantiate Base, Sub, and Helper
    oop base_inst = gc_allocate(BaseVtable, sizeof(struct ObjectInstance));
    oop sub_inst = gc_allocate(SubVtable, sizeof(struct ObjectInstance));
    oop helper_inst = gc_allocate(HelperVtable, sizeof(struct ObjectInstance));

    base_instance_root = base_inst;
    sub_instance_root = sub_inst;
    helper_instance_root = helper_inst;

    // --- TEST 1: Protected Access on Self (Succeeds) ---
    printf("TEST 1: Calling protected method on self (Base >> runProtectedExplicit)...\n");
    oop res1 = send(base_inst, Symbol_runProtectedExplicit, 0, NULL);
    printf("Result: %ld (expected 42)\n", (long)UNTAG_SMI(res1));
    assert(UNTAG_SMI(res1) == 42);
    printf("TEST 1 Passed!\n\n");

    // --- TEST 2: Protected Access on subclass instance self (Succeeds) ---
    printf("TEST 2: Calling inherited protected method on subclass self (Sub >> runProtectedExplicit)...\n");
    // Sub doesn't override runProtectedExplicit, it delegates to Base. But since receiver is sub_inst (self), it succeeds.
    oop res2 = send(sub_inst, Symbol_runProtectedExplicit, 0, NULL);
    printf("Result: %ld (expected 42)\n", (long)UNTAG_SMI(res2));
    assert(UNTAG_SMI(res2) == 42);
    printf("TEST 2 Passed!\n\n");

    // --- TEST 3: Protected Access violation from external context (Intercepted) ---
    printf("TEST 3: Calling protected method on an external object (Helper >> runProtectedOnOther)...\n");
    // Helper tries to call protectedMethod on base_inst. Should fail and trigger accessViolation returning 999.
    oop res3 = send(helper_inst, Symbol_runProtectedOnOther, 1, &base_inst);
    printf("Result: %ld (expected 999 via accessViolation:)\n", (long)UNTAG_SMI(res3));
    assert(UNTAG_SMI(res3) == 999);
    printf("TEST 3 Passed!\n\n");

    // --- TEST 4: Private Access on Self (Succeeds) ---
    printf("TEST 4: Calling private method on self (Base >> runPrivateOnSelf)...\n");
    oop res4 = send(base_inst, Symbol_runPrivateOnSelf, 0, NULL);
    printf("Result: %ld (expected 77)\n", (long)UNTAG_SMI(res4));
    assert(UNTAG_SMI(res4) == 77);
    printf("TEST 4 Passed!\n\n");

    // --- TEST 5: Private Access violation on subclass self (Intercepted) ---
    printf("TEST 5: Calling private method from subclass self (Sub >> runPrivateOnSelf)...\n");
    // Sub inherits privateMethod from Base, but since the call is from a method defined in SubVtable, it is blocked.
    // It should trigger accessViolation returning 999.
    oop res5 = send(sub_inst, Symbol_runPrivateOnSelf, 0, NULL);
    printf("Result: %ld (expected 999 via accessViolation:)\n", (long)UNTAG_SMI(res5));
    assert(UNTAG_SMI(res5) == 999);
    printf("TEST 5 Passed!\n\n");

    // --- TEST 6: GC Stability & Reference Checks ---
    printf("TEST 6: Running Garbage Collector and re-verifying stability...\n");
    gc_collect();
    printf("GC completed successfully!\n");

    oop res6 = send(base_inst, Symbol_runProtectedExplicit, 0, NULL);
    assert(UNTAG_SMI(res6) == 42);
    printf("TEST 6 Passed!\n\n");

    printf("All OEVM Access Modifier & Dynamic Capability tests PASSED successfully!\n");
    return 0;
}
