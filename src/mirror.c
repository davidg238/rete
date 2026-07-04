/* The mirror capability: confined privileged reflection (mirror step).
   One capability = one C module + one receiver-identity hook in send() (the
   serial.c / platform.c pattern). The reflective prim BODIES live in vm.c and are
   UNCHANGED; this module only re-exposes them mirror-mediated -- the reflected
   object moves from the prim's receiver slot to arg 0 (`mirror senderOf: ctx`).
   Because an ordinary receiver can no longer name the mirror, and the reflective
   `case` blocks were removed from the universal switch(pid) in dispatch.c, sending
   `sender`/`pc`/etc. to any other receiver falls through to vtable lookup -> DNU.
   That DNU is the confinement.

   No GC root: &MirrorObj and its vtable are static BSS, outside the heap range
   gc.c scans (is_valid_heap_object), exactly like &SerialObj / &PlatformObj.

   BUILD SEAM (designed, NOT wired this cycle -- spec ss6): reflection is now
   self-contained here and reached only via the single send() hook, so a production
   target that grants no mirror can drop the whole reflection surface from the
   binary. To do that later: (1) wrap the `if (receiver == mirror_object())` line in
   dispatch.c with `#ifndef OEVM_NO_REFLECTION`, and (2) remove src/mirror.c from the
   build source lists (run_tests_oevm.py, test_load_run.sh, test_portability_32.sh).
   Nothing else changes -- the vm.c prim bodies are not guarded this cycle. */
#include "mirror.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct Object MirrorObj;
static struct Vtable MirrorVtable;
static int mirror_initialized = 0;

oop mirror_object(void) {
    if (!mirror_initialized) {
        MirrorVtable.base.vtable = VtableVtable;
        MirrorVtable.delegate = NULL;
        MirrorVtable.bindings = NULL;
        MirrorVtable.capacity = 0;
        MirrorVtable.size = 0;
        MirrorVtable.is_nested = 0;
        MirrorObj.vtable = (oop)&MirrorVtable;
        mirror_initialized = 1;
    }
    return (oop)&MirrorObj;
}

/* Each branch shims to the unchanged vm.c prim. The reflectee is arg 0, so the
   prim's receiver := args[0] and its own args := args+1 (argc-1). The one
   exception is onBreak:, whose prim ignores receiver and reads the hook from
   args[0] -- pass the mirror args straight through. */
oop mirror_dispatch(oop selector, int argc, oop *args) {
    const char *sel = symbol_name((struct Symbol *)selector);
    /* oevm#15: every mirror selector reads args[0] (the reflectee). Guard before
       the first dereference so a malformed grant fails loud, not on a wild read. */
    if (argc < 1) {
        fprintf(stderr, "VM Error: Mirror #%s sent with no reflectee (argc=%d)\n", sel, argc);
        abort();
    }
    /* read-side queries */
    if (strcmp(sel, "vtableOf:") == 0)   return primitive_object_vtable(args[0], argc - 1, args + 1);
    if (strcmp(sel, "delegateOf:") == 0) return primitive_vtable_delegate(args[0], argc - 1, args + 1);
    if (strcmp(sel, "bindingsOf:") == 0) return primitive_vtable_bindings(args[0], argc - 1, args + 1);
    if (strcmp(sel, "senderOf:") == 0)   return primitive_method_context_sender(args[0], argc - 1, args + 1);
    if (strcmp(sel, "pcOf:") == 0)       return primitive_method_context_pc(args[0], argc - 1, args + 1);
    if (strcmp(sel, "homeOf:") == 0)     return primitive_closure_home(args[0], argc - 1, args + 1);
    if (strcmp(sel, "reg:at:") == 0)     return primitive_method_context_reg_at(args[0], argc - 1, args + 1);
    if (strcmp(sel, "reg:at:put:") == 0) return primitive_method_context_reg_at_put(args[0], argc - 1, args + 1);
    if (strcmp(sel, "numRegistersOf:") == 0) return primitive_method_context_num_registers(args[0], argc - 1, args + 1);
    /* read-only confined prims (Task 2) */
    if (strcmp(sel, "offsetOf:") == 0)             return primitive_method_offset(args[0], argc - 1, args + 1);
    if (strcmp(sel, "method:instructionAt:") == 0) return primitive_compiled_method_instruction_at(args[0], argc - 1, args + 1);
    if (strcmp(sel, "selectorsOf:") == 0)          return primitive_vtable_selectors(args[0], argc - 1, args + 1);
    if (strcmp(sel, "methodsOf:") == 0)            return primitive_vtable_methods(args[0], argc - 1, args + 1);
    /* C-assisted breakpoint prims (Task 7). breakAt:offset:/unbreakAt:offset: take
       (method, off) as args[0..1]; methodOf: maps a context to its CompiledMethod. */
    if (strcmp(sel, "breakAt:offset:") == 0)   return primitive_break_add(NULL, argc, args);
    if (strcmp(sel, "unbreakAt:offset:") == 0) return primitive_break_remove(NULL, argc, args);
    if (strcmp(sel, "methodOf:") == 0)         return primitive_method_of(args[0], argc - 1, args + 1);
    /* mutations */
    if (strcmp(sel, "fiber:singleStep:") == 0)         return primitive_debugger_set_single_step(args[0], argc - 1, args + 1);
    if (strcmp(sel, "vtable:bind:to:") == 0)           return primitive_vtable_bind(args[0], argc - 1, args + 1);
    if (strcmp(sel, "method:instructionAt:put:") == 0) return primitive_compiled_method_set_instruction_at(args[0], argc - 1, args + 1);
    /* onBreak: -- no reflectee; the closure IS args[0] (prim does (void)receiver). */
    if (strcmp(sel, "onBreak:") == 0)    return primitive_register_debugger_hook(NULL, argc, args);
    /* requestStep: -- no reflectee; bool IS args[0]. Operates on the single global
       execution context (the Fiber wrapper is gone, Task 6) so the hook never needs
       a fiber oop. */
    if (strcmp(sel, "requestStep:") == 0) return primitive_request_step(NULL, argc, args);
    /* Unknown selector on the mirror capability: DNU. */
    fprintf(stderr, "VM Error: Mirror doesNotUnderstand #%s\n", sel);
    abort();
}
