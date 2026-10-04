/* #297 SPIKE — THROWAWAY, branch 297-cbor-spike. #included by platform.c so it
   costs no source-list edits. Two arms of Cbor>>encode: in C:

   ARM A (cborEncode:kinds:) — the whole walk in C. `kinds` is a 4-slot Array of
   the List/Map/Record/Bytes classes; C reads their ivars by slot index (slot 0 is
   `outer`, so the first ivar is slot 1). Two passes: count, allocate once, write.
   Map keys are encoded into a static scratch stack (spike-only) and sorted by
   their encoded bytes (RFC 8949 §4.2 bytewise, as Cbor>>sortPairsBytewise:).

   ARM B (cborEmit:into:at: / cborHead:len:into:at: / bytesCopy:size:into:at: /
   bytesCompare:size:with:size:) — leaf prims; the walk stays in nsl (CborB).

   Error codes (answered as SMIs): -1 not a value, -2 depth exceeded,
   -3 spike limit (map too wide / scratch full). */

typedef struct { uint8_t *p; size_t n; size_t cap; } CbW;   /* p == NULL: count only */

static inline void cbw_byte(CbW *w, uint8_t b) {
    if (w->p && w->n < w->cap) w->p[w->n] = b;
    w->n++;
}
static inline void cbw_bytes(CbW *w, const uint8_t *src, size_t len) {
    if (w->p && w->n + len <= w->cap) memcpy(w->p + w->n, src, len);
    w->n += len;
}
static void cbw_head(CbW *w, unsigned m, uint64_t arg) {
    unsigned mt = m << 5;
    if (arg < 24) { cbw_byte(w, (uint8_t)(mt | arg)); return; }
    if (arg < 0x100ULL) { cbw_byte(w, (uint8_t)(mt | 24)); cbw_byte(w, (uint8_t)arg); return; }
    if (arg < 0x10000ULL) { cbw_byte(w, (uint8_t)(mt | 25));
        cbw_byte(w, (uint8_t)(arg >> 8)); cbw_byte(w, (uint8_t)arg); return; }
    if (arg < 0x100000000ULL) { cbw_byte(w, (uint8_t)(mt | 26));
        for (int s = 3; s >= 0; s--) cbw_byte(w, (uint8_t)(arg >> (8 * s))); return; }
    cbw_byte(w, (uint8_t)(mt | 27));
    for (int s = 7; s >= 0; s--) cbw_byte(w, (uint8_t)(arg >> (8 * s)));
}

/* A leaf: 1 = encoded, 0 = not a leaf, -1 = not a value (NaN). allow_raw: a bare
   RawBytes is a Bytes payload (arm B passes `rawLeaf`); arm A refuses it (rank -2). */
static int cb_leaf(CbW *w, oop v, int allow_raw) {
    if (v == NULL) { cbw_byte(w, 0xf6); return 1; }
    /* Path B has TWO trues: true_sym_obj (== results) and the image literal
       (oimg_singleton[1]). Spike finding — valueRank: checks only the first. */
    if (v == true_symbol || v == oimg_singleton[1]) { cbw_byte(w, 0xf5); return 1; }
    if (v == false_symbol || v == oimg_singleton[2]) { cbw_byte(w, 0xf4); return 1; }
    if (IS_SMI(v)) {
        intptr_t x = UNTAG_SMI(v);
        if (x >= 0) cbw_head(w, 0, (uint64_t)x); else cbw_head(w, 1, (uint64_t)(-(x + 1)));
        return 1;
    }
    oop vt = obj_vtable(v);
    if (LargeIntegerVtable && vt == LargeIntegerVtable) {
        struct LargeInteger *li = (struct LargeInteger *)v;
        if (li->sign >= 0) cbw_head(w, 0, li->mag); else cbw_head(w, 1, li->mag - 1ULL);
        return 1;
    }
    if (FloatVtable && vt == FloatVtable) {
        double d = ((struct Float *)v)->value;
        if (d != d) return -1;
        float f = (float)d; uint16_t h;
        if (double_to_half(d, &h) && half_to_double(h) == d) {
            cbw_byte(w, 0xf9); cbw_byte(w, (uint8_t)(h >> 8)); cbw_byte(w, (uint8_t)h);
        } else if ((double)f == d) {
            uint32_t u; memcpy(&u, &f, 4); cbw_byte(w, 0xfa);
            for (int s = 3; s >= 0; s--) cbw_byte(w, (uint8_t)(u >> (8 * s)));
        } else {
            uint64_t u; memcpy(&u, &d, 8); cbw_byte(w, 0xfb);
            for (int s = 7; s >= 0; s--) cbw_byte(w, (uint8_t)(u >> (8 * s)));
        }
        return 1;
    }
    if (StringVtable && vt == StringVtable) {
        struct String *s = (struct String *)v;
        cbw_head(w, 3, s->length); cbw_bytes(w, s->bytes, s->length); return 1;
    }
    if (SymbolVtable && vt == SymbolVtable) {
        struct Symbol *s = (struct Symbol *)v;
        cbw_head(w, 6, 39); cbw_head(w, 3, s->length);
        cbw_bytes(w, (const uint8_t *)s->name, s->length); return 1;
    }
    if (allow_raw && RawBytesVtable && vt == RawBytesVtable) {
        struct String *s = (struct String *)v;
        cbw_head(w, 2, s->length); cbw_bytes(w, s->bytes, s->length); return 1;
    }
    return 0;
}

