#ifndef OEVM_H
#define OEVM_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

/*
 * VM bring-up tracing. Compile with -DOEVM_TRACE to emit interpreter/dispatch
 * trace lines to stdout. Off by default so test output stays clean for the
 * black-box conformance harness.
 */
#ifdef OEVM_TRACE
#define OEVM_TRACE_PRINT(...) printf(__VA_ARGS__)
#else
#define OEVM_TRACE_PRINT(...) ((void)0)
#endif

/* 
 * oop: Object Oriented Pointer.
 * Under Piumarta's Open Extensible Object Model (COLA/Id style),
 * every reference is an oop, which points to a struct Object.
 */
typedef struct Object *oop;

/*
 * Small Integer (Smi) Tagging.
 * We reserve the least significant bit (LSB) = 1 to represent immediate 31-bit/63-bit integers.
 * Heap objects always have LSB = 0 (aligned to word boundaries).
 */
#define IS_SMI(ptr)      (((uintptr_t)(ptr) & 1) != 0)
#define TAG_SMI(val)     ((oop)(((uintptr_t)(val) << 1) | 1))
#define UNTAG_SMI(ptr)   ((intptr_t)(ptr) >> 1)

/*
 * Object: The root structure of all objects in the system.
 * The first field must point to a Vtable object.
 */
struct Object {
    oop vtable;
};

/*
 * Symbol: An interned, unique string token representing a message selector.
 * Since a Symbol is an Object, it starts with a struct Object base.
 */
struct Symbol {
    struct Object base;
    const char *name;
    size_t length;
};

/*
 * String: an immutable, NON-interned, packed UTF-8 byte sequence (std-lib 3c-1).
 * Unlike Symbol (separate malloc'd `name`), the bytes are INLINE in the heap
 * object — one allocation, and a GC leaf (gc.c trace_one must not scan `bytes`
 * as oops). Byte-granularity protocol only; runes are deferred (issue #1).
 */
struct String {
    struct Object base;     // vtable == StringVtable
    size_t length;          // byte length
    uint8_t bytes[];        // inline UTF-8 payload, `length` bytes (no NUL required)
};

/*
 * Binding: A mapping from a Symbol (selector) to a Method/Closure object.
 */
struct Binding {
    oop symbol; // Key
    oop method; // Value (Closure/Primitive)
};

/*
 * Vtable: Virtual Method Table mapping Symbols to methods.
 * A Vtable is itself an Object, so its first field points to a meta-vtable.
 * delegate: points to the parent/delegated vtable for lookup fallbacks.
 */
struct Vtable {
    struct Object base;
    oop delegate;
    struct Binding *bindings;
    size_t capacity;
    size_t size;
    int is_nested;
};

/*
 * MethodContext: The "Double-Agent" Stack Frame Layout.
 * 
 * Allocated sequentially on the stack array for performance by default.
 * Its memory footprint is engineered to mirror an Object payload exactly.
 * When stack reification is triggered, the vtable_ptr is patched from NULL
 * (or a Dummy Token) to a global MethodContextVtable, turning the frame
 * polymorphically into a standard Object.
 * 
 * Offsets:
 * [ Offset 0: vtable_ptr ] -> Initialized to NULL / Dummy Token (Fast Path)
 * [ Offset 1: prev_frame ] -> Dynamic Link (relative-SMI offset into the owning Stack; frame_abi.h)
 * [ Offset 2: saved_pc   ] -> Instruction Pointer return address
 * [ Offset 3: home_ctx   ] -> Lexical Link (discriminated: NULL | relative-SMI; frame_abi.h)
 * [ Offset 4: closure    ] -> The compiled closure being executed
 * [ Offset 5: gen_tag    ] -> 32-bit Lifecycle Generation Counter
 * [ Offset 6...: regs    ] -> Local Register Array (Virtual Address Space)
 */
struct MethodContext {
    oop vtable;               // Offset 0
    oop prev_frame;           // Offset 1
    const uint32_t *saved_pc; // Offset 2
    oop home_ctx;             // Offset 3
    oop closure;              // Offset 4: The compiled closure being executed

    /*
     * Offset 5: 32-bit Lifecycle Generation Counter.
     * We use a union with uintptr_t to ensure the offset index layout remains
     * exactly identical on both 32-bit and 64-bit systems.
     * On 64-bit hosts, this pads gen_tag to pointer size (8 bytes), keeping
     * regs at Offset 6.
     */
    union {
        uint32_t tag32;
        uintptr_t tag_word;
    } gen_tag;                // Offset 5

    /*
     * Offset 6...: Local Register Array (Virtual Address Space).
     * Flexible array member starting at offset 6.
     */
    oop regs[];               // Offset 6
};

