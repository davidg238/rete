/*
 * fiber_spike.c — L0 frame-ABI validation spike (throwaway).
 *
 * Validates, on a self-contained toy bytecode VM that depends on NOTHING in oevm,
 * the three properties the de-recursify rewrite rests on (docs/archive/fiber_implementation.md §10):
 *
 *   1. Non-recursive CALL/RET — the interpreter is ONE flat C loop. A bytecode call
 *      pushes a frame on the heap stack and `goto`s (here: reassigns pc); RET pops and
 *      delivers the result ON THE STACK. No C recursion per bytecode call.
 *
 *   2. Base-relative links survive stack growth (the §6a property). A recursive bytecode
 *      sum(n)=n+sum(n-1) run deep enough (n=2000) to force several realloc grows while
 *      many frames are live. Because frame back-links (saved_fb) and locals are addressed
 *      by INDEX (offset from base), not pointer, the realloc that moves `data` needs ZERO
 *      fixups. Assert sum(2000)==2001000 and grow count > 0.
 *
 *   3. 2-fiber stack-swap interleave (the bug the Zephyr hijack got wrong — ABBBAA). A
 *      scheduler task + 2 worker tasks, all bytecode, each its own heap stack. Workers
 *      loop `print(tag); yield` 3x via a LOWERED loop (counter local + JZ/JMP back-edge,
 *      i.e. whileTrue: as bytecode, NOT a C primitive). TRANSFER is the entire switch.
 *      Assert output is ABABAB (correct interleave), not ABBBAA.
 *
 * No GC here (out of scope; GC precision is asserted by the real L1, not the spike).
 *
 * Build & run:
 *   cc -O2 -Wall -Wextra -o /tmp/fiber_spike docs/spikes/fiber_spike.c && /tmp/fiber_spike
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ---- ISA ---------------------------------------------------------------- */
enum {
    OP_HALT, OP_PUSH, OP_POP, OP_ADD, OP_SUB, OP_OR, OP_AND,
    OP_PRINT_CHAR, OP_PRINT_INT, OP_LOAD_LOCAL, OP_STORE_LOCAL,
    OP_JMP, OP_JZ, OP_CALL, OP_RET, OP_TRANSFER
};

/* A task is just a heap stack + its saved registers (Toit's {Stack*, regs}).
 * data grows on demand via realloc; sp/fb/pc are all INDICES, never pointers —
 * that is the whole point: nothing on the stack points at the stack. */
typedef struct {
    intptr_t *data;
    long      len;   /* capacity in slots */
    long      sp;    /* next free slot (stack grows upward) */
    long      fb;    /* current frame base (index) */
    long      pc;    /* program counter (index into shared code[]) */
} Task;

/* ---- shared VM state ---------------------------------------------------- */
static intptr_t code[8192];   /* shared bytecode for all tasks of a demo */
static int      N;            /* assembler cursor */

static Task tasks[8];
static int  cur;              /* index of the running task */

static long grow_count;       /* reallocs observed (demo 2 reports this) */
static char outbuf[512];      /* captured PRINT_CHAR output (demo 3 asserts this) */
static int  outlen;

/* ---- tiny assembler ----------------------------------------------------- */
static int emit(intptr_t x) { code[N] = x; return N++; }

/* ---- stack ops (push may move data; indices stay valid — the §6a property) */
static void push(Task *t, intptr_t v) {
    if (t->sp >= t->len) {
        t->len *= 2;
        t->data = realloc(t->data, (size_t)t->len * sizeof(intptr_t));
        grow_count++;
    }
    t->data[t->sp++] = v;
}
static intptr_t pop(Task *t) { return t->data[--t->sp]; }