/* ---------------- ARM A ---------------- */

typedef struct { oop list, map, record, bytes; } CbKinds;

#define CB_SCRATCH 4096
#define CB_MAXMAP 64
static uint8_t cb_scratch[CB_SCRATCH];
static size_t cb_sp;

static inline oop cb_slot(oop o, int i) { return ((struct ObjectInstance *)o)->slots[i]; }
static inline intptr_t cb_asize(oop arr) { return UNTAG_SMI(cb_slot(arr, 0)); }

static int cb_enc(CbW *w, oop v, int d, const CbKinds *k);

static int cb_cmp(const uint8_t *a, size_t na, const uint8_t *b, size_t nb) {
    size_t lim = na < nb ? na : nb;
    int c = memcmp(a, b, lim);
    if (c) return c < 0 ? -1 : 1;
    return na < nb ? -1 : (na > nb ? 1 : 0);
}

static int cb_map(CbW *w, oop v, int d, const CbKinds *k) {
    oop keys = cb_slot(v, 1), vals = cb_slot(v, 2);
    intptr_t n = cb_asize(keys);
    int rc;
    cbw_head(w, 5, (uint64_t)n);
    if (w->p == NULL) {   /* count: keys first, then values — the nsl error order */
        for (intptr_t i = 1; i <= n; i++) if ((rc = cb_enc(w, cb_slot(keys, i), d + 1, k))) return rc;
        for (intptr_t i = 1; i <= n; i++) if ((rc = cb_enc(w, cb_slot(vals, i), d + 1, k))) return rc;
        return 0;
    }
    if (n > CB_MAXMAP) return -3;
    struct { size_t off, len; oop val; } e[CB_MAXMAP];
    size_t sp0 = cb_sp;
    for (intptr_t i = 0; i < n; i++) {
        CbW c = { NULL, 0, 0 };
        if ((rc = cb_enc(&c, cb_slot(keys, i + 1), d + 1, k))) { cb_sp = sp0; return rc; }
        if (cb_sp + c.n > CB_SCRATCH) { cb_sp = sp0; return -3; }
        size_t off = cb_sp; cb_sp += c.n;
        CbW kw = { cb_scratch + off, 0, c.n };
        if ((rc = cb_enc(&kw, cb_slot(keys, i + 1), d + 1, k))) { cb_sp = sp0; return rc; }
        e[i].off = off; e[i].len = c.n; e[i].val = cb_slot(vals, i + 1);
    }
    for (intptr_t i = 1; i < n; i++) {   /* stable insertion sort, as the nsl */
        __typeof__(e[0]) t = e[i]; intptr_t j = i - 1;
        while (j >= 0 && cb_cmp(cb_scratch + e[j].off, e[j].len, cb_scratch + t.off, t.len) > 0) {
            e[j + 1] = e[j]; j--;
        }
        e[j + 1] = t;
    }
    for (intptr_t i = 0; i < n; i++) {
        cbw_bytes(w, cb_scratch + e[i].off, e[i].len);
        if ((rc = cb_enc(w, e[i].val, d + 1, k))) { cb_sp = sp0; return rc; }
    }
    cb_sp = sp0;
    return 0;
}

