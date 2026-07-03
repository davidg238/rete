#include "../src/oevm.h"
#include "../src/mirror.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

extern oop IntegerVtable;
extern oop VtableVtable;
extern oop SymbolVtable;
extern oop BytecodeClosureVtable;
extern oop PrimitiveClosureVtable;
extern oop FiberVtable;
extern oop current_fiber;

extern void (*gc_additional_roots_hook)(void);
extern void mark_object(oop ptr);
extern void gc_collect(void);
extern oop gc_allocate(oop vtable, size_t size);
extern oop send(oop receiver, oop selector, int argc, oop *args);

static oop Symbol_run = NULL;
static oop Symbol_debug = NULL;
static oop Symbol_registerDebuggerHook = NULL;
static oop Symbol_regAt = NULL;
static oop Symbol_regAtPut = NULL;
static oop Symbol_singleStep = NULL;
static oop Symbol_bindTo = NULL;
static oop Symbol_instructionAtPut = NULL;

static oop TestVtable = NULL;
static oop test_instance = NULL;

static int breakpoint_hit_count = 0;
static int step_hit_count = 0;

static oop create_symbol(const char *name);  /* forward decl for hook below */

/* TEST 10: requestStep: hook — arms stepping from inside a break hook via the
   no-reflectee mirror prim instead of fiber:singleStep:. */
static int rs_step_count = 0;
static int rs_armed = 0;
static oop rs_hook_c(oop receiver, int argc, oop *args) {
    (void)receiver; (void)argc;
    oop ctx = args[0];
    oop pc_val = send(mirror_object(), create_symbol("pcOf:"), 1, &ctx);
    const uint32_t *pc_ptr = (const uint32_t *)(((uintptr_t)pc_val) >> 1);
    struct MethodContext *mc = (struct MethodContext *)ctx;
    struct CompiledMethod *cm = (struct CompiledMethod *)((struct Closure *)mc->closure)->entry_point;
    uint32_t last = cm->instructions[(pc_ptr - cm->instructions) - 1];
    if ((last & 0xFF) == OP_BREAK && !rs_armed) {
        rs_armed = 1;
        oop one = TAG_SMI(1);
        send(mirror_object(), create_symbol("requestStep:"), 1, &one);
    } else if (rs_armed) {
        rs_step_count++;
    }
    return NULL;
}

/* TEST 7: offsetOf: hook — captures the bytecode offset via the mirror prim. */
static long captured_offset = -1;
static long captured_pc = 0;
static oop offset_capture_hook_c(oop receiver, int argc, oop *args) {
    (void)receiver; (void)argc;
    oop ctx = args[0];
    /* Use mirror offsetOf: to get the bytecode offset of saved_pc within its method. */
    captured_offset = (long)UNTAG_SMI(send(mirror_object(), create_symbol("offsetOf:"), 1, &ctx));
    /* pcOf: returns the RAW saved_pc pointer as an SMI (offsetOf: is derived from it). */
    captured_pc = (long)UNTAG_SMI(send(mirror_object(), create_symbol("pcOf:"), 1, &ctx));
    return NULL;
}

/* TEST 6: a hook that arms stepping from inside itself must take effect. */
static int armed_step_count = 0;
static int arm_once = 0;
static oop arm_step_hook_c(oop receiver, int argc, oop *args) {
    (void)receiver; (void)argc;
    oop ctx = args[0];
    oop pc_val = send(mirror_object(), create_symbol("pcOf:"), 1, &ctx);
    const uint32_t *pc_ptr = (const uint32_t *)(((uintptr_t)pc_val) >> 1);
    struct MethodContext *mc = (struct MethodContext *)ctx;
    struct CompiledMethod *cm = (struct CompiledMethod *)((struct Closure *)mc->closure)->entry_point;
    uint32_t last = cm->instructions[(pc_ptr - cm->instructions) - 1];
    if ((last & 0xFF) == OP_BREAK && !arm_once) {
        arm_once = 1;                       /* on the breakpoint, request stepping */
        oop sa[] = { current_fiber, TAG_SMI(1) };
        send(mirror_object(), create_symbol("fiber:singleStep:"), 2, sa);
    } else if (arm_once) {
        armed_step_count++;                 /* these fire only if the arm survived */
    }
    return NULL;
}

/* TEST 12: C-assisted breakpoint match — hook fires on a (method,offset) bp set
   in the C table, with NO instruction mutation. The hook records the hit and
   asserts offsetOf:/methodOf: agree with the bp we set. */
static int bp_hit_count = 0;
static long bp_expected_off = -1;
static oop bp_expected_method = NULL;

/* TEST 13: numRegistersOf: confined read prim — from inside a break hook, bound a
   register dump to the method's num_registers. Asserts the prim returns the known
   register count and that reg:at: at (count-1) is in-bounds (does not abort). */
