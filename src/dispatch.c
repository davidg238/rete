#include "oevm.h"
#include "frame_abi.h"
#include "oimg.h"
#include "platform.h"
#include "platform_backend.h"
#include "serial.h"
#include "udp.h"
#include "mirror.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* RunnableFiberVtable (the last fiber vtable) was retired in Milestone A Task 6
   together with the struct Fiber wrapper it tagged — the flat loop now runs
   directly on g_active_stack (a plain Stack), so no fiber vtable exists. Its
   builtins.def slot 12 stays RESERVED (registry NULL) until Task 9 compacts. */
extern const struct Vtable Vtable_VtableVtable;

/* Define global static Vtables */
oop IntegerVtable = NULL;
oop FloatVtable = NULL;
oop LargeIntegerVtable = NULL;
oop MethodContextVtable = NULL;
oop StackVtable = NULL;
oop ContextVtable = NULL;
oop SymbolVtable = NULL;
oop StringVtable = NULL;
oop RawBytesVtable = NULL;
oop VtableVtable = NULL;
oop PrimitiveClosureVtable = NULL;
oop BytecodeClosureVtable = NULL;
oop BoxedBindingVtable = NULL;
oop Symbol_accessViolation = NULL;
oop debugger_hook = NULL;
oop Symbol_debug_ = NULL;

/* ---- LargeInteger: bounded sign+uint64 "bignum" --------------------------
   Promotion target when SMI arithmetic overflows the tagged range. Magnitude is
   capped at 2^64-1; results are demoted back to an SMI when they fit. Portable
   to -m32 (no __int128). */
oop largeint_alloc(int sign, uint64_t mag) {
    extern oop gc_allocate(oop vtable, size_t size);
    struct LargeInteger *li = (struct LargeInteger *)gc_allocate(
        LargeIntegerVtable, sizeof(struct LargeInteger));
    li->sign = (sign < 0) ? -1 : 1;
    li->mag = mag;
    return (oop)li;
}

/* ---- Stack: first-class heap object (de-recursify rewrite, L1 Task 2) ----
   LIVE (Task 6): the Stack IS the execution context — g_active_stack backs
   the running engine. Mirrors the pre-Milestone-A per-fiber stack allocation
   shape, but as a single GC-managed heap object instead of a malloc'd C
   array. */
oop stack_alloc(uint32_t capacity) {
    extern oop gc_allocate(oop vtable, size_t size);
    size_t bytes = sizeof(struct Stack) + (size_t)capacity * sizeof(oop);
    oop s = gc_allocate(StackVtable, bytes);
    struct Stack *st = (struct Stack *)s;
    st->capacity = capacity;
    st->sp = 0;
    st->current_off = 0;
    return s;
}

/* ---- Context: heap temp-vector for captured (closed-over) locals --------
   (de-recursify deferred-lift Phase 1, Task 1). LIVE (decision 2026-07-04):
   allocated and consumed by the compiler-emitted OP_MK_CONTEXT/LD_CTX/ST_CTX
   ops for escaping captures. `outer` may be a heap oop we must keep live
   across the allocation (a GC during gc_allocate would otherwise sweep it
   before it's linked into the new Context). */
oop context_alloc(uint32_t num_slots, oop outer, uint32_t home_generation) {
    extern oop gc_allocate(oop vtable, size_t size);
    extern void gc_temp_root_push(oop o);
    extern int gc_temp_roots_count;
    /* Hardened (Task 2 review carry-forward): context_alloc is now reachable
       from the LIVE interpreter (OP_MK_CONTEXT), where the caller may already
       hold other temp roots. A flat gc_temp_roots_clear() would wipe them;
       save/restore the count instead so we only pop what we pushed here. */
    int saved = gc_temp_roots_count;
    int pinned = (outer != NULL && !IS_SMI(outer));
    if (pinned) gc_temp_root_push(outer);
    oop c = gc_allocate(ContextVtable, sizeof(struct Context) + (size_t)num_slots * sizeof(oop));
    struct Context *ctx = (struct Context *)c;
    ctx->home_generation = home_generation;
    ctx->num_slots = num_slots;
    ctx->outer = outer;
    for (uint32_t i = 0; i < num_slots; i++) ctx->slots[i] = NULL;
    gc_temp_roots_count = saved;
    return c;
}

int oevm_is_integer(oop x) {
    return IS_SMI(x) ||
           (x != NULL && LargeIntegerVtable != NULL && obj_vtable(x) == LargeIntegerVtable);
}

/* Decompose an integer oop into sign (+1/-1) and uint64 magnitude. */
static void int_to_sm(oop x, int *sign, uint64_t *mag) {
    if (IS_SMI(x)) {
        intptr_t v = UNTAG_SMI(x);
        if (v < 0) { *sign = -1; *mag = -(uint64_t)v; }  /* -(uint64_t)v == |v|, no UB */
        else { *sign = 1; *mag = (uint64_t)v; }
    } else {
        struct LargeInteger *li = (struct LargeInteger *)x;
        *sign = (li->sign < 0) ? -1 : 1;
        *mag = li->mag;
    }
}

/* Normalise (sign, magnitude) to an SMI when it fits, else a LargeInteger. */
static oop sm_to_int(int sign, uint64_t mag) {
    if (mag == 0) return TAG_SMI(0);
    const unsigned PB = (unsigned)(sizeof(intptr_t) * 8 - 1);  /* payload incl. sign bit */
    uint64_t max_pos = ((uint64_t)1 << (PB - 1)) - 1;          /* 2^(W-2) - 1 */
    uint64_t max_neg = ((uint64_t)1 << (PB - 1));              /* 2^(W-2)     */
    if (sign > 0 && mag <= max_pos) return TAG_SMI((intptr_t)mag);
    if (sign < 0 && mag <= max_neg) return TAG_SMI(-(intptr_t)mag);
    return largeint_alloc(sign, mag);
}

/* Canonical integer representation (nsl-tuvm#4): demote a boxed LargeInteger
   to a tagged SMI when the value fits the tagged range. Never allocates and
   returns non-integers untouched, so it is safe on any constant (including
   blob-resident .oimg objects: sign/mag are direct scalar fields, and the
   vtable is read via the blob-aware obj_vtable). Constant pools may carry a
   boxed integer the runtime would represent as an SMI (the .oimg ref-word
   immediate is narrower than the tagged range); loading it unboxed keeps the
   one-value-one-representation invariant that `=`/`hash` (Dictionary keys)
   rely on. */
oop oevm_int_demote(oop v) {
    if (v == NULL || IS_SMI(v)) return v;
    if (obj_vtable(v) != LargeIntegerVtable) return v;
    struct LargeInteger *li = (struct LargeInteger *)v;
    /* same bounds as sm_to_int: SMI payload is (word-1) bits incl. sign */
    const unsigned PB = (unsigned)(sizeof(intptr_t) * 8 - 1);
    uint64_t max_pos = ((uint64_t)1 << (PB - 1)) - 1;
    uint64_t max_neg = ((uint64_t)1 << (PB - 1));
    if (li->sign > 0 && li->mag <= max_pos) return TAG_SMI((intptr_t)li->mag);
    if (li->sign < 0 && li->mag <= max_neg) return TAG_SMI(-(intptr_t)li->mag);
    return v;
}

/* Normalise a raw intptr_t result (may exceed the SMI range -> LargeInteger). */
oop int_from_intptr(intptr_t v) {
    if (v < 0) return sm_to_int(-1, -(uint64_t)v);
    return sm_to_int(1, (uint64_t)v);
}

static oop add_sm(int sa, uint64_t ma, int sb, uint64_t mb) {
    int sign;
    uint64_t mag;
    if (sa == sb) {
        if (__builtin_add_overflow(ma, mb, &mag)) {
            fprintf(stderr, "VM Error: LargeInteger overflow (magnitude exceeds 2^64-1)\n");
            abort();
        }
        sign = sa;
    } else if (ma >= mb) {
        mag = ma - mb; sign = sa;
    } else {
        mag = mb - ma; sign = sb;
    }
    return sm_to_int(sign, mag);
}

oop oevm_int_add(oop a, oop b) {
    int sa, sb; uint64_t ma, mb;
    int_to_sm(a, &sa, &ma); int_to_sm(b, &sb, &mb);
    return add_sm(sa, ma, sb, mb);
}

oop oevm_int_sub(oop a, oop b) {
    int sa, sb; uint64_t ma, mb;
    int_to_sm(a, &sa, &ma); int_to_sm(b, &sb, &mb);
    return add_sm(sa, ma, -sb, mb);   /* a - b == a + (-b) */
}

oop oevm_int_mul(oop a, oop b) {
    int sa, sb; uint64_t ma, mb;
    int_to_sm(a, &sa, &ma); int_to_sm(b, &sb, &mb);
    uint64_t mag;
    if (__builtin_mul_overflow(ma, mb, &mag)) {
        fprintf(stderr, "VM Error: LargeInteger overflow (magnitude exceeds 2^64-1)\n");
        abort();
    }
    return sm_to_int((sa == sb) ? 1 : -1, mag);
}

oop oevm_int_floordiv(oop a, oop b) {
    int sa, sb; uint64_t ma, mb;
    int_to_sm(a, &sa, &ma); int_to_sm(b, &sb, &mb);
    if (mb == 0) {
        fprintf(stderr, "VM Error: Division by zero\n");
        abort();
    }
    uint64_t q = ma / mb, r = ma % mb;
    int sign = (sa == sb) ? 1 : -1;
    if (sign < 0 && r != 0) q += 1;   /* floor toward negative infinity */
    return sm_to_int(sign, q);
}

double oevm_int_to_double(oop x) {
    int s; uint64_t m;
    int_to_sm(x, &s, &m);
    return (s < 0) ? -(double)m : (double)m;
}

/* Ordered comparison for the relational primitives (<, >, <=, >=).
   Returns <0 if a<b, 0 if a==b, >0 if a>b.

   Integers (SMI or LargeInteger) are compared EXACTLY via sign+magnitude and are
   never round-tripped through `double`: that loses precision above 2^53 and, on
   targets whose FPU is single-precision only (soft-float `double`, e.g. the
   nRF52840), can return a wrong result — which left an inlined `whileTrue:` loop
   condition (`i < N`) spinning forever on device. Only a genuine Float operand
   promotes both sides to double. */
static int prim_compare(oop a, oop b) {
    if (oevm_is_integer(a) && oevm_is_integer(b)) {
        int sa, sb; uint64_t ma, mb;
        int_to_sm(a, &sa, &ma);
        int_to_sm(b, &sb, &mb);
        if (sa != sb) return (sa < sb) ? -1 : 1;       /* different signs */
        int cmp = (ma < mb) ? -1 : ((ma > mb) ? 1 : 0); /* same sign: magnitudes */
        return (sa < 0) ? -cmp : cmp;                   /* invert for negatives */
    }
    double v1 = IS_SMI(a) ? (double)UNTAG_SMI(a)
        : ((a != NULL && obj_vtable(a) == FloatVtable) ? ((struct Float *)a)->value
        : (oevm_is_integer(a) ? oevm_int_to_double(a) : 0.0));
    double v2 = IS_SMI(b) ? (double)UNTAG_SMI(b)
        : ((b != NULL && obj_vtable(b) == FloatVtable) ? ((struct Float *)b)->value
        : (oevm_is_integer(b) ? oevm_int_to_double(b) : 0.0));
    return (v1 < v2) ? -1 : ((v1 > v2) ? 1 : 0);
}

