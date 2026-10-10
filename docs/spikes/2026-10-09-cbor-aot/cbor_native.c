/* THROWAWAY spike (rete#395): Cbor>>encode: as a native method, three arms, to price
   AOT-compiling the nsl encoder against the C encoder. Not part of the build unless
   OEVM_CBOR_NATIVE is defined (CMakeLists.txt hook, env OEVM_CBOR_NATIVE=1).

   arm 2 (B): feedback AOT — bytes, String/Symbol leaves and the builtin ranks in C;
              every send to an nsl-defined receiver kept as a re-entrant send().
   arm 3 (C): the ceiling — the whole walk in C over the kernel object layouts.
   arm 4 (D): arm B holding a FRESH object across every kept send, rooted on
              gc_temp_root_push (the way an AOT'd encodeMap: would hold its
              ByteBuilder) — the rooting price.
   arm 5 (E): arm D with the root omitted — the hazard. Under OEVM_GC_STRESS the fresh
              object is swept inside the send and its slot reused; `root lost` counts
              it. A green stress run of arm D proves nothing unless arm E goes red
              in the same binary.

   Installed the way hanoi_native.c installs its method: a firmware-static
   PrimitiveClosure bound to the interned selector in a one-binding vtable. */
#ifdef OEVM_CBOR_NATIVE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "oevm.h"
#include "oimg.h"

extern oop gc_allocate(oop vtable, size_t size);
extern void gc_temp_root_push(oop o);
extern void gc_temp_roots_clear(void);
extern int gc_temp_roots_count;
extern oop PrimitiveClosureVtable, VtableVtable, SymbolVtable, StringVtable,
           RawBytesVtable, FloatVtable, LargeIntegerVtable;

static oop vt_list, vt_map, vt_record, vt_bytes, o_true, o_false;
static oop s_valueRank, s_size, s_at, s_keyAt, s_valueAt, s_tag, s_fields,
           s_byteSize, s_u8At, s_encode;

#define OUT_MAX 2048
#define KB_MAX  1024
#define MAP_MAX 32
static uint8_t out_buf[OUT_MAX]; static size_t out_n;
static uint8_t kb_buf[KB_MAX];   static size_t kb_n;     /* key scratch, a stack */
static uint8_t *cur; static size_t *cur_n; static size_t cur_max;
static int err;

/* mode: 0 = all C (arm 3), 1 = sends kept (arm 2), 2 = sends + rooted fresh object
   (arm 4, timed: the price), 3 = sends + unrooted fresh object + an allocation after
   the send, as a send that allocates would (arm 5, the hazard), 4 = arm 5 with the
   root (arm 6, the guard: differs from arm 5 only by the root). */
static int mode;
static long sends_n, root_lost;
static oop bytes_outer;                       /* the Kernel instance, slot 0 of a Bytes */
/* gc.c's block header, for the mark test below (a spike reads what it must). */
struct HeapHeader { struct HeapHeader *next; size_t size; int marked; };

static void put(unsigned b) {
    if (*cur_n < cur_max) cur[(*cur_n)++] = (uint8_t)b; else err = 1;
}
static void head(unsigned m, uint64_t n) {
    if (n < 24) { put((m << 5) | (unsigned)n); return; }
    if (n < 256) { put((m << 5) | 24); put((unsigned)n); return; }
    if (n < 65536) { put((m << 5) | 25); put((unsigned)(n >> 8)); put((unsigned)n); return; }
    put((m << 5) | 26); put((unsigned)(n >> 24)); put((unsigned)(n >> 16)); put((unsigned)(n >> 8)); put((unsigned)n);
}

/* Every kept send goes through here. In modes 2 and 3 a fresh 16-byte RawBytes is
   allocated and held in a C local across the send, then checked: a swept-and-reused
   object no longer carries its vtable, length and fill. */