/* ---- the one flat interpreter loop ------------------------------------- */
static intptr_t run(void) {
    Task *t = &tasks[cur];
    long  pc = t->pc;
    for (;;) {
        intptr_t op = code[pc++];
        switch (op) {
        case OP_HALT:
            t->pc = pc;
            return (t->sp > 0) ? t->data[t->sp - 1] : 0;
        case OP_PUSH: { intptr_t v = code[pc++]; push(t, v); } break;
        case OP_POP:  t->sp--; break;
        case OP_ADD:  { intptr_t b = pop(t), a = pop(t); push(t, a + b); } break;
        case OP_SUB:  { intptr_t b = pop(t), a = pop(t); push(t, a - b); } break;
        case OP_OR:   { intptr_t b = pop(t), a = pop(t); push(t, a | b); } break;
        case OP_AND:  { intptr_t b = pop(t), a = pop(t); push(t, a & b); } break;
        case OP_PRINT_CHAR: { intptr_t c = pop(t); if (outlen < 511) outbuf[outlen++] = (char)c; } break;
        case OP_PRINT_INT:  { intptr_t v = pop(t); printf("%ld", (long)v); } break;
        case OP_LOAD_LOCAL:  { long i = code[pc++]; push(t, t->data[t->fb + i]); } break;
        case OP_STORE_LOCAL: { long i = code[pc++]; t->data[t->fb + i] = pop(t); } break;
        case OP_JMP: pc = code[pc]; break;
        case OP_JZ:  { intptr_t v = pop(t); long target = code[pc++]; if (v == 0) pc = target; } break;
        case OP_CALL: {
            long target  = code[pc++];
            long argc    = code[pc++];
            long nlocals = code[pc++];
            intptr_t argbuf[16];
            for (long i = argc - 1; i >= 0; i--) argbuf[i] = pop(t);  /* pop args off stack */
            push(t, t->fb);   /* saved_fb at base+0  (an INDEX => relative => realloc-safe) */
            push(t, pc);      /* saved_pc at base+1  (return address into immutable code[]) */
            t->fb = t->sp;    /* new frame base */
            for (long i = 0; i < argc; i++)    push(t, argbuf[i]);    /* locals 0..argc-1 = args */
            for (long i = argc; i < nlocals; i++) push(t, 0);          /* extra locals zeroed */
            pc = target;
        } break;
        case OP_RET: {
            intptr_t result   = pop(t);
            long     cur_fb   = t->fb;
            long     saved_pc = (long)t->data[cur_fb - 1];
            long     saved_fb = (long)t->data[cur_fb - 2];
            t->sp = cur_fb - 2;   /* drop locals + the two saved-link slots */
            t->fb = saved_fb;
            pc    = saved_pc;
            push(t, result);      /* result delivered ON THE STACK, not via a C return */
        } break;
        case OP_TRANSFER: {
            long target = code[pc++];
            intptr_t v  = pop(t);     /* value carried to the target */
            t->pc = pc;               /* park the current task */
            cur   = (int)target;
            t     = &tasks[cur];      /* swap to target's heap stack + regs */
            pc    = t->pc;
            push(t, v);               /* delivered on target's stack; it resumes at its own pc */
        } break;
        default:
            fprintf(stderr, "bad opcode %ld at pc=%ld\n", (long)op, pc - 1);
            exit(2);
        }
    }
}

/* ---- task setup -------------------------------------------------------- */
static void init_task(int idx, long entry_pc, long nlocals) {
    long cap = 8;                 /* start small so demo 2 forces real grows */
    if (cap < nlocals) cap = nlocals;
    Task *t = &tasks[idx];
    t->data = calloc((size_t)cap, sizeof(intptr_t));  /* locals pre-zeroed */
    t->len  = cap;
    t->sp   = nlocals;            /* reserve nlocals at the bottom (root-style frame, fb=0) */
    t->fb   = 0;
    t->pc   = entry_pc;
}
static void free_task(int idx) { free(tasks[idx].data); tasks[idx].data = NULL; }

/* ===================================================================== */
/* Demo 1: non-recursive CALL/RET — twice(21) = 42                       */
/* ===================================================================== */
static long build_twice(int x) {
    N = 0;
    long main_entry = N;
    emit(OP_PUSH); emit(x);
    emit(OP_CALL); int p_t = emit(0); emit(1); emit(1);   /* twice, argc=1, nlocals=1 */
    emit(OP_HALT);
    long twice = N; code[p_t] = twice;
    emit(OP_LOAD_LOCAL); emit(0);
    emit(OP_LOAD_LOCAL); emit(0);
    emit(OP_ADD);
    emit(OP_RET);
    return main_entry;
}

/* ===================================================================== */
/* Demo 2: base-relative links survive stack growth — sum(n)=n+sum(n-1)  */
/* ===================================================================== */
static long build_sum(int nval) {
    N = 0;
    long main_entry = N;
    emit(OP_PUSH); emit(nval);
    emit(OP_CALL); int p_main = emit(0); emit(1); emit(1);
    emit(OP_HALT);
    long sum_entry = N;
    code[p_main] = sum_entry;
    emit(OP_LOAD_LOCAL); emit(0);            /* n */
    emit(OP_JZ); int p_base = emit(0);       /* if n==0 -> base */
    emit(OP_LOAD_LOCAL); emit(0);            /* n  (left on stack for the ADD) */
    emit(OP_LOAD_LOCAL); emit(0);            /* n */
    emit(OP_PUSH); emit(1);
    emit(OP_SUB);                            /* n-1 */
    emit(OP_CALL); int p_rec = emit(0); emit(1); emit(1);
    code[p_rec] = sum_entry;                 /* sum(n-1) */
    emit(OP_ADD);                            /* n + sum(n-1) */
    emit(OP_RET);
    long base = N; code[p_base] = base;
    emit(OP_PUSH); emit(0);
    emit(OP_RET);
    return main_entry;
}