/* ---- Global method lookup cache -------------------------------------------
   Direct-mapped, RAM-resident. Key: (receiver_vtable, selector) pointer
   identity. An empty slot has vtable == NULL. Never touches the image blob. */
#ifndef METHOD_CACHE_SIZE
#define METHOD_CACHE_SIZE 4096   /* power of two; host default, -D lower for embedded */
#endif
struct MethodCacheEntry { oop vtable; oop selector; oop method; oop defining_vt; };
static struct MethodCacheEntry method_cache[METHOD_CACHE_SIZE];

#ifdef OEVM_CACHE_STATS
static struct MethodCacheStats method_cache_stats_counters;
struct MethodCacheStats method_cache_stats(void) { return method_cache_stats_counters; }
#define CACHE_STAT_INC(field) (method_cache_stats_counters.field++)
static void method_cache_report(void) {
    struct MethodCacheStats s = method_cache_stats_counters;
    unsigned long total = s.hits + s.misses;
    fprintf(stderr,
        "[method-cache] hits=%lu misses=%lu fills=%lu collisions=%lu flushes=%lu "
        "hit_rate=%.1f%%\n",
        s.hits, s.misses, s.fills, s.collisions, s.flushes,
        total ? (100.0 * (double)s.hits / (double)total) : 0.0);
}
static void method_cache_register_report(void) {
    static int registered = 0;
    if (!registered) { registered = 1; atexit(method_cache_report); }
}
#else
#define CACHE_STAT_INC(field) ((void)0)
#endif

static inline size_t method_cache_hash(oop vtable, oop selector) {
    return (size_t)((((uintptr_t)vtable >> 4) ^ ((uintptr_t)selector >> 3))
                    & (METHOD_CACHE_SIZE - 1));
}

void method_cache_flush(void) {
    memset(method_cache, 0, sizeof(method_cache));
    CACHE_STAT_INC(flushes);
}

extern const struct Vtable Vtable_SymbolVtable;
const struct Symbol true_sym_obj = { { (oop)&Vtable_SymbolVtable }, "true", 4 };
const struct Symbol false_sym_obj = { { (oop)&Vtable_SymbolVtable }, "false", 5 };
#define true_symbol ((oop)&true_sym_obj)
#define false_symbol ((oop)&false_sym_obj)

OEVM_HOT int is_truthy(oop obj) {
    /* The comparison/equality prims return exactly these two statics, so every
       hot conditional (loop bounds, ifTrue:) resolves on an identity compare.
       The strcmp below remains for blob/image-side `true` symbol literals,
       which are distinct objects with the same name. */
    if (obj == true_symbol) return 1;
    if (obj == false_symbol) return 0;
    if (IS_SMI(obj)) {
        return UNTAG_SMI(obj) != 0;
    }
    if (obj != NULL && obj_vtable(obj) == SymbolVtable) {
        struct Symbol *sym = (struct Symbol *)obj;
        return strcmp(symbol_name(sym), "true") == 0;
    }
    return 0;
}

/* Declare VM interpreter entry point */
extern oop run_interpreter(oop closure, oop receiver, int argc, oop *args);

/* Walk Vtable hierarchy to resolve selector lookup */
oop lookup(oop receiver_vtable, oop selector) {
    oop current = receiver_vtable;
    while (current != NULL) {
        struct Vtable *vt = (struct Vtable *)current;
        struct Binding *bs = vtable_bindings((oop)vt);
        for (size_t i = 0; i < vt->size; i++) {
            if (binding_symbol(&bs[i]) == selector) {
                return binding_method(&bs[i]);
            }
        }
        current = vtable_delegate((oop)vt);
    }
    return NULL;
}

oop lookup_defining_vtable(oop receiver_vtable, oop selector, oop *out_method) {
    oop current = receiver_vtable;
    while (current != NULL && !IS_SMI(current)) {
        struct Vtable *vt = (struct Vtable *)current;
        struct Binding *bs = vtable_bindings((oop)vt);
        for (size_t i = 0; i < vt->size; i++) {
            if (binding_symbol(&bs[i]) == selector) {
                if (out_method != NULL) {
                    *out_method = binding_method(&bs[i]);
                }
                return current;
            }
        }
        current = vtable_delegate((oop)vt);
    }
    return NULL;
}

/* Cached front-end to lookup_defining_vtable. Caller guarantees
   receiver_vtable != NULL. Misses (does-not-understand) are never cached. */
oop lookup_method_cached(oop receiver_vtable, oop selector,
                         oop *out_method, oop *out_defining_vt) {
#ifdef OEVM_CACHE_STATS
    method_cache_register_report();
#endif
    size_t h = method_cache_hash(receiver_vtable, selector);
    struct MethodCacheEntry *e = &method_cache[h];
    if (e->vtable == receiver_vtable && e->selector == selector) {
        CACHE_STAT_INC(hits);
        *out_method = e->method;
        *out_defining_vt = e->defining_vt;
        return e->defining_vt;
    }
    CACHE_STAT_INC(misses);
    oop method = NULL;
    oop defining_vt = lookup_defining_vtable(receiver_vtable, selector, &method);
    if (defining_vt != NULL && method != NULL) {
#ifdef OEVM_CACHE_STATS
        if (e->vtable != NULL &&
            (e->vtable != receiver_vtable || e->selector != selector)) {
            CACHE_STAT_INC(collisions);
        }
#endif
        e->vtable = receiver_vtable;
        e->selector = selector;
        e->method = method;
        e->defining_vt = defining_vt;
        CACHE_STAT_INC(fills);
    }
    *out_method = method;
    *out_defining_vt = defining_vt;
    return defining_vt;
}

/* Reified message passed to a `doesNotUnderstand:` handler. Slot 0 holds the
   selector symbol; the `selector` accessor is intercepted in send(). */
static struct Vtable MessageMockVtable;
static int message_mock_initialized = 0;

static oop make_message(oop selector) {
    extern oop gc_allocate(oop vtable, size_t size);
    if (!message_mock_initialized) {
        MessageMockVtable.base.vtable = VtableVtable;
        MessageMockVtable.size = 0;
        MessageMockVtable.delegate = NULL;
        message_mock_initialized = 1;
    }
    oop msg = gc_allocate((oop)&MessageMockVtable, sizeof(struct ObjectInstance) + sizeof(oop));
    ((struct ObjectInstance *)msg)->slots[0] = selector;
    return msg;
}

/* Fallback handler when a selector is not found in the Vtable chain.
   If the receiver defines `doesNotUnderstand:`, reify the message and invoke
   the handler directly (bypassing access control, as this is a VM-internal
   dispatch). Otherwise abort. */
static oop does_not_understand(oop receiver, oop selector, int argc, oop *args) {
    (void)argc; (void)args;

    oop method = NULL;
    oop curr = IS_SMI(receiver) ? IntegerVtable : (receiver ? obj_vtable(receiver) : NULL);
    while (curr != NULL && !IS_SMI(curr)) {
        struct Vtable *vt = (struct Vtable *)curr;
        struct Binding *bs = vtable_bindings((oop)vt);
        for (size_t i = 0; i < vt->size; i++) {
            struct Symbol *s = (struct Symbol *)binding_symbol(&bs[i]);
            if (s != NULL && obj_vtable((oop)s) == SymbolVtable &&
                strcmp(symbol_name(s), "doesNotUnderstand:") == 0) {
                method = binding_method(&bs[i]);
                break;
            }
        }
        if (method != NULL) break;
        curr = vtable_delegate((oop)vt);
    }

    if (method != NULL) {
        oop msg = make_message(selector);
        oop dnu_args[1] = { msg };
        struct Closure *cl = (struct Closure *)method;
        if (obj_vtable((oop)cl) == PrimitiveClosureVtable) {
            oop (*func)(oop, int, oop*) = (oop (*)(oop, int, oop*))closure_entry(cl);
            return func(receiver, 1, dnu_args);
        }
        return run_interpreter(method, receiver, 1, dnu_args);
    }

    struct Symbol *sym = (struct Symbol *)selector;
    fprintf(stderr, "VM Error: Message '%s' not understood by receiver %p (vtable: %p)\n",
            sym ? symbol_name(sym) : "?", receiver, receiver ? obj_vtable(receiver) : NULL);
    abort();
    return NULL;
}

static oop trigger_access_violation(oop receiver, oop selector, int argc, oop *args) {
    (void)argc; (void)args;
    struct Symbol *sym = (struct Symbol *)selector;
    fprintf(stderr, "VM Access Violation: Cannot access method '%s'\n",
            sym ? symbol_name(sym) : "?");

    oop target_selector = NULL;
    oop curr = IS_SMI(receiver) ? IntegerVtable : (receiver ? obj_vtable(receiver) : NULL);
    while (curr != NULL && !IS_SMI(curr)) {
        struct Vtable *vt = (struct Vtable *)curr;
        struct Binding *bs = vtable_bindings((oop)vt);
        for (size_t i = 0; i < vt->size; i++) {
            struct Symbol *s = (struct Symbol *)binding_symbol(&bs[i]);
            if (s != NULL && obj_vtable((oop)s) == SymbolVtable && strcmp(symbol_name(s), "accessViolation:") == 0) {
                target_selector = (oop)s;
                break;
            }
        }
        if (target_selector != NULL) break;
        curr = vtable_delegate((oop)vt);
    }
    
    if (target_selector == NULL) {
        if (Symbol_accessViolation == NULL) {
            extern oop gc_allocate(oop vtable, size_t size);
            oop sym_av = gc_allocate(SymbolVtable, sizeof(struct Symbol));
            ((struct Symbol *)sym_av)->name = "accessViolation:";
            ((struct Symbol *)sym_av)->length = 16;
            Symbol_accessViolation = sym_av;
        }
        target_selector = Symbol_accessViolation;
    }
    
    oop send_args[1] = { selector };
    return send(receiver, target_selector, 1, send_args);
}

/* BoundClass: a runtime "nested class bound to its enclosing instance".
   An ObjectInstance whose vtable is BoundClassVtable, with three slots:
     slot 0 = nested_vtable  (the nested class's vtable / class object)
     slot 1 = outer          (enclosing instance -> the new object's slot 0)
     slot 2 = slot_count      (SMI: the nested class's instance slot count)
   It is produced by the `__boundOuter:slotCount:` primitive, which synthetic
   nested-class accessors emit (one per nested class on the enclosing vtable).
   A BoundClass is runtime-only: never serialized, so it needs no image/registry
   entry. BoundClassVtable is a plain file-static Vtable, lazily initialised on
   first use (mirroring MessageMockVtable above). send() intercepts any receiver
   whose vtable is BoundClassVtable and constructs the nested instance, mirroring
   the compile-time OP_NEW + set-outer(slot 0) + OP_SEND init_<sel> path used for
   statically-named classes. */
static struct Vtable BoundClassVtable;
static int boundclass_vtable_initialized = 0;

static oop ensure_boundclass_vtable(void) {
    if (!boundclass_vtable_initialized) {
        BoundClassVtable.base.vtable = VtableVtable;
        BoundClassVtable.size = 0;
        BoundClassVtable.delegate = NULL;
        boundclass_vtable_initialized = 1;
    }
    return (oop)&BoundClassVtable;
}

/* Exported so platform.c (and future capability modules) can recognise a
   BoundClass class-object argument without duplicating the vtable. */
int boundclass_is(oop o) {
    return o != NULL && !IS_SMI(o) && boundclass_vtable_initialized &&
           obj_vtable(o) == (oop)&BoundClassVtable;
}