/*
 * Closure: A block closure object which captures lexical environment.
 * It is an Object (inherits from struct Object base).
 * home_ctx: Lexical link to the defining MethodContext frame — discriminated
 *   {NULL | relative-SMI} (frame_abi.h), decoded via vm.c's home_ctx_frame; NOT a raw pointer.
 * home_generation: Generation tag of the home context frame (to prevent dangling returns).
 * entry_point: Pointer to the compiled bytecode or primitive C function.
 */
struct Closure {
    struct Object base;
    oop home_ctx;
    uint32_t home_generation;
    void *entry_point;
    oop captured_env;   /* enclosing Context for captured-var access (deferred-lift P1);
                            NULL at top level. Runtime-only — Closures are never
                            .oimg-serialized, so this adds no format concern. */
    oop captured_self;  /* De-recursify Phase 2 (self-migration): the home method's
                            receiver, snapshotted at make_closure. Read via OP_LD_SELF
                            so a block reaches enclosing `self` fiber-independently
                            instead of walking the (relativized) home_ctx chain.
                            Runtime-only. */
};

/*
 * Boxed Float Object
 */
struct Float {
    struct Object base;
    double value;
};

/*
 * Boxed LargeInteger: a sign + 64-bit magnitude integer, used when a value
 * overflows the tagged SmallInteger (SMI) range. The VM promotes SMI arithmetic
 * to a LargeInteger on overflow and demotes a result back to an SMI when it
 * fits. Magnitude is bounded at 2^64-1 (a deliberately fixed-width "bignum"),
 * which is portable to -m32 (no __int128). magnitude 0 is always an SMI.
 */
struct LargeInteger {
    struct Object base;
    int64_t sign;       /* +1 or -1 */
    uint64_t mag;       /* magnitude; never 0 */
};

extern oop LargeIntegerVtable;
oop largeint_alloc(int sign, uint64_t mag);

/* Generic integer arithmetic over SMI / LargeInteger operands; each normalises
 * the result back to an SMI when it fits, else a LargeInteger. Used by the VM
 * arithmetic opcodes (on overflow / large operands) and the `//` primitive. */
int  oevm_is_integer(oop x);
oop  oevm_int_add(oop a, oop b);
oop  oevm_int_sub(oop a, oop b);
oop  oevm_int_mul(oop a, oop b);
oop  oevm_int_floordiv(oop a, oop b);
oop  int_from_intptr(intptr_t v);   /* normalise a raw result to SMI or LargeInteger */
double oevm_int_to_double(oop x);

/*
 * float_alloc: allocate a struct Float from the bump arena (src/gc.c).
 * Faster than gc_allocate for the high-churn float path; not GC-reclaimed
 * (see issue #3). Caller sets ->value after allocation.
 */
oop float_alloc(oop vtable);

/*
 * Fiber cooperative execution context
 */
struct Fiber {
    struct Object base;
    oop stack_obj;   /* the fiber's Stack heap object; ((struct Stack*)stack_obj)->data is the call-stack payload, ->capacity its size */
    struct MethodContext *current_context;
    size_t stack_ptr;
    oop status; // Runnable, Suspended, or Dead
    int single_step;
    int hook_step_request;   /* -1 = none; 0/1 = hook-requested post-hook stepping */
};

/*
 * Stack: a first-class heap object holding a contiguous region of
 * MethodContext frames (the de-recursify rewrite's language stack as heap
 * data — see docs/fiber_l0_frame_abi.md). DORMANT as of L1 Task 2: allocated
 * and GC-traced, but not yet wired into the running engine (Task 3+).
 */
struct Stack {
    struct Object base;      /* vtable = StackVtable; GC-managed heap object */
    uint32_t capacity;       /* slots in data[] */
    uint32_t sp;              /* bump cursor (top), word index */
    uint32_t current_off;    /* offset of the live frame within data[] */
    oop      data[];         /* frame region: MethodContext headers + regs */
};
extern oop StackVtable;
oop stack_alloc(uint32_t capacity);

/* Context: heap temp-vector for captured (closed-over) locals (deferred-lift
   Phase 1). Layout is the L0-reserved struct in frame_abi.h. Private runtime
   vtable — not in builtins.def, not .oimg-serialized. */
extern oop ContextVtable;
oop context_alloc(uint32_t num_slots, oop outer, uint32_t home_generation);

/*
 * ObjectInstance: Standard instance of a class/vtable with dynamic slots.
 */
struct ObjectInstance {
    struct Object base;
    oop slots[];
};