static int nr_hit = 0;
static long nr_reported = -1;
static long nr_top_reg = -2;
static oop nr_hook_c(oop receiver, int argc, oop *args) {
    (void)receiver; (void)argc;
    oop ctx = args[0];
    nr_reported = (long)UNTAG_SMI(send(mirror_object(), create_symbol("numRegistersOf:"), 1, &ctx));
    /* reg:at: at the top valid index (count-1) must be in-bounds (no abort). */
    oop ra[] = { ctx, TAG_SMI(nr_reported - 1) };
    oop top = send(mirror_object(), create_symbol("reg:at:"), 2, ra);
    nr_top_reg = IS_SMI(top) ? (long)UNTAG_SMI(top) : -99;
    nr_hit++;
    return NULL;
}
static oop bp_match_hook_c(oop receiver, int argc, oop *args) {
    (void)receiver; (void)argc;
    oop ctx = args[0];
    long off = (long)UNTAG_SMI(send(mirror_object(), create_symbol("offsetOf:"), 1, &ctx));
    oop meth = send(mirror_object(), create_symbol("methodOf:"), 1, &ctx);
    assert(off == bp_expected_off);
    assert(meth == bp_expected_method);
    bp_hit_count++;
    return NULL;
}

static void test_roots_callback(void) {
    mark_object(Symbol_run);
    mark_object(Symbol_debug);
    mark_object(Symbol_registerDebuggerHook);
    mark_object(Symbol_regAt);
    mark_object(Symbol_regAtPut);
    mark_object(Symbol_singleStep);
    mark_object(Symbol_bindTo);
    mark_object(Symbol_instructionAtPut);
    mark_object(TestVtable);
    mark_object(test_instance);
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
    for (size_t i = 0; i < v->size; i++) {
        if (v->bindings[i].symbol == symbol) {
            v->bindings[i].method = method;
            return;
        }
    }
    if (v->size >= v->capacity) {
        v->capacity *= 2;
        v->bindings = realloc(v->bindings, v->capacity * sizeof(struct Binding));
    }
    v->bindings[v->size].symbol = symbol;
    v->bindings[v->size].method = method;
    v->size++;
}

// C-level Debugger Hook to simulate guest debugger closures
static oop debug_hook_c(oop receiver, int argc, oop *args) {
    (void)receiver;
    assert(argc >= 1);
    oop context = args[0];

    // Read PC to determine if breakpoint or single-step
    oop pc_val = send(mirror_object(), create_symbol("pcOf:"), 1, &context);
    const uint32_t *pc_ptr = (const uint32_t *)(((uintptr_t)pc_val) >> 1);

    // Read active instructions
    struct MethodContext *ctx = (struct MethodContext *)context;
    struct Closure *cl = (struct Closure *)ctx->closure;
    struct CompiledMethod *cm = (struct CompiledMethod *)cl->entry_point;

    long pc_offset = 0;
    if (pc_ptr != NULL) {
        pc_offset = pc_ptr - cm->instructions;
    }

    // Determine current instruction opcode
    // If the PC was saved at the end of the instruction, the executed instruction is at PC - 1.
    // If it was a breakpoint check at the start of loop, PC was saved at pc+1, so it is at PC - 1.
    uint32_t last_inst = cm->instructions[pc_offset - 1];
    uint8_t last_op = last_inst & 0xFF;

    if (last_op == OP_BREAK) {
        breakpoint_hit_count++;
        printf("[Debugger] Hit OP_BREAK! Modifying R3 to Smi 99...\n");
        oop rap[] = { context, TAG_SMI(3), TAG_SMI(99) };
        send(mirror_object(), create_symbol("reg:at:put:"), 3, rap);
    } else {
        step_hit_count++;
        printf("[Debugger] Single-Step Trap! PC Offset: %ld, Last Opcode: 0x%X\n", pc_offset, last_op);
        
        // Read register R3 value
        oop ra[] = { context, TAG_SMI(3) };
        oop r3_val = send(mirror_object(), create_symbol("reg:at:"), 2, ra);
        printf("[Debugger] Register R3 Value: %ld\n", IS_SMI(r3_val) ? (long)UNTAG_SMI(r3_val) : -1);
    }

    return NULL;
}