/* Construct the nested instance for a send to a BoundClass receiver. Allocates
   ObjectInstance(nested_vtable, slot_count) with nil slots, sets the outer at
   slot 0, then runs `init_<selector>` (string-keyed lookup: the construction
   message's selector matches the nested class's param signature by Newspeak
   convention, e.g. `new`->init_new, `a:b:`->init_a:b:). The init symbol is built
   at runtime so it won't pointer-match the interned one — hence string lookup,
   as in does_not_understand. */
static oop run_method_on(oop method, oop self, int argc, oop *args) {
    struct Closure *cl = (struct Closure *)method;
    if (obj_vtable((oop)cl) == PrimitiveClosureVtable) {
        oop (*func)(oop, int, oop *) = (oop (*)(oop, int, oop *))closure_entry(cl);
        return func(self, argc, args);
    }
    return run_interpreter(method, self, argc, args);
}

static oop boundclass_construct(oop bc, oop selector, int argc, oop *args) {
    extern oop gc_allocate(oop vtable, size_t size);
    struct ObjectInstance *bci = (struct ObjectInstance *)bc;
    oop nested_vtable = bci->slots[0];
    oop outer = bci->slots[1];
    int slot_count = (int)UNTAG_SMI(bci->slots[2]);

    const char *sel = (selector != NULL && SymbolVtable != NULL &&
                       obj_vtable(selector) == SymbolVtable)
                      ? symbol_name((struct Symbol *)selector) : "new";

    // (0) Identity/equality: a BoundClass is a first-class value (it can be
    // stored and compared, e.g. `obj Inner = obj Inner`). Two BoundClasses are
    // equal when they name the same nested class bound to the same enclosing
    // instance — value equality over (nested_vtable, outer), since each access
    // builds a fresh wrapper.
    if (argc == 1 && (strcmp(sel, "=") == 0 || strcmp(sel, "==") == 0)) {
        oop other = args[0];
        int eq = (other != NULL && !IS_SMI(other) &&
                  obj_vtable(other) == (oop)&BoundClassVtable &&
                  ((struct ObjectInstance *)other)->slots[0] == nested_vtable &&
                  ((struct ObjectInstance *)other)->slots[1] == outer);
        return eq ? true_symbol : false_symbol;
    }

    (void)sel;

    // (1) Class-side send: a selector understood by the nested class's metaclass
    // (the class object's header vtable) is a class-side method — dispatch it
    // there with the BoundClass ITSELF as `self`. Keeping self=bc preserves the
    // access-time outer binding: class-side `outer X` reads it from the
    // BoundClass (slot 1), and a class-side `self new` re-enters to construct
    // with that outer. Construction selectors (`new`, `new:`, `a:b:`…) are never
    // class-side methods, so they fall through to (2).
    oop meta = obj_vtable(nested_vtable);
    if (meta != NULL && !IS_SMI(meta)) {
        oop method = NULL;
        lookup_defining_vtable(meta, selector, &method);
        if (method != NULL) {
            return run_method_on(method, bc, argc, args);
        }
    }

    // (2) Construct. Every class has exactly one `init_<sig>` on its own vtable,
    // named from its PARAMETERS — which need NOT match the construction
    // message's keyword (`new: x` builds `init_x:`, and the parser discards the
    // `new:` keyword entirely). The compile-time construction path likewise
    // ignores the message selector and calls the param-derived init, so we find
    // that sole init by its `init_` prefix and invoke it with the call's args.
    oop init_method = NULL;
    struct Vtable *nvt = (struct Vtable *)nested_vtable;
    struct Binding *bs = vtable_bindings((oop)nvt);
    for (size_t i = 0; i < nvt->size; i++) {
        struct Symbol *s = (struct Symbol *)binding_symbol(&bs[i]);
        if (s != NULL && obj_vtable((oop)s) == SymbolVtable &&
            strncmp(symbol_name(s), "init_", 5) == 0) {
            init_method = binding_method(&bs[i]);
            break;
        }
    }

    oop inst = gc_allocate(nested_vtable,
                           sizeof(struct ObjectInstance) + (size_t)slot_count * sizeof(oop));
    struct ObjectInstance *o = (struct ObjectInstance *)inst;
    for (int i = 0; i < slot_count; i++) o->slots[i] = NULL;
    if (slot_count > 0) o->slots[0] = outer;
    if (init_method != NULL) {
        run_method_on(init_method, inst, argc, args);
    }
    return inst;
}

/* Primitive selector ids. PRIM_NONE (0) means "not an intercepted primitive";
   every other id corresponds 1:1 (in order) to an entry in PRIM_NAMES below,
   so the classifier can derive the id directly from the table index. */
enum {
    PRIM_NONE = 0,
    PRIM_Array, PRIM_new, PRIM_at, PRIM_at_put, PRIM_size, PRIM_selector,
    PRIM_vtable, PRIM_delegate, PRIM_bindings, PRIM_sender, PRIM_pc, PRIM_home,
    PRIM_eqeq,
    /* NLR retired (Task 3) + fibers retired (Task 5/6): the retired prim enum
       slots (executeCleanup/resumeWith:/fiberCreate:/fiberSwitch:value:/
       currentFiber) were removed in Task 9a; the enum is now dense and
       prim_ids.h mirrors the platform prims by their new absolute number. */
    PRIM_outer,
    PRIM_registerDebuggerHook, PRIM_regAt, PRIM_regAt_put,
    PRIM_singleStep, PRIM_bind, PRIM_instructionAt_put, PRIM_createClosure,
    PRIM_to_do, PRIM_to_by_do, PRIM_amp, PRIM_pipe, PRIM_isNil, PRIM_notNil,
    PRIM_error, PRIM_lt, PRIM_gt, PRIM_le, PRIM_ge, PRIM_eq, PRIM_ifTrueIfFalse,
    PRIM_ifTrue, PRIM_ifFalse, PRIM_and, PRIM_or, PRIM_not, PRIM_println,
    PRIM_printString, PRIM_asString, PRIM_out, PRIM_do,
    PRIM_intDiv, PRIM_bitXor, PRIM_shl, PRIM_asFloat, PRIM_sqrt,
    PRIM_bitAnd, PRIM_bitOr, PRIM_shr,
    PRIM_boundOuter, PRIM_comma, PRIM_timesRepeat, PRIM_millis,
    PRIM_hash, PRIM_identityHash,
    /* --- std-lib step 1: real indexable + platform capability --- */
    PRIM_ix_array, PRIM_ix_basicAt, PRIM_ix_basicAt_put, PRIM_ix_basicSize,
    PRIM_pf_identicalTo, PRIM_pf_hashcode, PRIM_pf_print, PRIM_pf_printNewline,
    /* --- std-lib step 3b: capability-pure indexable allocation --- */
    PRIM_pf_basicNewIndexable,
    /* --- std-lib step 3c-3a: byte-Array -> String constructor --- */
    PRIM_pf_string,
    /* --- debugger Plan 2 task 4: stdin readline for debug transport --- */
    PRIM_pf_readLine,
    /* --- Milestone B Task 3: RawBytes leaf storage allocation/freeze.
       at:/at:put:/size on a RawBytes receiver reuse the existing generic
       PRIM_at/PRIM_at_put/PRIM_size ids (gated by receiver vtable below) —
       no new enum slots needed for those three. Appended last so no
       pre-existing PRIM id shifts (prim_ids.h mirrors these two by number). */
    PRIM_pf_basicNewBytes,
    PRIM_pf_freezeBytes,
    /* --- Milestone B Task 4: value-algebra type oracle. PRIM_pf_valueRank is
       platform-dispatched (mirrored in prim_ids.h as 72). The six predicates
       are serviced entirely by the builtin-receiver fast-path switch below
       (modeled on PRIM_at/PRIM_isNil) — platform.c never sees them, so they
       need no prim_ids.h mirror. Appended last: no pre-existing PRIM id
       shifts. --- */
    PRIM_pf_valueRank,
    PRIM_isValue, PRIM_isSymbol, PRIM_isList, PRIM_isMap, PRIM_isRecord, PRIM_isBytes,
    /* --- Milestone C Task 1: channel-space C substrate (src/space.c). The
       task brief specified ids 73-76 for these, arrived at from prim_ids.h's
       last MIRRORED id (72, PRIM_pf_valueRank) without accounting for the 6
       value-algebra predicate enum slots directly above (isValue..isBytes,
       Milestone B Task 4), which already occupy ids 73-78 despite having no
       prim_ids.h mirror (platform.c never sees them — see that task's
       comment). True append-only numbering — no existing id may shift, the
       binding constraint — is 79-82; flagged in the Task 1 report. Appended
       last, so no pre-existing PRIM id shifts. --- */
    PRIM_pf_spacePost, PRIM_pf_spaceRegister, PRIM_pf_spaceTake, PRIM_pf_spaceDequeue,
    /* --- Milestone D Task 1: the setjmp crash-boundary primitive. Appended
       last, so no pre-existing PRIM id shifts; counted (not computed) to be
       id 83 — see prim_ids.h. --- */
    PRIM_pf_protectedFire,
    /* --- Milestone D Task 4: the restart-sweep verb. Appended last, so no
       pre-existing PRIM id shifts; counted (not computed) to be id 84 — see
       prim_ids.h. --- */
    PRIM_pf_spaceDropReactionsOwnedBy,
    /* --- Milestone D Task 6: host virtual-clock timer seam. Appended last,
       so no pre-existing PRIM id shifts; counted (not computed) to be ids
       85-89 — see prim_ids.h. --- */
    PRIM_pf_timerAfter, PRIM_pf_timerEvery, PRIM_pf_timerCancel,
    PRIM_pf_monotonicMillis, PRIM_pf_advanceMillis,
    /* --- Milestone E Task 1: the terminate-only raise verb (`aRecord raise`).
       Appended last, so no pre-existing PRIM id shifts; counted (not computed)
       to be id 90 — see prim_ids.h. --- */
    PRIM_raise,
    /* --- Milestone E Task 1: `[text] error: [tag]` tagged-raise. Appended last,
       so no pre-existing PRIM id shifts; counted (not computed) to be id 91 —
       see prim_ids.h. --- */
    PRIM_errorTag,
    /* --- Milestone E Task 2: `[prot] on: [:e|pred] do: [:e|body]` predicate-filtered
       catch. Serviced entirely inside this prim switch (calls vm_on_do directly),
       exactly like the six value-algebra predicate prims (isValue..isBytes) — it is
       NOT a platform prim, so it has NO prim_ids.h mirror. Appended last, so no
       pre-existing PRIM id shifts; counted (not computed) to be id 92 — it is the
       92nd PRIM_NAMES entry. --- */
    PRIM_onDo,
    /* --- Milestone E Task 3: `[prot] ensure: [cleanup]` always-run cleanup.
       Serviced entirely inside this prim switch (calls vm_ensure directly), like
       on:do: and the six value-algebra predicate prims — NOT a platform prim, so
       it has NO prim_ids.h mirror. Appended last, so no pre-existing PRIM id
       shifts; counted (not computed) to be id 93 — the 93rd PRIM_NAMES entry. --- */
    PRIM_ensure,
    /* Zephyr rung-2 sub-project 1: the ISR->channel pulse seam. Appended last —
       no pre-existing id shifts. Ids are the 1-based position in PRIM_NAMES. */
    PRIM_pf_gpioPin,
    PRIM_pf_gpioFire,
    PRIM_pf_pumpEvents,
    /* Rung 3 Task 2: CBOR integer leaf. Encodes an Integer (SMI or
       LargeInteger) as CBOR major 0/1, shortest form. Appended last — no
       pre-existing id shifts. Counted (not computed) to be id 97 — the 97th
       PRIM_NAMES entry. Platform-dispatched (mirrored in prim_ids.h as 97). */
    PRIM_pf_cborEncodeInt,
    /* Rung 3 Task 3: CBOR float leaf. Encodes a Float as CBOR major 7, the
       shortest of f16/f32/f64 that round-trips the double exactly. Appended
       last — no pre-existing id shifts. Counted (not computed) to be id 98 —
       the 98th PRIM_NAMES entry. Platform-dispatched (mirrored in prim_ids.h
       as 98). */
    PRIM_pf_cborEncodeFloat,
    /* Rung 3 Task 4: the four CBOR DECODE leaves — cborReadHead:at: (99),
       cborIntFromMajor:arg: (100), cborReadFloat:at: (101), utf8Valid: (102).
       Appended last, in this order — no pre-existing id shifts. Counted (not
       computed) to be the 99th-102nd PRIM_NAMES entries. Platform-dispatched
       (mirrored in prim_ids.h as 99-102). Used by the Task 6 decode walk. */
    PRIM_pf_cborReadHead,
    PRIM_pf_cborIntFromMajor,
    PRIM_pf_cborReadFloat,
    PRIM_pf_utf8Valid,
    /* Rung 3 Task 6: String->Symbol leaf. Rebuilds a genuine (non-interned)
       Symbol from a String's bytes for CBOR tag-39 decode. Appended last — no
       pre-existing id shifts. Counted (not computed) to be id 103 — the 103rd
       PRIM_NAMES entry. Platform-dispatched (mirrored in prim_ids.h as 103). */
    PRIM_pf_asSymbol,
    /* Rung 4 (on-target measurement) Task 1: heap-telemetry leaf. Snapshots
       oevm_heap_stats() into a 5-slot Array of SMIs. Appended last — no
       pre-existing id shifts. Counted (not computed) to be id 104 — the 104th
       PRIM_NAMES entry. Platform-dispatched (mirrored in prim_ids.h as 104). */
    PRIM_pf_heapStats,
    /* Rung 9 (cells — tuvm#32 option A) Task 2: the cell seam. A channel is a
       STREAM (post/take) or a CELL (put/peek); the kind is fixed by the first
       write. These three are storage leaves — the kind RULE is enforced in nsl
       (Channel>>put:value: / peek:ifAbsent: raise a catchable condition), the
       same division as checkPath:. Appended last — no pre-existing id shifts.
       Counted (not computed) to be 105/106/107 — the 105th/106th/107th
       PRIM_NAMES entries. Platform-dispatched (mirrored in prim_ids.h). */
    PRIM_pf_spaceKind,
    PRIM_pf_spacePeek,
    PRIM_pf_spacePut,
    /* tuvm#20: overflow-shed post counter — the 108th entry (counted). */
    PRIM_pf_spaceOverruns,
    /* tuvm#24: sys-telemetry leaf — the 109th entry (counted). */
    PRIM_pf_sysStats,
    /* webserver-channel-device spike, Task 3: the http driver's platform prims —
       appended after sysStats, no pre-existing id shifts. Counted (not computed)
       to be the 110th/111th/112th PRIM_NAMES entries. Platform-dispatched
       (mirrored in prim_ids.h as 110-112). */
    PRIM_pf_httpListen,
    PRIM_pf_httpRespond,
    PRIM_pf_httpInject,
};

