#include "platform.h"
#include "platform_backend.h"
#include "prim_ids.h"
#include "oimg.h"
#include "space.h"   /* Milestone C Task 1: channel-space C substrate */
#include "pulse_backend.h"
#include "handles.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ---- Milestone D Task 6: host virtual-clock timer facility -------------
   A deterministic event source: the host test rig never reads wall time.
   `host_advance` is the ONLY thing that moves the clock, driven exclusively
   by the nsl-level `platform advanceMillis:` prim (below), so timer firing
   order is fully reproducible across runs and across Path A/B. Each live
   HostTimer is a (token, due_ms, interval_ms, repeating, live) slot in a
   small fixed table (HOST_MAX_TIMERS is a host-rig cap, not a VM limit — an
   on-target backend would back this with a real timer facility instead).
   `host_timer_add` reuses a dead slot before growing the table, so
   after:/every: churn (register, fire/cancel, register again) doesn't leak
   slots. The C side carries DATA ONLY (token, ms) — which channel, what
   message, and cancel bookkeeping all live in nsl's `Timers` class (lib/
   kernel.ns); `host_advance` posts just `TAG_SMI(token)` onto the single
   reserved `sys/timer` channel via `space_post`, exactly like the design
   doc's event-source seam (mirrors the space prims' existing TAG_SMI/
   space_post usage just below in this file). */
#define HOST_MAX_TIMERS 32
struct HostTimer { int token; uint64_t due_ms; uint64_t interval_ms; int repeating; int live; };
static struct HostTimer host_timers[HOST_MAX_TIMERS];
static int host_n_timers = 0;
static uint64_t host_clock_ms = 0;

static void host_timer_add(int token, uint64_t ms, int repeating) {
    int i;
    /* A repeating timer with a 0ms interval would never let due_ms advance
       past host_clock_ms in host_advance's fire loop below, hanging the
       process on the very next advanceMillis: call. Floor the interval to
       1ms for repeaters only — a one-shot (`repeating` false) with ms==0
       is harmless: it fires once on the next advance and deactivates, no
       loop involved, so its ms is left untouched. */
    if (repeating && ms == 0) ms = 1;
    for (i = 0; i < host_n_timers; i++) {
        if (!host_timers[i].live) {
            host_timers[i].token = token;
            host_timers[i].due_ms = host_clock_ms + ms;
            host_timers[i].interval_ms = ms;
            host_timers[i].repeating = repeating;
            host_timers[i].live = 1;
            return;
        }
    }
    if (host_n_timers < HOST_MAX_TIMERS) {
        host_timers[host_n_timers].token = token;
        host_timers[host_n_timers].due_ms = host_clock_ms + ms;
        host_timers[host_n_timers].interval_ms = ms;
        host_timers[host_n_timers].repeating = repeating;
        host_timers[host_n_timers].live = 1;
        host_n_timers++;
    }
    /* else: silently drop — host test rig; on-target uses a real timer facility */
}
static void host_timer_after(int token, uint64_t ms) { host_timer_add(token, ms, 0); }
static void host_timer_every(int token, uint64_t ms) { host_timer_add(token, ms, 1); }
static void host_timer_cancel(int token) {
    int i;
    for (i = 0; i < host_n_timers; i++)
        if (host_timers[i].live && host_timers[i].token == token) host_timers[i].live = 0;
}
static uint64_t host_monotonic_millis(void) { return host_clock_ms; }
/* Advance the virtual clock by `ms`; post each now-due timer's token onto
   sys/timer (9 = strlen("sys/timer"), mirroring the String-path prims'
   length-carrying convention elsewhere in this file). A repeater whose
   interval is smaller than the advance can fire more than once per call
   (the inner while loop keeps posting and rescheduling until it is no
   longer due); a one-shot fires at most once and then deactivates. Answers
   the total number of fires, for observability. */
static int host_advance(uint64_t ms) {
    int i, fired = 0;
    host_clock_ms += ms;
    for (i = 0; i < host_n_timers; i++) {
        struct HostTimer *t = &host_timers[i];
        while (t->live && t->due_ms <= host_clock_ms) {
            oop tok = TAG_SMI(t->token);
            space_post("sys/timer", 9, tok);
            fired++;
            if (t->repeating) t->due_ms += t->interval_ms; else { t->live = 0; break; }
        }
    }
    return fired;
}

/* Host GPIO mock: registration stores (pin, chan_id, handle_idx); the host-only
   gpioFire: prim simulates an edge by posting to the pulse mailbox — the exact
   (chan_id, handle_idx) an on-target GPIO ISR would post. Fixed table, -D override. */