int main(void) {
    printf("Setting up OEVM Interactive Debugger test environment...\n");

    gc_additional_roots_hook = test_roots_callback;

    // Boot solidified vtables
    extern void init_solidified_vtables(void);
    init_solidified_vtables();

    Symbol_run = create_symbol("run");
    Symbol_debug = create_symbol("debug:");
    Symbol_registerDebuggerHook = create_symbol("registerDebuggerHook:");
    Symbol_regAt = create_symbol("regAt:");
    Symbol_regAtPut = create_symbol("regAt:put:");
    Symbol_singleStep = create_symbol("fiber:singleStep:");
    Symbol_bindTo = create_symbol("vtable:bind:to:");
    Symbol_instructionAtPut = create_symbol("method:instructionAt:put:");

    TestVtable = create_test_vtable(NULL);
    test_instance = gc_allocate(TestVtable, sizeof(struct ObjectInstance));

    // Create a Debugger Hook closure
    oop dbg_cl = gc_allocate(PrimitiveClosureVtable, sizeof(struct Closure));
    ((struct Closure *)dbg_cl)->home_ctx = NULL;
    ((struct Closure *)dbg_cl)->home_generation = 0;
    ((struct Closure *)dbg_cl)->entry_point = (void *)debug_hook_c;

    // Register the debugger hook via mirror (onBreak:)
    oop reg_args[] = { dbg_cl };
    send(mirror_object(), create_symbol("onBreak:"), 1, reg_args);

    // ==========================================
    // TEST 1: Breakpoint Traps (OP_BREAK)
    // ==========================================
    printf("\n--- TEST 1: Breakpoint Traps ---\n");
    // Define a method with OP_BREAK:
    // Inst 0: OP_LD_CONST dest=3, src1=0 (Smi 10)
    // Inst 1: OP_BREAK
    // Inst 2: OP_RET R3
    struct CompiledMethod *cm_brk = malloc(sizeof(struct CompiledMethod));
    cm_brk->num_registers = 4;
    cm_brk->num_arguments = 0;
    cm_brk->instruction_count = 3;
    cm_brk->constant_count = 1;
    cm_brk->access_modifier = ACCESS_PUBLIC;
    uint32_t *insts_brk = malloc(3 * sizeof(uint32_t));
    insts_brk[0] = 0x00000300; // OP_LD_CONST dest=3, src1=0
    insts_brk[1] = (uint32_t)OP_BREAK;
    insts_brk[2] = 0x00000304; // OP_RET R3
    cm_brk->instructions = insts_brk;
    oop *consts_brk = malloc(1 * sizeof(oop));
    consts_brk[0] = TAG_SMI(10);
    cm_brk->constants = consts_brk;

    oop cl_brk = gc_allocate(BytecodeClosureVtable, sizeof(struct Closure));
    ((struct Closure *)cl_brk)->home_ctx = NULL;
    ((struct Closure *)cl_brk)->home_generation = 0;
    ((struct Closure *)cl_brk)->entry_point = cm_brk;

    add_test_method(TestVtable, Symbol_run, cl_brk);

    oop res1 = send(test_instance, Symbol_run, 0, NULL);
    printf("Result of run: %ld (expected 99 via debugger register modification)\n", (long)UNTAG_SMI(res1));
    assert(UNTAG_SMI(res1) == 99);
    assert(breakpoint_hit_count == 1);
    printf("TEST 1 Passed!\n");

    // ==========================================
    // TEST 2: Single-Stepping
    // ==========================================
    printf("\n--- TEST 2: Single-Stepping ---\n");
    // Define a method without OP_BREAK:
    // Inst 0: OP_LD_CONST dest=3, src1=0 (Smi 5)
    // Inst 1: OP_ADD dest=3, src1=3, src2=3 (5 + 5 = 10)
    // Inst 2: OP_RET R3
    struct CompiledMethod *cm_step = malloc(sizeof(struct CompiledMethod));
    cm_step->num_registers = 4;
    cm_step->num_arguments = 0;
    cm_step->instruction_count = 3;
    cm_step->constant_count = 1;
    cm_step->access_modifier = ACCESS_PUBLIC;
    uint32_t *insts_step = malloc(3 * sizeof(uint32_t));
    insts_step[0] = 0x00000300; // OP_LD_CONST dest=3, src1=0
    insts_step[1] = 0x03030306; // OP_ADD dest=3, src1=3, src2=3
    insts_step[2] = 0x00000304; // OP_RET R3
    cm_step->instructions = insts_step;
    oop *consts_step = malloc(1 * sizeof(oop));
    consts_step[0] = TAG_SMI(5);
    cm_step->constants = consts_step;

    oop cl_step = gc_allocate(BytecodeClosureVtable, sizeof(struct Closure));
    ((struct Closure *)cl_step)->home_ctx = NULL;
    ((struct Closure *)cl_step)->home_generation = 0;
    ((struct Closure *)cl_step)->entry_point = cm_step;

    // Rebind `run` -> cl_step via the mirror so the method cache is flushed
    // (the raw add_test_method helper mutates bindings without invalidating it).
    oop step_bind_args[] = { TestVtable, Symbol_run, cl_step };
    send(mirror_object(), Symbol_bindTo, 3, step_bind_args);

    // Turn on single-stepping on the current fiber
    // (Ensure interpreter initialized the fiber first)
    assert(current_fiber != NULL);
    oop step_args[] = { current_fiber, TAG_SMI(1) };
    send(mirror_object(), Symbol_singleStep, 2, step_args);

    oop res2 = send(test_instance, Symbol_run, 0, NULL);
    
    // Disable single-stepping
    oop no_step_args[] = { current_fiber, TAG_SMI(0) };
    send(mirror_object(), Symbol_singleStep, 2, no_step_args);

    printf("Result of single stepped run: %ld (expected 10)\n", (long)UNTAG_SMI(res2));
    assert(UNTAG_SMI(res2) == 10);
    printf("Step hit count: %d (expected 2 steps)\n", step_hit_count);
    assert(step_hit_count == 2);
    printf("TEST 2 Passed!\n");

    // ==========================================
    // TEST 3: Hot Code Swapping (Vtable Bindings)
    // ==========================================
    printf("\n--- TEST 3: Hot Code Swapping (Vtable Bindings) ---\n");
    // Bind cl_brk back so we can replace it dynamically
    add_test_method(TestVtable, Symbol_run, cl_brk);
    
    // We already have cl_brk (which returns 99) bound to 'run'.
    // Let's bind cl_step (returns 10) to 'run' dynamically via the mirror.
    oop bind_args[] = { TestVtable, Symbol_run, cl_step };
    send(mirror_object(), Symbol_bindTo, 3, bind_args);

    oop res3 = send(test_instance, Symbol_run, 0, NULL);
    printf("Result after vtable hot-swap: %ld (expected 10)\n", (long)UNTAG_SMI(res3));
    assert(UNTAG_SMI(res3) == 10);
    printf("TEST 3 Passed!\n");

    // ==========================================
    // TEST 4: Hot Code Swapping (Instruction Mutation)
    // ==========================================
    printf("\n--- TEST 4: Hot Code Swapping (Instruction Mutation) ---\n");
    // Modify instruction 0 of cl_step: change const to consts_step[0] (which is Smi 5), but wait!
    // Let's modify instruction 1 (OP_ADD dest=3, src1=3, src2=3) to another OP_LD_CONST dest=3, src1=0 (Smi 5),
    // so it returns 5 instead of 10!
    // Inst 1 original: 0x03030306
    // Inst 1 replacement: 0x00000300 (OP_LD_CONST dest=3, src1=0)
    oop mut_args[] = { cl_step, TAG_SMI(1), TAG_SMI(0x00000300) };
    send(mirror_object(), Symbol_instructionAtPut, 3, mut_args);

    oop res4 = send(test_instance, Symbol_run, 0, NULL);
    printf("Result after bytecode instruction mutation: %ld (expected 5)\n", (long)UNTAG_SMI(res4));
    assert(UNTAG_SMI(res4) == 5);
    printf("TEST 4 Passed!\n");

    // ==========================================
    // TEST 5: GC Stability & Reference Verification
    // ==========================================
    printf("\n--- TEST 5: GC Stability ---\n");
    gc_collect();
    printf("GC completed successfully!\n");

    oop res5 = send(test_instance, Symbol_run, 0, NULL);
    assert(UNTAG_SMI(res5) == 5);
    printf("TEST 5 Passed!\n\n");

    // ==========================================
    // TEST 6: Hook-armed stepping (hook_step_request channel)
    // ==========================================
    printf("--- TEST 6: Hook-Armed Stepping ---\n");
    // Reset state
    armed_step_count = 0;
    arm_once = 0;

    // Register the arm_step_hook_c as the debugger hook
    oop arm_hook_cl = gc_allocate(PrimitiveClosureVtable, sizeof(struct Closure));
    ((struct Closure *)arm_hook_cl)->home_ctx = NULL;
    ((struct Closure *)arm_hook_cl)->home_generation = 0;
    ((struct Closure *)arm_hook_cl)->entry_point = (void *)arm_step_hook_c;
    oop arm_reg_args[] = { arm_hook_cl };
    send(mirror_object(), create_symbol("onBreak:"), 1, arm_reg_args);

    // Create a method: OP_BREAK, then 2 OP_LD_CONST, then OP_RET
    // On OP_BREAK the hook arms stepping; the subsequent instructions
    // should fire the step hook.
    struct CompiledMethod *cm_arm = malloc(sizeof(struct CompiledMethod));
    cm_arm->num_registers = 4;
    cm_arm->num_arguments = 0;
    cm_arm->instruction_count = 4;
    cm_arm->constant_count = 1;
    cm_arm->access_modifier = ACCESS_PUBLIC;
    uint32_t *insts_arm = malloc(4 * sizeof(uint32_t));
    insts_arm[0] = (uint32_t)OP_BREAK;          /* fires hook; hook arms stepping    */
    insts_arm[1] = 0x00000300;                   /* OP_LD_CONST dest=3, src1=0        */
    insts_arm[2] = 0x00000300;                   /* OP_LD_CONST dest=3, src1=0        */
    insts_arm[3] = 0x00000304;                   /* OP_RET R3                         */
    cm_arm->instructions = insts_arm;
    oop *consts_arm = malloc(1 * sizeof(oop));
    consts_arm[0] = TAG_SMI(42);
    cm_arm->constants = consts_arm;

    oop cl_arm = gc_allocate(BytecodeClosureVtable, sizeof(struct Closure));
    ((struct Closure *)cl_arm)->home_ctx = NULL;
    ((struct Closure *)cl_arm)->home_generation = 0;
    ((struct Closure *)cl_arm)->entry_point = cm_arm;

    oop arm_bind_args[] = { TestVtable, Symbol_run, cl_arm };
    send(mirror_object(), Symbol_bindTo, 3, arm_bind_args);

    // Ensure single-stepping is OFF before we run
    oop off_args[] = { current_fiber, TAG_SMI(0) };
    send(mirror_object(), Symbol_singleStep, 2, off_args);

    oop res6 = send(test_instance, Symbol_run, 0, NULL);
    (void)res6;

    // Disable single-stepping after run (hook may have left it on)
    oop off2_args[] = { current_fiber, TAG_SMI(0) };
    send(mirror_object(), Symbol_singleStep, 2, off2_args);

    // Re-register original hook
    oop orig_reg_args[] = { dbg_cl };
    send(mirror_object(), create_symbol("onBreak:"), 1, orig_reg_args);

    printf("Armed step count: %d (expected > 0)\n", armed_step_count);
    assert(armed_step_count > 0);
    printf("TEST 6 Passed!\n\n");

    // ==========================================
    // TEST 7: method:instructionAt: read-back
    // ==========================================
    printf("--- TEST 7: method:instructionAt: read-back ---\n");
    /* cl_step at index 1 was patched in TEST 4 to 0x00000300.
       Read it back via the new mirror prim and assert equality. */
    {
        oop ia[] = { cl_step, TAG_SMI(1) };
        oop word = send(mirror_object(), create_symbol("method:instructionAt:"), 2, ia);
        printf("instructionAt: 1 = 0x%lX (expected 0x300)\n", (long)UNTAG_SMI(word));
        assert(IS_SMI(word));
        assert((uint32_t)UNTAG_SMI(word) == 0x00000300);
    }
    printf("TEST 7 Passed!\n\n");

    // ==========================================
    // TEST 8: offsetOf: in a break hook
    // ==========================================
    printf("--- TEST 8: offsetOf: in a break hook ---\n");
    /* Build a tiny method: [0] OP_BREAK, [1] OP_RET R0.
       The break hook fires after the interpreter advances pc past OP_BREAK,
       so saved_pc points to instruction[1], making offsetOf: == 1. */
    {
        struct CompiledMethod *cm_off = malloc(sizeof(struct CompiledMethod));
        cm_off->num_registers = 4;
        cm_off->num_arguments = 0;
        cm_off->instruction_count = 2;
        cm_off->constant_count = 0;
        cm_off->constants = NULL;
        cm_off->access_modifier = ACCESS_PUBLIC;
        uint32_t *insts_off = malloc(2 * sizeof(uint32_t));
        insts_off[0] = (uint32_t)OP_BREAK;    /* fires hook */
        insts_off[1] = 0x00000004;             /* OP_RET R0  */
        cm_off->instructions = insts_off;

        oop cl_off = gc_allocate(BytecodeClosureVtable, sizeof(struct Closure));
        ((struct Closure *)cl_off)->home_ctx = NULL;
        ((struct Closure *)cl_off)->home_generation = 0;
        ((struct Closure *)cl_off)->entry_point = cm_off;

        /* Install the offset-capture hook */
        oop off_hook_cl = gc_allocate(PrimitiveClosureVtable, sizeof(struct Closure));
        ((struct Closure *)off_hook_cl)->home_ctx = NULL;
        ((struct Closure *)off_hook_cl)->home_generation = 0;
        ((struct Closure *)off_hook_cl)->entry_point = (void *)offset_capture_hook_c;
        oop oreg[] = { off_hook_cl };
        send(mirror_object(), create_symbol("onBreak:"), 1, oreg);

        /* Bind and run */
        oop obind[] = { TestVtable, Symbol_run, cl_off };
        send(mirror_object(), Symbol_bindTo, 3, obind);
        send(test_instance, Symbol_run, 0, NULL);

        printf("captured_offset = %ld (expected 1)\n", captured_offset);
        assert(captured_offset == 1);

        /* pcOf: returns the raw saved_pc pointer as an SMI. Since offsetOf: == 1 and
           offsetOf: = saved_pc - cm_instructions, the raw pc must be &insts_off[1]. */
        printf("captured_pc = 0x%lX (expected &insts_off[1] = 0x%lX)\n",
               captured_pc, (long)(uintptr_t)&insts_off[1]);
        assert(captured_pc != 0);
        assert(captured_pc == (long)(uintptr_t)&insts_off[1]);

        /* Restore original hook */
        oop restore[] = { dbg_cl };
        send(mirror_object(), create_symbol("onBreak:"), 1, restore);
    }
    printf("TEST 8 Passed!\n\n");

    // ==========================================
    // TEST 9: selectorsOf: and methodsOf:
    // ==========================================
    printf("--- TEST 9: selectorsOf: and methodsOf: ---\n");
    {
        /* TestVtable currently has 'run' bound (from TEST 8 bind above).
           Verify selectorsOf: returns a kernel Array containing Symbol_run. */
        oop sels = send(mirror_object(), create_symbol("selectorsOf:"), 1, &TestVtable);
        /* kernel Array layout: slots[0] = TAG_SMI(n), slots[1..n] = elements */
        struct ObjectInstance *sels_inst = (struct ObjectInstance *)sels;
        intptr_t sels_n = UNTAG_SMI(sels_inst->slots[0]);
        printf("selectorsOf: size = %ld (expected >= 1)\n", sels_n);
        assert(sels_n >= 1);
        int found_run = 0;
        for (intptr_t i = 1; i <= sels_n; i++) {
            if (sels_inst->slots[i] == Symbol_run) { found_run = 1; break; }
        }
        printf("Symbol_run in selectorsOf: = %d (expected 1)\n", found_run);
        assert(found_run);

        /* methodsOf: parallel check: find the method at the same index as Symbol_run. */
        oop meths = send(mirror_object(), create_symbol("methodsOf:"), 1, &TestVtable);
        struct ObjectInstance *meths_inst = (struct ObjectInstance *)meths;
        intptr_t meths_n = UNTAG_SMI(meths_inst->slots[0]);
        assert(meths_n == sels_n);  /* parallel arrays, same length */
        int found_method = 0;
        for (intptr_t i = 1; i <= meths_n; i++) {
            if (sels_inst->slots[i] == Symbol_run && meths_inst->slots[i] != NULL)
                { found_method = 1; break; }
        }
        printf("method paired with Symbol_run = %d (expected 1)\n", found_method);
        assert(found_method);
    }
    printf("TEST 9 Passed!\n\n");

    // ==========================================
    // TEST 10: requestStep: arms stepping from inside the break hook
    // ==========================================
    printf("--- TEST 10: requestStep: (confined arm from break hook) ---\n");
    {
        oop rs_cl = gc_allocate(PrimitiveClosureVtable, sizeof(struct Closure));
        ((struct Closure *)rs_cl)->home_ctx = NULL;
        ((struct Closure *)rs_cl)->home_generation = 0;
        ((struct Closure *)rs_cl)->entry_point = (void *)rs_hook_c;
        oop rs_reg[] = { rs_cl };
        send(mirror_object(), create_symbol("onBreak:"), 1, rs_reg);

        /* Method: OP_BREAK, two OP_LD_CONST, OP_RET.
           hook fires on BREAK and arms stepping; subsequent instructions
           should fire step hooks incrementing rs_step_count. */
        struct CompiledMethod *cm_rs = malloc(sizeof(struct CompiledMethod));
        cm_rs->num_registers = 4;
        cm_rs->num_arguments = 0;
        cm_rs->instruction_count = 4;
        cm_rs->constant_count = 1;
        cm_rs->access_modifier = ACCESS_PUBLIC;
        uint32_t *insts_rs = malloc(4 * sizeof(uint32_t));
        insts_rs[0] = (uint32_t)OP_BREAK;   /* fires hook; hook arms stepping via requestStep: */
        insts_rs[1] = 0x00000300;            /* OP_LD_CONST dest=3, src1=0                      */
        insts_rs[2] = 0x00000300;            /* OP_LD_CONST dest=3, src1=0                      */
        insts_rs[3] = 0x00000304;            /* OP_RET R3                                       */
        cm_rs->instructions = insts_rs;
        oop *consts_rs = malloc(1 * sizeof(oop));
        consts_rs[0] = TAG_SMI(7);
        cm_rs->constants = consts_rs;

        oop cl_rs = gc_allocate(BytecodeClosureVtable, sizeof(struct Closure));
        ((struct Closure *)cl_rs)->home_ctx = NULL;
        ((struct Closure *)cl_rs)->home_generation = 0;
        ((struct Closure *)cl_rs)->entry_point = cm_rs;

        oop rs_bind[] = { TestVtable, Symbol_run, cl_rs };
        send(mirror_object(), Symbol_bindTo, 3, rs_bind);

        /* Ensure stepping is OFF before we start */
        oop rs_off[] = { current_fiber, TAG_SMI(0) };
        send(mirror_object(), Symbol_singleStep, 2, rs_off);

        send(test_instance, Symbol_run, 0, NULL);

        /* Disable stepping after run */
        oop rs_off2[] = { current_fiber, TAG_SMI(0) };
        send(mirror_object(), Symbol_singleStep, 2, rs_off2);

        /* Restore original hook */
        oop rs_restore[] = { dbg_cl };
        send(mirror_object(), create_symbol("onBreak:"), 1, rs_restore);

        printf("requestStep: step count: %d (expected > 0)\n", rs_step_count);
        assert(rs_step_count > 0);
    }
    printf("TEST 10 Passed!\n\n");

    // ==========================================
    // TEST 11: platform readLine
    // ==========================================
    printf("--- TEST 11: platform readLine ---\n");
    {
        extern oop platform_object(void);

        /* Write a line into a temp file, rewind, point stdin at it. */
        FILE *tf = tmpfile();
        assert(tf != NULL);
        fputs("dbg:hello\n", tf);
        rewind(tf);
        FILE *old_stdin = stdin;
        stdin = tf;

        /* First readLine: should return String "dbg:hello" */
        oop line = send(platform_object(), create_symbol("readLine"), 0, NULL);
        assert(line != NULL);
        assert(!IS_SMI(line));
        struct String *s = (struct String *)line;
        assert(s->length == 9);
        assert(memcmp(s->bytes, "dbg:hello", 9) == 0);
        printf("readLine returned: \"%.*s\" (expected \"dbg:hello\")\n",
               (int)s->length, s->bytes);

        /* Second readLine at EOF: should return nil */
        oop eof_line = send(platform_object(), create_symbol("readLine"), 0, NULL);
        assert(eof_line == NULL);
        printf("readLine at EOF returned: nil (expected nil)\n");

        stdin = old_stdin;
        fclose(tf);
    }
    printf("TEST 11 Passed!\n\n");

    // ==========================================
    // TEST 12: C-assisted breakpoint table match (no OP_BREAK patching)
    // ==========================================
    printf("--- TEST 12: C-assisted breakpoint (method,offset) match ---\n");
    {
        const long K = 1;   /* break at instruction index 1 */
        /* Heap method, NO OP_BREAK:
           [0] OP_LD_CONST dest=3, src1=0 (Smi 8)
           [1] OP_ADD dest=3, src1=3, src2=3  (8+8=16) <- breakpoint here
           [2] OP_RET R3 */
        struct CompiledMethod *cm_bp = malloc(sizeof(struct CompiledMethod));
        cm_bp->num_registers = 4;
        cm_bp->num_arguments = 0;
        cm_bp->instruction_count = 3;
        cm_bp->constant_count = 1;
        cm_bp->access_modifier = ACCESS_PUBLIC;
        uint32_t *insts_bp = malloc(3 * sizeof(uint32_t));
        insts_bp[0] = 0x00000300;            /* OP_LD_CONST dest=3, src1=0 */
        insts_bp[1] = 0x03030306;            /* OP_ADD dest=3, src1=3, src2=3 */
        insts_bp[2] = 0x00000304;            /* OP_RET R3 */
        cm_bp->instructions = insts_bp;
        oop *consts_bp = malloc(1 * sizeof(oop));
        consts_bp[0] = TAG_SMI(8);
        cm_bp->constants = consts_bp;
        uint32_t insts_bp_snapshot[3] = { insts_bp[0], insts_bp[1], insts_bp[2] };

        oop cl_bp = gc_allocate(BytecodeClosureVtable, sizeof(struct Closure));
        ((struct Closure *)cl_bp)->home_ctx = NULL;
        ((struct Closure *)cl_bp)->home_generation = 0;
        ((struct Closure *)cl_bp)->entry_point = cm_bp;

        /* The bp table keys on the CompiledMethod oop (== methodOf: a context). */
        bp_expected_method = (oop)cm_bp;
        bp_expected_off = K;
        bp_hit_count = 0;

        /* Install the bp-match hook */
        oop bp_hook_cl = gc_allocate(PrimitiveClosureVtable, sizeof(struct Closure));
        ((struct Closure *)bp_hook_cl)->home_ctx = NULL;
        ((struct Closure *)bp_hook_cl)->home_generation = 0;
        ((struct Closure *)bp_hook_cl)->entry_point = (void *)bp_match_hook_c;
        oop bp_reg[] = { bp_hook_cl };
        send(mirror_object(), create_symbol("onBreak:"), 1, bp_reg);

        oop bp_bind[] = { TestVtable, Symbol_run, cl_bp };
        send(mirror_object(), Symbol_bindTo, 3, bp_bind);

        /* Ensure stepping is OFF */
        oop bp_off[] = { current_fiber, TAG_SMI(0) };
        send(mirror_object(), Symbol_singleStep, 2, bp_off);

        /* Set the breakpoint via the confined mirror prim (method oop + SMI off). */
        oop ba[] = { (oop)cm_bp, TAG_SMI(K) };
        send(mirror_object(), create_symbol("breakAt:offset:"), 2, ba);

        oop rbp = send(test_instance, Symbol_run, 0, NULL);
        printf("Result of run with bp: %ld (expected 16)\n", (long)UNTAG_SMI(rbp));
        assert(UNTAG_SMI(rbp) == 16);          /* hook ran, instruction executed normally */
        printf("bp hit count: %d (expected 1)\n", bp_hit_count);
        assert(bp_hit_count == 1);
        /* CRITICAL: no instruction mutation — the method bytes are untouched. */
        assert(insts_bp[0] == insts_bp_snapshot[0]);
        assert(insts_bp[1] == insts_bp_snapshot[1]);
        assert(insts_bp[2] == insts_bp_snapshot[2]);
        printf("instructions unmutated: yes\n");

        /* Clear the breakpoint; assert no re-fire on a second run. */
        oop ua[] = { (oop)cm_bp, TAG_SMI(K) };
        send(mirror_object(), create_symbol("unbreakAt:offset:"), 2, ua);
        bp_hit_count = 0;
        oop rbp2 = send(test_instance, Symbol_run, 0, NULL);
        assert(UNTAG_SMI(rbp2) == 16);
        printf("bp hit count after unbreak: %d (expected 0)\n", bp_hit_count);
        assert(bp_hit_count == 0);

        /* Restore original hook */
        oop bp_restore[] = { dbg_cl };
        send(mirror_object(), create_symbol("onBreak:"), 1, bp_restore);
    }
    printf("TEST 12 Passed!\n\n");

    // ==========================================
    // TEST 13: numRegistersOf: confined read prim (register-count bound)
    // ==========================================
    printf("--- TEST 13: numRegistersOf: (bound register dump) ---\n");
    {
        /* Method with a KNOWN num_registers = 4 and an OP_BREAK so the hook fires
           on a live MethodContext:
           [0] OP_LD_CONST dest=3, src1=0 (Smi 13)
           [1] OP_BREAK          <- hook fires; reads numRegistersOf:
           [2] OP_RET R3 */
        struct CompiledMethod *cm_nr = malloc(sizeof(struct CompiledMethod));
        cm_nr->num_registers = 4;
        cm_nr->num_arguments = 0;
        cm_nr->instruction_count = 3;
        cm_nr->constant_count = 1;
        cm_nr->access_modifier = ACCESS_PUBLIC;
        uint32_t *insts_nr = malloc(3 * sizeof(uint32_t));
        insts_nr[0] = 0x00000300;            /* OP_LD_CONST dest=3, src1=0 */
        insts_nr[1] = (uint32_t)OP_BREAK;     /* fires hook */
        insts_nr[2] = 0x00000304;            /* OP_RET R3 */
        cm_nr->instructions = insts_nr;
        oop *consts_nr = malloc(1 * sizeof(oop));
        consts_nr[0] = TAG_SMI(13);
        cm_nr->constants = consts_nr;

        oop cl_nr = gc_allocate(BytecodeClosureVtable, sizeof(struct Closure));
        ((struct Closure *)cl_nr)->home_ctx = NULL;
        ((struct Closure *)cl_nr)->home_generation = 0;
        ((struct Closure *)cl_nr)->entry_point = cm_nr;

        nr_hit = 0; nr_reported = -1; nr_top_reg = -2;
        oop nr_hook_cl = gc_allocate(PrimitiveClosureVtable, sizeof(struct Closure));
        ((struct Closure *)nr_hook_cl)->home_ctx = NULL;
        ((struct Closure *)nr_hook_cl)->home_generation = 0;
        ((struct Closure *)nr_hook_cl)->entry_point = (void *)nr_hook_c;
        oop nr_reg[] = { nr_hook_cl };
        send(mirror_object(), create_symbol("onBreak:"), 1, nr_reg);

        oop nr_bind[] = { TestVtable, Symbol_run, cl_nr };
        send(mirror_object(), Symbol_bindTo, 3, nr_bind);

        oop nr_off[] = { current_fiber, TAG_SMI(0) };
        send(mirror_object(), Symbol_singleStep, 2, nr_off);

        send(test_instance, Symbol_run, 0, NULL);

        printf("numRegistersOf: = %ld (expected 4)\n", nr_reported);
        assert(nr_hit == 1);
        assert(nr_reported == 4);
        printf("reg:at: (count-1) in-bounds, value = %ld (no abort)\n", nr_top_reg);

        /* Restore original hook */
        oop nr_restore[] = { dbg_cl };
        send(mirror_object(), create_symbol("onBreak:"), 1, nr_restore);
    }
    printf("TEST 13 Passed!\n\n");

    // ==========================================
    // TEST 14: bindingsOf: raw BoxedBinding array
    // ==========================================
    printf("--- TEST 14: bindingsOf: (raw BoxedBinding array) ---\n");
    {
        /* TestVtable has Symbol_run bound. bindingsOf: returns a raw ObjectInstance
           whose slots[0..vt->size-1] are BoxedBinding objects (NO size header, unlike
           selectorsOf:/methodsOf: which wrap this into a kernel Array). The in-image
           debugger uses selectorsOf:/methodsOf:; this is the direct proof of the raw prim. */
        struct Vtable *tv = (struct Vtable *)TestVtable;
        oop binds = send(mirror_object(), create_symbol("bindingsOf:"), 1, &TestVtable);
        struct ObjectInstance *binds_inst = (struct ObjectInstance *)binds;
        printf("bindingsOf: TestVtable size = %zu\n", tv->size);
        assert(tv->size >= 1);
        int found_run_binding = 0;
        for (size_t i = 0; i < tv->size; i++) {
            struct BoxedBinding *bb = (struct BoxedBinding *)binds_inst->slots[i];
            assert(obj_vtable((oop)bb) == BoxedBindingVtable);
            if (bb->symbol == Symbol_run) {
                assert(bb->method != NULL);
                found_run_binding = 1;
            }
        }
        printf("Symbol_run BoxedBinding present with non-nil method = %d (expected 1)\n",
               found_run_binding);
        assert(found_run_binding);
    }
    printf("TEST 14 Passed!\n\n");

    printf("All OEVM Mirror-Based Interactive Debugger and Hot-Swap tests PASSED successfully!\n");
    return 0;
}