/* Must stay parallel to the enum above (index i -> id i+1). */
static const char *const PRIM_NAMES[] = {
    "Array", "new:", "at:", "at:put:", "size", "selector", "vtable", "delegate",
    "bindings", "sender", "pc", "home", "==",
    "outer", /* executeCleanup/resumeWith:/fiberCreate:/fiberSwitch:value:/currentFiber removed (Task 9a) */
    "registerDebuggerHook:",
    "regAt:", "regAt:put:", "singleStep:", "bind:to:", "instructionAt:put:",
    "createClosure:", "to:do:", "to:by:do:", "&", "|", "isNil", "notNil",
    "error:", "<", ">", "<=", ">=", "=", "ifTrue:ifFalse:", "ifTrue:",
    "ifFalse:", "and:", "or:", "not", "println", "printString", "asString",
    "out", "do:", "//", "bitXor:", "<<", "asFloat", "sqrt",
    "bitAnd:", "bitOr:", ">>",
    "__boundOuter:slotCount:", ",", "timesRepeat:", "millis",
    "hash", "identityHash",
    "array:", "basicAt:", "basicAt:put:", "basicSize",
    "object:identicalTo:", "objHashcode:", "print:", "printNewline:",
    "basicNew:indexable:",
    "string:",
    "readLine",
    "basicNewBytes:",
    "freezeBytes:",
    "valueRank:",
    "isValue", "isSymbol", "isList", "isMap", "isRecord", "isBytes",
    "spacePost:value:", "spaceRegister:block:owner:", "spaceTake:", "spaceDequeue",
    "protectedFire:value:",
    "spaceDropReactionsOwnedBy:",
    "timerAfter:token:", "timerEvery:token:", "timerCancel:",
    "monotonicMillis", "advanceMillis:",
    "raise",
    "error:tag:",
    "on:do:",
    "ensure:",
    "gpioPin:edge:post:on:",
    "gpioFire:",
    "pumpEvents",
    "cborEncodeInt:",
    "cborEncodeFloat:",
    "cborReadHead:at:",
    "cborIntFromMajor:arg:",
    "cborReadFloat:at:",
    "utf8Valid:",
    "asSymbol:",
    "heapStats",
    "spaceKind:", "spacePeek:", "spacePut:value:",
    "spaceOverruns:",
    "sysStats",
    "httpListen:", "httpRespondId:status:body:", "httpInject:path:",
};
#define N_PRIM_NAMES (sizeof(PRIM_NAMES) / sizeof(PRIM_NAMES[0]))

/* Pointer-keyed cache mapping each interned selector symbol to its primitive id
   (PRIM_NONE if it is not an intercepted primitive). Selectors are interned, so
   each unique name is one stable pointer; we strcmp-classify it once and
   remember the id. Open-addressed, power-of-two size, single-threaded
   (cooperative fibers) so no locking. */
#ifndef PRIM_CACHE_SIZE
#define PRIM_CACHE_SIZE 4096   /* power of two; host default, -D lower for embedded */
#endif
static struct { oop sym; int pid; } prim_cache[PRIM_CACHE_SIZE];

static int selector_prim_id(struct Symbol *sym) {
    uintptr_t h = ((uintptr_t)sym >> 4) & (PRIM_CACHE_SIZE - 1);
    while (prim_cache[h].sym != NULL) {
        if (prim_cache[h].sym == (oop)sym) return prim_cache[h].pid;
        h = (h + 1) & (PRIM_CACHE_SIZE - 1);
    }
    int pid = PRIM_NONE;
    for (size_t i = 0; i < N_PRIM_NAMES; i++) {
        if (strcmp(symbol_name(sym), PRIM_NAMES[i]) == 0) { pid = (int)i + 1; break; }
    }
    prim_cache[h].sym = (oop)sym;
    prim_cache[h].pid = pid;
    return pid;
}

/* Shared helper: allocate a BytecodeClosure capturing the running frame.
   Used by both PRIM_createClosure (reflective message path, still reachable)
   and OP_CLOSURE (new direct opcode path). */
oop make_closure(oop method) {
    extern struct MethodContext *current_frame;
    extern oop *vm_stack;
    extern oop gc_allocate(oop vtable, size_t size);
    oop cl = gc_allocate(BytecodeClosureVtable, sizeof(struct Closure));
    /* Capture the enclosing frame as a relative home_ctx (growth-ready). NULL when
       there is no current frame, preserving the old (oop)NULL no-home result. */
    ((struct Closure *)cl)->home_ctx =
        current_frame ? home_link_encode_relative(vm_stack, current_frame) : NULL;
    ((struct Closure *)cl)->home_generation = current_frame ? current_frame->gen_tag.tag32 : 0;
    ((struct Closure *)cl)->entry_point = (void *)method;
    ((struct Closure *)cl)->captured_env = NULL;  /* set by the OP_CLOSURE path when the creating method has a heap Context */
    /* De-recursify Phase 2 (self-migration, Approach B): snapshot the home method's
       receiver. A block's self IS its home method's receiver (shared across a method
       and all its nested blocks), so compute it O(1): a method creating-frame's
       receiver is regs[0]; a block creating-frame propagates its own captured_self.
       Same-fiber at creation, so valid regardless of home_ctx encoding. */
    {
        struct Closure *ccl = current_frame ? (struct Closure *)current_frame->closure : NULL;
        oop self_val;
        if (ccl == NULL)                self_val = NULL;                    /* no frame (boot) */
        else if (ccl->home_ctx == NULL) self_val = current_frame->regs[0]; /* method frame */
        else                            self_val = ccl->captured_self;     /* block frame: propagate */
        ((struct Closure *)cl)->captured_self = self_val;
    }
    return cl;
}

/* Allocate an immutable String with inline UTF-8 bytes (std-lib 3c-1). */
static oop make_string(const char *bytes, size_t len) {
    extern oop gc_allocate(oop vtable, size_t size);
    oop s = gc_allocate(StringVtable, sizeof(struct String) + len);
    ((struct String *)s)->length = len;
    if (len > 0) memcpy(((struct String *)s)->bytes, bytes, len);
    return s;
}

/* Milestone E Task 1 (v2): build the in-flight crash Record via the captured
   Kernel module instance (g_kernel_instance, stashed in send_resolve at the first
   main:args: send) and raise it. The Kernel instance is the ONLY object that
   reaches the `Record` class from any context — an app receiver cannot (the v1
   BLOCKED finding) — so we send it the public accessor `crashRecordTag:text:`,
   which answers `Record tag: aTag with: #text -> aText`. Then vm_raise longjmps to
   the active protection frame (or, unprotected, prints + aborts). `tag` and `text`
   are temp-rooted across the interned-selector alloc-free walk and the re-entrant
   send (GC bug class); the returned Record needs no root — no allocation happens
   between the send returning and vm_raise stashing it in g_raised_condition (a GC
   root). The interned crashRecordTag:text: selector is found by walking the kernel
   instance's vtable (string-compare, does_not_understand idiom) so the send matches
   the binding by identity. Fallbacks (pre-capture / helper missing — neither should
   occur) raise the bare text, preserving a sane terminate. */
static oop build_and_raise(oop tag, oop text) {
    extern void gc_temp_root_push(oop o);
    extern int gc_temp_roots_count;
    if (g_kernel_instance == NULL) {
        vm_raise(text);   /* pre-entry: impossible in practice, but keep terminate */
        return NULL;
    }
    oop sel = NULL;
    {
        oop curr = obj_vtable(g_kernel_instance);
        while (curr != NULL && !IS_SMI(curr)) {
            struct Vtable *vt = (struct Vtable *)curr;
            struct Binding *bs = vtable_bindings((oop)vt);
            for (size_t i = 0; i < vt->size; i++) {
                struct Symbol *s = (struct Symbol *)binding_symbol(&bs[i]);
                if (s != NULL && obj_vtable((oop)s) == SymbolVtable &&
                    strcmp(symbol_name(s), "crashRecordTag:text:") == 0) {
                    sel = (oop)s;
                    break;
                }
            }
            if (sel != NULL) break;
            curr = vtable_delegate((oop)vt);
        }
    }
    if (sel == NULL) {
        vm_raise(text);   /* kernel helper missing: keep terminate */
        return NULL;
    }
    int saved_roots = gc_temp_roots_count;
    gc_temp_root_push(tag);
    gc_temp_root_push(text);
    oop args2[2] = { tag, text };   /* crashRecordTag: aTag text: aText */
    oop rec = send(g_kernel_instance, sel, 2, args2);
    gc_temp_roots_count = saved_roots;
    vm_raise(rec);   /* longjmps when protected; never returns */
    return NULL;
}

