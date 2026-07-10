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

int main(void) {
    heap_init();
    kernel_boot();
    test_cbor_encode_int();
    test_cbor_encode_float();
    printf("test_cbor: OK\n");
    return 0;
}
