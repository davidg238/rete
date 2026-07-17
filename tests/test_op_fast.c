/* OP_LT/OP_LE/OP_GT/OP_GE + OP_AT/OP_AT_PUT fast-path opcodes (perf/op-cmp-at).
 *
 * All six share OP_SEND's operand layout (dest=argc, src1=selector const idx,
 * src2=receiver base reg, args contiguous above, result lands in R(src2)), so a
 * fast-path miss falls into the unchanged send path and semantics are the send's.
 * This test exercises the fast paths only:
 *
 *   - OP_LE / OP_LT / OP_GT / OP_GE on SMI x SMI -> true_sym_obj / false_sym_obj
 *   - OP_AT on an indexable (slot0 = SMI length): 1-based element read
 *   - OP_AT_PUT: 1-based element write, returns the stored value
 *
 * Fallback tiers (Float/LargeInteger compares, non-indexable receivers) are the
 * send path itself and are covered by the nsl suite.
 *
 * cc -Wall -std=c99 -Isrc src/vm.c src/gc.c src/dispatch.c src/platform.c
 *   src/serial.c src/udp.c src/mirror.c src/backend/posix/ (all five .c)
 *   src/oimg_loader.c src/solidified_image.c tests/test_op_fast.c
 *   -o /tmp/test_op_fast -lm
 */
#include "../src/oevm.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

extern void init_solidified_vtables(void);
extern oop run_interpreter(oop closure, oop receiver, int argc, oop *args);
extern oop BytecodeClosureVtable;
extern oop gc_allocate(oop vtable, size_t size);
extern oop indexable_new(intptr_t n);
extern const struct Symbol true_sym_obj;
extern const struct Symbol false_sym_obj;

/* Raw values on purpose: the test must compile against a VM that predates the
 * opcodes (RED), then match the enum once oevm.h defines them (GREEN). */
#define TOP_LT     0x19
#define TOP_LE     0x1A
#define TOP_GT     0x1B
#define TOP_GE     0x1C
#define TOP_AT     0x1D
#define TOP_AT_PUT 0x1E

static uint32_t enc(uint8_t op, uint8_t dest, uint8_t src1, uint8_t src2) {
    return (uint32_t)op | ((uint32_t)dest << 8) | ((uint32_t)src1 << 16) | ((uint32_t)src2 << 24);
}

/* Run one hand-assembled method: r1 = a, r2 = b, then `op` with argc/selector
 * layout (base reg 1), OP_RET r1. Returns the delivered result. */
static oop run_binop(uint8_t op, int argc, oop a, oop b, oop c) {
    struct CompiledMethod *method = calloc(1, sizeof(struct CompiledMethod));
    method->num_arguments = 0;
    method->num_registers = 4;          /* r0 recv, r1 base, r2 arg1, r3 arg2 */
    oop *consts = calloc(4, sizeof(oop));
    consts[0] = a; consts[1] = b; consts[2] = c;
    consts[3] = NULL;                    /* selector slot: fast path must not read it */
    method->constants = consts;
    method->constant_count = 4;
    method->access_modifier = ACCESS_PUBLIC;
    uint32_t *insts = calloc(6, sizeof(uint32_t));
    int n = 0;
    insts[n++] = enc(OP_LD_CONST, 1, 0, 0);
    insts[n++] = enc(OP_LD_CONST, 2, 1, 0);
    if (argc == 2) insts[n++] = enc(OP_LD_CONST, 3, 2, 0);
    insts[n++] = enc(op, (uint8_t)argc, /*selector const idx=*/3, /*base reg=*/1);
    insts[n++] = enc(OP_RET, 1, 0, 0);
    method->instructions = insts;
    method->instruction_count = (uint32_t)n;

    struct Closure *cl = (struct Closure *)gc_allocate(BytecodeClosureVtable, sizeof(struct Closure));
    cl->home_ctx = NULL;
    cl->home_generation = 0;
    cl->entry_point = method;
    cl->captured_env = NULL;
    oop result = run_interpreter((oop)cl, NULL, 0, NULL);
    free(consts); free(insts); free(method);
    return result;
}

int main(void) {
    init_solidified_vtables();
    assert(BytecodeClosureVtable != NULL);

    /* --- comparisons, SMI x SMI --- */
    assert(run_binop(TOP_LE, 1, TAG_SMI(5), TAG_SMI(7), NULL) == (oop)&true_sym_obj);
    assert(run_binop(TOP_LE, 1, TAG_SMI(9), TAG_SMI(7), NULL) == (oop)&false_sym_obj);
    assert(run_binop(TOP_LE, 1, TAG_SMI(7), TAG_SMI(7), NULL) == (oop)&true_sym_obj);
    assert(run_binop(TOP_LT, 1, TAG_SMI(7), TAG_SMI(7), NULL) == (oop)&false_sym_obj);
    assert(run_binop(TOP_LT, 1, TAG_SMI(-3), TAG_SMI(2), NULL) == (oop)&true_sym_obj);
    assert(run_binop(TOP_GT, 1, TAG_SMI(8), TAG_SMI(2), NULL) == (oop)&true_sym_obj);
    assert(run_binop(TOP_GT, 1, TAG_SMI(2), TAG_SMI(8), NULL) == (oop)&false_sym_obj);
    assert(run_binop(TOP_GE, 1, TAG_SMI(2), TAG_SMI(2), NULL) == (oop)&true_sym_obj);
    assert(run_binop(TOP_GE, 1, TAG_SMI(1), TAG_SMI(2), NULL) == (oop)&false_sym_obj);
    printf("cmp fast paths OK\n");

    /* --- OP_AT / OP_AT_PUT on an indexable --- */
    oop ix = indexable_new(3);
    ((struct ObjectInstance *)ix)->slots[2] = TAG_SMI(77);      /* ix at: 2 = 77 */
    assert(run_binop(TOP_AT, 1, ix, TAG_SMI(2), NULL) == TAG_SMI(77));
    assert(run_binop(TOP_AT_PUT, 2, ix, TAG_SMI(3), TAG_SMI(55)) == TAG_SMI(55));
    assert(((struct ObjectInstance *)ix)->slots[3] == TAG_SMI(55));
    printf("at fast paths OK\n");

    printf("OP FAST OK\n");
    return 0;
}