/* Milestone E F1: build + raise a crash Record for a bare `self error: text` — the
   default tag is the image-INTERNED `#error`, supplied by the kernel helper
   `crashRecordText:` (which answers `Record tag: #error with: #text -> aText`). The
   v1 PRIM_error minted a FRESH non-interned Symbol as the tag, so the spec §3/§6
   idiom `[ :e | e tag = #error ]` (Symbol `=` is pointer-identity) never matched and
   the error silently propagated past the handler. Routing through the kernel literal
   interns the tag to the same oop an app `#error` literal resolves to. Same rooting
   discipline and fallbacks as build_and_raise: `text` is temp-rooted across the
   interned-selector alloc-free vtable walk and the re-entrant send; the returned
   Record needs no root (no allocation between the send and vm_raise stashing it in
   the g_raised_condition GC root). Fallbacks (pre-capture / helper missing — neither
   should occur) raise the bare text, preserving a sane terminate. */
static oop build_and_raise_default(oop text) {
    extern void gc_temp_root_push(oop o);
    extern int gc_temp_roots_count;
    if (g_kernel_instance == NULL) {
        vm_raise(text);   /* pre-entry: impossible in practice, but keep terminate */
        return NULL;
    }
    oop sel = NULL;
    {
        oop curr = obj_vtable(g_kernel_instance);
        while (curr != NULL && !IS_SMI(curr)) {
            struct Vtable *vt = (struct Vtable *)curr;
            struct Binding *bs = vtable_bindings((oop)vt);
            for (size_t i = 0; i < vt->size; i++) {
                struct Symbol *s = (struct Symbol *)binding_symbol(&bs[i]);
                if (s != NULL && obj_vtable((oop)s) == SymbolVtable &&
                    strcmp(symbol_name(s), "crashRecordText:") == 0) {
                    sel = (oop)s;
                    break;
                }
            }
            if (sel != NULL) break;
            curr = vtable_delegate((oop)vt);
        }
    }
    if (sel == NULL) {
        vm_raise(text);   /* kernel helper missing: keep terminate */
        return NULL;
    }
    int saved_roots = gc_temp_roots_count;
    gc_temp_root_push(text);
    oop args1[1] = { text };   /* crashRecordText: aText */
    oop rec = send(g_kernel_instance, sel, 1, args1);
    gc_temp_roots_count = saved_roots;
    vm_raise(rec);   /* longjmps when protected; never returns */
    return NULL;
}

/* Resolve a send WITHOUT invoking a bytecode target (de-recursify Phase 2, Task 4).
   This is the former body of send(): it runs every inline case (intercepted
   primitives, native capabilities, BoundClass construction, doesNotUnderstand:,
   access violations, primitive closures) and returns that value with *out_method
   left NULL. The TWO cases that formerly recursed into run_interpreter — a
   `value`-family reflective block invoke on a BytecodeClosure, and the final
   vtable-resolved bytecode method — instead set the out-params (method, recv,
   argc) and return NULL, so the caller can push a frame (flat loop) or re-enter
   run_interpreter (the send() wrapper). The returned oop is meaningful only when
   out method is NULL. */