#define FRESH_LEN 16
static oop xsend(oop v, oop sel, int argc, oop *args) {
    sends_n++;
    if (mode < 2) return send(v, sel, argc, args);
    int rooted = mode == 2 || mode == 4;
    oop fresh = gc_allocate(RawBytesVtable, sizeof(struct String) + FRESH_LEN);
    ((struct String *)fresh)->length = FRESH_LEN;
    memset(((struct String *)fresh)->bytes, 0x5a, FRESH_LEN);
    if (rooted) gc_temp_root_push(fresh);
    oop r = send(v, sel, argc, args);
    int collected = 0;
    if (mode >= 3) {
        /* The send's own allocation, stood in for here: under OEVM_GC_STRESS this
           is where the collection runs with `fresh` held only by a C local. A
           different vtable, so a reused slot cannot read as the original. */
        size_t c0 = oevm_heap_stats().gc_cycles;
        oop again = gc_allocate(vt_bytes, sizeof(struct ObjectInstance) + 2 * sizeof(oop));
        ((struct ObjectInstance *)again)->slots[0] = bytes_outer;
        ((struct ObjectInstance *)again)->slots[1] = NULL;
        collected = oevm_heap_stats().gc_cycles != c0;
    }
    int ok = obj_vtable(fresh) == RawBytesVtable && ((struct String *)fresh)->length == FRESH_LEN;
    for (int i = 0; ok && i < FRESH_LEN; i++) ok = ((struct String *)fresh)->bytes[i] == 0x5a;
    /* After a collection a live block carries the mark; a swept one does not,
       whatever its body still reads (the sweep does not scribble). */
    if (ok && collected && !((struct HeapHeader *)fresh - 1)->marked) ok = 0;
    if (!ok) root_lost++;
    if (rooted) gc_temp_roots_count--;        /* pop this level's root */
    return r;
}

static int rank_of(oop v) {
    if (v == NULL) return 0;
    if (v == o_true || v == o_false) return 1;
    if (IS_SMI(v)) return 2;
    oop vt = obj_vtable(v);
    if (vt == LargeIntegerVtable) return 2;
    if (vt == FloatVtable) return 3;
    if (vt == StringVtable) return 4;
    if (vt == SymbolVtable) return 6;
    if (vt == RawBytesVtable) return -2;
    if (mode == 0) {
        if (vt == vt_bytes) return 5;
        if (vt == vt_list) return 7;
        if (vt == vt_map) return 8;
        if (vt == vt_record) return 9;
        return -1;
    }
    oop r = xsend(v, s_valueRank, 0, NULL);
    return IS_SMI(r) ? (int)UNTAG_SMI(r) : -1;
}

static intptr_t smi_send(oop v, oop sel) {
    oop r = xsend(v, sel, 0, NULL);
    if (!IS_SMI(r)) { err = 5; return 0; }
    return UNTAG_SMI(r);
}
static oop idx_send(oop v, oop sel, intptr_t i) {
    oop a = TAG_SMI(i);
    return xsend(v, sel, 1, &a);
}
/* An nsl Array: slot 0 = size SMI, elements at slots 1..n. */
static intptr_t arr_size(oop a) { return UNTAG_SMI(instance_slot((struct ObjectInstance *)a, 0)); }
static oop arr_at(oop a, intptr_t i) { return instance_slot((struct ObjectInstance *)a, (size_t)i); }
/* A nested-class instance (List/Map/Record/Bytes live inside Kernel) keeps its
   `outer` in slot 0; the declared slots follow. */
static oop slot(oop o, size_t i) { return instance_slot((struct ObjectInstance *)o, i + 1); }

static void enc(oop v, int d);

static void enc_map(oop v, int d) {
    int sends = mode != 0;
    intptr_t n = sends ? smi_send(v, s_size) : arr_size(slot(v, 0));
    if (err) return;
    if (n > MAP_MAX) { err = 6; return; }
    uint16_t ks[MAP_MAX], kl[MAP_MAX]; uint8_t idx[MAP_MAX];
    oop keys = sends ? NULL : slot(v, 0);
    oop vals = sends ? NULL : slot(v, 1);
    uint8_t *save_cur = cur; size_t *save_n = cur_n; size_t save_max = cur_max;
    size_t kb_base = kb_n;
    cur = kb_buf; cur_n = &kb_n; cur_max = KB_MAX;
    for (intptr_t i = 1; i <= n; i++) {
        oop k = sends ? idx_send(v, s_keyAt, i) : arr_at(keys, i);
        size_t start = kb_n;
        enc(k, d + 1);
        ks[i - 1] = (uint16_t)start; kl[i - 1] = (uint16_t)(kb_n - start);
        idx[i - 1] = (uint8_t)(i - 1);
    }
    cur = save_cur; cur_n = save_n; cur_max = save_max;
    if (err) return;
    for (intptr_t i = 1; i < n; i++) {
        uint8_t x = idx[i]; intptr_t j = i - 1;
        while (j >= 0) {
            uint8_t y = idx[j];
            size_t lim = kl[x] < kl[y] ? kl[x] : kl[y];
            int c = memcmp(kb_buf + ks[y], kb_buf + ks[x], lim);
            if (c == 0) c = (kl[y] > kl[x]) - (kl[y] < kl[x]);
            if (c <= 0) break;
            idx[j + 1] = y; j--;
        }
        idx[j + 1] = x;
    }
    head(5, (uint64_t)n);
    for (intptr_t i = 0; i < n; i++) {
        uint8_t x = idx[i];
        for (size_t b = 0; b < kl[x]; b++) put(kb_buf[ks[x] + b]);
        oop val = sends ? idx_send(v, s_valueAt, x + 1) : arr_at(vals, x + 1);
        enc(val, d + 1);
        if (err) return;
    }
    kb_n = kb_base;
}

