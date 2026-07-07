#include "platform.h"
#include "platform_backend.h"
#include "prim_ids.h"
#include "oimg.h"
#include "space.h"   /* Milestone C Task 1: channel-space C substrate */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern oop gc_allocate(oop vtable, size_t size);
extern oop StringVtable, SymbolVtable, VtableVtable;
extern oop RawBytesVtable;   /* Milestone B Task 3: RawBytes leaf storage */
extern oop LargeIntegerVtable, FloatVtable;   /* Milestone B Task 4: valueRank: */
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
    case PRIM_pf_basicNewBytes: {
        /* `platform basicNewBytes: n` — capability-pure raw byte storage
           allocation (Milestone B Task 3). Same allocation idiom as
           PRIM_pf_string (RawBytesVtable + struct String layout: length +
           inline bytes), but zero-filled rather than copied from an Array,
           and tagged with the DISTINCT RawBytesVtable so String never gains
           at:put: (dispatch.c gates that case on RawBytesVtable only). */
        intptr_t n = IS_SMI(args[0]) ? UNTAG_SMI(args[0]) : 0;
        if (n < 0) n = 0;
        oop s = gc_allocate(RawBytesVtable, sizeof(struct String) + (size_t)n);
        ((struct String *)s)->length = (size_t)n;
        if (n) memset(((struct String *)s)->bytes, 0, (size_t)n);
        { *out = s; return 1; }
    }
    case PRIM_pf_freezeBytes: {
        /* `platform freezeBytes: raw` — freeze-is-copy (message-algebra spec
           section 6): returns a FRESH RawBytes object with identical bytes;
           the original remains independently mutable (a later at:put: on
           either side must not be observed by the other). */
        oop raw = args[0];
        if (raw == NULL || IS_SMI(raw) || RawBytesVtable == NULL ||
            obj_vtable(raw) != RawBytesVtable) {
            fprintf(stderr, "VM Error: platform freezeBytes: expects a RawBytes receiver\n");
            abort();
        }
        struct String *src = (struct String *)raw;
        oop copy = gc_allocate(RawBytesVtable, sizeof(struct String) + src->length);
        ((struct String *)copy)->length = src->length;
        if (src->length) memcpy(((struct String *)copy)->bytes, src->bytes, src->length);
        { *out = copy; return 1; }
    }
    case PRIM_pf_valueRank: {
        /* `platform valueRank: v` — the C-side type oracle for the value
           algebra (Milestone B Task 4). SMI result: nil=0; Boolean=1;
           SMI/LargeInteger=2; Float=3 (NaN=-1); String=4; Symbol=6;
           RawBytes=-2 (internal, never a value); any other heap object=-1
           ("ask the object" — Bytes/List/Map/Record answer their own rank in
           nsl, Task 5). Boolean is tested by singleton identity BEFORE the
           generic SymbolVtable check so rank 1 wins over rank 6, matching
           dispatch.c's predicate fast-paths. */
        oop v = args[0];
        if (v == NULL) { *out = TAG_SMI(0); return 1; }                       /* nil */
        if (v == true_symbol || v == false_symbol) { *out = TAG_SMI(1); return 1; } /* Boolean */
        if (IS_SMI(v)) { *out = TAG_SMI(2); return 1; }                       /* SmallInteger */
        if (LargeIntegerVtable != NULL && obj_vtable(v) == LargeIntegerVtable) { *out = TAG_SMI(2); return 1; }
        if (FloatVtable != NULL && obj_vtable(v) == FloatVtable) {
            double d = ((struct Float *)v)->value;
            *out = (d != d) ? TAG_SMI(-1) : TAG_SMI(3);                      /* NaN -> -1 */
            return 1;
        }
        if (StringVtable != NULL && obj_vtable(v) == StringVtable) { *out = TAG_SMI(4); return 1; }
        if (SymbolVtable != NULL && obj_vtable(v) == SymbolVtable) { *out = TAG_SMI(6); return 1; } /* genuine Symbol */
        if (RawBytesVtable != NULL && obj_vtable(v) == RawBytesVtable) { *out = TAG_SMI(-2); return 1; }
        *out = TAG_SMI(-1);   /* any other heap object: ask it in nsl */
        return 1;
    }
    /* --- Milestone C Task 1: channel-space C substrate (src/space.c). Path
       arguments arrive as `struct String*`, StringVtable-gated the same way
       PRIM_pf_freezeBytes gates its RawBytes receiver above; nsl owns the
       verbs' own validation (value-check, A1 prefix confinement) before it
       ever reaches here (design doc section 6). --- */
    case PRIM_pf_spacePost: {
        /* `platform spacePost: path value: v` — the C-callable enqueue seam
           (design doc section 6.1's ISR-pulse proof). Answers args[1]
           unchanged (mirrors `out:` answering something useful to chain on;
           the capability-answering contract in the verb surface is nsl's). */
        oop path = args[0];
        if (path == NULL || IS_SMI(path) || StringVtable == NULL ||
            obj_vtable(path) != StringVtable) {
            fprintf(stderr, "VM Error: platform spacePost:value: expects a String path\n");
            abort();
        }
        struct String *p = (struct String *)path;
        space_post((const char *)p->bytes, p->length, args[1]);
        { *out = args[1]; return 1; }
    }
    case PRIM_pf_spaceRegister: {
        /* `platform spaceRegister: path block: aBlock owner: cap` — append a
           standing reaction, owner-tagged (design doc section 6 point 2).
           Answers the platform capability (no other natural receiver-ish
           answer exists here; mirrors PRIM_pf_print's convention). */
        oop path = args[0];
        if (path == NULL || IS_SMI(path) || StringVtable == NULL ||
            obj_vtable(path) != StringVtable) {
            fprintf(stderr, "VM Error: platform spaceRegister:block:owner: expects a String path\n");
            abort();
        }
        struct String *p = (struct String *)path;
        space_register((const char *)p->bytes, p->length, args[1], args[2]);
        { *out = platform_object(); return 1; }
    }
    case PRIM_pf_spaceTake: {
        /* `platform spaceTake: path` — remove-if-present-now; answers the
           value or nil (NULL). */
        oop path = args[0];
        if (path == NULL || IS_SMI(path) || StringVtable == NULL ||
            obj_vtable(path) != StringVtable) {
            fprintf(stderr, "VM Error: platform spaceTake: expects a String path\n");
            abort();
        }
        struct String *p = (struct String *)path;
        { *out = space_take((const char *)p->bytes, p->length); return 1; }
    }
    case PRIM_pf_spaceDequeue: {
        /* `platform spaceDequeue` — no args; answers the {value,
           blocksSnapshot} Array or nil (NULL) if the queue is empty. */
        { *out = space_dequeue(); return 1; }
    }
    case PRIM_pf_protectedFire: {
        /* `platform protectedFire: aBlock value: v` — Milestone D crash
           boundary. args[0] = block, args[1] = value. */
        *out = vm_protected_fire(args[0], args[1]);
        return 1;
    }
    case PRIM_pf_spaceDropReactionsOwnedBy: {
        /* `platform spaceDropReactionsOwnedBy: cap` — Milestone D restart
           sweep. Removes every reaction owned by `cap` across every
           channel, compacting in place. Answers the platform capability
           (mirrors PRIM_pf_spaceRegister's/PRIM_pf_print's convention). */
        space_drop_reactions_owned_by(args[0]);
        *out = platform_object();
        return 1;
    }
    default:
        return 0;
    }
}