OEVM_HOT oop send_resolve(oop receiver, oop selector, int argc, oop *args,
                 oop *out_method, oop *out_recv, int *out_argc) {
    *out_method = NULL;
    /* Milestone E Task 1: capture the Kernel module instance once, at the first
       `main:args:` send. Both boot paths (oimg_loader.c and the solidified
       run_solidified_entry_point) launch the app as `entry main: kernel_inst
       args: 0`, so arg 0 IS the Kernel instance — the only object that reaches
       the `Record` class from any context. The error:/error:tag: raise prims
       need it to build the crash Record (an app receiver cannot reach Record
       lexically — the v1 BLOCKED finding). The `== NULL` guard makes this a
       one-shot; it runs before any app code, so g_kernel_instance is always set
       before any error can fire. */
    if (g_kernel_instance == NULL && selector != NULL && !IS_SMI(selector) &&
        SymbolVtable != NULL && obj_vtable(selector) == SymbolVtable && argc >= 1 &&
        strcmp(symbol_name((struct Symbol *)selector), "main:args:") == 0) {
        g_kernel_instance = args[0];
    }
    // BoundClass receiver: ANY send constructs the nested instance (see
    // BoundClassVtable). Must precede primitive interception so a construction
    // selector that happens to collide with a primitive name (e.g. `size`) still
    // constructs rather than running the primitive.
    if (receiver != NULL && !IS_SMI(receiver) && boundclass_vtable_initialized &&
        obj_vtable(receiver) == (oop)&BoundClassVtable) {
        return boundclass_construct(receiver, selector, argc, args);
    }
    /* Native capabilities: serial / net-UDP / mirror. Receiver-identity gates;
       each does its own selector matching, adding nothing to PRIM_NAMES
       (std-lib 3c-3b, Task 2, mirror step). The three singletons live at fixed
       static addresses, so resolve them once and gate on cached pointers —
       the *_object() accessors are cross-TU calls that sat on every send. */
    {
        static oop cap_serial, cap_udp, cap_mirror;
        if (cap_serial == NULL) {
            cap_serial = serial_object();
            cap_udp = udp_object();
            cap_mirror = mirror_object();
        }
        if (receiver == cap_serial) return serial_dispatch(selector, argc, args);
        if (receiver == cap_udp) return udp_dispatch(selector, argc, args);
        if (receiver == cap_mirror) return mirror_dispatch(selector, argc, args);
    }
#ifdef OEVM_HANOI_NATIVE
    /* Experiment: acquisition + lazy wiring of the native Hanoi method. Sent to the
       HanoiAB instance (self); returns the native receiver. strcmp-gated so it needs
       no PRIM_NAMES id (mirrors the crashRecordTag: interception below). */
    if (selector != NULL && !IS_SMI(selector) && SymbolVtable != NULL &&
        obj_vtable(selector) == SymbolVtable &&
        strcmp(symbol_name((struct Symbol *)selector), "installNativeHanoi") == 0) {
        extern oop hanoi_native_acquire(oop);
        return hanoi_native_acquire(receiver);
    }
#endif
    // Intercept reflection & Array primitives
    if (selector != NULL && !IS_SMI(selector)) {
        struct Symbol *sym = (struct Symbol *)selector;
        int pid;
        if (SymbolVtable != NULL && obj_vtable((oop)sym) == SymbolVtable &&
            (pid = selector_prim_id(sym)) != PRIM_NONE) {
            // --- std-lib step 1: real indexable raw accessors. Distinct
            //     selectors (basic*) from the mock's at:/size, so the
            //     conformance mock is untouched. Fires only for the real
            //     IndexableVtable receiver. ---
            // std-lib step 3b: indexables are vtable-agnostic now (no kernel_array_vtable
            // global). basicAt:/basicAt:put:/basicSize are only emitted by kernel code
            // on receivers it knows are Arrays — Array methods on self, and (perf/
            // op-cmp-at) collection internals on their Array-typed slots (e.g.
            // OrderedCollection's `contents`). Every indexable has slot0 =
            // TAG_SMI(length). Gate on the pid + that shape. The compiler lowers
            // basicAt:/basicAt:put: sends to OP_AT/OP_AT_PUT (vm.c), which use this
            // same shape gate and fall back into OP_SEND (reaching here) on a miss.
            if (receiver != NULL && !IS_SMI(receiver) &&
                (pid == PRIM_ix_basicAt || pid == PRIM_ix_basicAt_put || pid == PRIM_ix_basicSize) &&
                IS_SMI(instance_slot((struct ObjectInstance *)receiver, 0))) {
                struct ObjectInstance *ix = (struct ObjectInstance *)receiver;
                intptr_t n = UNTAG_SMI(instance_slot(ix, 0));
                if (pid == PRIM_ix_basicSize) {
                    return instance_slot(ix, 0);
                }
                if (pid == PRIM_ix_basicAt) {
                    intptr_t idx = IS_SMI(args[0]) ? UNTAG_SMI(args[0]) : 0;
                    if (idx < 1 || idx > n) {
                        fprintf(stderr, "VM Error: basicAt: index %ld out of bounds (size %ld)\n",
                                (long)idx, (long)n);
                        abort();
                    }
                    return instance_slot(ix, (size_t)idx);
                }
                if (pid == PRIM_ix_basicAt_put) {
                    intptr_t idx = IS_SMI(args[0]) ? UNTAG_SMI(args[0]) : 0;
                    if (idx < 1 || idx > n) {
                        fprintf(stderr, "VM Error: basicAt:put: index %ld out of bounds (size %ld)\n",
                                (long)idx, (long)n);
                        abort();
                    }
                    ix->slots[idx] = args[1];
                    return args[1];
                }
            }
            /* Platform capability now lives in platform.c (std-lib 3c-3b). */
            if (receiver == platform_object()) {
                oop out;
                if (platform_dispatch(pid, argc, args, &out)) return out;
            }
            // Intercept `selector` on a reified doesNotUnderstand: message.
            if (receiver != NULL && !IS_SMI(receiver) &&
                obj_vtable(receiver) == (oop)&MessageMockVtable &&
                pid == PRIM_selector) {
                return instance_slot((struct ObjectInstance *)receiver, 0);
            }
            /* Reflection is confined to the `mirror` capability (mirror step).
               The 12 reflective cases (vtable/delegate/bindings/sender/pc/home/
               regAt:/regAt:put:/singleStep:/bind:to:/instructionAt:put:/
               registerDebuggerHook:) were lifted out of this switch into
               mirror.c; an ordinary receiver sending them now falls through to
               normal vtable dispatch -> DNU. See tools/check_no_ambient_reflection.sh. */
            switch (pid) {
            case PRIM_eqeq: {
                return primitive_object_equals(receiver, argc, args);
            }
            break;
            /* PRIM_fiberCreate / PRIM_fiberSwitch / PRIM_currentFiber cases deleted
               (Milestone A Task 5); enum slots reserved above. */
            case PRIM_outer: {
                return primitive_object_outer(receiver, argc, args);
            }
            break;
            case PRIM_createClosure: {
                return make_closure(args[0]);
            }
            break;
            // PRIM_to_do / PRIM_to_by_do deleted (Phase 1 de-recursify, Task 3):
            // the compiler now ALWAYS lowers `to:do:`/`to:by:do:` to a counted
            // loop driving OP_BLOCK_INVOKE (ns_compile_oevm.py
            // compile_to_do_inline / compile_to_by_do_runtime / emit_block_value),
            // so a compiled send never reaches this switch. Both formerly
            // invoked the body block via a recursive run_interpreter per
            // iteration — exactly the recursive block-invoke primitive Phase 1
            // removes. A reflective `perform: #to:do:`/`#to:by:do:` now falls
            // through to does_not_understand; ns-tests has no such send.
            case PRIM_amp: {
                // Bitwise-and on integers, boolean-and otherwise. `&` is
                // overloaded: SmallInteger>>& is bitwise (e.g. richards'
                // `control & 1`), Boolean>>& is non-short-circuit conjunction.
                if (IS_SMI(receiver) && IS_SMI(args[0])) {
                    return TAG_SMI(UNTAG_SMI(receiver) & UNTAG_SMI(args[0]));
                }
                return (is_truthy(receiver) && is_truthy(args[0])) ? true_symbol : false_symbol;
            }
            break;
            case PRIM_pipe: {
                if (IS_SMI(receiver) && IS_SMI(args[0])) {
                    return TAG_SMI(UNTAG_SMI(receiver) | UNTAG_SMI(args[0]));
                }
                return (is_truthy(receiver) || is_truthy(args[0])) ? true_symbol : false_symbol;
            }
            break;
            case PRIM_intDiv: {
                // SmallInteger>>// : floored integer division (rounds toward
                // negative infinity, unlike C's truncating /). Handles SMI and
                // LargeInteger operands uniformly.
                if (oevm_is_integer(receiver) && oevm_is_integer(args[0])) {
                    return oevm_int_floordiv(receiver, args[0]);
                }
                return TAG_SMI(0);
            }
            break;
            case PRIM_bitXor: {
                if (IS_SMI(receiver) && IS_SMI(args[0])) {
                    return TAG_SMI(UNTAG_SMI(receiver) ^ UNTAG_SMI(args[0]));
                }
                return TAG_SMI(0);
            }
            break;
            case PRIM_shl: {
                if (IS_SMI(receiver) && IS_SMI(args[0])) {
                    return TAG_SMI(UNTAG_SMI(receiver) << UNTAG_SMI(args[0]));
                }
                return TAG_SMI(0);
            }
            break;
            case PRIM_bitAnd: {
                if (IS_SMI(receiver) && IS_SMI(args[0])) {
                    return TAG_SMI(UNTAG_SMI(receiver) & UNTAG_SMI(args[0]));
                }
                return TAG_SMI(0);
            }
            break;
            case PRIM_bitOr: {
                if (IS_SMI(receiver) && IS_SMI(args[0])) {
                    return TAG_SMI(UNTAG_SMI(receiver) | UNTAG_SMI(args[0]));
                }
                return TAG_SMI(0);
            }
            break;
            case PRIM_shr: {
                if (IS_SMI(receiver) && IS_SMI(args[0])) {
                    return TAG_SMI(UNTAG_SMI(receiver) >> UNTAG_SMI(args[0]));
                }
                return TAG_SMI(0);
            }
            break;
            case PRIM_asFloat: {
                // Integer -> boxed Float; Float -> itself (still boxed fresh).
                extern oop gc_allocate(oop vtable, size_t size);
                double v = IS_SMI(receiver) ? (double)UNTAG_SMI(receiver)
                    : ((receiver != NULL && obj_vtable(receiver) == FloatVtable) ? ((struct Float *)receiver)->value : 0.0);
                struct Float *f = (struct Float *)float_alloc(FloatVtable);
                f->value = v;
                return (oop)f;
            }
            break;
            case PRIM_sqrt: {
                // __builtin_sqrt emits the hardware sqrt inline, so we avoid a
                // libm (-lm) link dependency the test runner does not pass.
                extern oop gc_allocate(oop vtable, size_t size);
                double v = IS_SMI(receiver) ? (double)UNTAG_SMI(receiver)
                    : ((receiver != NULL && obj_vtable(receiver) == FloatVtable) ? ((struct Float *)receiver)->value : 0.0);
                struct Float *f = (struct Float *)float_alloc(FloatVtable);
                f->value = __builtin_sqrt(v);
                return (oop)f;
            }
            break;
            case PRIM_millis: {
                /* In-language monotonic clock in ms. Receiver ignored; backed by
                   the overridable oevm_millis hook. Returns a SmallInteger. */
                return TAG_SMI((long)oevm_millis());
            }
            break;
            case PRIM_boundOuter: {
                // Build a BoundClass {nested_vtable=receiver, outer=args[0],
                // slot_count=args[1]}. Emitted by synthetic nested-class
                // accessors; consumed by the BoundClass intercept in send().
                extern oop gc_allocate(oop vtable, size_t size);
                oop bcvt = ensure_boundclass_vtable();
                oop bc = gc_allocate(bcvt, sizeof(struct ObjectInstance) + 3 * sizeof(oop));
                struct ObjectInstance *bci = (struct ObjectInstance *)bc;
                bci->slots[0] = receiver;   // nested_vtable
                bci->slots[1] = args[0];    // outer (enclosing instance)
                bci->slots[2] = args[1];    // SMI: slot_count
                return bc;
            }
            break;
            case PRIM_identityHash: {
                if (IS_SMI(receiver)) return receiver;
                if (receiver == NULL) return TAG_SMI(0);
                return TAG_SMI((intptr_t)(((uintptr_t)receiver) >> 4));
            }
            break;
            case PRIM_hash: {
                if (IS_SMI(receiver)) return receiver;            /* SMI hashes to itself */
                if (receiver == NULL) return TAG_SMI(1);          /* nil: fixed constant */
                if (StringVtable != NULL && obj_vtable(receiver) == StringVtable) {
                    struct String *s = (struct String *)receiver; /* content hash (FNV-1a) */
                    uint32_t h = 2166136261u;
                    for (size_t i = 0; i < s->length; i++) { h ^= s->bytes[i]; h *= 16777619u; }
                    return TAG_SMI((intptr_t)(h & 0x3FFFFFFF));
                }
                if (SymbolVtable != NULL && obj_vtable(receiver) == SymbolVtable)
                    return TAG_SMI((intptr_t)(((uintptr_t)receiver) >> 4)); /* interned: identity */
                if (FloatVtable != NULL && obj_vtable(receiver) == FloatVtable)
                    return TAG_SMI((intptr_t)((long)((struct Float *)receiver)->value) & 0x3FFFFFFF);
                if (LargeIntegerVtable != NULL && obj_vtable(receiver) == LargeIntegerVtable)
                    return TAG_SMI((intptr_t)(((struct LargeInteger *)receiver)->mag) & 0x3FFFFFFF);
                break;   /* heap object: fall through to Kernel.Object>>hash (overridable) */
            }
            break;
            case PRIM_isNil: {
                if (receiver == NULL) return true_symbol;
                if (IS_SMI(receiver)) return false_symbol;
                if ((StringVtable != NULL && obj_vtable(receiver) == StringVtable) ||
                    obj_vtable(receiver) == SymbolVtable ||
                    obj_vtable(receiver) == FloatVtable ||
                    obj_vtable(receiver) == LargeIntegerVtable) return false_symbol;
                // std-lib step 3a: real heap object — fall through to Object.nsl isNil
                break;
            }
            break;
            case PRIM_notNil: {
                if (receiver == NULL) return false_symbol;
                if (IS_SMI(receiver)) return true_symbol;
                if ((StringVtable != NULL && obj_vtable(receiver) == StringVtable) ||
                    obj_vtable(receiver) == SymbolVtable ||
                    obj_vtable(receiver) == FloatVtable ||
                    obj_vtable(receiver) == LargeIntegerVtable) return true_symbol;
                // std-lib step 3a: real heap object — fall through to Object.nsl notNil
                break;
            }
            break;
            case PRIM_error: {
                /* Milestone E Task 1 (v2) / F1: `self error: text` raises a crash
                   Record whose default tag is the image-INTERNED `#error`, supplied
                   by the kernel helper `crashRecordText:` (build_and_raise_default).
                   The Record is built in C via the captured Kernel instance — an app
                   receiver cannot reach the `Record` class lexically (the v1 BLOCKED
                   finding). Using the kernel `#error` literal interns the tag to the
                   same oop an app `[ :e | e tag = #error ]` predicate resolves to, so
                   identity-`=` matches (F1: the v1 fresh-Symbol mint never did). */
                return build_and_raise_default(args[0]);   /* normally longjmps */
            }
            break;
            case PRIM_errorTag: {
                /* Milestone E Task 1 (v2): `text error: tag` — the tagged-raise verb.
                   receiver/args[0] = text, args[1] = tag. Builds the crash Record via
                   the captured Kernel instance and raises it. */
                return build_and_raise(args[1], args[0]);   /* normally longjmps */
            }
            break;
            case PRIM_raise: {
                /* Milestone E Task 1: terminate-only re-raise of the receiver oop
                   (typically a crash Record). vm_raise longjmps to the active
                   protection frame with the receiver as the in-flight condition, or
                   (unprotected) prints + aborts. Never returns when protected. */
                vm_raise(receiver);
                return NULL;         /* unreachable when protected */
            }
            break;
            case PRIM_onDo: {
                /* Milestone E Task 2: `[prot] on: [:e|pred] do: [:e|body]`.
                   receiver = the protected block, args[0] = predicate block,
                   args[1] = handler block. vm_on_do runs prot protected; on a
                   caught fault it evaluates pred against the condition and either
                   runs handler (match) or re-raises to the next outer frame. */
                return vm_on_do(receiver, args[0], args[1]);
            }
            break;
            case PRIM_ensure: {
                /* Milestone E Task 3: `[prot] ensure: [cleanup]`.
                   receiver = the protected block, args[0] = cleanup block.
                   vm_ensure runs prot protected; on clean completion it runs
                   cleanup and returns the protected value; on a caught fault it
                   runs cleanup and re-raises to the next outer frame. */
                return vm_ensure(receiver, args[0]);
            }
            break;
            case PRIM_lt:
                return (prim_compare(receiver, args[0]) <  0) ? true_symbol : false_symbol;
            case PRIM_gt:
                return (prim_compare(receiver, args[0]) >  0) ? true_symbol : false_symbol;
            case PRIM_le:
                return (prim_compare(receiver, args[0]) <= 0) ? true_symbol : false_symbol;
            case PRIM_ge:
                return (prim_compare(receiver, args[0]) >= 0) ? true_symbol : false_symbol;
            case PRIM_eq: {
                if (receiver != NULL && !IS_SMI(receiver) && StringVtable != NULL &&
                    obj_vtable(receiver) == StringVtable) {
                    oop o = args[0];
                    // Identity fast path: equal string literals are deduplicated to
                    // one object in the constant pool (compiler add_string_constant),
                    // so the common case (comparing the same pooled literal) is O(1).
                    // Reflexivity holds for strings (no NaN-style x!=x), so this is
                    // always correct, not just an optimization for the pooled case.
                    if (receiver == o) return true_symbol;
                    if (o == NULL || IS_SMI(o) || obj_vtable(o) != StringVtable) return false_symbol;
                    struct String *a = (struct String *)receiver, *b = (struct String *)o;
                    if (a->length != b->length) return false_symbol;
                    return (memcmp(a->bytes, b->bytes, a->length) == 0) ? true_symbol : false_symbol;
                }
                if (IS_SMI(receiver) && IS_SMI(args[0])) {
                    return (receiver == args[0]) ? true_symbol : false_symbol;
                }
                // Exact value equality for integers (SMI and/or LargeInteger),
                // via sign+magnitude (prim_compare) — never through double.
                // Without this, two distinct LargeInteger boxes of equal value
                // fell through to identity below and compared UNEQUAL, which
                // breaks Dictionary keys the moment a value crosses the SMI
                // boundary (and `<`/`>` already compare large ints by value, so
                // `=` disagreeing with them was an internal inconsistency).
                if (oevm_is_integer(receiver) && oevm_is_integer(args[0])) {
                    return (prim_compare(receiver, args[0]) == 0) ? true_symbol : false_symbol;
                }
                // Mixed number equality only when BOTH sides are numbers
                // (so 1 = 1.0 holds). Previously a non-numeric operand fell
                // through to 0.0, making every pair of heap objects — and
                // `nil = anyObject` — compare equal. For non-numeric operands
                // fall back to identity, the default Object>>= semantics. A
                // LargeInteger vs Float comparison goes through double, matching
                // prim_compare's own mixed-operand fallback.
                int r_num = oevm_is_integer(receiver) || (receiver != NULL && obj_vtable(receiver) == FloatVtable);
                int a_num = oevm_is_integer(args[0]) || (args[0] != NULL && obj_vtable(args[0]) == FloatVtable);
                if (r_num && a_num) {
                    double v1 = IS_SMI(receiver) ? (double)UNTAG_SMI(receiver)
                              : (obj_vtable(receiver) == FloatVtable ? ((struct Float *)receiver)->value : oevm_int_to_double(receiver));
                    double v2 = IS_SMI(args[0]) ? (double)UNTAG_SMI(args[0])
                              : (obj_vtable(args[0]) == FloatVtable ? ((struct Float *)args[0])->value : oevm_int_to_double(args[0]));
                    return (v1 == v2) ? true_symbol : false_symbol;
                }
                /* Milestone B Task 7 (David-approved 2026-07-05): BUILTIN
                   receivers keep C identity semantics here — nil and SMI
                   (SMI/SMI and all-numeric pairs already returned above;
                   an SMI vs non-number lands here), Float/LargeInteger vs a
                   non-number, Symbol (interned, and a builtin receiver cannot
                   reach nsl methods — no delegation — so falling through
                   would DNU), and RawBytes (internal, never a value). String
                   receivers always returned in the content-equality branch
                   above. A REAL heap object instead breaks to normal vtable
                   dispatch so a heap class's `=` override (kernel List, later
                   Map/Record) actually runs; Object>>= (`^ self == other`,
                   kernel.nsl) preserves the identity default for heap classes
                   without a custom `=`. Modeled on PRIM_isNil/PRIM_hash's
                   heap fall-through in this same switch. */
                if (receiver == NULL || IS_SMI(receiver) ||
                    (FloatVtable != NULL && obj_vtable(receiver) == FloatVtable) ||
                    (LargeIntegerVtable != NULL && obj_vtable(receiver) == LargeIntegerVtable) ||
                    (SymbolVtable != NULL && obj_vtable(receiver) == SymbolVtable) ||
                    (RawBytesVtable != NULL && obj_vtable(receiver) == RawBytesVtable))
                    return (receiver == args[0]) ? true_symbol : false_symbol;
                break;   /* heap object: fall through to vtable `=` (overridable) */
            }
            break;
            // PRIM_ifTrueIfFalse / PRIM_ifTrue / PRIM_ifFalse deleted (Phase 1
            // de-recursify, Task 2): the compiler now ALWAYS lowers the
            // if-family to branch + OP_BLOCK_INVOKE (ns_compile_oevm.py
            // compile_if_inline / emit_block_value), so a compiled `ifTrue:`
            // etc. send never reaches this switch. A reflective `perform:` of
            // one of these selectors now falls through to normal vtable
            // dispatch -> does_not_understand (no ambient recursive primitive
            // left to catch it); a repo-wide grep of ns-tests found no such
            // reflective sends.
            // PRIM_and / PRIM_or deleted (Phase 1 de-recursify): the compiler now
            // ALWAYS lowers `and:`/`or:` to short-circuit branch + OP_BLOCK_INVOKE
            // (compile_logic_inline / emit_block_value), so a compiled send never
            // reaches this switch. Both formerly invoked the arm block via a
            // recursive run_interpreter — exactly the recursive block-invoke
            // primitive Phase 1 removes. A reflective `perform: #and:`/`#or:` now
            // falls through to does_not_understand; ns-tests has no such send.
            case PRIM_not: {
                return is_truthy(receiver) ? false_symbol : true_symbol;
            }
            break;
            case PRIM_println: {
                if (IS_SMI(receiver)) {
                    printf("%ld\n", (long)UNTAG_SMI(receiver));
                } else if (receiver != NULL && obj_vtable(receiver) == FloatVtable) {
                    printf("%g\n", ((struct Float *)receiver)->value);
                } else if (receiver != NULL && StringVtable != NULL && obj_vtable(receiver) == StringVtable) {
                    struct String *s = (struct String *)receiver;
                    fwrite(s->bytes, 1, s->length, stdout);
                    fputc('\n', stdout);
                } else if (receiver != NULL && obj_vtable(receiver) == SymbolVtable) {
                    printf("%s\n", symbol_name((struct Symbol *)receiver));
                } else if (receiver == NULL) {
                    printf("nil\n");
                } else {
                    // std-lib step 3a: a real heap object's println is served
                    // by Object.nsl (via printString) — fall through to vtable method lookup.
                    break;
                }
                platform_console_flush();
                return receiver;
            }
            break;
            case PRIM_printString: {
                char buf[128];
                if (IS_SMI(receiver)) {
                    snprintf(buf, sizeof(buf), "%ld", (long)UNTAG_SMI(receiver));
                } else if (receiver != NULL && obj_vtable(receiver) == FloatVtable) {
                    snprintf(buf, sizeof(buf), "%g", ((struct Float *)receiver)->value);
                } else if (receiver != NULL && obj_vtable(receiver) == LargeIntegerVtable) {
                    struct LargeInteger *li = (struct LargeInteger *)receiver;
                    snprintf(buf, sizeof(buf), "%s%llu",
                             li->sign < 0 ? "-" : "", (unsigned long long)li->mag);
                } else if (receiver != NULL && StringVtable != NULL && obj_vtable(receiver) == StringVtable) {
                    return receiver;                          /* a String prints as itself */
                } else if (receiver != NULL && obj_vtable(receiver) == SymbolVtable) {
                    struct Symbol *sy = (struct Symbol *)receiver;
                    /* booleans share the Symbol representation (no True/False
                       classes); they print as their bare word, no '#' — matched
                       by name, exactly as is_truthy does. */
                    if (receiver == true_symbol || (sy->length == 4 && memcmp(symbol_name(sy), "true", 4) == 0))
                        return make_string("true", 4);
                    if (receiver == false_symbol || (sy->length == 5 && memcmp(symbol_name(sy), "false", 5) == 0))
                        return make_string("false", 5);
                    /* genuine Symbol: '#' prefix */
                    size_t n = sy->length;
                    char *tmp = (char *)malloc(n + 1);
                    tmp[0] = '#'; memcpy(tmp + 1, symbol_name(sy), n);
                    oop r = make_string(tmp, n + 1); free(tmp); return r;
                } else if (receiver == NULL) {
                    snprintf(buf, sizeof(buf), "nil");
                } else {
                    // std-lib step 3a: a real heap object's printString is served
                    // by Object.nsl — fall through to vtable method lookup.
                    break;
                }
                return make_string(buf, strlen(buf));
            }
            break;
            case PRIM_asString: {
                char buf[128];
                if (IS_SMI(receiver)) {
                    snprintf(buf, sizeof(buf), "%ld", (long)UNTAG_SMI(receiver));
                } else if (receiver != NULL && obj_vtable(receiver) == FloatVtable) {
                    snprintf(buf, sizeof(buf), "%g", ((struct Float *)receiver)->value);
                } else if (receiver != NULL && obj_vtable(receiver) == LargeIntegerVtable) {
                    struct LargeInteger *li = (struct LargeInteger *)receiver;
                    snprintf(buf, sizeof(buf), "%s%llu",
                             li->sign < 0 ? "-" : "", (unsigned long long)li->mag);
                } else if (receiver != NULL && StringVtable != NULL && obj_vtable(receiver) == StringVtable) {
                    return receiver;                          /* a String asString is itself */
                } else if (receiver != NULL && obj_vtable(receiver) == SymbolVtable) {
                    return receiver;
                } else if (receiver == NULL) {
                    snprintf(buf, sizeof(buf), "nil");
                } else {
                    // std-lib step 3a: a real heap object's asString is served
                    // by Object.nsl — fall through to vtable method lookup.
                    break;
                }
                extern oop gc_allocate(oop vtable, size_t size);
                oop sym_obj = gc_allocate(SymbolVtable, sizeof(struct Symbol));
                char *new_str = malloc(strlen(buf) + 1);
                strcpy(new_str, buf);
                ((struct Symbol *)sym_obj)->name = new_str;
                ((struct Symbol *)sym_obj)->length = strlen(buf);
                return sym_obj;
            }
            break;
            case PRIM_out: {
                // SOMns stdout: each `... out` emits one newline-terminated line.
                if (IS_SMI(receiver)) {
                    printf("%ld\n", (long)UNTAG_SMI(receiver));
                } else if (receiver != NULL && obj_vtable(receiver) == FloatVtable) {
                    printf("%g\n", ((struct Float *)receiver)->value);
                } else if (receiver != NULL && StringVtable != NULL && obj_vtable(receiver) == StringVtable) {
                    struct String *s = (struct String *)receiver;
                    fwrite(s->bytes, 1, s->length, stdout);
                    fputc('\n', stdout);
                } else if (receiver != NULL && obj_vtable(receiver) == SymbolVtable) {
                    printf("%s\n", symbol_name((struct Symbol *)receiver));
                } else {
                    printf("[Object %p]\n", (void *)receiver);
                }
                platform_console_flush();
                return receiver;
            }
            break;
            case PRIM_comma: {
                /* String concatenation -> new String (std-lib 3c-1). Only
                   String-String is defined; anything else falls through to DNU. */
                if (receiver != NULL && !IS_SMI(receiver) && StringVtable != NULL &&
                    obj_vtable(receiver) == StringVtable &&
                    args[0] != NULL && !IS_SMI(args[0]) && obj_vtable(args[0]) == StringVtable) {
                    extern oop gc_allocate(oop vtable, size_t size);
                    struct String *a = (struct String *)receiver;
                    struct String *b = (struct String *)args[0];
                    size_t n = a->length + b->length;
                    oop s = gc_allocate(StringVtable, sizeof(struct String) + n);
                    ((struct String *)s)->length = n;
                    memcpy(((struct String *)s)->bytes, a->bytes, a->length);
                    memcpy(((struct String *)s)->bytes + a->length, b->bytes, b->length);
                    return s;
                }
            }
            break;
            // PRIM_timesRepeat deleted (Phase 2 de-recursify, Task 1): the
            // compiler now ALWAYS lowers `timesRepeat:` to a counting loop
            // driving OP_BLOCK_INVOKE (ns_compile_oevm.py compile_times_repeat /
            // emit_block_value), so a compiled send never reaches this switch.
            // It formerly invoked the body block via a recursive run_interpreter
            // per iteration — exactly the recursive block-invoke primitive this
            // phase removes. A reflective `perform: #timesRepeat:` now falls
            // through to does_not_understand; ns-tests has no such send.
            case PRIM_size: {
                if (receiver != NULL && !IS_SMI(receiver) && StringVtable != NULL &&
                    obj_vtable(receiver) == StringVtable)
                    return TAG_SMI((intptr_t)((struct String *)receiver)->length);
                /* RawBytes (Milestone B Task 3): same struct String C layout. */
                if (receiver != NULL && !IS_SMI(receiver) && RawBytesVtable != NULL &&
                    obj_vtable(receiver) == RawBytesVtable)
                    return TAG_SMI((intptr_t)((struct String *)receiver)->length);
            }
            break;
            case PRIM_at: {
                if (receiver != NULL && !IS_SMI(receiver) && StringVtable != NULL &&
                    obj_vtable(receiver) == StringVtable) {
                    struct String *s = (struct String *)receiver;
                    intptr_t idx = IS_SMI(args[0]) ? UNTAG_SMI(args[0]) : 0;
                    if (idx < 1 || idx > (intptr_t)s->length) { fprintf(stderr, "VM Error: String at: %ld out of bounds (size %zu)\n", (long)idx, s->length); abort(); }
                    return TAG_SMI((intptr_t)s->bytes[idx - 1]);   /* 1-based; returns 0..255 */
                }
                /* RawBytes (Milestone B Task 3): frozen crash string is the
                   exact `index out of bounds` (spec section 7 error-strings
                   table) — no embellishment, unlike String's message above. */
                if (receiver != NULL && !IS_SMI(receiver) && RawBytesVtable != NULL &&
                    obj_vtable(receiver) == RawBytesVtable) {
                    struct String *s = (struct String *)receiver;   /* reused C layout */
                    intptr_t idx = IS_SMI(args[0]) ? UNTAG_SMI(args[0]) : 0;
                    if (idx < 1 || idx > (intptr_t)s->length) { fprintf(stderr, "index out of bounds\n"); abort(); }
                    return TAG_SMI((intptr_t)s->bytes[idx - 1]);   /* 1-based; returns 0..255 */
                }
            }
            break;
            case PRIM_at_put: {
                /* RawBytes ONLY (Milestone B Task 3) — a String receiver must
                   NOT gain at:put:; falling through this case unmatched (no
                   return) hits `break` below and continues to normal vtable
                   dispatch, i.e. does_not_understand for String, exactly as
                   before this case existed. */
                if (receiver != NULL && !IS_SMI(receiver) && RawBytesVtable != NULL &&
                    obj_vtable(receiver) == RawBytesVtable) {
                    struct String *s = (struct String *)receiver;
                    intptr_t idx = IS_SMI(args[0]) ? UNTAG_SMI(args[0]) : 0;
                    if (idx < 1 || idx > (intptr_t)s->length) { fprintf(stderr, "index out of bounds\n"); abort(); }
                    intptr_t val = IS_SMI(args[1]) ? UNTAG_SMI(args[1]) : -1;
                    if (val < 0 || val > 255) { fprintf(stderr, "byte out of range\n"); abort(); }
                    s->bytes[idx - 1] = (uint8_t)val;
                    return args[1];
                }
            }
            break;
            /* --- Milestone B Task 4: predicate fast-paths for BUILTIN
               receivers only (nil, Boolean singletons, SMI/LargeInteger,
               Float, String, Symbol, RawBytes). Modeled on PRIM_at/PRIM_isNil
               above: identity/vtable-gated, so any heap instance falls
               through this whole group unmatched (no return, no break out of
               the outer switch dodge) straight to normal vtable lookup —
               its nsl Kernel.Object defaults land in Task 5. Boolean is
               tested by singleton identity BEFORE the generic SymbolVtable
               check so rank-1 Boolean wins over rank-6 Symbol. --- */
            case PRIM_isValue: {
                if (receiver == NULL) return true_symbol;                    /* nil */
                if (IS_SMI(receiver)) return true_symbol;                    /* SmallInteger */
                if (receiver == true_symbol || receiver == false_symbol) return true_symbol; /* Boolean */
                if (LargeIntegerVtable != NULL && obj_vtable(receiver) == LargeIntegerVtable) return true_symbol;
                if (FloatVtable != NULL && obj_vtable(receiver) == FloatVtable) {
                    double d = ((struct Float *)receiver)->value;
                    return (d == d) ? true_symbol : false_symbol;           /* NaN -> not a value */
                }
                if (StringVtable != NULL && obj_vtable(receiver) == StringVtable) return true_symbol;
                if (SymbolVtable != NULL && obj_vtable(receiver) == SymbolVtable) return true_symbol; /* genuine Symbol (true/false already handled above) */
                if (RawBytesVtable != NULL && obj_vtable(receiver) == RawBytesVtable) return false_symbol; /* internal, never a value */
                break;   /* heap object: fall through to nsl Object default (Task 5) */
            }
            break;
            case PRIM_isSymbol: {
                if (receiver == NULL) return false_symbol;
                if (IS_SMI(receiver)) return false_symbol;
                if (receiver == true_symbol || receiver == false_symbol) return false_symbol; /* Boolean, not Symbol: rank 1 wins over rank 6 */
                if (LargeIntegerVtable != NULL && obj_vtable(receiver) == LargeIntegerVtable) return false_symbol;
                if (FloatVtable != NULL && obj_vtable(receiver) == FloatVtable) return false_symbol;
                if (StringVtable != NULL && obj_vtable(receiver) == StringVtable) return false_symbol;
                if (SymbolVtable != NULL && obj_vtable(receiver) == SymbolVtable) return true_symbol;  /* genuine Symbol */
                if (RawBytesVtable != NULL && obj_vtable(receiver) == RawBytesVtable) return false_symbol;
                break;
            }
            break;
            case PRIM_isList:
            case PRIM_isMap:
            case PRIM_isRecord:
            case PRIM_isBytes: {
                /* No builtin receiver is ever a List/Map/Record/(kernel)Bytes
                   (those are heap kernel classes that answer their own is*
                   predicate in nsl, Task 5). For any builtin receiver here
                   (nil/Boolean/Integer/Float/String/Symbol/RawBytes) the
                   answer is unconditionally false; heap instances still fall
                   through untouched below. */
                if (receiver == NULL) return false_symbol;
                if (IS_SMI(receiver)) return false_symbol;
                if (receiver == true_symbol || receiver == false_symbol) return false_symbol;
                if (LargeIntegerVtable != NULL && obj_vtable(receiver) == LargeIntegerVtable) return false_symbol;
                if (FloatVtable != NULL && obj_vtable(receiver) == FloatVtable) return false_symbol;
                if (StringVtable != NULL && obj_vtable(receiver) == StringVtable) return false_symbol;
                if (SymbolVtable != NULL && obj_vtable(receiver) == SymbolVtable) return false_symbol;
                if (RawBytesVtable != NULL && obj_vtable(receiver) == RawBytesVtable) return false_symbol;
                break;   /* heap object: fall through to nsl Object default (Task 5) */
            }
            break;
            default: break;
            }
        }
    }

    // Determine the receiver's Vtable
    oop receiver_vtable;
    if (IS_SMI(receiver)) {
        receiver_vtable = IntegerVtable;
    } else if (receiver == NULL) {
        // Safe fallback for null/nil references
        receiver_vtable = NULL;
    } else {
        receiver_vtable = obj_vtable(receiver);
        if (receiver_vtable == BytecodeClosureVtable) {
            if (SymbolVtable != NULL && obj_vtable(selector) == SymbolVtable) {
                struct Symbol *sym = (struct Symbol *)selector;
                const char *sname = symbol_name(sym);
                if (strncmp(sname, "value", 5) == 0) {
                    int colons = 0;
                    for (const char *p = sname; *p; p++) {
                        if (*p == ':') colons++;
                    }
                    /* Reflective block invoke: hand the closure back as a
                       bytecode target (frame push / re-entrant run). */
                    *out_method = receiver;
                    *out_recv = receiver;
                    *out_argc = colons;
                    return NULL;
                }
            }
        }
    }

    if (receiver_vtable == NULL) {
        return does_not_understand(receiver, selector, argc, args);
    }

    // Lookup method closure and its defining Vtable
    oop method = NULL;
    oop defining_vt = NULL;
    lookup_method_cached(receiver_vtable, selector, &method, &defining_vt);
    if (defining_vt == NULL || method == NULL) {
        return does_not_understand(receiver, selector, argc, args);
    }

    struct Closure *cl = (struct Closure *)method;
    if (obj_vtable((oop)cl) == BytecodeClosureVtable) {
        struct CompiledMethod *cm = (struct CompiledMethod *)closure_entry(cl);
        if (!check_access_allowed(receiver, selector, cm, defining_vt)) {
            return trigger_access_violation(receiver, selector, argc, args);
        }
    }

    if (obj_vtable((oop)cl) == PrimitiveClosureVtable) {
        // Primitive closure: call native C function pointer directly
        oop (*func)(oop, int, oop*) = (oop (*)(oop, int, oop*))closure_entry(cl);
        return func(receiver, argc, args);
    } else {
        // Bytecode closure: hand it back for a frame push (flat loop) or a
        // re-entrant run_interpreter (the send() wrapper).
        *out_method = method;
        *out_recv = receiver;
        *out_argc = argc;
        return NULL;
    }
}

