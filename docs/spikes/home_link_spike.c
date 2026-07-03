/*
 * home_link_spike.c — L0 lexical-link (home_ctx) representation spike (throwaway).
 *
 * The §10 spike validated the DYNAMIC link (prev_frame): a base-relative index that
 * survives stack realloc. That link is always SAME-STACK, so "relative" is unambiguous
 * and forced — and it is what gives us growable per-fiber stacks. No fork there.
 *
 * This spike isolates the OPEN fork (fiber_l0_frame_abi.md §6): the LEXICAL link
 * (home_ctx), which a closure follows to read an OUTER frame's variable. Unlike the
 * dynamic link, a closure's home can live on a DIFFERENT fiber's stack, or OUTLIVE the
 * home frame entirely (escape). It asks: which home-link representation is actually safe?
 *
 * Three candidates (the §6 options):
 *   BARE_REL   — home = relative offset only         (draft Opt 1, as written)
 *   OFFSET_SID — home = {stack_id, relative offset}  (Opt 1 hardened with stack identity)
 *   REIFY      — on escape, copy home's live var to a heap box; home = heap ptr (Opt 2/Toit)
 *
 * It does NOT use a bytecode VM (the §10 spike already proved the dispatch mechanics);
 * it models exactly the link + its resolution under the two cases that bite: cross-fiber
 * invocation, and a closure outliving its home frame (the gen_tag-guarded case today).
 *
 * Build & run:
 *   cc -O2 -Wall -Wextra -o /tmp/home_link_spike docs/spikes/home_link_spike.c && /tmp/home_link_spike
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

/* A per-fiber stack: growable, indices stable across realloc (the §6a property). */
typedef struct { intptr_t *data; long len, sp; int id; } Stack;

static Stack stacks[4];
static long  grow_total;

static void st_init(Stack *s, int id) {
    s->len = 4; s->sp = 0; s->id = id;
    s->data = calloc((size_t)s->len, sizeof(intptr_t));
}
static long st_push(Stack *s, intptr_t v) {            /* returns the slot index */
    if (s->sp >= s->len) {
        s->len *= 2;
        s->data = realloc(s->data, (size_t)s->len * sizeof(intptr_t));
        grow_total++;
    }
    long i = s->sp; s->data[s->sp++] = v; return i;
}

/* A closure capturing one outer variable under each candidate representation. */
typedef struct {
    /* BARE_REL / OFFSET_SID */
    long rel_off;     /* index of the captured var within its home stack */
    int  home_sid;    /* which stack the home lives on (OFFSET_SID only)  */
    /* REIFY */
    intptr_t heap_box; /* a heap-stable copy made at escape (Opt 2)        */
    int  reified;
} Closure;

/* Resolution of "read my captured outer variable", per candidate.
 * `cur` is the fiber the closure is being INVOKED on (may differ from its home). */
static intptr_t read_bare(const Closure *c, const Stack *cur) {
    return cur->data[c->rel_off];                 /* relative to WHATEVER stack runs us */
}
static intptr_t read_sid(const Closure *c) {
    return stacks[c->home_sid].data[c->rel_off];  /* relative to the identified stack */
}
static intptr_t read_reify(const Closure *c) {
    return c->heap_box;                           /* heap-stable, independent of stacks */
}

int main(void) {
    const intptr_t X = 42;     /* the outer variable the closure should always read */
    int ok = 1;

    st_init(&stacks[0], 0);    /* fiber A */
    st_init(&stacks[1], 1);    /* fiber B */
    Stack *A = &stacks[0], *B = &stacks[1];

    /* Fiber A builds a frame whose local (the captured var) = X, at index `xi`. */
    st_push(A, 0); st_push(A, 0);          /* filler below the frame */
    long xi = st_push(A, X);               /* the captured outer variable */

    /* Fiber B happens to hold a DIFFERENT value at the SAME index — this is the trap
       a bare relative offset walks into when invoked on the wrong stack. */
    st_push(B, 111); st_push(B, 222);
    long bxi = st_push(B, 999);
    (void)bxi;                             /* bxi == xi by construction */

    /* The closure captures A's var, three ways. */
    Closure c = {0};
    c.rel_off  = xi;
    c.home_sid = A->id;
    /* REIFY copies the live value to a heap box AT ESCAPE (here: at capture-that-escapes). */
    c.heap_box = A->data[xi];
    c.reified  = 1;

    /* Force A's stack to grow several times while the frame is live (the §6a property:
       indices stay valid, the data pointer moves). Do the same to B. */
    for (int i = 0; i < 50; i++) { st_push(A, 7); st_push(B, 8); }

    /* ---- Demo 1: SAME-fiber read after growth (sanity; relative is fine here) ---- */
    intptr_t r1 = read_bare(&c, A);
    int p1 = (r1 == X);
    ok &= p1;
    printf("Demo 1  same-fiber, post-growth   : bare-rel reads %ld (want 42)   %s   (%ld total grows)\n",
           (long)r1, p1 ? "PASS" : "FAIL", grow_total);

    /* ---- Demo 2: CROSS-fiber read (closure created on A, invoked on B) ---- */
    intptr_t r_bare  = read_bare(&c, B);   /* resolves against B's stack — WRONG base */
    intptr_t r_sid   = read_sid(&c);       /* resolves against A via stack id  */
    intptr_t r_reify = read_reify(&c);     /* heap box */
    int bare_hazard  = (r_bare != X);      /* the hazard we expect to see */
    int sid_ok       = (r_sid == X);
    int reify_ok     = (r_reify == X);
    ok &= (bare_hazard && sid_ok && reify_ok);
    printf("Demo 2  cross-fiber (A->B)        : bare-rel=%ld %s | offset+sid=%ld %s | reify=%ld %s\n",
           (long)r_bare,  bare_hazard ? "HAZARD-CONFIRMED" : "(no hazard?!)",
           (long)r_sid,   sid_ok   ? "PASS" : "FAIL",
           (long)r_reify, reify_ok ? "PASS" : "FAIL");

    /* ---- Demo 3: closure OUTLIVES its home frame (escape past pop) ----
       A returns: pop the frame, then a later unrelated frame overwrites slot xi. */
    A->sp = xi;                 /* pop A's frame (frame storage now dead/reusable) */
    st_push(A, 777);            /* a new frame reuses slot xi with a different value */
    intptr_t e_sid   = read_sid(&c);    /* offset into A: now sees the STALE/reused slot */
    intptr_t e_reify = read_reify(&c);  /* heap box: still correct */
    int sid_dangles  = (e_sid != X);    /* the hazard offset+sid still can't avoid */
    int reify_lives  = (e_reify == X);
    ok &= (sid_dangles && reify_lives);
    printf("Demo 3  escape past home pop      : offset+sid=%ld %s | reify=%ld %s\n",
           (long)e_sid,   sid_dangles ? "DANGLES (as predicted)" : "(survived?!)",
           (long)e_reify, reify_lives ? "PASS" : "FAIL");

    printf("\nVerdict:\n");
    printf("  BARE relative offset  (draft Opt 1)      : UNSAFE cross-fiber — reads the wrong stack.\n");
    printf("  offset + stack-id     (Opt 1 hardened)   : safe cross-fiber, UNSAFE on escape-past-pop.\n");
    printf("  reify-on-escape       (Opt 2 / Toit)     : safe in BOTH — cost = a heap copy at escape.\n");
    printf("\n%s\n", ok ? "SPIKE PASS — hazards reproduced and the safe representation identified."
                        : "SPIKE FAIL — a hazard/safety expectation did not hold.");
    return ok ? 0 : 1;
}