static void enc(oop v, int d) {
    if (err) return;
    if (d > 32) { err = 2; return; }
    int sends = mode != 0;
    int r = rank_of(v);
    switch (r) {
    case 0: put(0xf6); return;
    case 1: put(v == o_true ? 0xf5 : 0xf4); return;
    case 2: {
        int major; uint64_t arg;
        if (IS_SMI(v)) {
            intptr_t x = UNTAG_SMI(v);
            if (x >= 0) { major = 0; arg = (uint64_t)x; } else { major = 1; arg = (uint64_t)(-(x + 1)); }
        } else {
            struct LargeInteger *li = (struct LargeInteger *)v;
            if (li->sign >= 0) { major = 0; arg = li->mag; } else { major = 1; arg = li->mag - 1ULL; }
        }
        head((unsigned)major, arg); return;
    }
    case 3: err = 4; return;
    case 4: {
        struct String *s = (struct String *)v;
        head(3, s->length);
        for (size_t i = 0; i < s->length; i++) put(s->bytes[i]);
        return;
    }
    case 6: {
        const char *nm = symbol_name((struct Symbol *)v);
        size_t n = strlen(nm);
        put(0xd8); put(0x27); head(3, n);
        for (size_t i = 0; i < n; i++) put((uint8_t)nm[i]);
        return;
    }
    case 5: {
        if (sends) {
            intptr_t n = smi_send(v, s_byteSize); if (err) return;
            head(2, (uint64_t)n);
            for (intptr_t i = 1; i <= n; i++) { oop b = idx_send(v, s_u8At, i); put((unsigned)UNTAG_SMI(b)); }
        } else {
            struct String *raw = (struct String *)slot(v, 0);
            head(2, raw->length);
            for (size_t i = 0; i < raw->length; i++) put(raw->bytes[i]);
        }
        return;
    }
    case 7: {
        if (sends) {
            intptr_t n = smi_send(v, s_size); if (err) return;
            head(4, (uint64_t)n);
            for (intptr_t i = 1; i <= n; i++) { enc(idx_send(v, s_at, i), d + 1); if (err) return; }
        } else {
            oop items = slot(v, 0); intptr_t n = arr_size(items);
            head(4, (uint64_t)n);
            for (intptr_t i = 1; i <= n; i++) { enc(arr_at(items, i), d + 1); if (err) return; }
        }
        return;
    }
    case 8: enc_map(v, d); return;
    case 9: {
        put(0xd8); put(0x1b); put(0x82);
        oop tag = sends ? xsend(v, s_tag, 0, NULL) : slot(v, 0);
        enc(tag, d + 1); if (err) return;
        oop fields = sends ? xsend(v, s_fields, 0, NULL) : slot(v, 1);
        enc(fields, d + 1);
        return;
    }
    default: err = 3; return;
    }
}