/* Re-entrant single-shot send: resolves then, for a bytecode target, runs it via
   a nested run_interpreter (behavior-identical to the pre-Task-4 send()). Kept for
   the cold C callers — does_not_understand's bytecode handler, run_method_on's
   nested-class init, trigger_access_violation, the debugger hook, and any capability
   / primitive C code that sends. The hot method/block-send path in the flat
   interpreter calls send_resolve directly and pushes a frame instead. */
oop send(oop receiver, oop selector, int argc, oop *args) {
    oop method = NULL, recv = NULL;
    int rargc = 0;
    oop result = send_resolve(receiver, selector, argc, args,
                              &method, &recv, &rargc);
    if (method != NULL) {
        return run_interpreter(method, recv, rargc, args);
    }
    return result;
}

/* Super-send dispatch: look up `selector` starting at `start_vtable` (the
   defining class's delegate/superclass) rather than at the receiver's own
   vtable, then invoke with the unchanged `receiver` (self). This is how
   `super foo` reaches an overridden method on the superclass. Access checks are
   skipped — a super-send is statically bound to the enclosing class, so it is
   inherently authorised. */
/* Resolve a super-send without invoking a bytecode target (de-recursify Phase 2).
   Mirrors send_resolve: services the inline cases (DNU, primitive closure) and
   returns their value with out method NULL; for a bytecode target sets the
   out-params (method, recv, argc) and returns NULL. */