/* Opcodes for our register-based VM */
enum Opcode {
    OP_LD_CONST = 0,
    OP_LD_REG,
    OP_SEND,
    /* 0x03 retired: OP_SEND_IC (dead IC path — never emitted by the compiler;
       handler + send_ic() + struct InlineCache deleted in de-recursify Phase 2
       Task 3). Value kept reserved to avoid renumbering serialized .oimg images. */
    OP_SEND_IC_RETIRED,
    OP_RET,
    OP_NLR_RETIRED,   /* reserved; compacted in Milestone A renumber */
    OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_DIV,
    OP_JMP,
    OP_JMP_IF_FALSE,
    OP_LD_SLOT,
    OP_ST_SLOT,
    OP_NEW,
    OP_SEND_IMPLICIT,
    OP_BREAK = 0x10,
    // Super-send. Register layout at `c`: [c]=start vtable (the defining class's
    // delegate, a VTABLE: constant), [c+1]=self (receiver), [c+2..]=args; `a`=argc,
    // `b`=selector const. Result is written back to register [c].
    OP_SEND_SUPER = 0x11,
    OP_LD_LEXICAL = 0x12,   /* dest, src1=depth, src2=idx : lexical read  */
    OP_ST_LEXICAL = 0x13,   /* dest=value, src1=depth, src2=idx : lexical write */
    OP_CLOSURE    = 0x14,   /* dest, bx=method-const-idx : create BytecodeClosure */
    OP_LD_SENDER  = 0x15,   /* dest, src1=ctx-reg : load prev_frame (sender) of the context in R(src1) */
    OP_BLOCK_INVOKE = 0x16, /* a=argc, b=selector-const, c=receiver reg (== OP_SEND layout).
                               Fast-path: invoke a BytecodeClosure as a block; else fall back to send(). */
    /* De-recursify deferred-lift Phase 1, Task 2: temp-vector (Context) ops.
       INERT — not yet emitted by the compiler. Convention: a frame's own
       Context lives in the FIXED register `method->num_arguments + 1` (no
       spare 4th operand byte exists in the 32-bit dest/src1/src2 ISA; see
       the handlers in vm.c for the walk). */
    OP_MK_CONTEXT = 0x17,   /* dest, num_slots(src1), outer-sentinel(src2) : alloc frame Context.
                               src2==0xFF => outer = running closure's captured_env (block);
                               else       => outer = NULL (method root). */
    OP_LD_CTX     = 0x18,   /* dest, depth(src1), idx(src2) : read from regs[num_arguments+1], walk `depth` outers, read slots[idx] */
    OP_ST_CTX     = 0x19,   /* srcval(dest), depth(src1), idx(src2) : same walk, write slots[idx] = regs[dest] */
    OP_LD_SELF    = 0x1A,   /* dest = reg; load closure->captured_self (self-migration, P2) */
};

enum AccessModifier {
    ACCESS_PUBLIC = 0,
    ACCESS_PROTECTED = 1,
    ACCESS_PRIVATE = 2
};

/* Compiled method structure wrapping instruction stream and constants */
struct CompiledMethod {
    const uint32_t *instructions;
    size_t instruction_count;
    int num_registers;
    int num_arguments;
    const oop *constants;
    size_t constant_count;
    int access_modifier;
};

/*
 * BoxedBinding: A heap-allocated first-class association wrapper for a Vtable Binding.
 */
struct BoxedBinding {
    struct Object base;
    oop symbol;
    oop method;
};

/*
 * Global Static Vtables
 */
extern oop IntegerVtable;
extern oop FloatVtable;
extern oop MethodContextVtable;
extern oop FiberVtable;
extern oop SymbolVtable;
extern oop StringVtable;
extern oop VtableVtable; // Meta-vtable for Vtables
extern oop PrimitiveClosureVtable;
extern oop BytecodeClosureVtable;
extern oop BoxedBindingVtable;
extern oop RunnableFiberVtable;
extern oop SuspendedFiberVtable;
extern oop DeadFiberVtable;

extern oop current_fiber;
extern const struct Symbol true_sym_obj;   /* canonical #true  (defined in dispatch.c) — == result */
extern const struct Symbol false_sym_obj;  /* canonical #false (defined in dispatch.c) — == result */

/*
 * Reflection / Introspection Primitives
 */
oop primitive_object_vtable(oop receiver, int argc, oop *args);
oop primitive_vtable_delegate(oop receiver, int argc, oop *args);
oop primitive_vtable_bindings(oop receiver, int argc, oop *args);
oop primitive_method_context_sender(oop receiver, int argc, oop *args);
oop primitive_method_context_pc(oop receiver, int argc, oop *args);
oop primitive_closure_home(oop receiver, int argc, oop *args);
oop primitive_object_equals(oop receiver, int argc, oop *args);
oop primitive_object_outer(oop receiver, int argc, oop *args);

