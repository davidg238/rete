/*
 * test_frame_abi.c — L0 acceptance proof (nodus/oevm/docs/archive/fiber_l0_frame_abi.md §7e).
 *
 * Asserts the FROZEN link encoding (src/frame_abi.h) against the REAL
 * struct MethodContext: the dynamic link is a base-relative offset that survives
 * a stack move with zero fixups, and the home_ctx discriminator (Option B) is
 * total 2-way (relative / none, mutually exclusive and exhaustive).
 *
 * Standalone, header-only:
 *   cc -std=c99 -Isrc tests/test_frame_abi.c -o /tmp/test_frame_abi && /tmp/test_frame_abi
 */
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include "../src/frame_abi.h"   /* pulls oevm.h */

/* Phase 2: home_link relative offset survives a base move with zero fixups
   (the property that lets Phase 3 realloc the stack). */
static void test_home_link_survives_base_move(void) {
    oop base_a[64] = {0};
    /* a "frame" at word index 5 of base_a */
    struct MethodContext *frame_a = (struct MethodContext *)&base_a[5];
    oop enc = home_link_encode_relative(base_a, frame_a);
    assert(home_link_is_relative(enc));            /* it's a relative SMI */
    assert(home_link_decode_frame(base_a, enc) == frame_a);

    /* Move the stack: copy words to a fresh, differently-located buffer. */
    oop base_b[64] = {0};
    for (int i = 0; i < 64; i++) base_b[i] = base_a[i];
    struct MethodContext *frame_b_expected = (struct MethodContext *)&base_b[5];
    /* Same stored offset, new base → resolves to the moved frame, no fixup. */
    assert(home_link_decode_frame(base_b, enc) == frame_b_expected);
    printf("test_home_link_survives_base_move: PASS\n");
}

int main(void) {
    printf("L0 frame-ABI encoding unit test (real struct MethodContext)\n");

    oop buf[64];  memset(buf, 0, sizeof buf);
    oop *base = buf;

    /* Two frames live on the stack: a caller at word offset 8, a callee at 20.
       We only use their ADDRESSES (no field deref), so no bounds concern. */
    struct MethodContext *caller = (struct MethodContext *)&base[8];

    /* --- Dynamic link: encode/decode round-trip --- */
    oop link = frame_link_encode(base, caller);
    assert(IS_SMI(link));
    assert(UNTAG_SMI(link) == 8);
    assert(frame_link_decode(base, link) == caller);
    printf("  dynamic link: offset=%ld, decode==caller  OK\n", (long)UNTAG_SMI(link));

    /* --- Bottom sentinel: NULL means "no caller" --- */
    assert(frame_link_encode(base, NULL) == NULL);
    assert(frame_link_decode(base, NULL) == NULL);
    printf("  bottom sentinel (NULL <-> no caller)       OK\n");

    /* --- THE property: the SAME encoded link survives a stack realloc/move
           with ZERO fixups (only the base pointer changes). --- */
    oop buf2[64]; memset(buf2, 0, sizeof buf2);
    memcpy(buf2, buf, sizeof buf);                 /* "realloc moved the stack" */
    oop *base2 = buf2;
    struct MethodContext *moved_caller = (struct MethodContext *)&base2[8];
    assert((void *)moved_caller != (void *)caller);            /* really moved */
    assert(frame_link_decode(base2, link) == moved_caller);    /* link byte-identical */
    printf("  relative link survives stack move          OK  (caller %p -> %p)\n",
           (void *)caller, (void *)moved_caller);

    /* --- Lexical link discriminator (Option B): total 2-way, relative / none --- */
    oop h_rel = home_link_encode_relative(base, caller);
    assert(home_link_is_relative(h_rel));
    assert(!home_link_is_none(h_rel));              /* relative is relative-and-not-none */
    assert(home_link_decode_frame(base, h_rel) == caller);

    assert(home_link_is_none(NULL));
    assert(!home_link_is_relative(NULL));           /* NULL is none-and-not-relative */
    printf("  home_ctx discriminator (relative/none, total 2-way)  OK\n");

    test_home_link_survives_base_move();

    printf("L0 frame-ABI unit test: PASSED\n");
    return 0;
}