oop send_super_resolve(oop start_vtable, oop receiver, oop selector, int argc, oop *args,
                       oop *out_method, oop *out_recv, int *out_argc) {
    *out_method = NULL;
    if (start_vtable == NULL || IS_SMI(start_vtable)) {
        return does_not_understand(receiver, selector, argc, args);
    }
    oop method = NULL;
    oop defining_vt = lookup_defining_vtable(start_vtable, selector, &method);
    if (defining_vt == NULL || method == NULL) {
        return does_not_understand(receiver, selector, argc, args);
    }
    struct Closure *cl = (struct Closure *)method;
    if (obj_vtable((oop)cl) == PrimitiveClosureVtable) {
        oop (*func)(oop, int, oop *) = (oop (*)(oop, int, oop *))closure_entry(cl);
        return func(receiver, argc, args);
    }
    *out_method = method;
    *out_recv = receiver;
    *out_argc = argc;
    return NULL;
}

oop send_super(oop start_vtable, oop receiver, oop selector, int argc, oop *args) {
    oop method = NULL, recv = NULL;
    int rargc = 0;
    oop result = send_super_resolve(start_vtable, receiver, selector, argc, args,
                                    &method, &recv, &rargc);
    if (method != NULL) {
        return run_interpreter(method, recv, rargc, args);
    }
    return result;
}

