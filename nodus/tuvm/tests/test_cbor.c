/* Unit test for the CBOR integer leaf primitive `cborEncodeInt:` (prim id 97,
   src/platform.c platform_dispatch — Rung 3 Task 2). Encodes an Integer (SMI or
   LargeInteger) as CBOR major type 0 (>=0) or major type 1 (<0), shortest form,
   per RFC 8949. The golden hex vectors are authored by python cbor2:
   `cbor2.dumps(v).hex()` — they are authoritative; do not edit them.

   Build (Model B link set, mirrors tests/test_raw_bytes.c):
     gcc -std=c99 -Isrc tests/test_cbor.c \
         src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c \
         src/platform.c src/serial.c src/udp.c src/mirror.c src/space.c \
         src/handles.c src/backend/posix/platform_posix.c \
         src/backend/posix/serial_posix.c src/backend/posix/udp_posix.c \
         src/backend/posix/pulse_posix.c src/kernel_boot.c -lm -o /tmp/test_cbor
         && /tmp/test_cbor */
#include "../src/oevm.h"
#include "../src/prim_ids.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>

extern void kernel_boot(void);
extern void heap_init(void);
extern int  platform_dispatch(int pid, int argc, oop *args, oop *out);
extern oop  largeint_alloc(int sign, uint64_t mag);
extern oop  RawBytesVtable;
extern oop  gc_allocate(oop vtable, size_t size);

/* golden pairs from python: cbor2.dumps(v).hex() */
static void test_cbor_encode_int(void) {
    struct { const char *hex; int is_large; int sign; uint64_t mag; long smi; } cases[] = {
        {"00",0,0,0, 0},          {"17",0,0,0, 23},        {"1818",0,0,0, 24},
        {"18ff",0,0,0, 255},      {"190100",0,0,0, 256},   {"19ffff",0,0,0, 65535},
        {"1a00010000",0,0,0, 65536}, {"1affffffff",0,0,0, 4294967295L},
        {"1b0000000100000000",1,+1,4294967296ULL,0},
        {"1bffffffffffffffff",1,+1,18446744073709551615ULL,0}, /* +(2^64-1) */
        {"20",0,0,0, -1},         {"3863",0,0,0, -100},
        {"3bfffffffffffffffe",1,-1,18446744073709551615ULL,0}, /* -(2^64-1) => arg 2^64-2 */
    };
    for (size_t i=0;i<sizeof cases/sizeof cases[0];i++) {
        oop v = cases[i].is_large
              ? largeint_alloc(cases[i].sign, cases[i].mag)
              : TAG_SMI(cases[i].smi);
        oop out=NULL; int ok = platform_dispatch(PRIM_pf_cborEncodeInt, 1, &v, &out);
        assert(ok && out && obj_vtable(out)==RawBytesVtable);
        struct String *b=(struct String*)out;
        char got[40]={0}; for(size_t k=0;k<b->length;k++) sprintf(got+2*k,"%02x",b->bytes[k]);
        assert(strcmp(got, cases[i].hex)==0 && "int encode mismatch");
    }
    printf("test_cbor_encode_int OK\n");
}

/* golden pairs from python: cbor2.dumps(float(x)).hex() — shortest of f16/f32/f64
   that round-trips the double exactly (CBOR major type 7). */
static void test_cbor_encode_float(void) {
    struct { const char *hex; double v; } cases[] = {
        {"f90000", 0.0}, {"f93c00", 1.0}, {"f93e00", 1.5},   /* f16-exact */
        {"f98000", -0.0},                                    /* f16 negative zero */
        {"f90001", ldexp(1.0,-24)},                          /* f16 smallest subnormal 2^-24 */
        {"fa47c35000", 100000.0},                            /* f32-exact, not f16 */
        {"fb3ff199999999999a", 1.1},                         /* f64-only */
        {"f97c00", INFINITY}, {"f9fc00", -INFINITY},
    };
    for (size_t i=0;i<sizeof cases/sizeof cases[0];i++) {
        oop f = float_alloc(FloatVtable); ((struct Float*)f)->value = cases[i].v;
        oop out=NULL; int ok = platform_dispatch(PRIM_pf_cborEncodeFloat,1,&f,&out);
        assert(ok && obj_vtable(out)==RawBytesVtable);
        struct String *b=(struct String*)out; char got[40]={0};
        for(size_t k=0;k<b->length;k++) sprintf(got+2*k,"%02x",b->bytes[k]);
        assert(strcmp(got,cases[i].hex)==0 && "float encode mismatch");
    }
    printf("test_cbor_encode_float OK\n");
}

/* Rung 3 Task 4: the four CBOR decode leaves (prims 99-102). Reads result-array
   slots directly (slots[i]) rather than via instance_slot() — the leaves build a
   fresh heap indexable (never in-blob), so the two are equivalent here. */
