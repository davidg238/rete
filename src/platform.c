#include "platform.h"
#include "platform_backend.h"
#include "prim_ids.h"
#include "oimg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern oop gc_allocate(oop vtable, size_t size);
extern oop StringVtable, SymbolVtable, VtableVtable;
extern const struct Symbol true_sym_obj, false_sym_obj;   /* match oevm.h forms */
#define true_symbol  ((oop)&true_sym_obj)
#define false_symbol ((oop)&false_sym_obj)

/* ---- moved from dispatch.c: std-lib step 1: real indexable representation --
   Layout mirrors struct ObjectInstance: slots[0] holds the element count as a
   tagged SMI (so the GC word-scan early-rejects it via IS_SMI), slots[1..n]
   hold elements (1-based). This is the capability-pure backing store that the
   kernel Array class (lib/kernel.ns) wraps via basicAt:/basicSize. */
static struct Vtable IndexableVtable;

/* One-time init of the indexable vtable. Lazy (works on both boot paths). */
static int indexable_initialized = 0;
static void indexable_init(void) {
    if (indexable_initialized) return;
    IndexableVtable.base.vtable = VtableVtable;
    IndexableVtable.delegate = NULL;
    IndexableVtable.bindings = NULL;
    IndexableVtable.capacity = 0;
    IndexableVtable.size = 0;
    IndexableVtable.is_nested = 0;
    indexable_initialized = 1;
}

/* Allocate a fresh indexable of `n` nil-filled element slots. */
oop indexable_new(intptr_t n) {
    indexable_init();
    if (n < 0) n = 0;
    oop ix = gc_allocate((oop)&IndexableVtable,
                         sizeof(struct ObjectInstance) + ((size_t)n + 1) * sizeof(oop));
    struct ObjectInstance *inst = (struct ObjectInstance *)ix;
    inst->slots[0] = TAG_SMI(n);
    for (intptr_t i = 1; i <= n; i++) inst->slots[i] = NULL;
    return ix;
}

/* ---- moved from dispatch.c: std-lib step 1: the platform capability object -
   A single object whose messages ARE the genuinely-VM-level primitives
   (allocation / identity / I-O). Reached only by receiver identity, so it is
   capability-pure: code that was not handed this object cannot send to it.
   Lazily built so it is available on both boot paths (Model A solidified and
   Model B image), neither of which is guaranteed to call kernel_boot. The
   object + its vtable are static (BSS), not arena-allocated, so they need no
   GC-root registration; revisit only if platform is ever heap-allocated. */
static struct Object PlatformObj;
static struct Vtable PlatformVtable;
static int platform_initialized = 0;

oop platform_object(void) {
    if (!platform_initialized) {
        indexable_init();   /* platform array: depends on the indexable vtable */
        PlatformVtable.base.vtable = VtableVtable;
        PlatformVtable.delegate = NULL;
        PlatformVtable.bindings = NULL;
        PlatformVtable.capacity = 0;
        PlatformVtable.size = 0;
        PlatformVtable.is_nested = 0;
        PlatformObj.vtable = (oop)&PlatformVtable;
        platform_initialized = 1;
    }
    return (oop)&PlatformObj;
}

