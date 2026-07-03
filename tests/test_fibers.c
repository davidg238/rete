#include "../src/oevm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* External VM references */
extern oop gc_allocate(oop vtable, size_t size);
extern void gc_collect(void);
extern oop send(oop receiver, oop selector, int argc, oop *args);
extern oop run_interpreter(oop closure, oop receiver, int argc, oop *args);

extern oop current_fiber;
extern oop RunnableFiberVtable;
extern oop SuspendedFiberVtable;
extern oop DeadFiberVtable;
extern oop FiberVtable;

extern oop SymbolVtable;
extern oop VtableVtable;
extern oop PrimitiveClosureVtable;
extern oop BytecodeClosureVtable;
extern oop MethodContextVtable;

extern struct MethodContext *current_frame;

static oop Symbol_fiberCreate = NULL;
static oop Symbol_fiberSwitch_value = NULL;

static void init_symbols(void) {
    if (VtableVtable == NULL) {
        extern const struct Vtable Vtable_VtableVtable;
        extern const struct Vtable Vtable_SymbolVtable;
        extern const struct Vtable Vtable_BytecodeClosureVtable;
        extern const struct Vtable Vtable_PrimitiveClosureVtable;
        extern const struct Vtable Vtable_RunnableFiberVtable;
        extern const struct Vtable Vtable_SuspendedFiberVtable;
        extern const struct Vtable Vtable_DeadFiberVtable;

        VtableVtable = (oop)&Vtable_VtableVtable;
        SymbolVtable = (oop)&Vtable_SymbolVtable;
        BytecodeClosureVtable = (oop)&Vtable_BytecodeClosureVtable;
        PrimitiveClosureVtable = (oop)&Vtable_PrimitiveClosureVtable;
        RunnableFiberVtable = (oop)&Vtable_RunnableFiberVtable;
        SuspendedFiberVtable = (oop)&Vtable_SuspendedFiberVtable;
        DeadFiberVtable = (oop)&Vtable_DeadFiberVtable;
        FiberVtable = RunnableFiberVtable;
    }

    Symbol_fiberCreate = gc_allocate(SymbolVtable, sizeof(struct Symbol));
    ((struct Symbol *)Symbol_fiberCreate)->name = "fiberCreate:";
    ((struct Symbol *)Symbol_fiberCreate)->length = strlen("fiberCreate:");

    Symbol_fiberSwitch_value = gc_allocate(SymbolVtable, sizeof(struct Symbol));
    ((struct Symbol *)Symbol_fiberSwitch_value)->name = "fiberSwitch:value:";
    ((struct Symbol *)Symbol_fiberSwitch_value)->length = strlen("fiberSwitch:value:");
}