static int cb_enc(CbW *w, oop v, int d, const CbKinds *k) {
    if (d > 32) return -2;
    int r = cb_leaf(w, v, 0);
    if (r == 1) return 0;
    if (r < 0) return -1;
    oop vt = obj_vtable(v);
    int rc;
    if (vt == k->bytes) {
        struct String *s = (struct String *)cb_slot(v, 1);
        cbw_head(w, 2, s->length); cbw_bytes(w, s->bytes, s->length); return 0;
    }
    if (vt == k->list) {
        oop arr = cb_slot(v, 1); intptr_t n = cb_asize(arr);
        cbw_head(w, 4, (uint64_t)n);
        for (intptr_t i = 1; i <= n; i++) if ((rc = cb_enc(w, cb_slot(arr, i), d + 1, k))) return rc;
        return 0;
    }
    if (vt == k->record) {
        cbw_head(w, 6, 27); cbw_head(w, 4, 2);
        if ((rc = cb_enc(w, cb_slot(v, 1), d + 1, k))) return rc;
        return cb_enc(w, cb_slot(v, 2), d + 1, k);
    }
    if (vt == k->map) return cb_map(w, v, d, k);
    return -1;
}

static oop cb_class_vt(oop c) { return boundclass_is(c) ? cb_slot(c, 0) : c; }

static oop cb_encode_a(oop v, oop kinds) {
    CbKinds k = { cb_class_vt(cb_slot(kinds, 1)), cb_class_vt(cb_slot(kinds, 2)),
                  cb_class_vt(cb_slot(kinds, 3)), cb_class_vt(cb_slot(kinds, 4)) };
    CbW c = { NULL, 0, 0 };
    cb_sp = 0;
    int rc = cb_enc(&c, v, 1, &k);
    if (rc) return TAG_SMI(rc);
    oop b = gc_allocate(RawBytesVtable, sizeof(struct String) + c.n);
    ((struct String *)b)->length = c.n;
    CbW w = { ((struct String *)b)->bytes, 0, c.n };
    cb_sp = 0;
    rc = cb_enc(&w, v, 1, &k);
    if (rc) return TAG_SMI(rc);
    return b;
}

/* ---------------- ARM B ---------------- */

/* Write via `fn` into RawBytes buf at 0-based i: new index, -1 no room, -2 not a leaf. */
static oop cb_into(oop buf, oop at, int (*fn)(CbW *, void *), void *ctx) {
    struct String *s = (struct String *)buf;
    size_t i = (size_t)UNTAG_SMI(at);
    CbW c = { NULL, 0, 0 };
    if (!fn(&c, ctx)) return TAG_SMI(-2);
    if (i + c.n > s->length) return TAG_SMI(-1);
    CbW w = { s->bytes + i, 0, c.n };
    fn(&w, ctx);
    return TAG_SMI((intptr_t)(i + c.n));
}
static int cb_emit_fn(CbW *w, void *ctx) { return cb_leaf(w, *(oop *)ctx, 1) == 1; }
static int cb_head_fn(CbW *w, void *ctx) {
    intptr_t *mn = (intptr_t *)ctx; cbw_head(w, (unsigned)mn[0], (uint64_t)mn[1]); return 1;
}

/* ---------------- ARM A' (OEOM emulation) ----------------
   Same C walk as A, but a container is NOT recognised by comparing its vtable
   against the four kinds: it is dispatched the way an OEOM native binding would
   be — lookup_method_cached(vtable, selector) per container value, then an
   indirect call. The selector stands in for the per-class native method (each of
   List/Map/Record/Bytes defines `valueRank`, Object answers -1 → not a value).
   The method→function map is a <=4-way compare that real OEOM would replace with
   closure_entry(binding) — so A' slightly OVER-states the dispatch cost. */
extern oop lookup_method_cached(oop receiver_vtable, oop selector, oop *out_method, oop *out_defining_vt);

typedef int (*CbFn)(CbW *, oop, int, const CbKinds *);
static oop a2_sel;
static oop a2_meth[4];
static CbFn a2_fn[4];
static int cb_enc2(CbW *w, oop v, int d, const CbKinds *k);