static long runs[5];
static oop run(oop v, int m) {
    mode = m;
    out_n = 0; kb_n = 0; err = 0; sends_n = 0; root_lost = 0;
    cur = out_buf; cur_n = &out_n; cur_max = OUT_MAX;
    int roots0 = gc_temp_roots_count;
    enc(v, 1);
    if (err) { fprintf(stderr, "VM Error: cbor_native err=%d\n", err); abort(); }
    if (gc_temp_roots_count != roots0) { fprintf(stderr, "VM Error: cbor_native root stack %d != %d\n", gc_temp_roots_count, roots0); abort(); }
    /* First run of each mode reports its send count and, for the fresh-object arms,
       how many fresh objects did not survive their send. */
    if (runs[m]++ == 0 || (m >= 3 && root_lost))
        fprintf(stderr, "cbor_native: mode=%d sends=%ld root lost %ld\n", m, sends_n, root_lost);
    oop raw = gc_allocate(RawBytesVtable, sizeof(struct String) + out_n);
    ((struct String *)raw)->length = out_n;
    memcpy(((struct String *)raw)->bytes, out_buf, out_n);
    gc_temp_root_push(raw);
    oop b = gc_allocate(vt_bytes, sizeof(struct ObjectInstance) + 2 * sizeof(oop));
    ((struct ObjectInstance *)b)->slots[0] = bytes_outer;
    ((struct ObjectInstance *)b)->slots[1] = raw;
    gc_temp_roots_count--;
    return b;
}
static oop prim_encode_sends(oop recv, int argc, oop *args)  { (void)recv; (void)argc; return run(args[0], 1); }
static oop prim_encode_c(oop recv, int argc, oop *args)      { (void)recv; (void)argc; return run(args[0], 0); }
static oop prim_encode_rooted(oop recv, int argc, oop *args) { (void)recv; (void)argc; return run(args[0], 2); }
static oop prim_encode_unrooted(oop recv, int argc, oop *args) { (void)recv; (void)argc; return run(args[0], 3); }
static oop prim_encode_guard(oop recv, int argc, oop *args)    { (void)recv; (void)argc; return run(args[0], 4); }

#define ARMS 5
static struct Closure cl[ARMS]; static struct Binding bd[ARMS][1];
static struct Vtable vt[ARMS]; static struct Object sing[ARMS];
static int wired;

static oop find_sel(oop start_vt, const char *name) {
    oop curr = start_vt;
    while (curr != NULL && !IS_SMI(curr)) {
        struct Vtable *v = (struct Vtable *)curr;
        struct Binding *bs = vtable_bindings((oop)v);
        for (size_t i = 0; i < v->size; i++) {
            struct Symbol *s = (struct Symbol *)binding_symbol(&bs[i]);
            if (s != NULL && obj_vtable((oop)s) == SymbolVtable && strcmp(symbol_name(s), name) == 0)
                return (oop)s;
        }
        curr = vtable_delegate((oop)v);
    }
    fprintf(stderr, "VM Error: cbor_native: selector %s not found\n", name); abort();
}

/* installNativeCbor: aList map: aMap record: aRecord bytes: aBytes true: t false: f cbor: c arm: n
   arm 2 = B (sends), 3 = C (all C), 4 = D (rooted fresh), 5 = E (unrooted fresh) */
oop cbor_native_acquire(int argc, oop *args) {
    if (argc != 8) { fprintf(stderr, "VM Error: cbor_native_acquire argc=%d\n", argc); abort(); }
    if (!wired) {
        vt_list = obj_vtable(args[0]); vt_map = obj_vtable(args[1]);
        vt_record = obj_vtable(args[2]); vt_bytes = obj_vtable(args[3]);
        bytes_outer = instance_slot((struct ObjectInstance *)args[3], 0);
        o_true = args[4]; o_false = args[5];
        s_valueRank = find_sel(vt_list, "valueRank");
        s_size = find_sel(vt_list, "size");  s_at = find_sel(vt_list, "at:");
        s_keyAt = find_sel(vt_map, "keyAt:"); s_valueAt = find_sel(vt_map, "valueAt:");
        s_tag = find_sel(vt_record, "tag");   s_fields = find_sel(vt_record, "fields");
        s_byteSize = find_sel(vt_bytes, "byteSize"); s_u8At = find_sel(vt_bytes, "u8At:");
        s_encode = find_sel(obj_vtable(args[6]), "encode:");
        void *entry[ARMS] = { (void *)prim_encode_sends, (void *)prim_encode_c,
                              (void *)prim_encode_rooted, (void *)prim_encode_unrooted,
                              (void *)prim_encode_guard };
        for (int a = 0; a < ARMS; a++) {
            cl[a].base.vtable = PrimitiveClosureVtable;
            cl[a].entry_point = entry[a];
            bd[a][0].symbol = s_encode; bd[a][0].method = (oop)&cl[a];
            vt[a].base.vtable = VtableVtable; vt[a].delegate = NULL;
            vt[a].bindings = bd[a]; vt[a].capacity = 1; vt[a].size = 1; vt[a].is_nested = 0;
            sing[a].vtable = (oop)&vt[a];
        }
        wired = 1;
    }
    intptr_t arm = UNTAG_SMI(args[7]);
    if (arm < 2 || arm > 6) { fprintf(stderr, "VM Error: cbor_native arm=%ld\n", (long)arm); abort(); }
    return (oop)&sing[arm - 2];
}
#endif /* OEVM_CBOR_NATIVE */