int platform_dispatch(int pid, int argc, oop *args, oop *out) {
    (void)argc;
    switch (pid) {
    case PRIM_ix_array:
        { *out = indexable_new(IS_SMI(args[0]) ? UNTAG_SMI(args[0]) : 0); return 1; }
    case PRIM_pf_identicalTo:
        { *out = (args[0] == args[1]) ? true_symbol : false_symbol; return 1; }
    case PRIM_pf_hashcode:
        if (IS_SMI(args[0])) { *out = args[0]; return 1; }
        if (args[0] == NULL) { *out = TAG_SMI(0); return 1; }
        { *out = TAG_SMI((intptr_t)(((uintptr_t)args[0]) >> 4)); return 1; }
    case PRIM_pf_print:
        if (args[0] == NULL) {
            platform_console_write("nil", 3);
        } else if (IS_SMI(args[0])) {
            char buf[32];
            int len = snprintf(buf, sizeof buf, "%ld", (long)UNTAG_SMI(args[0]));
            if (len > 0) platform_console_write(buf, (size_t)len);
        } else if (SymbolVtable != NULL &&
                   obj_vtable(args[0]) == SymbolVtable) {
            const char *nm = symbol_name((struct Symbol *)args[0]);
            platform_console_write(nm, strlen(nm));
        }
        { *out = platform_object(); return 1; }
    case PRIM_pf_printNewline:
        platform_console_write("\n", 1);
        platform_console_flush();
        { *out = platform_object(); return 1; }
    case PRIM_pf_basicNewIndexable: {
        /* `platform basicNew: <class> indexable: n` — capability-pure
           indexable allocation. args[0] is the class object: a BoundClass
           wrapper (slot0 = its nested vtable) or a bare (oop)&Vtable. The
           new instance is stamped with that vtable, slot0 = TAG_SMI(n)
           length header, elements 1..n nil. Replaces 3a's magic
           kernel_array_vtable `new:`. */
        oop cls = args[0];
        oop vt = boundclass_is(cls)
                 ? ((struct ObjectInstance *)cls)->slots[0]
                 : cls;
        intptr_t n = IS_SMI(args[1]) ? UNTAG_SMI(args[1]) : 0;
        if (n < 0) n = 0;
        oop a = gc_allocate(vt, sizeof(struct ObjectInstance) + ((size_t)n + 1) * sizeof(oop));
        struct ObjectInstance *inst = (struct ObjectInstance *)a;
        inst->slots[0] = TAG_SMI(n);
        for (intptr_t i = 1; i <= n; i++) inst->slots[i] = NULL;
        { *out = a; return 1; }
    }
    case PRIM_pf_string: {
        /* `platform string: anArray` — build an immutable String from a
           Kernel indexable Array of byte-SMIs (0..255). The Toit
           bytes.to-string analog. Byte-granular: no UTF-8 validation
           (dialect #10/#12). GC is non-moving (cf. PRIM_comma), so reading
           `ix` after the single gc_allocate is safe. */
        oop arr = args[0];
        if (arr == NULL || IS_SMI(arr) ||
            !IS_SMI(instance_slot((struct ObjectInstance *)arr, 0))) {
            fprintf(stderr, "VM Error: platform string: expects an indexable Array\n");
            abort();
        }
        struct ObjectInstance *ix = (struct ObjectInstance *)arr;
        intptr_t n = UNTAG_SMI(instance_slot(ix, 0));
        oop s = gc_allocate(StringVtable, sizeof(struct String) + (size_t)n);
        ((struct String *)s)->length = (size_t)n;
        for (intptr_t i = 1; i <= n; i++) {
            oop e = instance_slot(ix, (size_t)i);
            if (!IS_SMI(e)) {
                fprintf(stderr, "VM Error: platform string: element %ld is not a byte SMI\n", (long)i);
                abort();
            }
            intptr_t b = UNTAG_SMI(e);
            if (b < 0 || b > 255) {
                fprintf(stderr, "VM Error: platform string: byte %ld out of range 0..255\n", (long)b);
                abort();
            }
            ((struct String *)s)->bytes[i - 1] = (uint8_t)b;
        }
        { *out = s; return 1; }
    }
    case PRIM_pf_readLine: {
        /* Read one line from stdin (debug transport). Returns a kernel String
           with the newline stripped, or nil at EOF. Uses the same allocation
           idiom as PRIM_pf_string above (StringVtable / struct String). */
        char buf[512];
        int n = platform_debug_readline(buf, (int)sizeof buf);
        if (n < 0) { *out = NULL; return 1; }           /* EOF -> nil */
        oop s = gc_allocate(StringVtable, sizeof(struct String) + (size_t)n);
        ((struct String *)s)->length = (size_t)n;
        if (n) memcpy(((struct String *)s)->bytes, buf, (size_t)n);
        { *out = s; return 1; }
    }
    default:
        return 0;
    }
}