oop primitive_register_debugger_hook(oop receiver, int argc, oop *args);
/* Confined requestStep: prim (Task 3 — reachable ONLY via mirror_dispatch).
   Sets current fiber's single_step + hook_step_request from inside a break hook. */
oop primitive_request_step(oop receiver, int argc, oop *args);
/* Confined read-only mirror prims (Task 2 — reachable ONLY via mirror_dispatch). */
oop primitive_method_offset(oop receiver, int argc, oop *args);
oop primitive_compiled_method_instruction_at(oop receiver, int argc, oop *args);
/* Confined C-assisted breakpoint prims (Task 7 — reachable ONLY via mirror_dispatch). */
oop primitive_break_add(oop receiver, int argc, oop *args);
oop primitive_break_remove(oop receiver, int argc, oop *args);
oop primitive_method_of(oop receiver, int argc, oop *args);
oop primitive_vtable_selectors(oop receiver, int argc, oop *args);
oop primitive_vtable_methods(oop receiver, int argc, oop *args);
/* std-lib step 1: the singleton platform capability object (src/dispatch.c).
   Lazily constructed; the only handle to the VM-level allocation / identity /
   I-O primitives. As of step 2 both boot entry seams pass this to the entry
   class's init and main:args: (ns_compile_oevm.py generated C; oimg_loader.c). */
oop platform_object(void);
/* std-lib step 3c-3b: serial native capability (src/serial.c).
   Lazy singleton; own selector matching for basicReadByte/basicWriteByte:/basicAtEnd. */
oop serial_object(void);
oop serial_dispatch(oop selector, int argc, oop *args);

oop primitive_method_context_reg_at(oop receiver, int argc, oop *args);
oop primitive_method_context_reg_at_put(oop receiver, int argc, oop *args);
/* confined read prim (Task 8): register count of a context's method, to bound a dump */
oop primitive_method_context_num_registers(oop receiver, int argc, oop *args);
oop primitive_fiber_set_single_step(oop receiver, int argc, oop *args);
oop primitive_vtable_bind(oop receiver, int argc, oop *args);
oop primitive_compiled_method_set_instruction_at(oop receiver, int argc, oop *args);

/* Overridable in-language millisecond clock (same pattern as oevm_oom_handler).
   Host default uses CLOCK_MONOTONIC; the Zephyr appliance reassigns it to
   k_uptime_get() before boot. Exposed to Newspeak as the unary `millis`. */
extern uint64_t (*oevm_millis)(void);

int boundclass_is(oop o);
oop make_closure(oop method);
oop send(oop receiver, oop selector, int argc, oop *args);
/* Resolve a send without invoking a bytecode target (de-recursify Phase 2). On a
   bytecode/value-family target, sets the out-params (method, recv, argc) and
   returns NULL; otherwise services the send inline and returns the value (out
   method left NULL). See dispatch.c. */
oop send_resolve(oop receiver, oop selector, int argc, oop *args,
                 oop *out_method, oop *out_recv, int *out_argc);
oop send_super_resolve(oop start_vtable, oop receiver, oop selector, int argc, oop *args,
                       oop *out_method, oop *out_recv, int *out_argc);
oop send_super(oop start_vtable, oop receiver, oop selector, int argc, oop *args);
oop lookup_defining_vtable(oop receiver_vtable, oop selector, oop *out_method);
/* Global method lookup cache (src/dispatch.c). RAM-resident, direct-mapped. */
struct MethodCacheStats { unsigned long hits, misses, fills, collisions, flushes; };
oop lookup_method_cached(oop receiver_vtable, oop selector, oop *out_method, oop *out_defining_vt);
void method_cache_flush(void);
#ifdef OEVM_CACHE_STATS
struct MethodCacheStats method_cache_stats(void);
#endif
int check_access_allowed(oop receiver, oop selector, struct CompiledMethod *target_method, oop target_defining_vtable);
int is_truthy(oop obj);

/*
 * Fiber Primitives
 */
oop primitive_fiber_create(oop receiver, int argc, oop *args);
oop primitive_fiber_switch(oop receiver, int argc, oop *args);

/*
 * Firmware kernel bootstrap (Task 11). Constructs the app-independent kernel
 * vtables + the kernel unwindTo:value: method, interns the kernel-owned
 * symbols, assigns the global vtable/symbol pointers, and fills the builtin
 * registry + singleton table. Model-B images reference these by index.
 */
void kernel_boot(void);

/* oimg.h needs oop + all struct definitions above — include last. */
#include "oimg.h"

#endif /* OEVM_H */