static int cb2_bytes(CbW *w, oop v, int d, const CbKinds *k) {
    (void)d; (void)k;
    struct String *s = (struct String *)cb_slot(v, 1);
    cbw_head(w, 2, s->length); cbw_bytes(w, s->bytes, s->length); return 0;
}
static int cb2_list(CbW *w, oop v, int d, const CbKinds *k) {
    oop arr = cb_slot(v, 1); intptr_t n = cb_asize(arr); int rc;
    cbw_head(w, 4, (uint64_t)n);
    for (intptr_t i = 1; i <= n; i++) if ((rc = cb_enc2(w, cb_slot(arr, i), d + 1, k))) return rc;
    return 0;
}
static int cb2_record(CbW *w, oop v, int d, const CbKinds *k) {
    int rc;
    cbw_head(w, 6, 27); cbw_head(w, 4, 2);
    if ((rc = cb_enc2(w, cb_slot(v, 1), d + 1, k))) return rc;
    return cb_enc2(w, cb_slot(v, 2), d + 1, k);
}
static int cb2_map(CbW *w, oop v, int d, const CbKinds *k) {
    oop keys = cb_slot(v, 1), vals = cb_slot(v, 2);
    intptr_t n = cb_asize(keys);
    int rc;
    cbw_head(w, 5, (uint64_t)n);
    if (w->p == NULL) {
        for (intptr_t i = 1; i <= n; i++) if ((rc = cb_enc2(w, cb_slot(keys, i), d + 1, k))) return rc;
        for (intptr_t i = 1; i <= n; i++) if ((rc = cb_enc2(w, cb_slot(vals, i), d + 1, k))) return rc;
        return 0;
    }
    if (n > CB_MAXMAP) return -3;
    struct { size_t off, len; oop val; } e[CB_MAXMAP];
    size_t sp0 = cb_sp;
    for (intptr_t i = 0; i < n; i++) {
        CbW c = { NULL, 0, 0 };
        if ((rc = cb_enc2(&c, cb_slot(keys, i + 1), d + 1, k))) { cb_sp = sp0; return rc; }
        if (cb_sp + c.n > CB_SCRATCH) { cb_sp = sp0; return -3; }
        size_t off = cb_sp; cb_sp += c.n;
        CbW kw = { cb_scratch + off, 0, c.n };
        if ((rc = cb_enc2(&kw, cb_slot(keys, i + 1), d + 1, k))) { cb_sp = sp0; return rc; }
        e[i].off = off; e[i].len = c.n; e[i].val = cb_slot(vals, i + 1);
    }
    for (intptr_t i = 1; i < n; i++) {
        __typeof__(e[0]) t = e[i]; intptr_t j = i - 1;
        while (j >= 0 && cb_cmp(cb_scratch + e[j].off, e[j].len, cb_scratch + t.off, t.len) > 0) {
            e[j + 1] = e[j]; j--;
        }
        e[j + 1] = t;
    }
    for (intptr_t i = 0; i < n; i++) {
        cbw_bytes(w, cb_scratch + e[i].off, e[i].len);
        if ((rc = cb_enc2(w, e[i].val, d + 1, k))) { cb_sp = sp0; return rc; }
    }
    cb_sp = sp0;
    return 0;
}

static int cb_enc2(CbW *w, oop v, int d, const CbKinds *k) {
    if (d > 32) return -2;
    int r = cb_leaf(w, v, 0);
    if (r == 1) return 0;
    if (r < 0) return -1;
    oop m = NULL, dv = NULL;
    lookup_method_cached(obj_vtable(v), a2_sel, &m, &dv);
    if (m == NULL) return -1;
    if (m == a2_meth[0]) return a2_fn[0](w, v, d, k);
    if (m == a2_meth[1]) return a2_fn[1](w, v, d, k);
    if (m == a2_meth[2]) return a2_fn[2](w, v, d, k);
    if (m == a2_meth[3]) return a2_fn[3](w, v, d, k);
    return -1;
}

/* kinds is used ONCE, to learn which method each class binds (the stand-in for
   the image carrying a native binding); the walk itself never reads it. */
static oop cb_encode_a2(oop v, oop kinds, oop sel) {
    if (a2_sel != sel) {
        static const CbFn fns[4] = { cb2_list, cb2_map, cb2_record, cb2_bytes };
        for (int i = 0; i < 4; i++) {
            oop m = NULL, dv = NULL;
            lookup_method_cached(cb_class_vt(cb_slot(kinds, i + 1)), sel, &m, &dv);
            if (m == NULL) return TAG_SMI(-4);
            a2_meth[i] = m; a2_fn[i] = fns[i];
        }
        a2_sel = sel;
    }
    CbKinds k = { 0 };
    CbW c = { NULL, 0, 0 };
    cb_sp = 0;
    int rc = cb_enc2(&c, v, 1, &k);
    if (rc) return TAG_SMI(rc);
    oop b = gc_allocate(RawBytesVtable, sizeof(struct String) + c.n);
    ((struct String *)b)->length = c.n;
    CbW w = { ((struct String *)b)->bytes, 0, c.n };
    cb_sp = 0;
    rc = cb_enc2(&w, v, 1, &k);
    if (rc) return TAG_SMI(rc);
    return b;
}
