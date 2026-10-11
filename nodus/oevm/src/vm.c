#ifndef __ZEPHYR__
#define _POSIX_C_SOURCE 200809L
#endif
#include "oevm.h"
#include "platform_backend.h"
#include "frame_abi.h"
#include "oimg.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <setjmp.h>

/* In-language monotonic millisecond clock. Function-pointer hook so the Zephyr
   appliance can override it with k_uptime_get(). The POSIX path is compiled out
   under __ZEPHYR__, so the firmware link needs no POSIX timer symbols (the app
   reassigns the pointer before first use anyway). */
static uint64_t default_millis(void) {
#ifdef __ZEPHYR__
    return 0;   /* overridden by the app before first use */
#else
    /* MS SINCE THIS PROCESS STARTED, not since the machine booted. The contract this
       hook exists to satisfy is k_uptime_get()'s — "ms since boot" — and on the host a
       process IS the boot. Raw CLOCK_MONOTONIC broke that: `0 millis` answered the
       machine's uptime, so any node reading it as its own awake time (the deep-sleep
       clock's elapsedSec accrual) got days instead of seconds, and past ~12.4 days of
       host uptime the value no longer fits a 31-bit SMI at all. Every delta-taking
       caller is unaffected — a constant offset cancels in a subtraction.

       OEVM_MILLIS_FIXED_MS pins the answer to a constant, so a host test can pose a
       specific awake window (the same shape of host-only knob as OEVM_NTP_SECONDS and
       OEVM_RTC_FILE). Resolved once; unset in every normal run. */
    static uint64_t base_ms = 0;
    static int64_t fixed_ms = -1;
    struct timespec ts;
    uint64_t now;
    if (fixed_ms < 0) {
        const char *e = getenv("OEVM_MILLIS_FIXED_MS");
        fixed_ms = (e && *e) ? (int64_t)strtoll(e, NULL, 10) : -2;
    }
    if (fixed_ms >= 0) return (uint64_t)fixed_ms;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    now = (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
    if (base_ms == 0) base_ms = now;
    return now - base_ms;
#endif
}
uint64_t (*oevm_millis)(void) = default_millis;

extern oop gc_allocate(oop vtable, size_t size);



/* Global Stack Memory for Sequential Stack Allocation */
/* Milestone A Task 7: the exec-context stack is FIXED-capacity. INIT_WORDS is the
   hard ceiling — the stack is allocated once at this size and never grows or moves,
   so raw frame pointers stay valid for the life of the run. Pushing past it aborts
   loudly (stack_overflow_abort). The growable-stack machinery (grow-on-demand,
   realloc-copy, in-frame rebase, the CAP/GROW factor macros) was removed with the
   no-NLR runtime. -D lower on embedded to shrink the fixed footprint. */
#ifndef VM_STACK_INIT_WORDS
#define VM_STACK_INIT_WORDS 16384
#endif
oop  g_active_stack = NULL;   /* the running execution context's Stack object (GC root); main bootstraps it lazily.
                                 With the Fiber wrapper removed (Milestone A Task 6), this + vm_stack/vm_stack_ptr
                                 + current_frame IS the entire running state — there is no Fiber object. */
oop *vm_stack = NULL;         /* == ((struct Stack*)g_active_stack)->data once bootstrapped */
size_t vm_stack_ptr = 0;      /* live top cursor (the flat loop's authoritative stack top) */

/* Active Frame Register */
struct MethodContext *current_frame = NULL;

/* Milestone D crash boundary: one active protection frame (the reactor's).
   Built as "unwind to a marked frame" — in D exactly one marker exists, but the
   code path is the general one so phase-2 on:do: generalizes it (exceptions.md C4). */
static jmp_buf *g_protect_top = NULL;   /* NULL = no protection active */
oop g_raised_condition = NULL;          /* rooted by gc_collect; see gc.c */
/* Milestone E Task 1: the Kernel module instance (Kernel usingPlatform: platform),
   captured by dispatch.c's send_resolve at the first `main:args:` send — the only
   object that reaches the `Record` class from any context. The error:/error:tag:
   raise prims send it `crashRecordTag:text:` to build the in-flight crash Record.
   Rooted in gc_collect (see gc.c); NULL until app entry (both boot paths pass the
   kernel instance as main:args: arg 0, before any app code can raise). */
oop g_kernel_instance = NULL;

extern int gc_temp_roots_count;         /* gc.c:645 — save/restore across a fire */

/* Milestone E Task 1 diagnostic helper: if `condition` is a crash Record (built
   by Object>>error:tag: as `Record tag: aTag with: #text -> aText`), pull its
   #text String for the unprotected abort message. Guarded: we only ever run this
   on the die-anyway abort path, and we go through the receiver's OWN
   `at:ifAbsent:` method (found by walking its vtable, string-compare — the
   does_not_understand idiom) rather than poking raw slots, so a heap object that
   is not Record-shaped simply doesn't resolve the selector and we answer NULL.
   The ifAbsent block is nil: every Record we raise always carries #text, so the
   present-key path returns the value and never sends `value` to that nil. */
static oop vm_condition_text(oop condition) {
    if (condition == NULL || IS_SMI(condition)) return NULL;
    if (SymbolVtable == NULL) return NULL;
    oop atIfAbsentSel = NULL;
    oop curr = obj_vtable(condition);
    while (curr != NULL && !IS_SMI(curr)) {
        struct Vtable *vt = (struct Vtable *)curr;
        struct Binding *bs = vtable_bindings((oop)vt);
        for (size_t i = 0; i < vt->size; i++) {
            struct Symbol *s = (struct Symbol *)binding_symbol(&bs[i]);
            if (s != NULL && obj_vtable((oop)s) == SymbolVtable &&
                strcmp(symbol_name(s), "at:ifAbsent:") == 0) {
                atIfAbsentSel = (oop)s;
                break;
            }
        }
        if (atIfAbsentSel != NULL) break;
        curr = vtable_delegate((oop)vt);
    }
    if (atIfAbsentSel == NULL) return NULL;
    /* Fresh interned #text tag to probe the fields Map (rooted across the send). */
    extern void gc_temp_root_push(oop o);
    extern int gc_temp_roots_count;
    int saved_roots = gc_temp_roots_count;
    gc_temp_root_push(condition);
    static const char textName[] = "text";
    oop textSym = symbol_new(textName, sizeof textName - 1);   /* rete#226 */
    gc_temp_root_push(textSym);
    oop probe_args[2] = { textSym, NULL };   /* ifAbsent nil: key is always present */
    oop v = send(condition, atIfAbsentSel, 2, probe_args);
    gc_temp_roots_count = saved_roots;
    if (v != NULL && StringVtable && obj_vtable(v) == StringVtable) return v;
    return NULL;
}

/* The raise primitive. `self error:` calls this. Terminate-only: never returns to
   the raise point when protected. Unprotected -> preserve today's print+abort. */
void vm_raise(oop condition) {
    if (g_protect_top != NULL) {
        g_raised_condition = condition;
        longjmp(*g_protect_top, 1);
    }
    /* no protection frame: behave exactly as PRIM_error did before Milestone D,
       with Milestone E Task 1's extension for a Record condition (print #text). */
    if (condition && StringVtable && obj_vtable(condition) == StringVtable) {
        struct String *s = (struct String *)condition;
        fprintf(stderr, "Error: ");
        fwrite(s->bytes, 1, s->length, stderr);
        fprintf(stderr, "\n");
    } else if (condition && SymbolVtable && obj_vtable(condition) == SymbolVtable) {
        fprintf(stderr, "Error: %s\n", symbol_name((struct Symbol *)condition));
    } else {
        oop text = vm_condition_text(condition);   /* Record #text, else NULL */
        if (text != NULL) {
            struct String *s = (struct String *)text;
            fprintf(stderr, "Error: ");
            fwrite(s->bytes, 1, s->length, stderr);
            fprintf(stderr, "\n");
        } else {
            fprintf(stderr, "Error: ?\n");
        }
    }
    abort();
}

/* Run one block to completion under a fresh protection frame. Clean -> nil;
   caught fault -> the raised condition. Unwind = restore the three scalars that
   define live execution state (mirrors exit_frame, vm.c:887-888) + the temp-root
   count. The half-built object graph of the crashed block becomes unreachable and
   is swept normally (non-moving GC; nothing to fix up). */
extern oop run_interpreter(oop closure, oop receiver, int argc, oop *args); /* defined below in this file */
oop vm_protected_fire(oop block, oop value) {
    jmp_buf env;
    jmp_buf *saved_top          = g_protect_top;
    size_t saved_sp             = vm_stack_ptr;
    struct MethodContext *saved_frame = current_frame;
    int saved_temp_roots        = gc_temp_roots_count;
    oop result;
    g_protect_top = &env;
    if (setjmp(env) == 0) {
        (void)run_interpreter(block, block, 1, &value);
        result = NULL;                     /* block result discarded; clean == nil */
    } else {
        vm_stack_ptr        = saved_sp;
        current_frame       = saved_frame;
        gc_temp_roots_count = saved_temp_roots;
        result              = g_raised_condition;
        g_raised_condition  = NULL;
    }
    g_protect_top = saved_top;
    return result;
}

/* Milestone E Task 2: predicate-filtered catch. `[prot] on: [:e|pred] do: [:e|body]`.
   A vm_protected_fire variant: run protBlock under a fresh protection frame; on a
   caught fault evaluate predBlock against the condition and either run handlerBlock
   (match) or re-raise to the next outer frame (no match). Terminate-only: the
   handler runs AFTER the unwind; there is no resume/retry.

   Clean completion -> the protected block's value. Matched catch -> the handler's
   value. Non-match -> vm_raise(cond) after restoring g_protect_top, which longjmps
   to the next outer protection frame (cascade) and never returns here.

   GC (precise, non-moving, exhaustion-triggered): on the caught path `cond`,
   `predBlock` and `handlerBlock` are all live across the run_interpreter(pred) and
   run_interpreter(handler) RE-ENTRIES (each can allocate and collect), and with no
   C-stack scan they have no other root during those calls — so all three are pushed
   as temp roots via the gc_temp_roots_count SAVE/RESTORE idiom (build_and_raise /
   context_alloc precedent) before the first re-entry and popped after the last.
   (The protBlock and its arg on the clean path are rooted for the duration by being
   stored into the GC-scanned frame run_interpreter builds, exactly as in
   vm_protected_fire.) */
oop vm_on_do(oop protBlock, oop predBlock, oop handlerBlock) {
    extern void gc_temp_root_push(oop o);
    jmp_buf env;
    jmp_buf *saved_top          = g_protect_top;
    size_t saved_sp             = vm_stack_ptr;
    struct MethodContext *saved_frame = current_frame;
    int saved_temp_roots        = gc_temp_roots_count;
    oop arg = NULL;                        /* protected block is zero-arg; ignored */
    g_protect_top = &env;
    if (setjmp(env) == 0) {
        oop result = run_interpreter(protBlock, protBlock, 1, &arg);
        g_protect_top = saved_top;
        return result;                     /* clean: the protected block's value */
    }
    /* Caught a fault: unwind to this frame (mirror vm_protected_fire). */
    vm_stack_ptr        = saved_sp;
    current_frame       = saved_frame;
    gc_temp_roots_count = saved_temp_roots;
    /* Restore the OUTER protection level BEFORE evaluating pred/handler or
       re-raising, so a fault inside them (or a non-match re-raise) cascades to the
       next outer handler frame-by-frame. */
    g_protect_top       = saved_top;
    oop cond            = g_raised_condition;
    g_raised_condition  = NULL;
    /* Root cond, predBlock, handlerBlock across the two re-entries. */
    int roots_here = gc_temp_roots_count;
    gc_temp_root_push(cond);
    gc_temp_root_push(predBlock);
    gc_temp_root_push(handlerBlock);
    oop verdict = run_interpreter(predBlock, predBlock, 1, &cond);
    if (is_truthy(verdict)) {
        oop hres = run_interpreter(handlerBlock, handlerBlock, 1, &cond);
        gc_temp_roots_count = roots_here;
        return hres;                       /* matched: the handler's value */
    }
    gc_temp_roots_count = roots_here;
    vm_raise(cond);                        /* no match: cascade; never returns */
    return NULL;                           /* unreachable */
}

/* Milestone E Task 3: always-run cleanup. `[prot] ensure: [cleanup]`.
   A vm_on_do sibling: run protBlock under a fresh protection frame, then run
   cleanupBlock on BOTH exits — clean completion (return the protected value)
   AND a caught fault (re-raise the condition AFTER cleanup). Terminate-only:
   cleanup runs after the unwind; there is no resume/retry.

   Innermost-first ordering falls out of natural C-stack nesting: an inner
   vm_ensure's setjmp catches, runs its cleanup and re-raises BEFORE the outer
   frame's setjmp sees the fault — so `inner` prints before `outer` (test 06).

   Policy reservation: E ships only the `always` policy. The `{block, policy}`
   marker is reserved for a later `ifCurtailed:` (curtailed-only) — a one-line
   guard on the CLEAN path here — and is NOT branched on now.

   GC (precise, non-moving, exhaustion-triggered): on the CLEAN path `result` is
   a live oop held across the cleanup run_interpreter RE-ENTRY (cleanup can
   allocate -> collect) with no other root (no C-stack scan), so `result` (and
   `cleanupBlock`) are pushed as temp roots before the re-entry and popped after.
   On the CAUGHT path `cond` and `cleanupBlock` are likewise rooted across the
   cleanup re-entry before the re-raise. The gc_temp_roots_count SAVE/RESTORE
   idiom balances every exit. */
oop vm_ensure(oop protBlock, oop cleanupBlock) {
    extern void gc_temp_root_push(oop o);
    jmp_buf env;
    jmp_buf *saved_top          = g_protect_top;
    size_t saved_sp             = vm_stack_ptr;
    struct MethodContext *saved_frame = current_frame;
    int saved_temp_roots        = gc_temp_roots_count;
    oop arg = NULL;                        /* prot + cleanup are zero-arg; ignored */
    g_protect_top = &env;
    if (setjmp(env) == 0) {
        oop result = run_interpreter(protBlock, protBlock, 1, &arg);
        /* Restore the OUTER protection level BEFORE running cleanup, so a fault
           inside cleanup cascades to the next outer frame (matching the caught
           path) rather than re-entering this now-spent setjmp. */
        g_protect_top = saved_top;
        /* result + cleanupBlock are live across the cleanup re-entry (it can
           allocate -> GC); root them (no conservative C-stack scan). */
        int roots_here = gc_temp_roots_count;
        gc_temp_root_push(result);
        gc_temp_root_push(cleanupBlock);
        (void)run_interpreter(cleanupBlock, cleanupBlock, 1, &arg);
        gc_temp_roots_count = roots_here;
        return result;                     /* clean: the protected block's value */
    }
    /* Caught a fault: unwind to this frame (mirror vm_on_do). */
    vm_stack_ptr        = saved_sp;
    current_frame       = saved_frame;
    gc_temp_roots_count = saved_temp_roots;
    /* Restore the OUTER protection level BEFORE running cleanup / re-raising, so a
       fault inside cleanup (or the re-raise itself) cascades to the next outer
       frame rather than the spent local setjmp. */
    g_protect_top       = saved_top;
    oop cond            = g_raised_condition;
    g_raised_condition  = NULL;
    /* Root cond + cleanupBlock across the cleanup re-entry. */
    int roots_here = gc_temp_roots_count;
    gc_temp_root_push(cond);
    gc_temp_root_push(cleanupBlock);
    (void)run_interpreter(cleanupBlock, cleanupBlock, 1, &arg);
    gc_temp_roots_count = roots_here;
    vm_raise(cond);                        /* re-raise AFTER cleanup; never returns */
    return NULL;                           /* unreachable */
}

/* Debugger single-step state (Milestone A Task 6: rehomed off the deleted struct
   Fiber to file-scope globals — this is debugger state, not scheduling state).
   g_single_step: 1 while stepping. g_hook_step_request: -1 = none, else the
   post-hook step arm that survives the break-hook save/restore. */
int g_single_step = 0;
int g_hook_step_request = -1;

/* Precise GC roots for the boot/first-send window (oevm#21). While
   run_interpreter lazily bootstraps the main fiber + Stack + boot symbols, the
   incoming `receiver` and `args` are live but not yet written into a scanned
   frame — held only in C locals. With no conservative C-stack scan, a GC_STRESS
   collection during bootstrap would otherwise sweep the boot entry instance and
   reuse its slot for g_active_stack (corrupting the stack). gc_collect marks
   these so the in-flight receiver/args are PRECISE roots for exactly that
   window; both are NULL/0 outside it (no-ops). */
oop  g_boot_inflight_receiver = NULL;
oop *g_boot_inflight_args = NULL;
int  g_boot_inflight_argc = 0;

/* Monotonic generation counter for lifecycle protection */
uint32_t current_generation_tag = 0;

/* Symbol singletons */
oop Symbol_value = NULL;

/* Set to 1 while the debugger hook is executing; routes single-step requests
   (debugger single-step state) through hook_step_request instead of applying
   them immediately. */
int in_debugger_hook = 0;

/* The registered break hook (dispatch.c); used by both the OP_BREAK path and the
   Task 7 C-assisted breakpoint match at the top of the interpreter loop. */
extern oop debugger_hook;

/* C-assisted breakpoint table (Task 7). A breakpoint is a (method, offset) pair:
   `method` is the CompiledMethod oop (== closure_entry of the running closure) and
   `offset` is the instruction index within cm_instructions(method). We do NOT patch
   OP_BREAK into methods (solidified .rodata methods are read-only and blob methods
   abort on mutation); instead the interpreter checks the current (method, curoff)
   at the TOP of its loop while the table is non-empty and fires the registered
   debugger_hook on a match. Continue is trivial: returning from the hook lets the
   instruction execute and the breakpoint re-fires next visit -- no restore/re-arm. */
static struct { oop method; long off; } g_bp[64];
static int g_bp_count = 0;

/* ---------------------------------------------------------------------------
   #164 SPIKE: exact per-method call counts.

   The question spans cannot answer is "which code inside the box is expensive".
   Spans measure hand-placed brackets, so finding out costs a guess, a build and
   a reflash per question. This counts every method activation exactly, at the
   one chokepoint every activation already passes through, and needs no sampling
   rate, no ISR and no bracket.

   Same mechanic as span.c, different key: a fixed array of counters, zero
   allocation on the hot path. The key is the CompiledMethod POINTER, which is
   already this system's method identity -- it is what the breakpoint table above
   keys on (oimg_blob_base + the offset custos sends) -- and it is stable
   because the GC is non-moving mark-sweep. A count field inside CompiledMethod
   would be cheaper still and is NOT available: solidified methods live in
   .rodata, as the bp comment above already records.

   NAMELESS, exactly like a span slot. C never learns that this pointer is
   #encode: -- nsl walks `mirror methodsOf:`/`selectorsOf:` and asks
   `mirror callsOf: m`, so the name<->method table lives in nsl.

   OFF BY DEFAULT (-DOEVM_METHOD_PROFILE to build it in), following #40's
   OEVM_NO_DEBUG_HOOKS precedent, because the ESP32 has 816 B of dram0 spare
   (#53) and this table is 2 KB. */
#ifdef OEVM_METHOD_PROFILE
#ifndef OEVM_MPROF_SLOTS
#define OEVM_MPROF_SLOTS 256        /* 256 * 8 B = 2 KB on a 32-bit target */
#endif
/* Open addressing with linear probing. Power-of-two capacity so the mask is an
   AND; a load factor above ~0.7 degrades the probe, which is why overflow is
   COUNTED rather than absorbed silently -- a full table must not read as "this
   method was never called". */
static const void *g_mprof_key[OEVM_MPROF_SLOTS];
static uint32_t    g_mprof_n[OEVM_MPROF_SLOTS];
static uint32_t    g_mprof_overflow = 0;   /* activations dropped: table full */
static uint32_t    g_mprof_used = 0;

/* Allocation attribution (design 2026-08-12). Columns on the SAME rows as the
   activation counter, so bytes-per-activation needs no join -- which is #163's
   finding asked from the other side. Stage 1 only: these live inside
   OEVM_METHOD_PROFILE and cost a production build nothing. */
static uint32_t g_mprof_bytes[MPROF_HEAP_N][OEVM_MPROF_SLOTS];
static uint32_t g_mprof_acnt[MPROF_HEAP_N][OEVM_MPROF_SLOTS];
static uint32_t g_mprof_alloc_overflow = 0;   /* allocations dropped: table full */
/* Allocations with no frame to attribute to -- boot, and prim contexts. Reported
   separately rather than folded into a sentinel row: if this bucket dominates,
   the attribution is not yet answering the question, and that must be VISIBLE
   rather than averaged into a plausible-looking table. */
static uint32_t g_mprof_nf_bytes[MPROF_HEAP_N];
static uint32_t g_mprof_nf_cnt[MPROF_HEAP_N];

/* #165 D1: the shipping control. The spike's OEVM_MPROF_RUNTIME_GATE flipped
   this on an activation countdown because Zephyr has no getenv and a count
   makes two measurement arms exactly reproducible. That seam stays below, opt
   in, for benchmarking. This flag is the one porta drives, and it defaults OFF:
   a node pays the 0.90% only inside a window it was asked for. */
static int g_mprof_enabled = 0;

/* SPIKE MEASUREMENT SEAM ONLY (-DOEVM_MPROF_RUNTIME_GATE), not a shipping feature.
   Cross-binary timing cannot resolve this hook: an OFF-vs-ON build of fib(35) put
   the ON build 4% FASTER, which is the code-layout roulette the benchmark
   discipline warns about, not a measurement. A runtime flag puts both arms in ONE
   binary -- same layout, same cache, same boot -- so the delta is the table
   operation and nothing else.

   The trigger is an ACTIVATION COUNT, not an env var and not a clock: getenv does
   not exist on Zephyr, and a count makes the two arms exactly reproducible and
   exactly equal in size on both host and device. The workload runs a fixed number
   of identical iterations; the gate flips at a count that falls on an iteration
   boundary, so the first half is the OFF arm and the second half the ON arm.

   ONE decrement and one branch on the hot path for ALL of this scaffolding --
   both arms pay it identically, so it cancels out of the delta. It does inflate
   the baseline slightly, which makes the measured percentage a mild UNDERSTATEMENT
   of the true relative cost. */
#ifdef OEVM_MPROF_RUNTIME_GATE
#ifndef OEVM_MPROF_FLIP_AT
#define OEVM_MPROF_FLIP_AT 3000000u    /* activations before the ON arm begins */
#endif
#ifndef OEVM_MPROF_DUMP_AT
#define OEVM_MPROF_DUMP_AT 0u          /* 0 = never dump from the hot path */
#endif
static int      g_mprof_phase = 0;
static uint32_t g_mprof_countdown = OEVM_MPROF_FLIP_AT;
static void mprof_report(const char *why);

/* Cold: runs twice in a whole measurement. */
static void mprof_phase_advance(void) {
    if (g_mprof_phase == 0) {
        g_mprof_enabled = 1;
        g_mprof_phase = 1;
        mprof_report("flip");
        g_mprof_countdown = (OEVM_MPROF_DUMP_AT > OEVM_MPROF_FLIP_AT)
                          ? (OEVM_MPROF_DUMP_AT - OEVM_MPROF_FLIP_AT) : 0xFFFFFFFFu;
    } else {
        /* RE-ARMS: once counting, dump every (DUMP_AT - FLIP_AT) activations.
           A one-shot dump has to be aimed, and aiming it needs the activation
           rate — which is the thing being measured. A periodic dump lands
           whatever the rate turns out to be, and the counts are cumulative so
           any single window is readable on its own. */
        mprof_report("dump");
        g_mprof_countdown = (OEVM_MPROF_DUMP_AT > OEVM_MPROF_FLIP_AT)
                          ? (OEVM_MPROF_DUMP_AT - OEVM_MPROF_FLIP_AT) : 0xFFFFFFFFu;
    }
}
#endif

/* Find or create the row for `m`. Returns the slot, or -1 when the table is full
   -- the caller COUNTS that rather than absorbing it, per the rule above. The
   interning is SHARED so an activation and an allocation for the same method can
   never land in different rows.

   Knuth multiplicative hash on the pointer, shifted past the alignment bits (a
   CompiledMethod is pointer-aligned, so the low bits are always zero and would
   collapse the table onto every eighth slot). */
static OEVM_HOT int mprof_slot(const void *m) {
    uint32_t h = (uint32_t)(((uintptr_t)m >> 4) * 2654435761u) & (OEVM_MPROF_SLOTS - 1);
    uint32_t start = h;
    for (;;) {
        if (g_mprof_key[h] == m) return (int)h;
        if (g_mprof_key[h] == NULL) { g_mprof_key[h] = m; g_mprof_used++; return (int)h; }
        h = (h + 1) & (OEVM_MPROF_SLOTS - 1);
        if (h == start) return -1;
    }
}

static OEVM_HOT void mprof_bump(const void *m) {
#ifdef OEVM_MPROF_RUNTIME_GATE
    if (--g_mprof_countdown == 0) mprof_phase_advance();
#endif
    if (!g_mprof_enabled) return;
    /* Behaviour-preserving against the pre-refactor code, which wrote n = 1 on
       creation: mprof_slot creates the row with n still 0 and this increments
       it. Holds because g_mprof_n is static (zero-init) and mprof_start() zeroes
       it -- cases 1-4 of test_mprof_gate.c pin exactly that. */
    int s = mprof_slot(m);
    if (s < 0) { g_mprof_overflow++; return; }
    g_mprof_n[s]++;
}

/* Neither static nor OEVM_HOT: gc.c calls this across a translation unit, so it
   cannot inline. Acceptable for a stage-1 spike -- and the cost of that call is
   one of the numbers this stage exists to measure (design D4). */
void mprof_alloc(unsigned heap, size_t bytes) {
    if (!g_mprof_enabled) return;
    if (heap >= MPROF_HEAP_N) return;
    struct MethodContext *ctx = current_frame;
    if (ctx == NULL || ctx->closure == NULL) {
        g_mprof_nf_bytes[heap] += (uint32_t)bytes;
        g_mprof_nf_cnt[heap]++;
        return;
    }
    /* The SAME pointer push_frame keys on: closure_entry() resolves the blob
       indirection, so a Model B image and a solidified one agree. */
    const void *m = (const void *)closure_entry((struct Closure *)ctx->closure);
    int s = mprof_slot(m);
    if (s < 0) { g_mprof_alloc_overflow++; return; }
    g_mprof_bytes[heap][s] += (uint32_t)bytes;
    g_mprof_acnt[heap][s]++;
}

/* SPIKE-ONLY readback, and it exists because the real one is not reachable yet:
   the mirror is granted to the image's SUPERVISOR (oimg_loader.c:238), not to the
   app, so an ordinary node app cannot ask `mirror callsOf:` and publish
   a `diag/calls` subtree without the supervisor handing something down. Rather than design
   that grant inside a spike, dump from C at exit under an env gate.
   A destructor rather than an atexit registration so the hot path pays NOTHING for
   the dump's existence -- no init flag, no branch in mprof_bump. */
/* Reported as a BLOB-RELATIVE OFFSET, not a pointer. A CompiledMethod carries no
   name, and a raw address is unresolvable off-node — it moves with the link and
   means nothing to a reader holding only the image. `ptr - oimg_blob_base` is the
   file offset the serializer assigned, so it joins directly against the
   <image>.oimg.map.json sidecar and names every method AND every block.

   A method outside the blob (Model A / solidified .rodata) has no such offset;
   it prints as `@<pointer>` so it is visibly a different kind of thing rather
   than a plausible wrong offset. */
static void mprof_report(const char *why) {
    int i;
    uint32_t total = 0;
    for (i = 0; i < OEVM_MPROF_SLOTS; i++) total += g_mprof_n[i];
    fprintf(stderr, "mprof[%s]: methods=%u activations=%u overflow=%u cap=%d\n",
            why, g_mprof_used, total, g_mprof_overflow, OEVM_MPROF_SLOTS);
    for (i = 0; i < OEVM_MPROF_SLOTS; i++) {
        if (!g_mprof_key[i]) continue;
        if (is_in_blob(g_mprof_key[i])) {
            fprintf(stderr, "mprof: %lu %u\n",
                    (unsigned long)((const uint8_t *)g_mprof_key[i] - oimg_blob_base),
                    g_mprof_n[i]);
        } else {
            fprintf(stderr, "mprof: @%p %u\n", g_mprof_key[i], g_mprof_n[i]);
        }
    }

    /* Allocation attribution (design 2026-08-12). A SEPARATE line prefix, never
       extra columns on `mprof:` above: custos profile's row pattern is anchored
       at exactly two fields, so widening that line makes it match NOTHING and
       report "no mprof: lines found" -- a silent false negative in the only
       tool that reads this. */
    fprintf(stderr, "mprofa[%s]: noframe_arena=%u/%u noframe_cheap=%u/%u allocOverflow=%u\n",
            why, g_mprof_nf_bytes[MPROF_HEAP_ARENA], g_mprof_nf_cnt[MPROF_HEAP_ARENA],
            g_mprof_nf_bytes[MPROF_HEAP_CHEAP], g_mprof_nf_cnt[MPROF_HEAP_CHEAP],
            g_mprof_alloc_overflow);
    for (i = 0; i < OEVM_MPROF_SLOTS; i++) {
        if (!g_mprof_key[i]) continue;
        /* Rows that never allocated are SKIPPED, and that is the measurement:
           the count of mprofa: lines IS the distinct-allocating-method
           population that sizes stage 2 (design D8). Emitting zeroes would bury
           it inside the activation dump's 200-odd rows. */
        if (g_mprof_bytes[MPROF_HEAP_ARENA][i] == 0 &&
            g_mprof_bytes[MPROF_HEAP_CHEAP][i] == 0) continue;
        if (is_in_blob(g_mprof_key[i])) {
            fprintf(stderr, "mprofa: %lu %u %u %u %u\n",
                    (unsigned long)((const uint8_t *)g_mprof_key[i] - oimg_blob_base),
                    g_mprof_bytes[MPROF_HEAP_ARENA][i], g_mprof_acnt[MPROF_HEAP_ARENA][i],
                    g_mprof_bytes[MPROF_HEAP_CHEAP][i], g_mprof_acnt[MPROF_HEAP_CHEAP][i]);
        } else {
            fprintf(stderr, "mprofa: @%p %u %u %u %u\n", g_mprof_key[i],
                    g_mprof_bytes[MPROF_HEAP_ARENA][i], g_mprof_acnt[MPROF_HEAP_ARENA][i],
                    g_mprof_bytes[MPROF_HEAP_CHEAP][i], g_mprof_acnt[MPROF_HEAP_CHEAP][i]);
        }
    }
    /* rete#312: the dump is complete. A live reader (custos profile -port) stops here
       instead of guessing from a quiet line; neither reader's row pattern matches it. */
    fprintf(stderr, "mprof[%s]: end\n", why);
}

/* #165 D1/D6 lifecycle. start = zero + enable; stop = disable + report. Zeroing
   at START rather than at stop is what makes a window mean one window: the
   counts are cumulative by construction, so a second capture that inherited the
   first one's table would report a sum spanning both and no reader could tell. */
void mprof_start(void) {
    int i; unsigned h;
    for (i = 0; i < OEVM_MPROF_SLOTS; i++) { g_mprof_key[i] = NULL; g_mprof_n[i] = 0; }
    for (h = 0; h < MPROF_HEAP_N; h++) {
        for (i = 0; i < OEVM_MPROF_SLOTS; i++) { g_mprof_bytes[h][i] = 0; g_mprof_acnt[h][i] = 0; }
        g_mprof_nf_bytes[h] = 0;
        g_mprof_nf_cnt[h] = 0;
    }
    g_mprof_used = 0;
    g_mprof_overflow = 0;
    g_mprof_alloc_overflow = 0;
    g_mprof_enabled = 1;
}

void mprof_stop(void) {
    g_mprof_enabled = 0;
    mprof_report("window");
}

int mprof_is_running(void) { return g_mprof_enabled; }

/* Test-only shims. mprof_bump is static and OEVM_HOT; exposing it directly
   would put a non-inlinable call on the activation path. */
void mprof_bump_for_test(const void *m) { mprof_bump(m); }
unsigned mprof_used_for_test(void) { return g_mprof_used; }
unsigned long mprof_bytes_for_test(unsigned heap) {
    unsigned long t = 0; int i;
    if (heap >= MPROF_HEAP_N) return 0;
    for (i = 0; i < OEVM_MPROF_SLOTS; i++) t += g_mprof_bytes[heap][i];
    return t;
}
unsigned long mprof_noframe_bytes_for_test(unsigned heap) {
    return (heap < MPROF_HEAP_N) ? (unsigned long)g_mprof_nf_bytes[heap] : 0ul;
}

/* On a hosted run the process exits, so report there too. A destructor rather
   than an atexit registration keeps the hot path free of an init flag. Absent on
   Zephyr, where nothing ever exits -- the device path reports from
   mprof_phase_advance instead. */
#ifndef __ZEPHYR__
__attribute__((destructor))
static void mprof_dump_at_exit(void) {
    if (getenv("OEVM_MPROF_DUMP")) mprof_report("exit");
}
#endif

static uint32_t mprof_count(const void *m) {
    uint32_t h = (uint32_t)(((uintptr_t)m >> 4) * 2654435761u) & (OEVM_MPROF_SLOTS - 1);
    uint32_t start = h;
    for (;;) {
        if (g_mprof_key[h] == m)    return g_mprof_n[h];
        if (g_mprof_key[h] == NULL) return 0;
        h = (h + 1) & (OEVM_MPROF_SLOTS - 1);
        if (h == start) return 0;
    }
}
#endif /* OEVM_METHOD_PROFILE */

static void bp_add(oop method, long off) {
    int i;
    for (i = 0; i < g_bp_count; i++) {
        if (g_bp[i].method == method && g_bp[i].off == off) return; /* idempotent */
    }
    if (g_bp_count >= (int)(sizeof(g_bp) / sizeof(g_bp[0]))) {
        fprintf(stderr, "VM Error: breakpoint table full (max %d)\n",
                (int)(sizeof(g_bp) / sizeof(g_bp[0])));
        abort();
    }
    g_bp[g_bp_count].method = method;
    g_bp[g_bp_count].off = off;
    g_bp_count++;
}

static void bp_remove(oop method, long off) {
    int i;
    for (i = 0; i < g_bp_count; i++) {
        if (g_bp[i].method == method && g_bp[i].off == off) {
            g_bp[i] = g_bp[g_bp_count - 1];
            g_bp_count--;
            return;
        }
    }
}

static int bp_hit(oop method, long off) {
    int i;
    for (i = 0; i < g_bp_count; i++) {
        if (g_bp[i].method == method && g_bp[i].off == off) return 1;
    }
    return 0;
}

/* Primitive C function signature */
typedef oop (*PrimitiveFunc)(oop receiver, int argc, oop *args);

/* Forward declarations */
extern oop lookup(oop receiver_vtable, oop selector);
extern oop send(oop receiver, oop selector, int argc, oop *args);

/*
 * Control flow and reflection primitives
 */
oop primitive_object_equals(oop receiver, int argc, oop *args) {
    if (argc < 1) {
        return (oop)&false_sym_obj;
    }
    return (receiver == args[0]) ? (oop)&true_sym_obj : (oop)&false_sym_obj;
}

/* Push a new bytecode-method/block activation onto the active fiber stack and
   make it current (de-recursify Phase 2, the flip). Mirrors the old
   run_interpreter prologue exactly: relative prev_frame (dynamic link), ABSOLUTE
   home_ctx (capacity is permanently fixed — Milestone A), fresh generation
   tag, regs[0]=receiver, regs[1..argc]=args, remaining regs NULL. Advances
   vm_stack_ptr. Does NOT allocate (no GC hazard). The caller reloads
   ctx/method/pc/pc_end from current_frame and continues the flat loop; OP_RET
   later pops this frame and delivers its result into the caller's dest register. */

/* Milestone A Task 7: the stack is fixed-capacity (VM_STACK_INIT_WORDS) and never
   grows or moves. A push that would exceed the cap aborts loudly — StackOverflow is
   not catchable in-image in the no-NLR runtime. */
static void stack_overflow_abort(size_t need, size_t cap) {
    fprintf(stderr, "Stack Overflow: need %zu words, cap %zu (fixed stack, "
                    "no grow in the no-NLR runtime)\n", need, cap);
    abort();
}

#ifndef OEVM_NO_DEBUG_HOOKS
static void apply_restart(struct MethodContext *ctx);   /* rete#313 */
#endif

static OEVM_HOT struct MethodContext *push_frame(oop closure, oop receiver, int argc, oop *args) {
    struct Closure *cl = (struct Closure *)closure;
    struct CompiledMethod *m = (struct CompiledMethod *)closure_entry(cl);
#ifdef OEVM_METHOD_PROFILE
    /* #164: the whole hook. `m` is already loaded for num_registers below, so the
       key costs nothing to obtain -- this is a hash, a probe and an increment. */
    mprof_bump((const void *)m);
#endif
    size_t frame_words = sizeof(struct MethodContext) / sizeof(oop) + m->num_registers;
    /* Fixed-capacity stack (Task 7): bounds-check the push against the hard cap.
       The stack never moves, so `args`/current_frame/vm_stack stay valid — no rebase. */
    if (vm_stack_ptr + frame_words > ((struct Stack *)g_active_stack)->capacity)
        stack_overflow_abort(vm_stack_ptr + frame_words,
                             ((struct Stack *)g_active_stack)->capacity);
    struct MethodContext *prev_ctx = current_frame;
    struct MethodContext *ctx = (struct MethodContext *)&vm_stack[vm_stack_ptr];
    vm_stack_ptr += frame_words;
    ctx->vtable = NULL;
    ctx->prev_frame = frame_link_encode(vm_stack, prev_ctx);   /* L0 relative dynamic link */
    ctx->saved_pc = NULL;
    ctx->closure = closure;
    /* method closure homes to itself (encoded relative); a block closure's
       inherited home_ctx is already a relative SMI — pass it through. */
    ctx->home_ctx = cl->home_ctx ? cl->home_ctx
                                 : home_link_encode_relative(vm_stack, ctx);
    ctx->gen_tag.tag32 = ++current_generation_tag;
    ctx->regs[0] = receiver;
    /* Clamp once instead of bounds-checking every arg; excess args (a caller
       bug the old per-arg check tolerated silently) are still dropped. */
    int ncopy = (argc < m->num_registers - 1) ? argc : m->num_registers - 1;
    for (int i = 0; i < ncopy; i++) ctx->regs[i + 1] = args[i];
    for (int i = ncopy + 1; i < m->num_registers; i++) ctx->regs[i] = NULL;
    current_frame = ctx;
    return ctx;
}

/* Decode a home_ctx discriminator value (frame_abi.h Option B) to the home
   MethodContext frame it refers to, on the CURRENT fiber's stack.
     NULL            -> NULL (no lexical home: a method's own closure)
     relative SMI    -> stack frame at vm_stack[offset] (same fiber)
   These two cases are TOTAL: any other value is a corrupt frame link.
   Single chokepoint for home_ctx decode. */
static inline struct MethodContext *home_ctx_frame(oop home) {
    if (home == NULL) return NULL;
    if (IS_SMI(home)) return home_link_decode_frame(vm_stack, home);
    /* A non-NULL, non-SMI home_ctx cannot arise by construction (frame_abi.h's
       discriminator is total over {NULL, relative-SMI}). Fail loudly. */
    fprintf(stderr, "VM Error: home_ctx is neither NULL nor a relative SMI (corrupt frame link)\n");
    abort();
}

/*
 * Primary Bytecode Interpreter Loop (de-recursify Phase 2: flat, non-recursive).
 *
 * ONE C activation per fiber. Every bytecode method/block invoke PUSHES a frame
 * (push_frame) and `continue`s this loop instead of recursing into a nested
 * run_interpreter; OP_RET pops the frame via the relative dynamic link and
 * delivers the result into the caller's destination register (decoded from the
 * caller's suspended send instruction, src2 == bits 24-31 — uniform across
 * OP_SEND / OP_SEND_IMPLICIT / OP_SEND_SUPER / OP_BLOCK_INVOKE). Primitives still
 * run as inline C calls.
 *
 * run_interpreter STAYS re-entrant for these cold, single-shot, non-looping
 * callers (F4): the debugger hook, does_not_understand's bytecode handler,
 * run_method_on's nested-class init, and (Milestone D) vm_protected_fire, which
 * runs one reaction firing under a setjmp crash boundary — once per reactor
 * firing, so per-firing rather than strictly one-shot, but still non-looping and
 * bounded (the block runs to completion, then returns or unwinds). Each such call
 * pushes its own entry frame and bottoms out at it (entry_ctx), returning to its
 * C caller — the loop never recurses on the hot send path.
 */
OEVM_HOT oop run_interpreter(oop closure, oop receiver, int argc, oop *args) {
    /* oevm#21: any allocation between here and where `receiver`/`args` are written
       into the (GC-scanned) frame can sweep them, since with no conservative
       C-stack scan they have no other root. That window exists ONLY on the first
       send(s), while the main fiber/Stack and the boot symbols are still being
       lazily created. Pin them as precise roots for exactly that window so the
       boot entry instance survives a GC_STRESS collection even under ASan
       fake-stack. */
    int boot_pinning = (g_active_stack == NULL)
                       || (Symbol_value == NULL);
    if (boot_pinning) {
        g_boot_inflight_receiver = receiver;
        g_boot_inflight_args = args;
        g_boot_inflight_argc = argc;
    }
    /* Lazy exec-context bootstrap on the first interpreter run (was "fiber 0"):
       a single Stack object + cursor. No Fiber wrapper — the flat loop runs
       directly on g_active_stack (a GC root; see gc.c). */
    if (g_active_stack == NULL) {
        g_active_stack = stack_alloc(VM_STACK_INIT_WORDS);
        vm_stack = ((struct Stack *)g_active_stack)->data;
        vm_stack_ptr = 0;
        current_frame = NULL;
    }
    if (Symbol_value == NULL) {
        static const char valueName[] = "value";
        Symbol_value = symbol_new(valueName, sizeof valueName - 1);   /* rete#226 */
    }
    struct Closure *cl = (struct Closure *)closure;
    struct CompiledMethod *method = (struct CompiledMethod *)closure_entry(cl);

    // Allocate stack frame sequentially (Fast Path: vtable starts as NULL)
    size_t frame_words = sizeof(struct MethodContext) / sizeof(oop) + method->num_registers;
    /* Fixed-capacity stack (Task 7): bounds-check the push against the hard cap.
       The stack never moves, so `args` stays valid — no rebase. */
    if (vm_stack_ptr + frame_words > ((struct Stack *)g_active_stack)->capacity)
        stack_overflow_abort(vm_stack_ptr + frame_words,
                             ((struct Stack *)g_active_stack)->capacity);
    // Save previous frame pointer to restore on exit
    struct MethodContext *prev_ctx = current_frame;
    /* prev_ctx (the caller frame) is restored to current_frame at exit_frame; held
       as a base-relative link so nested pushes/pops leave it valid. */
    oop entry_prev_link = frame_link_encode(vm_stack, prev_ctx);

    struct MethodContext *ctx = (struct MethodContext *)&vm_stack[vm_stack_ptr];
    size_t saved_stack_ptr = vm_stack_ptr;
    vm_stack_ptr += frame_words;
    // Initialize frame layout
    ctx->vtable = NULL; 
    ctx->prev_frame = frame_link_encode(vm_stack, prev_ctx);  /* L0 relative dynamic link */
    ctx->saved_pc = NULL; // Updated dynamically if calling nested sends
    ctx->closure = closure;
    
    // Lexical Link: nested closure -> inherited relative SMI; method closure -> self,
    // encoded relative (growth-ready).
    ctx->home_ctx = cl->home_ctx ? cl->home_ctx
                                 : home_link_encode_relative(vm_stack, ctx);
    ctx->gen_tag.tag32 = ++current_generation_tag;

    // Initialize registers (R0 = receiver, R1.. = args)
    ctx->regs[0] = receiver;
    for (int i = 0; i < argc; i++) {
        if (i + 1 < method->num_registers) {
            ctx->regs[i + 1] = args[i];
        }
    }
    // Initialize local registers to NULL
    for (int i = argc + 1; i < method->num_registers; i++) {
        ctx->regs[i] = NULL;
    }

    // Set current active frame
    current_frame = ctx;
    /* The frame THIS invocation pushed. The flat loop bottoms out (returns to the
       C caller) only when OP_RET pops back to entry_ctx — that is the
       fiber/main entry for the hot path, or the re-entry frame for the cold F4
       callers (see run_interpreter's header — incl. Milestone D's
       vm_protected_fire). */
    /* Bottom-out marker held for the whole invocation. A nested push may grow the
       stack and move frames, so track it as a base-relative OFFSET, not a pointer. */
    size_t entry_off = (size_t)((oop *)ctx - vm_stack);

    /* Window closed: receiver (regs[0]) and args (regs[1..]) are now in the
       GC-scanned frame, so scan_vm_stack roots them precisely. Drop the boot
       pins (no-op when boot_pinning was false). (oevm#21) */
    if (boot_pinning) {
        g_boot_inflight_receiver = NULL;
        g_boot_inflight_args = NULL;
        g_boot_inflight_argc = 0;
    }

    const uint32_t *pc = cm_instructions(method);
    const uint32_t *pc_end = pc + method->instruction_count;
    oop return_val = NULL;
    while (pc < pc_end) {
        /* C-assisted breakpoint match (Task 7). Gated: when no breakpoints are set
           this is a single predicted-not-taken branch per instruction. On a match
           fire debugger_hook exactly like the OP_BREAK path, but pause BEFORE pc
           (saved_pc = pc) so after the hook pc executes normally and the bp re-fires
           on the next visit -- no instruction mutation, no restore/re-arm. */
#ifndef OEVM_NO_DEBUG_HOOKS
        if (g_bp_count > 0 && debugger_hook != NULL && !in_debugger_hook) {
            long curoff = (long)(pc - cm_instructions(method));
            if (bp_hit((oop)method, curoff)) {
                ctx->saved_pc = pc;              /* pause BEFORE executing pc */
                ctx->vtable = MethodContextVtable;

                int was_stepping = g_single_step;
                g_single_step = 0; g_hook_step_request = -1;
                in_debugger_hook = 1;

                oop dbg_arg = (oop)ctx;
                struct Closure *dbg_cl = (struct Closure *)debugger_hook;
                if (obj_vtable((oop)dbg_cl) == PrimitiveClosureVtable) {
                    oop (*func)(oop, int, oop*) = (oop (*)(oop, int, oop*))closure_entry(dbg_cl);
                    func(debugger_hook, 1, &dbg_arg);
                } else {
                    run_interpreter(debugger_hook, debugger_hook, 1, &dbg_arg);
                }

                in_debugger_hook = 0;
                g_single_step = (g_hook_step_request >= 0)
                              ? g_hook_step_request : was_stepping;
                g_hook_step_request = -1;
                apply_restart(ctx);
                pc = ctx->saved_pc;              /* unchanged -> fall through, execute pc */
                /* rete#313: the hook may have restarted this frame into another method. */
                method = (struct CompiledMethod *)closure_entry((struct Closure *)ctx->closure);
                pc_end = cm_instructions(method) + method->instruction_count;
            }
        }
#endif /* !OEVM_NO_DEBUG_HOOKS */

        uint32_t inst = *pc;
        uint8_t op = inst & 0xFF;

#ifndef OEVM_NO_DEBUG_HOOKS
        if (op == OP_BREAK) {
            extern oop debugger_hook;
            if (debugger_hook != NULL) {
                ctx->saved_pc = pc + 1;
                ctx->vtable = MethodContextVtable;

                int was_stepping = g_single_step;
                g_single_step = 0; g_hook_step_request = -1;
                in_debugger_hook = 1;

                oop dbg_arg = (oop)ctx;
                struct Closure *dbg_cl = (struct Closure *)debugger_hook;
                if (obj_vtable((oop)dbg_cl) == PrimitiveClosureVtable) {
                    oop (*func)(oop, int, oop*) = (oop (*)(oop, int, oop*))closure_entry(dbg_cl);
                    func(debugger_hook, 1, &dbg_arg);
                } else {
                    run_interpreter(debugger_hook, debugger_hook, 1, &dbg_arg);
                }

                in_debugger_hook = 0;
                g_single_step = (g_hook_step_request >= 0)
                              ? g_hook_step_request : was_stepping;
                g_hook_step_request = -1;
                apply_restart(ctx);
                pc = ctx->saved_pc;
                /* rete#313: the hook may have restarted this frame into another method. */
                method = (struct CompiledMethod *)closure_entry((struct Closure *)ctx->closure);
                pc_end = cm_instructions(method) + method->instruction_count;
                continue;
            } else {
                pc++;
                continue;
            }
        }
#endif /* !OEVM_NO_DEBUG_HOOKS */

        pc++;
        uint8_t dest = (inst >> 8) & 0xFF;
        uint8_t src1 = (inst >> 16) & 0xFF;
        uint8_t src2 = (inst >> 24) & 0xFF;

        switch (op) {
            case OP_LD_CONST:
                if (src1 < method->constant_count && dest < method->num_registers) {
                    ctx->regs[dest] = oevm_int_demote(cm_const_at(method, src1)); /* canonicalize boxed int constants (https://github.com/davidg238/nsl-tuvm/issues/4) */
                }
                break;

            case OP_LD_REG:
                if (src1 < method->num_registers && dest < method->num_registers) {
                    ctx->regs[dest] = ctx->regs[src1];
                }
                break;

            case OP_LD_SELF:
                /* self-migration: enclosing self, snapshotted on the running block's
                   closure — fiber-independent (no home_ctx walk). */
                ctx->regs[dest] = ((struct Closure *)ctx->closure)->captured_self;
                break;

            case OP_LD_LEXICAL: {
                /* Identical to the old `R0 home..home regAt: idx` send chain (§3.1):
                   start at the running receiver/closure, walk home `depth` times via
                   the SAME prim, then read register `idx`. */
                oop cur = ctx->regs[0];
                for (uint8_t d = 0; d < src1; d++) cur = primitive_closure_home(cur, 0, NULL);
                oop idx = TAG_SMI(src2);
                ctx->regs[dest] = primitive_method_context_reg_at(cur, 1, &idx);
                break;
            }
            case OP_ST_LEXICAL: {
                oop cur = ctx->regs[0];
                for (uint8_t d = 0; d < src1; d++) cur = primitive_closure_home(cur, 0, NULL);
                oop putargs[2] = { TAG_SMI(src2), ctx->regs[dest] };
                primitive_method_context_reg_at_put(cur, 2, putargs);
                break;
            }
            case OP_CLOSURE: {
                uint16_t mconst = (inst >> 16) & 0xFFFF;
                oop cl = make_closure(cm_const_at(method, mconst));
                /* De-recursify deferred lift Phase 1: capture the creating
                   frame's own Context (fixed register num_arguments+1) as the
                   new closure's captured_env, so the block reaches its
                   lexically-enclosing captured locals through the heap Context
                   chain — fiber-independent and outliving the frame — rather
                   than the stack home_ctx. NULL when the creating frame has no
                   Context (capture-free frame: that register is nil-seeded). */
                ((struct Closure *)cl)->captured_env = ctx->regs[method->num_arguments + 1];
                ctx->regs[dest] = cl;
                break;
            }
            /* De-recursify deferred-lift Phase 1, Task 2: temp-vector (Context)
               ops. LIVE (decision 2026-07-04): the compiler's finalize_contexts()
               post-pass emits these for escaping captures, producing the
               fiber-independent heap-Context closure story; tests/test_ctx_ops.c
               also hand-assembles them as a unit exerciser. Convention: a frame's
               own Context always lives in the FIXED register `method->num_arguments
               + 1` (there is no free 4th operand byte in this 32-bit dest/src1/
               src2 ISA — bits 24-31 are already `src2` for every op, as OP_SEND
               establishes). The compiler reserves that register and routes
               captured-local access through it; the VM here is stateless about
               *why* that register holds what it holds. */
            case OP_MK_CONTEXT: {
                uint8_t num_slots = src1;
                oop outer = (src2 == 0xFF)
                    ? ((struct Closure *)ctx->closure)->captured_env   /* block: chain to enclosing */
                    : NULL;                                             /* method root: no outer */
                ctx->regs[dest] = context_alloc(num_slots, outer, ctx->gen_tag.tag32);
                break;
            }
            case OP_LD_CTX: {
                struct Context *cur = (struct Context *)ctx->regs[method->num_arguments + 1];
                for (uint8_t d = 0; d < src1; d++) cur = (struct Context *)cur->outer;
                ctx->regs[dest] = cur->slots[src2];
                break;
            }
            case OP_ST_CTX: {
                struct Context *cur = (struct Context *)ctx->regs[method->num_arguments + 1];
                for (uint8_t d = 0; d < src1; d++) cur = (struct Context *)cur->outer;
                cur->slots[src2] = ctx->regs[dest];
                break;
            }
            case OP_LD_SENDER: {
                ctx->regs[dest] = primitive_method_context_sender(ctx->regs[src1], 0, NULL);
                break;
            }

            /* perf/op-cmp-at fast-path opcodes. OP_SEND layout throughout; a
               fast-path miss jumps to do_send below, so the miss path IS the
               send — no second copy of any semantics. */
            case OP_LT:
            case OP_LE:
            case OP_GT:
            case OP_GE: {
                oop a = ctx->regs[src2];
                oop b = ctx->regs[src2 + 1];
                if (IS_SMI(a) && IS_SMI(b)) {
                    intptr_t v1 = UNTAG_SMI(a);
                    intptr_t v2 = UNTAG_SMI(b);
                    int t = (op == OP_LT) ? (v1 < v2)
                          : (op == OP_LE) ? (v1 <= v2)
                          : (op == OP_GT) ? (v1 > v2)
                          :                 (v1 >= v2);
                    ctx->regs[src2] = t ? (oop)&true_sym_obj : (oop)&false_sym_obj;
                    break;
                }
                goto do_send;   /* Float/LargeInteger/other: prim path, unchanged */
            }
            case OP_AT: {
                oop r = ctx->regs[src2];
                oop i = ctx->regs[src2 + 1];
                /* Shape gate == PRIM_ix_basicAt's: non-SMI instance whose slot0
                   is an SMI length. Emitter discipline (kernel-internal
                   basicAt: on known Arrays) is what makes the gate sound —
                   identical to the prim it replaces. */
                if (r != NULL && !IS_SMI(r) && IS_SMI(i) &&
                    IS_SMI(instance_slot((struct ObjectInstance *)r, 0))) {
                    struct ObjectInstance *ix = (struct ObjectInstance *)r;
                    intptr_t n = UNTAG_SMI(instance_slot(ix, 0));
                    intptr_t idx = UNTAG_SMI(i);
                    if (idx >= 1 && idx <= n) {
                        ctx->regs[src2] = instance_slot(ix, (size_t)idx);
                        break;
                    }
                    /* out of bounds: fall to the send -> prim -> its exact
                       error message and abort */
                }
                goto do_send;
            }
            case OP_AT_PUT: {
                oop r = ctx->regs[src2];
                oop i = ctx->regs[src2 + 1];
                if (r != NULL && !IS_SMI(r) && IS_SMI(i) &&
                    IS_SMI(instance_slot((struct ObjectInstance *)r, 0))) {
                    struct ObjectInstance *ix = (struct ObjectInstance *)r;
                    intptr_t n = UNTAG_SMI(instance_slot(ix, 0));
                    intptr_t idx = UNTAG_SMI(i);
                    if (idx >= 1 && idx <= n) {
                        ix->slots[idx] = ctx->regs[src2 + 2];
                        ctx->regs[src2] = ctx->regs[src2 + 2];  /* result = stored value */
                        break;
                    }
                }
                goto do_send;
            }

            case OP_SEND:
            do_send: {
                // Constants[src1] = Selector Symbol
                // Registers[src2] = Target Receiver
                // dest = Start index of argument registers
                // (Argument count is decoded from the instruction stream or method metadata)
                oop selector = cm_const_at(method, src1);
                oop target = ctx->regs[src2];

                // Let's deduce argc from instruction dest field
                int send_argc = dest;
                oop *send_args = (send_argc > 0) ? &ctx->regs[src2 + 1] : NULL;

                // Save PC return address in context before dispatching. saved_pc
                // points JUST AFTER this send (pc was already advanced), so
                // saved_pc-1 IS this instruction — OP_RET decodes src2 from it.
                ctx->saved_pc = pc;

                OEVM_TRACE_PRINT("SEND START: selector=%s, receiver_reg=%d, value=%p\n", symbol_name((struct Symbol*)selector), src2, (void*)target);
                oop m_out = NULL, r_out = NULL;
                int argc_out = 0;
                oop result = send_resolve(target, selector, send_argc, send_args,
                                          &m_out, &r_out, &argc_out);
                if (m_out != NULL) {
                    /* Bytecode/value-family target: push its frame and run it in
                       this loop. OP_RET delivers the result back into R(src2). */
                    push_frame(m_out, r_out, argc_out, send_args);
                    ctx = current_frame;
                    method = (struct CompiledMethod *)closure_entry((struct Closure *)ctx->closure);
                    pc = cm_instructions(method);
                    pc_end = pc + method->instruction_count;
                    continue;
                }
                OEVM_TRACE_PRINT("SEND END: selector=%s, receiver_reg=%d, result=%p\n", symbol_name((struct Symbol*)selector), src2, (void*)result);

                /* Fixed stack (Task 7): a primitive (incl. a nested run_interpreter,
                   which restores current_frame at exit_frame) never moves frames, so
                   ctx == current_frame here — the former grow-recovery reload is dead
                   and removed. Same at OP_BLOCK_INVOKE/OP_SEND_IMPLICIT/OP_SEND_SUPER. */
                // Save send result in R(src2)
                ctx->regs[src2] = result;
                break;
            }

            case OP_BLOCK_INVOKE: {
                // Layout identical to OP_SEND. Fast-path: a BytecodeClosure is
                // invoked as a block (still recursive in Phase 1); any other
                // receiver falls back to a normal send (e.g. Association>>value).
                oop target = ctx->regs[src2];
                int send_argc = dest;
                oop *send_args = (send_argc > 0) ? &ctx->regs[src2 + 1] : NULL;

                ctx->saved_pc = pc;
                if (target != NULL && obj_vtable(target) == BytecodeClosureVtable) {
                    /* Fast path: invoke the block — push its frame in this loop.
                       The selector const is only needed by the fallback send, so
                       it is not loaded here. */
                    push_frame(target, target, send_argc, send_args);
                    ctx = current_frame;
                    method = (struct CompiledMethod *)closure_entry((struct Closure *)ctx->closure);
                    pc = cm_instructions(method);
                    pc_end = pc + method->instruction_count;
                    continue;
                }
                /* Fallback: an ordinary send (e.g. Association>>value). */
                oop selector = cm_const_at(method, src1);
                oop m_out = NULL, r_out = NULL;
                int argc_out = 0;
                oop result = send_resolve(target, selector, send_argc, send_args,
                                          &m_out, &r_out, &argc_out);
                if (m_out != NULL) {
                    push_frame(m_out, r_out, argc_out, send_args);
                    ctx = current_frame;
                    method = (struct CompiledMethod *)closure_entry((struct Closure *)ctx->closure);
                    pc = cm_instructions(method);
                    pc_end = pc + method->instruction_count;
                    continue;
                }

                /* Fixed stack (Task 7): ctx == current_frame; reload removed. */
                ctx->regs[src2] = result;
                break;
            }

            case OP_RET:
                return_val = (dest < method->num_registers) ? ctx->regs[dest] : NULL;
                goto do_ret_deliver;

            case OP_ADD:
            case OP_SUB:
            case OP_MUL:
            case OP_DIV: {
                oop op1 = ctx->regs[src1];
                oop op2 = ctx->regs[src2];
                if (IS_SMI(op1) && IS_SMI(op2)) {
                    intptr_t v1 = UNTAG_SMI(op1);
                    intptr_t v2 = UNTAG_SMI(op2);
                    if (op == OP_DIV) {
                        if (v2 == 0) {
                            fprintf(stderr, "VM Error: Division by zero\n");
                            abort();
                        }
                        // Smalltalk `/` is EXACT division: an Integer when it
                        // divides evenly, otherwise a rational. With no Fraction
                        // type we yield a Float for the inexact case (e.g.
                        // mandelbrot's `2 * y / size`). Flooring integer
                        // division is the separate `//` primitive.
                        if (v1 % v2 == 0) {
                            ctx->regs[dest] = TAG_SMI(v1 / v2);
                        } else {
                            struct Float *f_obj = (struct Float *)float_alloc(FloatVtable);
                            f_obj->value = (double)v1 / (double)v2;
                            ctx->regs[dest] = (oop)f_obj;
                        }
                    } else {
                        // SMI +/-/*: detect overflow and promote to LargeInteger.
                        // Even without C-level overflow the result can exceed the
                        // tagged SMI range, so normalise via int_from_intptr.
                        intptr_t res = 0;
                        int of = (op == OP_ADD) ? __builtin_add_overflow(v1, v2, &res)
                               : (op == OP_SUB) ? __builtin_sub_overflow(v1, v2, &res)
                               : __builtin_mul_overflow(v1, v2, &res);
                        if (of) {
                            ctx->regs[dest] = (op == OP_ADD) ? oevm_int_add(op1, op2)
                                            : (op == OP_SUB) ? oevm_int_sub(op1, op2)
                                            : oevm_int_mul(op1, op2);
                        } else {
                            oop tagged = TAG_SMI(res);
                            ctx->regs[dest] = (UNTAG_SMI(tagged) == res)
                                            ? tagged : int_from_intptr(res);
                        }
                    }
                } else if (oevm_is_integer(op1) && oevm_is_integer(op2)) {
                    // At least one operand is a LargeInteger; both are integral.
                    if (op == OP_ADD) ctx->regs[dest] = oevm_int_add(op1, op2);
                    else if (op == OP_SUB) ctx->regs[dest] = oevm_int_sub(op1, op2);
                    else if (op == OP_MUL) ctx->regs[dest] = oevm_int_mul(op1, op2);
                    else {
                        // Exact `/` on large integers isn't exercised; yield a
                        // Float of the ratio (lossy) rather than a Fraction.
                        double d2 = oevm_int_to_double(op2);
                        if (d2 == 0.0) { fprintf(stderr, "VM Error: Division by zero\n"); abort(); }
                        struct Float *f_obj = (struct Float *)float_alloc(FloatVtable);
                        f_obj->value = oevm_int_to_double(op1) / d2;
                        ctx->regs[dest] = (oop)f_obj;
                    }
                } else {
                    double v1 = IS_SMI(op1) ? (double)UNTAG_SMI(op1)
                        : (obj_vtable(op1) == LargeIntegerVtable ? oevm_int_to_double(op1) : ((struct Float *)op1)->value);
                    double v2 = IS_SMI(op2) ? (double)UNTAG_SMI(op2)
                        : (obj_vtable(op2) == LargeIntegerVtable ? oevm_int_to_double(op2) : ((struct Float *)op2)->value);
                    double res = 0.0;
                    if (op == OP_ADD) res = v1 + v2;
                    else if (op == OP_SUB) res = v1 - v2;
                    else if (op == OP_MUL) res = v1 * v2;
                    else if (op == OP_DIV) {
                        if (v2 == 0.0) {
                            fprintf(stderr, "VM Error: Division by zero\n");
                            abort();
                        }
                        res = v1 / v2;
                    }
                    struct Float *f_obj = (struct Float *)float_alloc(FloatVtable);
                    f_obj->value = res;
                    ctx->regs[dest] = (oop)f_obj;
                }
                break;
            }

            case OP_JMP: {
                int16_t offset = (int16_t)(inst >> 8);
                pc += offset;
                break;
            }

            case OP_JMP_IF_FALSE: {
                // Phase A runtime no-op: the only emitted OP_JMP_IF_FALSE (unwindTo:value: in the
                // compiler) is fed by ==, whose primitive returns the true_sym_obj/false_sym_obj
                // boolean symbols; is_truthy matches them by name. Conformance gate holds.
                oop cond = ctx->regs[dest];
                if (!is_truthy(cond)) {
                    int16_t offset = (int16_t)(inst >> 16);
                    pc += offset;
                }
                break;
            }

            case OP_LD_SLOT: {
                oop obj = ctx->regs[src2];
                if (obj == NULL || IS_SMI(obj)) {
                    fprintf(stderr, "VM Error: Invalid receiver for LD_SLOT\n");
                    abort();
                }
                struct ObjectInstance *inst_obj = (struct ObjectInstance *)obj;
                ctx->regs[dest] = instance_slot(inst_obj, src1);
                break;
            }

            case OP_ST_SLOT: {
                oop obj = ctx->regs[src2];
                if (obj == NULL || IS_SMI(obj)) {
                    fprintf(stderr, "VM Error: Invalid receiver for ST_SLOT\n");
                    abort();
                }
                struct ObjectInstance *inst_obj = (struct ObjectInstance *)obj;
                if (is_in_blob(inst_obj)) {
                    fprintf(stderr, "VM Error: attempt to mutate immutable image object %p\n", (void*)inst_obj);
                    abort();
                }
                oop val = ctx->regs[dest];
                inst_obj->slots[src1] = val;
                break;
            }

            case OP_NEW: {
                oop vt = ctx->regs[src1];
                int slot_count = src2;
                struct ObjectInstance *new_obj = (struct ObjectInstance *)gc_allocate(vt, sizeof(struct ObjectInstance) + slot_count * sizeof(oop));
                for (int i = 0; i < slot_count; i++) {
                    new_obj->slots[i] = NULL;
                }
                ctx->regs[dest] = (oop)new_obj;
                break;
            }

            case OP_SEND_IMPLICIT: {
                oop selector = cm_const_at(method, src1);
                oop rcvr = ctx->regs[src2];
                int send_argc = dest;
                oop *send_args = (send_argc > 0) ? &ctx->regs[src2 + 1] : NULL;

                ctx->saved_pc = pc;

                oop resolved_rcvr = rcvr;
                oop target_method = NULL;

                // 1. Lexical Scope Chain lookup: walk the outer chains and check local bindings only
                oop current = rcvr;
                while (current != NULL && !IS_SMI(current)) {
                    struct Vtable *vt = (struct Vtable *)obj_vtable(current);
                    if (vt != NULL && !IS_SMI((oop)vt)) {
                        struct Binding *bs = vtable_bindings((oop)vt);
                        for (size_t i = 0; i < vt->size; i++) {
                            if (binding_symbol(&bs[i]) == selector) {
                                target_method = binding_method(&bs[i]);
                                break;
                            }
                        }
                        if (target_method != NULL) {
                            resolved_rcvr = current;
                            break;
                        }
                        if (vt->is_nested) {
                            struct ObjectInstance *inst_obj = (struct ObjectInstance *)current;
                            current = instance_slot(inst_obj, 0);
                        } else {
                            break;
                        }
                    } else {
                        break;
                    }
                }

                // 2. Inheritance Chain lookup of self: fallback if not found in lexical chain
                if (target_method == NULL) {
                    resolved_rcvr = rcvr;
                    oop receiver_vtable = IS_SMI(rcvr) ? IntegerVtable : (rcvr ? obj_vtable(rcvr) : NULL);
                    if (receiver_vtable != NULL) {
                        target_method = lookup(receiver_vtable, selector);
                    }
                }

                oop result = NULL;
                if (target_method == NULL) {
                    // Not found in the lexical/inheritance chains: fall back to the
                    // full send resolution (intercepted primitives / DNU). A bytecode
                    // target here is rare (implicit lookup is a superset for methods)
                    // but handled uniformly as a frame push rather than C recursion.
                    oop m_out = NULL, r_out = NULL;
                    int argc_out = 0;
                    result = send_resolve(rcvr, selector, send_argc, send_args,
                                          &m_out, &r_out, &argc_out);
                    if (m_out != NULL) {
                        push_frame(m_out, r_out, argc_out, send_args);
                        ctx = current_frame;
                        method = (struct CompiledMethod *)closure_entry((struct Closure *)ctx->closure);
                        pc = cm_instructions(method);
                        pc_end = pc + method->instruction_count;
                        continue;
                    }
                } else {
                    struct Closure *tcl = (struct Closure *)target_method;
                    if (obj_vtable((oop)tcl) == PrimitiveClosureVtable) {
                        oop (*func)(oop, int, oop*) = (oop (*)(oop, int, oop*))closure_entry(tcl);
                        result = func(resolved_rcvr, send_argc, send_args);
                    } else {
                        // Bytecode target: push its frame and run it in this loop.
                        push_frame(target_method, resolved_rcvr, send_argc, send_args);
                        ctx = current_frame;
                        method = (struct CompiledMethod *)closure_entry((struct Closure *)ctx->closure);
                        pc = cm_instructions(method);
                        pc_end = pc + method->instruction_count;
                        continue;
                    }
                }

                /* Fixed stack (Task 7): ctx == current_frame; reload removed. */
                ctx->regs[src2] = result;
                break;
            }

            case OP_SEND_SUPER: {
                // Register layout at src2(c): [c]=start vtable, [c+1]=self,
                // [c+2..]=args. dest(a)=argc, src1(b)=selector const.
                oop start_vt = ctx->regs[src2];
                oop self_rcvr = ctx->regs[src2 + 1];
                oop selector = cm_const_at(method, src1);
                int send_argc = dest;
                oop *send_args = (send_argc > 0) ? &ctx->regs[src2 + 2] : NULL;

                ctx->saved_pc = pc;
                oop m_out = NULL, r_out = NULL;
                int argc_out = 0;
                oop result = send_super_resolve(start_vt, self_rcvr, selector, send_argc, send_args,
                                                &m_out, &r_out, &argc_out);
                if (m_out != NULL) {
                    // Bytecode super target: push its frame. Result delivered to
                    // R(src2) by OP_RET (src2 == c, the same field OP_SEND uses).
                    push_frame(m_out, r_out, argc_out, send_args);
                    ctx = current_frame;
                    method = (struct CompiledMethod *)closure_entry((struct Closure *)ctx->closure);
                    pc = cm_instructions(method);
                    pc_end = pc + method->instruction_count;
                    continue;
                }

                /* Fixed stack (Task 7): ctx == current_frame; reload removed. */
                ctx->regs[src2] = result;
                break;
            }

            default:
                fprintf(stderr, "VM Error: Unknown opcode %d\n", op);
                abort();
        }

        /* Normal instruction completion falls through to the step hook. The
           return-delivery label below is reached only via goto. */
        goto step_hook;

    do_ret_deliver:
        /* Return `return_val` from the current frame `ctx`. Pop it; if it is this
           invocation's entry frame, bottom out to the C caller; otherwise resume
           the caller at its saved_pc and deliver the value into the caller's dest
           register (src2 of the caller's suspended send == bits 24-31). */
        {
            struct MethodContext *caller = frame_link_decode(vm_stack, ctx->prev_frame);
            vm_stack_ptr = (size_t)((oop *)ctx - vm_stack);
            if ((size_t)((oop *)ctx - vm_stack) == entry_off || caller == NULL) {
                goto exit_frame;
            }
            current_frame = caller;
            ctx = caller;
            method = (struct CompiledMethod *)closure_entry((struct Closure *)ctx->closure);
            pc = ctx->saved_pc;
            pc_end = cm_instructions(method) + method->instruction_count;
            uint8_t rdst = ((uint32_t)*(ctx->saved_pc - 1) >> 24) & 0xFF;
            ctx->regs[rdst] = return_val;
        }
        goto step_hook;

    step_hook: ;
        /* Mirror the breakpoint-match hook's re-entrancy guard (!in_debugger_hook):
         * when dbg:step arms stepping from inside the debugger's command loop,
         * without this guard this hook fires on the debugger's OWN unwinding
         * bytecode frames (onBreakContext: epilogue, etc.) — single-stepping the
         * debugger instead of the app. The guard keeps stepping confined to the app. */
#ifndef OEVM_NO_DEBUG_HOOKS
        /* rete#311 dbg:pause: a line waiting on the debug transport stops the program
         * here exactly as a step does (platform_backend.h, platform_debug_pending);
         * the hook's command loop then reads it. One extra load per bytecode. */
        if ((g_single_step | platform_debug_pending) && !in_debugger_hook) {
            extern oop debugger_hook;
            if (debugger_hook != NULL) {
                ctx->saved_pc = pc;
                ctx->vtable = MethodContextVtable;

                int was_stepping = g_single_step;
                g_single_step = 0;
                g_hook_step_request = -1;
                in_debugger_hook = 1;

                oop dbg_arg = (oop)ctx;
                struct Closure *dbg_cl = (struct Closure *)debugger_hook;
                if (obj_vtable((oop)dbg_cl) == PrimitiveClosureVtable) {
                    oop (*func)(oop, int, oop*) = (oop (*)(oop, int, oop*))closure_entry(dbg_cl);
                    func(debugger_hook, 1, &dbg_arg);
                } else {
                    run_interpreter(debugger_hook, debugger_hook, 1, &dbg_arg);
                }

                in_debugger_hook = 0;
                g_single_step = (g_hook_step_request >= 0)
                              ? g_hook_step_request : was_stepping;
                g_hook_step_request = -1;
                apply_restart(ctx);
                pc = ctx->saved_pc;
                /* rete#313: the hook may have restarted this frame into another method. */
                method = (struct CompiledMethod *)closure_entry((struct Closure *)ctx->closure);
                pc_end = cm_instructions(method) + method->instruction_count;
            }
        }
#endif /* !OEVM_NO_DEBUG_HOOKS */
    }

exit_frame:
    /* Restore caller's frame pointer and stack position. The fixed stack never moves
       (Task 7) and there is no scheduler to switch between execution contexts
       anymore (Tasks 5–6), so a nested run_interpreter always returns with
       current_frame back at this value — the post-send reload blocks were dead
       and have been removed. */
    vm_stack_ptr = saved_stack_ptr;
    current_frame = frame_link_decode(vm_stack, entry_prev_link);
    return return_val;
}

/*
 * Reflection / Introspection Primitives
 */

oop primitive_object_vtable(oop receiver, int argc, oop *args) {
    (void)argc; (void)args;
    if (IS_SMI(receiver)) return IntegerVtable;
    if (receiver == NULL) return NULL;
    return obj_vtable(receiver);
}

oop primitive_object_outer(oop receiver, int argc, oop *args) {
    (void)argc; (void)args;
    if (receiver == NULL || IS_SMI(receiver)) return NULL;
    struct Vtable *vt = (struct Vtable *)obj_vtable(receiver);
    if (vt != NULL && !IS_SMI((oop)vt) && vt->is_nested) {
        struct ObjectInstance *inst = (struct ObjectInstance *)receiver;
        return instance_slot(inst, 0);
    }
    return NULL;
}

oop primitive_vtable_delegate(oop receiver, int argc, oop *args) {
    (void)argc; (void)args;
    if (receiver == NULL || IS_SMI(receiver)) {
        fprintf(stderr, "VM Error: delegate requested on non-object\n");
        abort();
    }
    struct Vtable *vt = (struct Vtable *)receiver;
    return vtable_delegate((oop)vt);
}

oop primitive_vtable_bindings(oop receiver, int argc, oop *args) {
    (void)argc; (void)args;
    if (receiver == NULL || IS_SMI(receiver)) {
        fprintf(stderr, "VM Error: bindings requested on non-object\n");
        abort();
    }
    struct Vtable *vt = (struct Vtable *)receiver;
    struct ObjectInstance *arr = (struct ObjectInstance *)gc_allocate(NULL, sizeof(struct ObjectInstance) + vt->size * sizeof(oop));

    /* arr lives only in a C local until it is returned. With the conservative
       C-stack scan gone, the per-iteration gc_allocate(BoxedBinding) below can
       trigger a GC_STRESS collection that would reclaim arr — pin it as a temp
       root. gc_allocate does not zero memory, so seed every slot to NULL FIRST,
       before any allocation, so a GC that now traces the rooted arr never reads
       garbage out of a half-built array. */
    extern void gc_temp_root_push(oop o);
    extern void gc_temp_roots_clear(void);
    for (size_t i = 0; i < vt->size; i++) arr->slots[i] = NULL;
    gc_temp_root_push((oop)arr);

    struct Binding *bs = vtable_bindings((oop)vt);
    for (size_t i = 0; i < vt->size; i++) {
        struct BoxedBinding *bb = (struct BoxedBinding *)gc_allocate(BoxedBindingVtable, sizeof(struct BoxedBinding));
        bb->symbol = binding_symbol(&bs[i]);
        bb->method = binding_method(&bs[i]);
        arr->slots[i] = (oop)bb;
    }
    gc_temp_roots_clear();
    return (oop)arr;
}

oop primitive_method_context_sender(oop receiver, int argc, oop *args) {
    (void)argc; (void)args;
    if (receiver == NULL || IS_SMI(receiver)) return NULL;
    struct MethodContext *ctx = (struct MethodContext *)receiver;
    oop prev = (oop)frame_link_decode(vm_stack, ctx->prev_frame);
    if (prev != NULL && !IS_SMI(prev)) {
        struct MethodContext *prev_ctx = (struct MethodContext *)prev;
        if (obj_vtable((oop)prev_ctx) == NULL) {
            prev_ctx->vtable = MethodContextVtable;
        }
    }
    return prev;
}

oop primitive_method_context_pc(oop receiver, int argc, oop *args) {
    (void)argc; (void)args;
    if (receiver == NULL || IS_SMI(receiver)) return NULL;
    struct MethodContext *ctx = (struct MethodContext *)receiver;
    return TAG_SMI((uintptr_t)ctx->saved_pc);
}

oop primitive_closure_home(oop receiver, int argc, oop *args) {
    (void)argc; (void)args;
    if (receiver == NULL || IS_SMI(receiver)) return NULL;
    /* `home` may be sent to a MethodContext (e.g. from get_self_reg when walking
     * the lexical chain for an inner block).  A MethodContext's lexical link is
     * ctx->home_ctx (offset 3), NOT its dynamic link prev_frame (offset 1).
     * Distinguish by vtable: Closure vtables are non-NULL heap objects;
     * an unreified MethodContext has vtable==NULL, a reified one has MethodContextVtable.
     * If the vtable is neither BytecodeClosureVtable nor PrimitiveClosureVtable,
     * treat the receiver as a MethodContext and return its home_ctx field. */
    oop home;
    oop vt = obj_vtable(receiver);
    if (vt != BytecodeClosureVtable && vt != PrimitiveClosureVtable) {
        home = ((struct MethodContext *)receiver)->home_ctx;   /* a frame's lexical link */
    } else {
        home = ((struct Closure *)receiver)->home_ctx;          /* a closure's lexical link */
    }
    return (oop)home_ctx_frame(home);   /* decode relative SMI → frame ptr (growth-ready) */
}

/* The fiber scheduler primitives are gone (Milestone A Task 5); Task 6 removed
   the Fiber wrapper struct entirely — the running execution context is now
   exactly g_active_stack (a Stack heap object) + vm_stack/vm_stack_ptr +
   current_frame. */

static oop get_top_level_method_closure(oop closure) {
    /* The lexical walk this function used to perform is unreachable through
       dispatch. Its only caller (find_defining_vtable, from the ACCESS_PRIVATE
       branch of check_access_allowed) is reached only when receiver ==
       caller_self == current_frame->regs[0]; a private method's receiver is a
       user instance, and a block frame's regs[0] is the closure (push_frame at
       OP_BLOCK_INVOKE) — so the closure here is always a METHOD closure
       (home_ctx == NULL) and the old lexical loop never iterated. Removing it
       drops the last home_ctx frame-identity walk from this path at zero
       runtime cost. The invariant is pinned: a non-NULL home_ctx here would
       mean a block reached access control, which the model does not produce —
       fail loudly (house posture) rather than silently walk a dead path. Note
       the whole send-visibility access subsystem is vestigial (oevm#36). See
       design 2026-07-03-derecursify-e1-retire-dead-access-walk-design.md §2. */
    if (((struct Closure *)closure)->home_ctx != NULL) {
        fprintf(stderr, "VM Error: access-control method lookup on a block closure "
            "(home_ctx != NULL) — unexpected for a top-level method closure.\n");
        abort();
    }
    return closure;
}

static oop find_defining_vtable(oop receiver_vtable, oop closure) {
    oop method_closure = get_top_level_method_closure(closure);
    oop curr = receiver_vtable;
    while (curr != NULL && !IS_SMI(curr)) {
        struct Vtable *vt = (struct Vtable *)curr;
        struct Binding *bs = vtable_bindings((oop)vt);
        for (size_t i = 0; i < vt->size; i++) {
            if (binding_method(&bs[i]) == method_closure) {
                return curr;
            }
        }
        curr = vtable_delegate((oop)vt);
    }
    return NULL;
}

OEVM_HOT int check_access_allowed(oop receiver, oop selector, struct CompiledMethod *target_method, oop target_defining_vtable) {
    (void)selector;
    if (target_method->access_modifier == ACCESS_PUBLIC) {
        return 1;
    }
    if (current_frame == NULL) {
        return 1; // Trust trusted C entry point calls
    }
    
    oop caller_self = current_frame->regs[0];
    
    if (target_method->access_modifier == ACCESS_PROTECTED) {
        if (receiver == caller_self) {
            return 1;
        }
        return 0;
    }
    
    if (target_method->access_modifier == ACCESS_PRIVATE) {
        if (receiver != caller_self) {
            return 0;
        }
        oop caller_defining_vtable = find_defining_vtable(obj_vtable(caller_self), current_frame->closure);
        if (caller_defining_vtable == target_defining_vtable) {
            return 1;
        }
        return 0;
    }
    
    return 0;
}

extern oop debugger_hook;
extern oop Symbol_debug_;
extern oop SymbolVtable;

oop primitive_register_debugger_hook(oop receiver, int argc, oop *args) {
    (void)receiver;
    if (argc < 1) {
        fprintf(stderr, "VM Error: registerDebuggerHook: expects 1 argument\n");
        abort();
    }
    debugger_hook = args[0];
    if (Symbol_debug_ == NULL) {
        static const char debugName[] = "debug:";
        oop sym = symbol_new(debugName, sizeof debugName - 1);   /* rete#226 */
        Symbol_debug_ = sym;
    }
    return receiver;
}

oop primitive_method_context_reg_at(oop receiver, int argc, oop *args) {
    if (receiver == NULL || IS_SMI(receiver)) {
        fprintf(stderr, "VM Error: regAt: requested on non-context\n");
        abort();
    }
    if (argc < 1 || !IS_SMI(args[0])) {
        fprintf(stderr, "VM Error: regAt: expects index argument\n");
        abort();
    }
    int idx = UNTAG_SMI(args[0]);
    struct MethodContext *ctx = (struct MethodContext *)receiver;
    struct Closure *cl = (struct Closure *)ctx->closure;
    struct CompiledMethod *cm = (struct CompiledMethod *)closure_entry(cl);
    if (idx < 0 || idx >= cm->num_registers) {
        fprintf(stderr, "VM Error: regAt: index %d out of bounds (max %d)\n", idx, cm->num_registers);
        abort();
    }
    return ctx->regs[idx];
}

/* Confined read prim (Task 8) -- reachable ONLY via mirror_dispatch. Returns the
   number of registers of the method running in the given context, so a debugger
   can bound a register dump (reg:at: aborts on idx >= num_registers). Same
   receiver/closure/CompiledMethod walk as reg_at, with no index argument. */
oop primitive_method_context_num_registers(oop receiver, int argc, oop *args) {
    (void)argc; (void)args;
    if (receiver == NULL || IS_SMI(receiver)) {
        fprintf(stderr, "VM Error: numRegisters requested on non-context\n");
        abort();
    }
    struct MethodContext *ctx = (struct MethodContext *)receiver;
    struct Closure *cl = (struct Closure *)ctx->closure;
    struct CompiledMethod *cm = (struct CompiledMethod *)closure_entry(cl);
    return TAG_SMI(cm->num_registers);
}

oop primitive_method_context_reg_at_put(oop receiver, int argc, oop *args) {
    if (receiver == NULL || IS_SMI(receiver)) {
        fprintf(stderr, "VM Error: regAt:put: requested on non-context\n");
        abort();
    }
    if (argc < 2 || !IS_SMI(args[0])) {
        fprintf(stderr, "VM Error: regAt:put: expects index and value arguments\n");
        abort();
    }
    int idx = UNTAG_SMI(args[0]);
    oop val = args[1];
    struct MethodContext *ctx = (struct MethodContext *)receiver;
    struct Closure *cl = (struct Closure *)ctx->closure;
    struct CompiledMethod *cm = (struct CompiledMethod *)closure_entry(cl);
    if (idx < 0 || idx >= cm->num_registers) {
        fprintf(stderr, "VM Error: regAt:put: index %d out of bounds (max %d)\n", idx, cm->num_registers);
        abort();
    }
    ctx->regs[idx] = val;
    return val;
}

/* Confined requestStep: prim -- reachable ONLY via mirror_dispatch (never through
   dispatch.c switch). Operates on the single global execution context (the Fiber
   wrapper is gone, Task 6) so the hook never needs a fiber oop. Sets both
   g_single_step and g_hook_step_request so the arm survives the post-hook restore
   (Task 1's merge formula). */
oop primitive_request_step(oop receiver, int argc, oop *args) {
    (void)receiver; (void)argc;
    int v = (args[0] != NULL && (!IS_SMI(args[0]) || UNTAG_SMI(args[0]) != 0)) ? 1 : 0;
    g_single_step = v;
    g_hook_step_request = v;
    return NULL;
}

/* singleStep: (mirror alias `fiber:singleStep:`). Task 6: with the Fiber wrapper
   gone the reflectee is vestigial — single-step is a global on the one execution
   context. `receiver` is accepted for the mirror's arg shape but not dereferenced. */
oop primitive_debugger_set_single_step(oop receiver, int argc, oop *args) {
    (void)receiver;
    if (argc < 1) {
        fprintf(stderr, "VM Error: singleStep: expects 1 argument\n");
        abort();
    }
    int step_val = 0;
    if (IS_SMI(args[0])) {
        step_val = UNTAG_SMI(args[0]) != 0;
    } else if (args[0] != NULL) {
        step_val = 1;
    }
    if (in_debugger_hook) {
        g_hook_step_request = step_val;   /* applied after the hook returns */
    }
    g_single_step = step_val;
    return receiver;
}

#ifndef OEVM_NO_DEBUG_HOOKS   /* hot patching is a debug facility (rete#313) */
/* rete#313: one method replaced while the program runs. The patch is the method
   recompiled from edited source and re-encoded against THIS image's offsets by
   nodus/nsl/tools/patch_method.py; its words are the .oimg's own RefWords, so the
   loader resolves them exactly as it resolves the blob's (oimg_resolve). It arrives
   as text, one debug line on every target (dbg:patch <text>), whitespace-separated:
       oevm-patch 1 vtable <word> selector <word>
       registers <n> arguments <n> access <n>
       instructions <k> <u32 hex>... constants <c> <word hex>...
   A line is capped by the debug transport's buffer (platform.c PRIM_pf_readLine), so
   a longer patch arrives cut and is refused here as malformed. The CompiledMethod is
   never freed (a replaced method may still be running); its closure is a GC object
   rooted by the patch tables. Answers 1, or 0 with the reason on stderr. */
static const char *patch_cur;
static const char *patch_end;

static void patch_skip(void) {
    while (patch_cur < patch_end && (*patch_cur == ' ' || *patch_cur == '\n' || *patch_cur == '\t')) patch_cur++;
}

/* The next whitespace-separated token, as a number in base; 0 when there is none
   or it is not all digits. */
static int patch_num(int base, unsigned long *v) {
    char tok[24];
    size_t n = 0;
    patch_skip();
    while (patch_cur < patch_end && *patch_cur != ' ' && *patch_cur != '\n' && *patch_cur != '\t') {
        if (n == sizeof tok - 1) return 0;
        tok[n++] = *patch_cur++;
    }
    if (n == 0) return 0;
    tok[n] = 0;
    char *e;
    *v = strtoul(tok, &e, base);
    return *e == 0;
}

/* The next token is exactly word, and the one after it a number in base. */
static int patch_field(const char *word, int base, unsigned long *v) {
    size_t n = strlen(word);
    patch_skip();
    if ((size_t)(patch_end - patch_cur) <= n || memcmp(patch_cur, word, n) != 0) return 0;
    patch_cur += n;
    if (*patch_cur != ' ' && *patch_cur != '\n' && *patch_cur != '\t') return 0;
    return patch_num(base, v);
}

oop primitive_patch(oop text, int argc, oop *args) {
    (void)argc; (void)args;
    if (text == NULL || IS_SMI(text) || obj_vtable(text) != StringVtable) {
        fprintf(stderr, "patch: wants the patch text, a String\n");
        return TAG_SMI(0);
    }
    struct String *ts = (struct String *)text;
    patch_cur = (const char *)ts->bytes;
    patch_end = patch_cur + ts->length;
    unsigned long ver, vt_w, sel_w, nregs, nargs, access, ninst, ncon, w;
    uint32_t *insts = NULL;
    oop *consts = NULL;
    struct CompiledMethod *m = NULL;
    const char *why = "malformed";
    if (!patch_field("oevm-patch", 10, &ver) || ver != 1
        || !patch_field("vtable", 16, &vt_w) || !patch_field("selector", 16, &sel_w)
        || !patch_field("registers", 10, &nregs) || !patch_field("arguments", 10, &nargs)
        || !patch_field("access", 10, &access) || !patch_field("instructions", 10, &ninst))
        goto bad;
    why = "out of memory";
    insts = oevm_malloc((ninst ? ninst : 1) * sizeof *insts);
    if (insts == NULL) goto bad;
    why = "malformed";
    for (unsigned long i = 0; i < ninst; i++) {
        if (!patch_num(16, &w)) goto bad;
        insts[i] = (uint32_t)w;
    }
    if (!patch_field("constants", 10, &ncon)) goto bad;
    why = "out of memory";
    consts = oevm_malloc((ncon ? ncon : 1) * sizeof *consts);
    if (consts == NULL) goto bad;
    why = "malformed";
    for (unsigned long i = 0; i < ncon; i++) {
        if (!patch_num(16, &w)) goto bad;
        consts[i] = oimg_resolve((uintptr_t)w);
    }
    patch_skip();
    if (patch_cur != patch_end) goto bad;              /* trailing words: not this format */
    oop vt = oimg_resolve((uintptr_t)vt_w), sel = oimg_resolve((uintptr_t)sel_w);
    why = "vtable or selector does not resolve";
    if (vt == NULL || sel == NULL || obj_vtable(sel) != SymbolVtable) goto bad;
    why = "out of memory";
    m = oevm_malloc(sizeof *m);
    if (m == NULL) goto bad;
    *m = (struct CompiledMethod){insts, ninst, (int)nregs, (int)nargs, consts, ncon, (int)access};
    oop cl = gc_allocate(BytecodeClosureVtable, sizeof(struct Closure));
    struct Closure *c = (struct Closure *)cl;
    c->home_ctx = NULL;          /* a method closure homes to its own frame (push_frame) */
    c->home_generation = 0;
    c->entry_point = m;
    c->captured_env = NULL;
    c->captured_self = NULL;
    oop prev = lookup(vt, sel);
    if (method_patch_count == METHOD_PATCHES_MAX || method_override_install(vt, sel, cl) != 0) {
        fprintf(stderr, "patch: the patch tables are full\n");
        return TAG_SMI(0);
    }
    if (prev != NULL) method_patches[method_patch_count++] = (struct MethodPatch){prev, cl};
    return TAG_SMI(1);
bad:
    fprintf(stderr, "patch: %s\n", why);
    oevm_free(insts);
    oevm_free(consts);
    oevm_free(m);
    return TAG_SMI(0);
}

/* rete#313: restart a stopped frame from its first instruction, into its method's
   replacement when it has one (the method_patches chain). Only the frame the
   debugger stopped in, and only a method's (a block's frame belongs to its home).
   While the hook runs, the debugger's own frames sit above the stopped one, so the
   primitive only records the request; the interpreter applies it when the hook
   returns (apply_restart), when the stopped frame is the top of the stack again and
   a larger replacement can grow it in place. Receiver and arguments are kept;
   every other register is cleared, the heap Context slot included (the prologue
   makes a fresh one). Answers 1, or 0 with the reason on stderr. */
static struct { struct MethodContext *ctx; oop closure; } g_restart;

oop primitive_restart_frame(oop frame, int argc, oop *args) {
    (void)argc; (void)args;
    struct MethodContext *ctx = (struct MethodContext *)frame;
    if (ctx == NULL || IS_SMI(frame) || ctx->vtable != MethodContextVtable) {
        fprintf(stderr, "restart: not a stopped frame\n");
        return TAG_SMI(0);
    }
    if (((struct Closure *)ctx->closure)->home_ctx != NULL) {
        fprintf(stderr, "restart: a block's frame does not restart, only a method's\n");
        return TAG_SMI(0);
    }
    oop cl = ctx->closure;
    for (int moved = 1; moved; ) {
        moved = 0;
        for (int i = 0; i < method_patch_count; i++)
            if (method_patches[i].from == cl) { cl = method_patches[i].to; moved = 1; break; }
    }
    struct CompiledMethod *old = (struct CompiledMethod *)closure_entry((struct Closure *)ctx->closure);
    struct CompiledMethod *m = (struct CompiledMethod *)closure_entry((struct Closure *)cl);
    if (m->num_arguments != old->num_arguments) {
        fprintf(stderr, "restart: the replacement takes %d arguments; the frame has %d\n",
                m->num_arguments, old->num_arguments);
        return TAG_SMI(0);
    }
    g_restart.ctx = ctx;
    g_restart.closure = cl;
    return TAG_SMI(1);
}

/* The hook's epilogue: apply a restart the hook requested for ctx. */
static void apply_restart(struct MethodContext *ctx) {
    if (g_restart.ctx == NULL) return;
    struct MethodContext *want = g_restart.ctx;
    oop cl = g_restart.closure;
    g_restart.ctx = NULL;
    g_restart.closure = NULL;
    if (want != ctx) return;
    struct CompiledMethod *old = (struct CompiledMethod *)closure_entry((struct Closure *)ctx->closure);
    struct CompiledMethod *m = (struct CompiledMethod *)closure_entry((struct Closure *)cl);
    size_t base = (size_t)((oop *)ctx - vm_stack) + sizeof(struct MethodContext) / sizeof(oop);
    if (vm_stack_ptr != base + (size_t)old->num_registers) {
        fprintf(stderr, "restart: the stopped frame is not the top of the stack; not restarted\n");
        return;
    }
    if (base + (size_t)m->num_registers > ((struct Stack *)g_active_stack)->capacity)
        stack_overflow_abort(base + m->num_registers, ((struct Stack *)g_active_stack)->capacity);
    vm_stack_ptr = base + (size_t)m->num_registers;
    ctx->closure = cl;
    for (int i = m->num_arguments + 1; i < m->num_registers; i++) ctx->regs[i] = NULL;
    ctx->saved_pc = cm_instructions(m);
}
#endif /* !OEVM_NO_DEBUG_HOOKS */

oop primitive_vtable_bind(oop receiver, int argc, oop *args) {
    if (receiver == NULL || IS_SMI(receiver)) {
        fprintf(stderr, "VM Error: bind:to: requested on non-vtable\n");
        abort();
    }
    if (argc < 2) {
        fprintf(stderr, "VM Error: bind:to: expects selector and method arguments\n");
        abort();
    }
    oop selector = args[0];
    oop method = args[1];
    struct Vtable *vt = (struct Vtable *)receiver;

    if (is_in_blob(vt)) {
#ifndef OEVM_NO_DEBUG_HOOKS
        /* rete#313: an image vtable is read-only; the binding goes over it. */
        if (method_override_install(receiver, selector, method) != 0) {
            fprintf(stderr, "VM Error: bind:to: method override table full (%d)\n", METHOD_OVERRIDES_MAX);
            abort();
        }
        return receiver;
#else
        fprintf(stderr, "VM Error: attempt to mutate immutable image object %p\n", (void*)vt);
        abort();
#endif
    }

    /* Any binding mutation can stale a cached (vtable,selector)->method entry. */
    method_cache_flush();

    struct Binding *bs = vtable_bindings((oop)vt);
    for (size_t i = 0; i < vt->size; i++) {
        if (binding_symbol(&bs[i]) == selector) {
            vt->bindings[i].method = method;
            return receiver;
        }
    }
    
    if (vt->size >= vt->capacity) {
        size_t new_capacity = vt->capacity == 0 ? 8 : vt->capacity * 2;
        /* Assumes vt->bindings is either NULL or oevm_malloc/oevm_realloc storage.
           rete#191: hanoi_native.c:77 breaks that assumption for Vtable_HanoiNative
           (static, not heap) — pre-existing hazard, left standing deliberately. */
        struct Binding *grown = oevm_realloc(vt->bindings, new_capacity * sizeof(struct Binding));
        /* rete#190: this primitive is reached only through the privileged mirror
           capability (mirror.c vtable:bind:to:) and every other error path in this
           function already aborts (non-vtable receiver, bad argc, immutable image).
           There is no established "answer nil" convention here — bind:to: always
           answers the receiver — so returning nil on OOM would read as a plausible
           success and silently drop the binding. Abort loud, consistent with the
           rest of this function, rather than invent a new failure channel. Failing
           BEFORE reassigning vt->bindings/vt->capacity also keeps the original
           (still-valid, per oevm_realloc's contract) storage and size intact. */
        if (grown == NULL) {
            fprintf(stderr, "VM Error: bind:to: could not grow vtable bindings (out of memory)\n");
            abort();
        }
        vt->bindings = grown;
        vt->capacity = new_capacity;
    }
    vt->bindings[vt->size].symbol = selector;
    vt->bindings[vt->size].method = method;
    vt->size++;
    return receiver;
}

oop primitive_compiled_method_set_instruction_at(oop receiver, int argc, oop *args) {
    if (receiver == NULL || IS_SMI(receiver)) {
        fprintf(stderr, "VM Error: instructionAt:put: requested on non-object\n");
        abort();
    }
    if (argc < 2 || !IS_SMI(args[0]) || !IS_SMI(args[1])) {
        fprintf(stderr, "VM Error: instructionAt:put: expects index and instruction arguments\n");
        abort();
    }
    int idx = UNTAG_SMI(args[0]);
    uint32_t bytecode = (uint32_t)UNTAG_SMI(args[1]);
    
    struct CompiledMethod *cm = NULL;
    struct Closure *cl = (struct Closure *)receiver;
    if (obj_vtable((oop)cl) == BytecodeClosureVtable) {
        cm = (struct CompiledMethod *)closure_entry(cl);
    } else {
        cm = (struct CompiledMethod *)receiver;
    }

    if (idx < 0 || (size_t)idx >= cm->instruction_count) {
        fprintf(stderr, "VM Error: instructionAt:put: index %d out of bounds (max %zu)\n", idx, cm->instruction_count);
        abort();
    }

    /* Guard before the flash_rom copy too: it reassigns cm->instructions, a field of cm itself. */
    if (is_in_blob(cm)) {
        fprintf(stderr, "VM Error: attempt to mutate immutable image object %p\n", (void*)cm);
        abort();
    }

    extern uintptr_t flash_rom_start;
    extern uintptr_t flash_rom_end;
    uintptr_t insts_addr = (uintptr_t)cm_instructions(cm);
    if (insts_addr >= flash_rom_start && insts_addr < flash_rom_end) {
        uint32_t *heap_insts = oevm_malloc(cm->instruction_count * sizeof(uint32_t));
        /* rete#190: same reasoning as bind:to: above — this primitive is reached
           only through the privileged mirror capability (mirror.c
           method:instructionAt:put:), every other error path in this function
           already aborts (non-object receiver, bad args, OOB index, immutable
           image), and instructionAt:put: has no "answer nil" convention to fall
           back on: its job is to mutate a specific instruction word, and there is
           no bounded, honest value to hand back short of doing that. Abort loud
           rather than write through NULL or silently skip the requested mutation. */
        if (heap_insts == NULL) {
            fprintf(stderr, "VM Error: instructionAt:put: could not copy flash-ROM instructions (out of memory)\n");
            abort();
        }
        memcpy(heap_insts, cm_instructions(cm), cm->instruction_count * sizeof(uint32_t));
        cm->instructions = heap_insts;
    }

    uint32_t *writable_insts = (uint32_t *)cm_instructions(cm);
    writable_insts[idx] = bytecode;
    return args[1];
}

/* --- Confined read-only mirror primitives (Task 2) ---
   Reachable ONLY via mirror_dispatch; never in dispatch.c switch(pid). */

extern oop indexable_new(intptr_t n);

oop primitive_method_offset(oop receiver, int argc, oop *args) {
    (void)argc; (void)args;
    if (receiver == NULL || IS_SMI(receiver)) return TAG_SMI(0);
    struct MethodContext *ctx = (struct MethodContext *)receiver;
    if (ctx->saved_pc == NULL) return TAG_SMI(0);
    struct CompiledMethod *cm = (struct CompiledMethod *)closure_entry((struct Closure *)ctx->closure);
    return TAG_SMI((long)((const uint32_t *)ctx->saved_pc - cm_instructions(cm)));
}

oop primitive_compiled_method_instruction_at(oop receiver, int argc, oop *args) {
    (void)argc;
    if (receiver == NULL || IS_SMI(receiver)) {
        fprintf(stderr, "VM Error: method:instructionAt: requires a non-null receiver\n");
        abort();
    }
    if (argc < 1 || !IS_SMI(args[0])) {
        fprintf(stderr, "VM Error: method:instructionAt: requires SMI index argument\n");
        abort();
    }
    struct Closure *cl = (struct Closure *)receiver;
    struct CompiledMethod *cm = (obj_vtable((oop)cl) == BytecodeClosureVtable)
        ? (struct CompiledMethod *)closure_entry(cl)
        : (struct CompiledMethod *)receiver;
    int idx = (int)UNTAG_SMI(args[0]);
    if (idx < 0 || (size_t)idx >= cm->instruction_count) {
        fprintf(stderr, "VM Error: method:instructionAt: index %d out of bounds (max %zu)\n", idx, cm->instruction_count);
        abort();
    }
    return TAG_SMI((long)(uint32_t)cm_instructions(cm)[idx]);
}

/* rete#323: the debugger names a method by its BLOB OFFSET -- the image map's key
   and the profiler's (`mprof:`). The bp table still keys on the CompiledMethod
   pointer, oimg_blob_base + off. A CompiledMethod carries no vtable, so an in-blob
   offset cannot be checked to BE a method; one that is not arms an entry that never
   fires, and custos only sends offsets read from the map. No blob (Path A), out of
   range, or misaligned: nil, and nothing is armed. */
static oop bp_key_at(oop off) {
    if (!IS_SMI(off) || oimg_blob_base == NULL) return NULL;
    intptr_t n = UNTAG_SMI(off);
    if (n < 0 || n % (intptr_t)sizeof(oop) != 0 || oimg_blob_base + n >= oimg_blob_end) return NULL;
    return (oop)(oimg_blob_base + n);
}

#ifdef OEVM_METHOD_PROFILE
/* #164 readback, confined to the mirror exactly like the breakpoint prims below.
   The argument is whatever nsl already holds: an element of `mirror methodsOf:`
   (a Bytecode/Primitive Closure) or a bare
   CompiledMethod -- all normalize to the pointer mprof_bump keyed on.

   NO PRIM ID. mirror_dispatch branches on the selector NAME, so this adds nothing
   to prim_ids.h and shifts nothing -- the append-only registry is untouched. */
oop primitive_calls_of(oop receiver, int argc, oop *args) {
    (void)argc; (void)args;
    if (receiver == NULL || IS_SMI(receiver)) return TAG_SMI(0);
    {
        oop vt = obj_vtable(receiver);
        const void *key = (vt == BytecodeClosureVtable || vt == PrimitiveClosureVtable)
                        ? (const void *)closure_entry((struct Closure *)receiver)
                        : (const void *)receiver;
        return TAG_SMI((intptr_t)mprof_count(key));
    }
}

/* Distinct methods seen, and activations DROPPED because the table was full.
   Reported separately and never folded into a count, so a saturated table cannot
   read as "that method was never called" -- silence and success must look
   different. A non-zero overflow invalidates the whole run, not one row. */
oop primitive_calls_stats(oop receiver, int argc, oop *args) {
    (void)receiver; (void)argc; (void)args;
    {
        /* Total activations is SUMMED here rather than kept as a running counter:
           a second increment on the hot path would be pure cost for a number the
           table already contains. */
        uint32_t total = 0; int i;
        oop arr = indexable_new(4);
        struct ObjectInstance *inst;
        for (i = 0; i < OEVM_MPROF_SLOTS; i++) total += g_mprof_n[i];
        inst = (struct ObjectInstance *)arr;
        inst->slots[1] = TAG_SMI((intptr_t)g_mprof_used);
        inst->slots[2] = TAG_SMI((intptr_t)g_mprof_overflow);
        inst->slots[3] = TAG_SMI((intptr_t)OEVM_MPROF_SLOTS);
        inst->slots[4] = TAG_SMI((intptr_t)total);
        return arr;
    }
}

/* Zero every counter -- the bounded-window opener, same role span_reset_all plays
   for #72's capture. All-slots on purpose: a per-method reset would let a caller
   clear half a window and publish the result as if it were whole. */
oop primitive_calls_reset(oop receiver, int argc, oop *args) {
    (void)receiver; (void)argc; (void)args;
    {
        int i;
        for (i = 0; i < OEVM_MPROF_SLOTS; i++) { g_mprof_key[i] = NULL; g_mprof_n[i] = 0; }
        g_mprof_overflow = 0; g_mprof_used = 0;
    }
    return NULL;
}
#endif /* OEVM_METHOD_PROFILE */

/* Confined C-assisted breakpoint prims (Task 7 -- reachable ONLY via mirror_dispatch).
   Each takes (SMI blob offset, SMI pc) and answers the offset, or nil when
   bp_key_at refuses it (rete#323). */
oop primitive_break_add(oop receiver, int argc, oop *args) {
    (void)receiver;
    if (argc < 2 || !IS_SMI(args[1])) {
        fprintf(stderr, "VM Error: breakAt:offset: requires (SMI blob offset, SMI pc)\n");
        abort();
    }
    oop key = bp_key_at(args[0]);
    if (key == NULL) return NULL;
    bp_add(key, (long)UNTAG_SMI(args[1]));
    return args[0];
}

oop primitive_break_remove(oop receiver, int argc, oop *args) {
    (void)receiver;
    if (argc < 2 || !IS_SMI(args[1])) {
        fprintf(stderr, "VM Error: unbreakAt:offset: requires (SMI blob offset, SMI pc)\n");
        abort();
    }
    oop key = bp_key_at(args[0]);
    if (key == NULL) return NULL;
    bp_remove(key, (long)UNTAG_SMI(args[1]));
    return args[0];
}

/* The blob offset of a running context's CompiledMethod (rete#323); nil with no
   blob, or for a method outside it. */
oop primitive_blob_offset_of(oop receiver, int argc, oop *args) {
    (void)argc; (void)args;
    if (receiver == NULL || IS_SMI(receiver) || oimg_blob_base == NULL) return NULL;
    struct MethodContext *ctx = (struct MethodContext *)receiver;
    const uint8_t *cm = (const uint8_t *)closure_entry((struct Closure *)ctx->closure);
    if (cm < oimg_blob_base || cm >= oimg_blob_end) return NULL;
    return TAG_SMI((intptr_t)(cm - oimg_blob_base));
}

/* The captured locals of a frame (rete#323): the slots of the heap Context in its
   register num_arguments + 1 (the compiler's ctx_reg; OP_LD_CTX reads the same
   register) as a fresh Array. nil when that register holds no Context -- a frame
   with no captures, or one whose OP_MK_CONTEXT prologue has not run (frame regs
   are nil-seeded). */
oop primitive_lexical_slots_of(oop receiver, int argc, oop *args) {
    (void)argc; (void)args;
    if (receiver == NULL || IS_SMI(receiver)) return NULL;
    struct MethodContext *mc = (struct MethodContext *)receiver;
    struct CompiledMethod *cm = (struct CompiledMethod *)closure_entry((struct Closure *)mc->closure);
    int r = cm->num_arguments + 1;
    if (r >= cm->num_registers) return NULL;
    oop c = mc->regs[r];
    if (c == NULL || IS_SMI(c) || obj_vtable(c) != ContextVtable) return NULL;
    uint32_t n = ((struct Context *)c)->num_slots;
    oop arr = indexable_new((intptr_t)n);   /* slots[0] = size, [1..n] the elements */
    c = mc->regs[r];                        /* re-read after the allocation */
    struct ObjectInstance *inst = (struct ObjectInstance *)arr;
    for (uint32_t i = 0; i < n; i++) inst->slots[i + 1] = ((struct Context *)c)->slots[i];
    return arr;
}

oop primitive_vtable_selectors(oop receiver, int argc, oop *args) {
    (void)argc; (void)args;
    if (receiver == NULL || IS_SMI(receiver)) {
        fprintf(stderr, "VM Error: selectorsOf: requires a vtable receiver\n");
        abort();
    }
    struct Vtable *vt = (struct Vtable *)receiver;
    intptr_t n = (intptr_t)vt->size;
    oop arr = indexable_new(n);
    struct ObjectInstance *inst = (struct ObjectInstance *)arr;
    struct Binding *bs = vtable_bindings((oop)vt);
    for (intptr_t i = 0; i < n; i++) {
        inst->slots[i + 1] = binding_symbol(&bs[i]);
    }
    return arr;
}

oop primitive_vtable_methods(oop receiver, int argc, oop *args) {
    (void)argc; (void)args;
    if (receiver == NULL || IS_SMI(receiver)) {
        fprintf(stderr, "VM Error: methodsOf: requires a vtable receiver\n");
        abort();
    }
    struct Vtable *vt = (struct Vtable *)receiver;
    intptr_t n = (intptr_t)vt->size;
    oop arr = indexable_new(n);
    struct ObjectInstance *inst = (struct ObjectInstance *)arr;
    struct Binding *bs = vtable_bindings((oop)vt);
    for (intptr_t i = 0; i < n; i++) {
        inst->slots[i + 1] = binding_method(&bs[i]);
    }
    return arr;
}