static oop mkbytes(const uint8_t *p, size_t n) {
    oop b = gc_allocate(RawBytesVtable, sizeof(struct String) + n);
    ((struct String*)b)->length = n;
    if (n) memcpy(((struct String*)b)->bytes, p, n);
    return b;
}
static long smi(oop x){ return (long)UNTAG_SMI(x); }
static oop slot(oop o, size_t i){ return ((struct ObjectInstance*)o)->slots[i]; }

/* utf8Valid convenience: build the byte buffer, dispatch, return the resulting
   symbol. Input and result are handled within one call so the caller never holds
   an unrooted heap byte-buffer across a later allocation (this test runs green
   under OEVM_GC_STRESS=1, which collects before every allocation). */
static oop utf8_of(const uint8_t *p, size_t n) {
    oop b = mkbytes(p, n), o = NULL;
    platform_dispatch(PRIM_pf_utf8Valid, 1, &b, &o);
    return o;
}

static void test_cbor_decode_leaves(void) {
    /* NOTE ON GC-STRESS DISCIPLINE: every heap byte-buffer input is consumed
       (dispatched, and its result-array slots read) BEFORE the next mkbytes /
       allocation, so no unrooted heap input is held across a collection. The
       leaves themselves pin their intermediate result oops (readFloat's Float,
       readHead's large argv) as GC temp roots, so decode is safe under
       OEVM_GC_STRESS=1. */
    oop TRUE_ = (oop)&true_sym_obj, FALSE_ = (oop)&false_sym_obj;

    /* head: 0x1a 00 01 00 00  => major 0, arg 65536, next 5. A single dispatch is
       GC-stress-safe: the leaf copies the head bytes into locals before its own
       internal indexable_new. (Each input gets a fresh buffer, never reused across
       a second dispatch, since it would be swept during the first dispatch's
       internal allocation under OEVM_GC_STRESS=1.) */
    uint8_t h[]={0x1a,0,1,0,0}; oop hb=mkbytes(h,5);
    oop a[2]={hb,TAG_SMI(0)}, out=NULL;
    assert(platform_dispatch(PRIM_pf_cborReadHead,2,a,&out) && out!=TAG_SMI(-1));
    assert(smi(slot(out,1))==0);                           /* major */
    assert(oevm_int_to_double(slot(out,2))==65536.0);      /* arg */
    assert(smi(slot(out,3))==5);                           /* next */

    /* head index past end => TAG_SMI(-1) (own fresh buffer) */
    uint8_t hp[]={0x1a,0,1,0,0}; oop hpb=mkbytes(hp,5);
    oop apast[2]={hpb,TAG_SMI(5)}, outp=NULL;
    assert(platform_dispatch(PRIM_pf_cborReadHead,2,apast,&outp) && outp==TAG_SMI(-1));

    /* head large-arg: 0x1b 80 00 00 00 00 00 00 00 => major 0, ai 27, arg 2^63,
       next 9. On a 64-bit host 2^63 > INTPTR_MAX>>1, so argv is a heap
       LargeInteger held live across indexable_new(3) — exercises the temp-root
       pin (Task-4 GC BUG 2; without the pin this fails under OEVM_GC_STRESS=1). */
    uint8_t hl[]={0x1b,0x80,0,0,0,0,0,0,0}; oop hlb=mkbytes(hl,9);
    oop al[2]={hlb,TAG_SMI(0)}, outl=NULL;
    assert(platform_dispatch(PRIM_pf_cborReadHead,2,al,&outl) && outl!=TAG_SMI(-1));
    assert(smi(slot(outl,1))==0);                          /* major */
    assert(oevm_int_to_double(slot(outl,2))==9223372036854775808.0);  /* arg 2^63 */
    assert(smi(slot(outl,3))==9);                          /* next */

    /* head truncated: 0x1a with only 2 more bytes => TAG_SMI(-1) */
    uint8_t ht[]={0x1a,0,1}; oop htb=mkbytes(ht,3);
    oop at[2]={htb,TAG_SMI(0)}, outt=NULL;
    assert(platform_dispatch(PRIM_pf_cborReadHead,2,at,&outt) && outt==TAG_SMI(-1));

    /* intFromMajor: major 0, arg 5 => +5 */
    oop a0[2]={TAG_SMI(0), TAG_SMI(5)}, o0=NULL;
    assert(platform_dispatch(PRIM_pf_cborIntFromMajor,2,a0,&o0) && o0);
    assert(oevm_int_to_double(o0)==5.0);

    /* intFromMajor: major 1, arg 0 => -1 */
    oop a1[2]={TAG_SMI(1), TAG_SMI(0)}, o1=NULL;
    assert(platform_dispatch(PRIM_pf_cborIntFromMajor,2,a1,&o1) && o1);
    assert(oevm_int_to_double(o1)==-1.0);

    /* intFromMajor: major 1 arg (2^64-1) => -2^64 => out of range (sentinel) */
    oop big = largeint_alloc(+1, 18446744073709551615ULL);
    oop a2[2]={TAG_SMI(1), big}, o2=NULL;
    int r = platform_dispatch(PRIM_pf_cborIntFromMajor,2,a2,&o2);
    assert(r && o2==CBOR_OOR_SENTINEL);   /* leaf signals OOR; nsl maps to fail */

    /* readFloat: 0xfa 47 c3 50 00 => 100000.0, next 5. The Float leaf pins its
       result Float across indexable_new(2) (Task-4 GC BUG 1, unconditional). */
    uint8_t fb[]={0xfa,0x47,0xc3,0x50,0x00}; oop fbb=mkbytes(fb,5);
    oop af[2]={fbb,TAG_SMI(0)}, of=NULL;
    assert(platform_dispatch(PRIM_pf_cborReadFloat,2,af,&of) && of!=TAG_SMI(-1));
    assert(((struct Float*)slot(of,1))->value==100000.0);
    assert(smi(slot(of,2))==5);

    /* readFloat: f16 0xf9 3c 00 => 1.0, next 3 */
    uint8_t f16[]={0xf9,0x3c,0x00}; oop f16b=mkbytes(f16,3);
    oop af16[2]={f16b,TAG_SMI(0)}, of16=NULL;
    assert(platform_dispatch(PRIM_pf_cborReadFloat,2,af16,&of16) && of16!=TAG_SMI(-1));
    assert(((struct Float*)slot(of16,1))->value==1.0);
    assert(smi(slot(of16,2))==3);

    /* readFloat truncated => TAG_SMI(-1) */
    uint8_t ftr[]={0xfb,0,0}; oop ftrb=mkbytes(ftr,3);
    oop aftr[2]={ftrb,TAG_SMI(0)}, oftr=NULL;
    assert(platform_dispatch(PRIM_pf_cborReadFloat,2,aftr,&oftr) && oftr==TAG_SMI(-1));

    /* utf8Valid: good "é" (c3 a9) valid; bad (c3 28) invalid; overlong 2-byte
       (c0 80), surrogate (ed a0 80), > U+10FFFF (f4 90 80 80) all rejected;
       valid 4-byte emoji (f0 9f 98 80 = U+1F600) accepted. */
    { uint8_t good[]={0xc3,0xa9};        assert(utf8_of(good,2)==TRUE_); }
    { uint8_t bad[] ={0xc3,0x28};        assert(utf8_of(bad,2) ==FALSE_); }
    { uint8_t ov[]  ={0xc0,0x80};        assert(utf8_of(ov,2)  ==FALSE_); }
    { uint8_t su[]  ={0xed,0xa0,0x80};   assert(utf8_of(su,3)  ==FALSE_); }
    { uint8_t hi[]  ={0xf4,0x90,0x80,0x80}; assert(utf8_of(hi,4)==FALSE_); }
    { uint8_t em[]  ={0xf0,0x9f,0x98,0x80}; assert(utf8_of(em,4)==TRUE_); }

    printf("test_cbor_decode_leaves OK\n");
}

