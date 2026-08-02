/*
 * frame_abi.h — L0 FROZEN frame/link encoding (de-recursify rewrite).
 *
 * Centralises, in one place, the relative<->address conversion for:
 *   - the DYNAMIC link  (prev_frame): base-relative, always same-stack;
 *   - the LEXICAL link  (home_ctx):   discriminated, TOTAL 2-WAY (Option B
 *        / §6.1) — NULL (no lexical home) | relative SMI (on-stack, current
 *        fiber).
 *
 * Contract: docs/fiber_l0_frame_abi.md.  STATUS: L0 contract, LIVE. The dynamic
 * link (prev_frame) has been relative since L1; the lexical link (home_ctx) is
 * relative as of de-recursify Phase 2 (self-migration + flip): every home_ctx
 * encode site uses home_link_encode_relative and every read routes through
 * vm.c's home_ctx_frame chokepoint. Growable stacks (Phase 3) rely only on the
 * relative encoding (links survive a realloc-copy untouched). A non-NULL,
 * non-SMI home_ctx is impossible by construction — it can only mean a
 * corrupt frame link (see vm.c's home_ctx_frame chokepoint).
 * Header-only (static inline) so the L0 unit test links nothing.
 *
 * Validated standalone (the L0 acceptance proof, §7e):
 *   cc -std=c99 -Isrc tests/test_frame_abi.c -o /tmp/test_frame_abi && /tmp/test_frame_abi
 * (Not yet wired into ns-oevm/tools/run_tests_oevm.py — that runner lives on a
 *  different repo's master; wire it in when this branch integrates.)
 */
#ifndef OEVM_FRAME_ABI_H
#define OEVM_FRAME_ABI_H

#include "oevm.h"   /* oop, struct Object/MethodContext, IS_SMI/TAG_SMI/UNTAG_SMI */

/* ---- Dynamic link (prev_frame): base-relative, same-stack ---------------
 * Encoding:  NULL             => no caller (stack-bottom sentinel)
 *            TAG_SMI(offset)  => caller MethodContext at stack_base[offset]
 * `offset` is the word index of the frame from its owning fiber stack base.
 * Survives a stack realloc with ZERO fixups: only stack_base changes; every
 * stored offset stays valid (the §10-spike property, here against the real
 * struct). offset 0 is a legitimate frame (TAG_SMI(0) is non-NULL & IS_SMI),
 * so it is distinct from the NULL "no caller" sentinel. */
static inline oop frame_link_encode(const oop *stack_base,
                                    const struct MethodContext *frame) {
    if (frame == NULL) return NULL;
    return TAG_SMI((const oop *)frame - stack_base);
}
static inline struct MethodContext *frame_link_decode(oop *stack_base, oop link) {
    if (link == NULL) return NULL;
    return (struct MethodContext *)(stack_base + UNTAG_SMI(link));
}

/* ---- Lexical link (home_ctx): discriminated, TOTAL 2-way (Option B, §6.1) -
 * Encoding:  NULL             => no lexical home (a method's own closure)
 *            TAG_SMI(offset)  => on-stack home on the CURRENT fiber (relative)
 * These two cases are exhaustive: a non-NULL, non-SMI home_ctx cannot arise
 * by construction and indicates a corrupt frame link, not a third case.
 * The IS_SMI test is the TOTAL discriminator (NULL is neither). This replaced
 * the legacy "SMI home_ctx == stop the walk" convention at vm.c:507 / :1156;
 * the live mechanism here is the relative-SMI convention, "SMI == follow it". */
static inline int home_link_is_none(oop home)     { return home == NULL; }
static inline int home_link_is_relative(oop home) { return home != NULL &&  IS_SMI(home); }

static inline oop home_link_encode_relative(const oop *stack_base,
                                            const struct MethodContext *frame) {
    return TAG_SMI((const oop *)frame - stack_base);
}
/* Precondition: home_link_is_relative(home). */
static inline struct MethodContext *home_link_decode_frame(oop *stack_base, oop home) {
    return (struct MethodContext *)(stack_base + UNTAG_SMI(home));
}

/* ---- Heap lexical context (deferred-lift closure capture) ----------------
 * The heap lexical context for escape-safe closure capture: built eagerly at
 * closure-creation time (deferred-lift), rather than boxed lazily off of a
 * stack frame when it escapes. L1 builds the boxing, the ContextVtable, and
 * GC tracing. A Context boxes the live outer variables of an escaping frame
 * so they outlive its stack slot — the substrate of full upward (Toit-class)
 * closures (§6b).
 * Layout mirrors an Object payload (vtable first) so the GC treats it as an
 * ordinary heap object. */
struct Context {
    struct Object base;        /* vtable = ContextVtable (L1); GC-traced as heap obj */
    uint32_t home_generation;  /* stale-home-frame generation tag for the boxed frame */
    uint32_t num_slots;        /* number of boxed outer variables */
    oop      outer;            /* enclosing lexical link: Context | TAG_SMI | NULL */
    oop      slots[];          /* the boxed outer variables */
};

#endif /* OEVM_FRAME_ABI_H */