void test_fiber_creation_and_switching(void) {
    printf("--- Running Fiber Creation and Context Switching Test ---\n");

    init_symbols();

    // 1. Create a compiled method for the fiber execution body.
    // Universal single-stack mechanism (post de-recursify Phase 3 Task 1):
    // primitive_fiber_create always seeds R0 with the fiber's own closure (the
    // "standard block receiver"), never an externally-injected receiver — the
    // old POSIX-only entry_receiver override is gone. So the fiber body cannot
    // reach the main fiber via R0 anymore; bake it in as a constant instead
    // (consts[2], filled in below once main_fiber_oop is known).
    struct CompiledMethod *method = malloc(sizeof(struct CompiledMethod));
    method->constant_count = 3;
    method->num_registers = 4;
    method->num_arguments = 0;
    method->instruction_count = 4;

    oop *consts = malloc(3 * sizeof(oop));
    consts[0] = Symbol_fiberSwitch_value;
    consts[1] = TAG_SMI(123);
    consts[2] = NULL; /* filled in with main_fiber_oop below, before the launcher runs */
    method->constants = consts;

    uint32_t *insts = malloc(4 * sizeof(uint32_t));
    insts[0] = 0x00020100; // OP_LD_CONST dest=1, src1=2 (main_fiber_oop, filled in below)
    insts[1] = 0x00010200; // OP_LD_CONST dest=2, src1=1
    insts[2] = 0x00000202; // OP_SEND dest=2, src1=0, src2=0
    insts[3] = 0x00000004; // OP_RET dest=0
    method->instructions = insts;

    // 2. Create the fiber body closure
    struct Closure *cl = (struct Closure *)gc_allocate(BytecodeClosureVtable, sizeof(struct Closure));
    cl->home_ctx = NULL;
    cl->home_generation = 0;
    cl->entry_point = method;

    // 3. Instantiate the Fiber
    oop args[1] = { (oop)cl };
    oop fiber_oop = send(NULL, Symbol_fiberCreate, 1, args);
    assert(fiber_oop != NULL);
    assert(!IS_SMI(fiber_oop));
    printf("Successfully created suspended fiber at %p\n", (void *)fiber_oop);

    // Ensure main_fiber is lazy-initialized
    struct CompiledMethod *dummy_m = malloc(sizeof(struct CompiledMethod));
    dummy_m->constant_count = 0;
    dummy_m->num_registers = 1;
    dummy_m->num_arguments = 0;
    dummy_m->instruction_count = 1;
    uint32_t *dummy_insts = malloc(1 * sizeof(uint32_t));
    dummy_insts[0] = 0x00000004; // OP_RET dest=0
    dummy_m->instructions = dummy_insts;
    dummy_m->constants = NULL;

    struct Closure *dummy_cl = (struct Closure *)gc_allocate(BytecodeClosureVtable, sizeof(struct Closure));
    dummy_cl->home_ctx = NULL;
    dummy_cl->home_generation = 0;
    dummy_cl->entry_point = dummy_m;
    run_interpreter((oop)dummy_cl, NULL, 0, NULL);

    oop main_fiber_oop = current_fiber;
    assert(main_fiber_oop != NULL);
    printf("Main fiber initialized at %p\n", (void *)main_fiber_oop);

    /* Universal single-stack mechanism: R0 of the fiber's initial frame is
       always the fiber's own closure (set by primitive_fiber_create), not an
       externally-injected receiver. Give the fiber body a way to reach the
       main fiber by baking main_fiber_oop into its constant pool (consts[2],
       loaded into R1 by insts[0] above) instead. */
    consts[2] = main_fiber_oop;

    // 4. Create a bytecode launcher method to initiate the switch inside the interpreter loop!
    struct CompiledMethod *launcher_m = malloc(sizeof(struct CompiledMethod));
    launcher_m->constant_count = 2;
    launcher_m->num_registers = 4;
    launcher_m->num_arguments = 1; // target_fiber
    launcher_m->instruction_count = 3;

    oop *launcher_consts = malloc(2 * sizeof(oop));
    launcher_consts[0] = TAG_SMI(777);
    launcher_consts[1] = Symbol_fiberSwitch_value;
    launcher_m->constants = launcher_consts;

    uint32_t *launcher_insts = malloc(3 * sizeof(uint32_t));
    launcher_insts[0] = 0x00000200; // OP_LD_CONST dest=2, src1=0 (777)
    launcher_insts[1] = 0x00010202; // OP_SEND dest=2 (argc), src1=1 (fiberSwitch:value:), src2=0 (receiver is main_fiber)
    launcher_insts[2] = 0x00000004; // OP_RET dest=0
    launcher_m->instructions = launcher_insts;

    struct Closure *launcher_cl = (struct Closure *)gc_allocate(BytecodeClosureVtable, sizeof(struct Closure));
    launcher_cl->home_ctx = NULL;
    launcher_cl->home_generation = 0;
    launcher_cl->entry_point = launcher_m;

    printf("Executing bytecode launcher to switch to new fiber...\n");
    oop launcher_args[1] = { fiber_oop };
    oop res = run_interpreter((oop)launcher_cl, main_fiber_oop, 1, launcher_args);

    printf("Returned back to C with result: %ld (expected 123)\n", (long)UNTAG_SMI(res));
    assert(res == TAG_SMI(123));

    // 5. Test Garbage Collector tracing active and suspended stacks
    printf("Running garbage collector tracing test...\n");
    gc_collect();
    printf("Garbage collection test passed!\n");

    /* currentFiber primitive: after the switch test, control is back on the main
       fiber. Sending #currentFiber (a no-arg universal prim) to a valid receiver
       must return the active fiber (== current_fiber == main_fiber_oop). */
    {
        oop Symbol_currentFiber = gc_allocate(SymbolVtable, sizeof(struct Symbol));
        ((struct Symbol *)Symbol_currentFiber)->name = "currentFiber";
        ((struct Symbol *)Symbol_currentFiber)->length = strlen("currentFiber");
        extern oop current_fiber;
        oop cf = send(main_fiber_oop, Symbol_currentFiber, 0, NULL);
        assert(cf != NULL);
        assert(cf == current_fiber);
        printf("currentFiber primitive returns the active fiber: OK\n");
    }

    free((void *)method->constants);
    free((void*)method->instructions);
    free(method);
    free(dummy_insts);
    free(dummy_m);
    free(launcher_consts);
    free((void*)launcher_insts);
    free(launcher_m);
}

int main(void) {
    test_fiber_creation_and_switching();
    printf("All fiber scheduling tests PASSED successfully!\n");
    return 0;
}