#ifndef HOST_MAX_GPIO
#define HOST_MAX_GPIO 16
#endif
struct HostGpio { int pin; int chan_id; int handle_idx; int live; };
static struct HostGpio host_gpio[HOST_MAX_GPIO];
static int host_n_gpio = 0;

static void host_gpio_register(int pin, int chan_id, int handle_idx) {
    int i;
    for (i = 0; i < host_n_gpio; i++) {
        if (host_gpio[i].live && host_gpio[i].pin == pin) {   /* re-register same pin: replace */
            handle_free(host_gpio[i].handle_idx);   /* release the outgoing M6 pin — no slot leak */
            host_gpio[i].chan_id = chan_id; host_gpio[i].handle_idx = handle_idx; return;
        }
    }
    for (i = 0; i < host_n_gpio; i++) {
        if (!host_gpio[i].live) {
            host_gpio[i].pin = pin; host_gpio[i].chan_id = chan_id;
            host_gpio[i].handle_idx = handle_idx; host_gpio[i].live = 1; return;
        }
    }
    if (host_n_gpio < HOST_MAX_GPIO) {
        host_gpio[host_n_gpio].pin = pin; host_gpio[host_n_gpio].chan_id = chan_id;
        host_gpio[host_n_gpio].handle_idx = handle_idx; host_gpio[host_n_gpio].live = 1;
        host_n_gpio++;
    } else { fprintf(stderr, "host gpio table full\n"); abort(); }
}
static void host_gpio_fire(int pin) {
    int i;
    for (i = 0; i < host_n_gpio; i++)
        if (host_gpio[i].live && host_gpio[i].pin == pin)
            pulse_post_isr(host_gpio[i].chan_id, host_gpio[i].handle_idx);
}

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
           blocksSnapshot, channelPath} 3-slot Array (Milestone D) or nil
           (NULL) if the queue is empty. */
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
    /* --- Milestone D Task 6: host virtual-clock timer seam. args order
       mirrors the keyword selector left-to-right (spaceRegister:block:owner:
       convention above): `timerAfter: ms token: tok` -> args[0]=ms,
       args[1]=tok. answers the platform capability where there is no more
       natural answer (same convention as spaceRegister/spaceDropReactions-
       OwnedBy above); monotonicMillis/advanceMillis: answer their own SMI
       result instead. */
    case PRIM_pf_timerAfter:
        host_timer_after(UNTAG_SMI(args[1]), (uint64_t)UNTAG_SMI(args[0]));
        *out = platform_object();
        return 1;
    case PRIM_pf_timerEvery:
        host_timer_every(UNTAG_SMI(args[1]), (uint64_t)UNTAG_SMI(args[0]));
        *out = platform_object();
        return 1;
    case PRIM_pf_timerCancel:
        host_timer_cancel(UNTAG_SMI(args[0]));
        *out = platform_object();
        return 1;
    case PRIM_pf_monotonicMillis:
        *out = TAG_SMI((long)host_monotonic_millis());
        return 1;
    case PRIM_pf_advanceMillis:
        *out = TAG_SMI(host_advance((uint64_t)UNTAG_SMI(args[0])));
        return 1;
    /* --- Zephyr rung-2 sub-project 1 Task 3: host GPIO prims + pumpEvents.
       gpioPin:edge:post:on: registers a host GPIO slot; args mirror the
       keyword order left-to-right: args[0]=pin, args[1]=edge, args[2]=msg,
       args[3]=chanPath (String). gpioFire: is host-only — it simulates an
       edge by posting straight to the pulse mailbox via the same
       (chan_id, handle_idx) an on-target GPIO ISR would post. pumpEvents
       drains the mailbox into the channel space, answering the fire count. */
    case PRIM_pf_gpioPin: {
        oop path = args[3];
        if (path == NULL || IS_SMI(path) || StringVtable == NULL ||
            obj_vtable(path) != StringVtable) {
            fprintf(stderr, "VM Error: platform gpioPin:edge:post:on: expects a String channel\n");
            abort();
        }
        struct String *p = (struct String *)path;
        int chan_id = space_intern((const char *)p->bytes, p->length);
        int hidx = handle_new(args[2]);                 /* pin the message oop via M6 */
        host_gpio_register((int)UNTAG_SMI(args[0]), chan_id, hidx);
        *out = platform_object();
        return 1;
    }
    case PRIM_pf_gpioFire:
        host_gpio_fire((int)UNTAG_SMI(args[0]));
        *out = platform_object();
        return 1;
    case PRIM_pf_pumpEvents:
        *out = TAG_SMI(pulse_pump());
        return 1;
    default:
        return 0;
    }
}
