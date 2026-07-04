/* oevm/src/kernel_boot.c
 *
 * Task 11 — Firmware kernel bootstrap for the decoupled XIP image model.
 *
 * This file owns the APP-INDEPENDENT kernel objects that, under Model A, were
 * emitted into every per-program `solidified_image.c`:
 *   - the root meta-vtables (VtableVtable, SymbolVtable, …) and the
 *     IntegerVtable / FloatVtable / MethodContextVtable;
 *   - the kernel-owned symbols the firmware references regardless of app.
 *
 * `kernel_boot()` assigns the global vtable/symbol pointer vars (DEFINED in
 * dispatch.c / vm.c — we only assign them, never redefine) and POPULATES the
 * `builtin_registry[]` + `oimg_singleton[]` tables (the arrays themselves are
 * DEFINED, zero-initialized, in oimg_loader.c so Model A still links cleanly).
 *
 * Index order matches src/builtins.def (compacted in Task 9a):
 *   0..2   singletons : nil(NULL) / true / false
 *   3..10  vtables    : Integer, Float, MethodContext, Symbol, Vtable,
 *                       PrimitiveClosure, BytecodeClosure, BoxedBinding
 *   11..15 symbols    : == sender value accessViolation: debug:
 *   16..17 vtables    : LargeInteger, String
 */
#include "oevm.h"
#include "oimg.h"
#include <stddef.h>

/* ------------------------------------------------------------------ *
 * Forward-declared firmware-static kernel storage.
 * ------------------------------------------------------------------ */
const struct Vtable Vtable_VtableVtable;
const struct Vtable Vtable_SymbolVtable;
const struct Vtable Vtable_StringVtable;
const struct Vtable Vtable_BytecodeClosureVtable;
const struct Vtable Vtable_PrimitiveClosureVtable;
const struct Vtable Vtable_BoxedBindingVtable;
/* All fiber vtables are gone: RunnableFiberVtable (the last one) was retired with
   the struct Fiber wrapper in Task 6; Suspended/Dead went with the scheduler (Task 5). */
const struct Vtable Vtable_IntegerVtable;
const struct Vtable Vtable_FloatVtable;
const struct Vtable Vtable_LargeIntegerVtable;
const struct Vtable Vtable_MethodContextVtable;
const struct Vtable Vtable_StackVtable;
const struct Vtable Vtable_ContextVtable;

/* ------------------------------------------------------------------ *
 * Kernel-owned interned symbols (firmware static storage).
 * Names + lengths mirror StaticSymbol_* from the old solidified image and the
 * Symbol_* globals the VM hardcodes.
 * ------------------------------------------------------------------ */
static const struct Symbol KSym__eq_eq          = { { (oop)&Vtable_SymbolVtable }, "==", 2 };
static const struct Symbol KSym_sender          = { { (oop)&Vtable_SymbolVtable }, "sender", 6 };
/* KSym_executeCleanup / KSym_resumeWith_ / KSym_unwindTo_value_ removed (Task 9a):
 * NLR-only symbols retired in Task 3; their registry rows are gone. */
static const struct Symbol KSym_value           = { { (oop)&Vtable_SymbolVtable }, "value", 5 };
static const struct Symbol KSym_accessViolation_= { { (oop)&Vtable_SymbolVtable }, "accessViolation:", 16 };
static const struct Symbol KSym_debug_          = { { (oop)&Vtable_SymbolVtable }, "debug:", 6 };

/* Singleton true/false symbols (nil is the VM's NULL). */
static const struct Symbol KSym_true            = { { (oop)&Vtable_SymbolVtable }, "true", 4 };
static const struct Symbol KSym_false           = { { (oop)&Vtable_SymbolVtable }, "false", 5 };

/* ------------------------------------------------------------------ *
 * Vtable definitions.
 * ------------------------------------------------------------------ */
const struct Vtable Vtable_IntegerVtable = {
    { (oop)&Vtable_VtableVtable }, NULL, (struct Binding *)NULL, 0, 0, 0
};
const struct Vtable Vtable_FloatVtable = {
    { (oop)&Vtable_VtableVtable }, NULL, (struct Binding *)NULL, 0, 0, 0
};
const struct Vtable Vtable_LargeIntegerVtable = {
    { (oop)&Vtable_VtableVtable }, NULL, (struct Binding *)NULL, 0, 0, 0
};

/* MethodContextVtable has no kernel-bound methods (its sole binding,
   unwindTo:value:, was removed with the NLR machinery in Milestone A). */
const struct Vtable Vtable_MethodContextVtable = {
    { (oop)&Vtable_VtableVtable },
    NULL, (struct Binding *)NULL,
    0, 0,
    0
};

