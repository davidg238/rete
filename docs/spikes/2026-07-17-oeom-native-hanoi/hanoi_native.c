/* src/hanoi_native.c
 *
 * Experiment (2026-07-17): OEOM native-substitution spike. Behind OEVM_HANOI_NATIVE
 * (off by default). Installs the first PrimitiveClosure-in-vtable binding that
 * services a *user* selector (hanoiMove:from:to:temp:) and lets a bench send to it,
 * to measure the native-AOT ceiling vs the bytecode interpreter on Towers of Hanoi.
 * Design: docs/design/2026-07-17-oeom-native-substitution-hanoi-spike-design.md
 */
#include "oevm.h"
#include "oimg.h"
#include <string.h>
#include <stdint.h>

#ifdef OEVM_HANOI_NATIVE

/* Pure-C recursion. Zero allocation, holds no oops -> GC-trivial. The volatile
   counter fences dead-code elimination and doubles as the 2^n-1 correctness check. */
static volatile uint32_t g_hanoi_moves;

static OEVM_HOT void hanoi_c(intptr_t n) {
    if (n > 1) { hanoi_c(n - 1); hanoi_c(n - 1); }
    g_hanoi_moves++;
}

/* The native method. Primitive ABI: oop(oop,int,oop*). args[0] = n (SMI).
   Returns the move count as an SMI so the bench can assert == 2^n-1 — which also
   proves the send dispatched HERE (to the PrimitiveClosure) rather than DNU'ing. */
oop OEVM_HOT prim_hanoi(oop receiver, int argc, oop *args) {
    (void)receiver; (void)argc;
    g_hanoi_moves = 0;
    hanoi_c(UNTAG_SMI(args[0]));
    return TAG_SMI((intptr_t)g_hanoi_moves);
}

/* Firmware-static native machinery (.data; mutable so acquire() can fill it once).
   mark() skips these at the is_valid_heap_object guard (gc.c:503) — not blob, not
   rodata, not float-arena, not a heap object => immortal, trace-free. */
static struct Closure prim_hanoi_closure;
static struct Binding hanoi_bindings[1];
static struct Vtable  Vtable_HanoiNative;
static struct Object  hanoi_native_singleton;
static int hanoi_wired;

/* Acquisition + one-time lazy wiring. Sent to `self` (the HanoiAB instance) so
   `receiver` carries HanoiAB's class vtable. Walk it + its delegate chain (as
   lookup() does) to find the canonical interned "hanoiMove:from:to:temp:" symbol
   oop the compiled send will present (dispatch.c:868 template), and bind the native
   closure to it. Idempotent. Runs post-load, so image symbols exist. */
oop hanoi_native_acquire(oop receiver) {
    extern oop PrimitiveClosureVtable;
    extern oop VtableVtable;
    extern oop SymbolVtable;
    if (!hanoi_wired) {
        oop sel = NULL;
        oop curr = obj_vtable(receiver);
        while (curr != NULL && !IS_SMI(curr)) {
            struct Vtable *vt = (struct Vtable *)curr;
            struct Binding *bs = vtable_bindings((oop)vt);
            for (size_t i = 0; i < vt->size; i++) {
                struct Symbol *s = (struct Symbol *)binding_symbol(&bs[i]);
                if (s != NULL && obj_vtable((oop)s) == SymbolVtable &&
                    strcmp(symbol_name(s), "hanoiMove:from:to:temp:") == 0) {
                    sel = (oop)s; break;
                }
            }
            if (sel != NULL) break;
            curr = vtable_delegate((oop)vt);
        }
        if (sel == NULL) return NULL;   /* selector not found: bench misconfigured */

        prim_hanoi_closure.base.vtable = PrimitiveClosureVtable;
        prim_hanoi_closure.entry_point = (void *)prim_hanoi;
        hanoi_bindings[0].symbol = sel;
        hanoi_bindings[0].method = (oop)&prim_hanoi_closure;
        Vtable_HanoiNative.base.vtable = VtableVtable;
        Vtable_HanoiNative.delegate = NULL;
        Vtable_HanoiNative.bindings = hanoi_bindings;
        Vtable_HanoiNative.capacity = 1;
        Vtable_HanoiNative.size = 1;
        Vtable_HanoiNative.is_nested = 0;
        hanoi_native_singleton.vtable = (oop)&Vtable_HanoiNative;
        hanoi_wired = 1;
    }
    return (oop)&hanoi_native_singleton;
}

#endif /* OEVM_HANOI_NATIVE */
