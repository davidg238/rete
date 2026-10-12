/* OP_MK_CONTEXT / OP_LD_CTX / OP_ST_CTX round-trip (de-recursify deferred-lift
 * Phase 1, Task 2). LIVE opcodes — emitted by the compiler's finalize_contexts()
 * for escaping captures; this test exercises them directly at the VM level. Hand-assembles a tiny CompiledMethod:
 *
 *   num_arguments = 0  =>  CTX_REG (the fixed frame-Context register, per the
 *   controller convention `method->num_arguments + 1`) = register 1.
 *
 *   r0 = receiver (unused after prologue, reused as scratch)
 *   r1 = CTX_REG  (holds the Context OP_MK_CONTEXT allocates)
 *   r2 = result
 *
 *   0: OP_MK_CONTEXT dest=r1, num_slots(src1)=1, outer(src2)=0        (outer=NULL, method root)
 *   1: OP_LD_CONST   dest=r0, src1=0                                   (r0 = SMI 42)
 *   2: OP_ST_CTX     dest(srcval)=r0, depth(src1)=0, idx(src2)=0       (ctx.slots[0] = 42)
 *   3: OP_LD_CTX     dest=r2, depth(src1)=0, idx(src2)=0               (r2 = ctx.slots[0])
 *   4: OP_RET        dest=r2
 *
 * Standalone, links the Model A (solidified_image.c) set the other C unit
 * tests use — see tests/test_context.c / tests/test_fibers.c for precedent.
 *
 * cc -Wall -std=c99 -Isrc src/vm.c src/gc.c src/dispatch.c src/platform.c
 *   src/serial.c src/udp.c src/mirror.c src/backend/posix/*.c
 *   src/oimg_loader.c src/solidified_image.c tests/test_ctx_ops.c
 *   -o /tmp/test_ctx_ops -lm
 */
#include "../src/oevm.h"
#include "../src/frame_abi.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

extern void init_solidified_vtables(void);
extern oop run_interpreter(oop closure, oop receiver, int argc, oop *args);
extern oop BytecodeClosureVtable;
extern oop gc_allocate(oop vtable, size_t size);

static uint32_t enc(uint8_t op, uint8_t dest, uint8_t src1, uint8_t src2) {
    return (uint32_t)op | ((uint32_t)dest << 8) | ((uint32_t)src1 << 16) | ((uint32_t)src2 << 24);
}

int main(void) {
    init_solidified_vtables();
    assert(BytecodeClosureVtable != NULL);

    struct CompiledMethod *method = malloc(sizeof(struct CompiledMethod));
    method->num_arguments = 0;   /* CTX_REG = num_arguments + 1 = 1 */
    method->num_registers = 3;   /* r0 = receiver/scratch, r1 = CTX_REG, r2 = result */
    method->instruction_count = 5;

    oop *consts = malloc(1 * sizeof(oop));
    consts[0] = TAG_SMI(42);
    method->constants = consts;
    method->constant_count = 1;

    uint32_t *insts = malloc(5 * sizeof(uint32_t));
    insts[0] = enc(OP_MK_CONTEXT, /*dest=*/1, /*num_slots=*/1, /*outer=*/0);
    insts[1] = enc(OP_LD_CONST,   /*dest=*/0, /*src1=const idx*/0, 0);
    insts[2] = enc(OP_ST_CTX,     /*srcval reg=*/0, /*depth=*/0, /*idx=*/0);
    insts[3] = enc(OP_LD_CTX,     /*dest=*/2, /*depth=*/0, /*idx=*/0);
    insts[4] = enc(OP_RET,        /*dest=*/2, 0, 0);
    method->instructions = insts;

    struct Closure *cl = (struct Closure *)gc_allocate(BytecodeClosureVtable, sizeof(struct Closure));
    cl->home_ctx = NULL;
    cl->home_generation = 0;
    cl->entry_point = method;
    cl->captured_env = NULL;

    oop result = run_interpreter((oop)cl, NULL, 0, NULL);
    printf("CTX OPS result: %ld (expected 42)\n", (long)UNTAG_SMI(result));
    assert(result == TAG_SMI(42));

    free(consts);
    free(insts);
    free(method);

    printf("CTX OPS OK\n");
    return 0;
}