/* ===================================================================== */
/* Demo 3: 2-fiber stack-swap interleave — expect ABABAB                  */
/* ===================================================================== */
enum { SCHED = 0, WA = 1, WB = 2 };

static long build_worker(int tag) {
    long entry = N;
    emit(OP_POP);                            /* discard initial resume value from scheduler */
    emit(OP_PUSH); emit(3);
    emit(OP_STORE_LOCAL); emit(0);           /* counter = 3 */
    long wloop = N;
    emit(OP_LOAD_LOCAL); emit(0);
    emit(OP_JZ); int p_done = emit(0);
    emit(OP_PUSH); emit(tag);
    emit(OP_PRINT_CHAR);
    emit(OP_LOAD_LOCAL); emit(0);
    emit(OP_PUSH); emit(1);
    emit(OP_SUB);
    emit(OP_STORE_LOCAL); emit(0);           /* counter -= 1 */
    emit(OP_PUSH); emit(0);                   /* yield marker */
    emit(OP_TRANSFER); emit(SCHED);
    emit(OP_POP);                            /* discard resume value */
    emit(OP_JMP); int p_back = emit(0);
    code[p_back] = wloop;
    long wdone = N; code[p_done] = wdone;
    emit(OP_PUSH); emit(1);                   /* done marker */
    emit(OP_TRANSFER); emit(SCHED);
    emit(OP_POP);
    emit(OP_JMP); int p_back2 = emit(0);
    code[p_back2] = wdone;
    return entry;
}

static long g_wa, g_wb;   /* worker entry pcs */

static long build_fibers(void) {
    N = 0;
    /* scheduler: locals 0=doneA, 1=doneB */
    long sched_entry = N;
    long sloop = N;
    emit(OP_PUSH); emit(0);
    emit(OP_TRANSFER); emit(WA);             /* run A; on return its marker is on top */
    emit(OP_LOAD_LOCAL); emit(0);
    emit(OP_OR);
    emit(OP_STORE_LOCAL); emit(0);           /* doneA |= marker */
    emit(OP_PUSH); emit(0);
    emit(OP_TRANSFER); emit(WB);
    emit(OP_LOAD_LOCAL); emit(1);
    emit(OP_OR);
    emit(OP_STORE_LOCAL); emit(1);           /* doneB |= marker */
    emit(OP_LOAD_LOCAL); emit(0);
    emit(OP_LOAD_LOCAL); emit(1);
    emit(OP_AND);
    emit(OP_JZ); int p_loop = emit(0);       /* if !(doneA & doneB) -> sloop */
    emit(OP_HALT);
    code[p_loop] = sloop;
    g_wa = build_worker('A');
    g_wb = build_worker('B');
    return sched_entry;
}

/* ===================================================================== */
int main(void) {
    int ok = 1;

    /* ---- Demo 1 ---- */
    grow_count = 0;
    long e1 = build_twice(21);
    init_task(0, e1, 0);
    cur = 0;
    intptr_t r1 = run();
    free_task(0);
    int p1 = (r1 == 42);
    ok &= p1;
    printf("Demo 1  non-recursive CALL/RET     : twice(21) = %ld (expect 42)            %s\n",
           (long)r1, p1 ? "PASS" : "FAIL");

    /* ---- Demo 2 ---- */
    grow_count = 0;
    long e2 = build_sum(2000);
    init_task(0, e2, 0);
    cur = 0;
    intptr_t r2 = run();
    free_task(0);
    int p2 = (r2 == 2001000 && grow_count > 0);
    ok &= p2;
    printf("Demo 2  relative links vs realloc  : sum(2000) = %ld (expect 2001000), %ld grows  %s\n",
           (long)r2, grow_count, p2 ? "PASS" : "FAIL");

    /* ---- Demo 3 ---- */
    grow_count = 0; outlen = 0;
    long e3 = build_fibers();
    init_task(SCHED, e3,   2);   /* 2 locals: doneA, doneB */
    init_task(WA,    g_wa, 1);   /* 1 local: counter */
    init_task(WB,    g_wb, 1);
    cur = SCHED;
    run();
    free_task(SCHED); free_task(WA); free_task(WB);
    outbuf[outlen] = '\0';
    int p3 = (strcmp(outbuf, "ABABAB") == 0);
    ok &= p3;
    printf("Demo 3  2-fiber stack-swap         : out = \"%s\" (expect ABABAB)            %s\n",
           outbuf, p3 ? "PASS" : "FAIL");

    printf("\n%s\n", ok ? "SPIKE PASS — L0 frame ABI validated; safe to start L0."
                        : "SPIKE FAIL — fix the ABI on the toy before touching the engine.");
    return ok ? 0 : 1;
}