/* Root meta-vtables: their own meta is VtableVtable (self for VtableVtable). */
const struct Vtable Vtable_VtableVtable          = { { (oop)&Vtable_VtableVtable }, NULL, NULL, 0, 0, 0 };
const struct Vtable Vtable_SymbolVtable          = { { (oop)&Vtable_VtableVtable }, NULL, NULL, 0, 0, 0 };
const struct Vtable Vtable_StringVtable          = { { (oop)&Vtable_VtableVtable }, NULL, NULL, 0, 0, 0 };
const struct Vtable Vtable_BytecodeClosureVtable = { { (oop)&Vtable_VtableVtable }, NULL, NULL, 0, 0, 0 };
const struct Vtable Vtable_PrimitiveClosureVtable= { { (oop)&Vtable_VtableVtable }, NULL, NULL, 0, 0, 0 };
const struct Vtable Vtable_BoxedBindingVtable    = { { (oop)&Vtable_VtableVtable }, NULL, NULL, 0, 0, 0 };
/* No fiber vtable is defined here or in VM-CORE anymore — the last one
   (RunnableFiberVtable) was retired with the Fiber wrapper in Task 6. */
/* Stack (L1 Task 2): private runtime vtable, not a builtins.def registry
 * entry — internal runtime state no image ever names. LIVE (Task 6): the
 * Stack IS the running engine's execution context. */
const struct Vtable Vtable_StackVtable           = { { (oop)&Vtable_VtableVtable }, NULL, NULL, 0, 0, 0 };
/* Context (deferred-lift Phase 1, Task 1): private runtime vtable, not a
 * builtins.def registry entry — internal runtime state no image ever
 * names. LIVE (decision 2026-07-04): the compiler emits OP_MK_CONTEXT/
 * LD_CTX/ST_CTX for escaping captures. */
const struct Vtable Vtable_ContextVtable         = { { (oop)&Vtable_VtableVtable }, NULL, NULL, 0, 0, 0 };

/* ------------------------------------------------------------------ *
 * kernel_boot — wire globals + populate registry/singletons.
 * ------------------------------------------------------------------ */
void kernel_boot(void) {
    static int booted = 0;
    if (booted) return;
    booted = 1;

    extern oop Symbol_value;
    extern oop Symbol_accessViolation;
    extern oop Symbol_debug_;

    /* (a) Assign global vtable pointers (as init_solidified_vtables did). */
    VtableVtable           = (oop)&Vtable_VtableVtable;
    SymbolVtable           = (oop)&Vtable_SymbolVtable;
    StringVtable           = (oop)&Vtable_StringVtable;
    BytecodeClosureVtable  = (oop)&Vtable_BytecodeClosureVtable;
    PrimitiveClosureVtable = (oop)&Vtable_PrimitiveClosureVtable;
    BoxedBindingVtable     = (oop)&Vtable_BoxedBindingVtable;
    /* No fiber vtable to assign: RunnableFiberVtable was retired in Task 6 (registry slot 12 RESERVED). */
    StackVtable            = (oop)&Vtable_StackVtable;
    ContextVtable          = (oop)&Vtable_ContextVtable;
    IntegerVtable          = (oop)&Vtable_IntegerVtable;
    FloatVtable            = (oop)&Vtable_FloatVtable;
    LargeIntegerVtable     = (oop)&Vtable_LargeIntegerVtable;
    MethodContextVtable    = (oop)&Vtable_MethodContextVtable;
    /* Legacy FiberVtable and the Suspended/Dead lifecycle vtables were removed with
     * the fiber scheduler (Task 5). */

    /* (a') Assign global kernel symbol vars to the firmware-static symbols. */
    Symbol_value          = (oop)&KSym_value;
    Symbol_accessViolation= (oop)&KSym_accessViolation_;
    Symbol_debug_         = (oop)&KSym_debug_;

    /* (b) Singletons 0..2: nil(NULL) / true / false. */
    oimg_singleton[0] = NULL;                 /* nil == NULL in this VM */
    oimg_singleton[1] = (oop)&KSym_true;
    oimg_singleton[2] = (oop)&KSym_false;

    /* (c) builtin_registry[] in builtins.def index order. */
    builtin_registry[0]  = oimg_singleton[0]; /* nil  */
    builtin_registry[1]  = oimg_singleton[1]; /* true */
    builtin_registry[2]  = oimg_singleton[2]; /* false */

    builtin_registry[3]  = IntegerVtable;
    builtin_registry[4]  = FloatVtable;
    builtin_registry[5]  = MethodContextVtable;
    builtin_registry[6]  = SymbolVtable;
    builtin_registry[7]  = VtableVtable;
    builtin_registry[8]  = PrimitiveClosureVtable;
    builtin_registry[9]  = BytecodeClosureVtable;
    builtin_registry[10] = BoxedBindingVtable;
    /* Fiber vtable rows (legacy Fiber/Runnable/Suspended/Dead) removed in Task 9a. */

    builtin_registry[11] = (oop)&KSym__eq_eq;
    builtin_registry[12] = (oop)&KSym_sender;
    /* executeCleanup / resumeWith: / unwindTo:value: rows removed in Task 9a. */
    builtin_registry[13] = (oop)&KSym_value;
    builtin_registry[14] = (oop)&KSym_accessViolation_;
    builtin_registry[15] = (oop)&KSym_debug_;

    builtin_registry[16] = LargeIntegerVtable;
    builtin_registry[17] = StringVtable;
}