/* Rung 3 Task 6: `asSymbol:` (prim 103) — String -> genuine (non-interned)
   Symbol. Build a String receiver, dispatch, assert the result is a Symbol whose
   name bytes match.
   rete#190 stage 2: the Symbol's name bytes are now INLINE, so the prim copies
   them out of the String AFTER its single gc_allocate. In the VM that is safe
   because args live in a scanned VM-stack register; HERE the receiver is a C
   local and the GC does not scan the C stack, so this test roots it itself.
   Do NOT read the temp root as something the gate proves: removing it still
   passes under OEVM_GC_STRESS=1 (measured), because the collector is
   non-moving and a swept object's bytes stay readable in the arena — the
   failure would be a silent read of dead memory, not a crash. The root is here
   to make the invariant hold by construction, which is the only way it can be
   established, and the previous version of this comment claimed the opposite
   mechanism ("bytes copied into the Symbol's malloc'd name") for a malloc that
   no longer exists. */
static void test_cbor_as_symbol(void) {
    extern oop StringVtable, SymbolVtable;
    extern oop gc_allocate(oop vtable, size_t size);
    extern void gc_temp_root_push(oop o);
    extern int gc_temp_roots_count;
    const char *txt = "reading"; size_t n = strlen(txt);
    oop s = gc_allocate(StringVtable, sizeof(struct String) + n);
    ((struct String*)s)->length = n; memcpy(((struct String*)s)->bytes, txt, n);
    int saved_roots = gc_temp_roots_count;
    gc_temp_root_push(s);
    oop out = NULL;
    int ok = platform_dispatch(PRIM_pf_asSymbol, 1, &s, &out);
    assert(ok && out && obj_vtable(out) == SymbolVtable);
    struct Symbol *sym = (struct Symbol*)out;
    assert(sym->length == n && memcmp(sym->name, txt, n) == 0);
    gc_temp_roots_count = saved_roots;
    printf("test_cbor_as_symbol OK\n");
}

int main(void) {
    heap_init();
    kernel_boot();
    test_cbor_encode_int();
    test_cbor_encode_float();
    test_cbor_decode_leaves();
    test_cbor_as_symbol();
    printf("test_cbor: OK\n");
    return 0;
}
