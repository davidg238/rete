/* rete#222 / #192 / #213 — the selector prim-id cache must never spin.
   Build (Model A link set, same as test_debugger.c), with a DELIBERATELY TINY table:
     gcc -std=c99 -Isrc -DPRIM_CACHE_SIZE=8 tests/test_prim_cache.c <ENG_A> -lm -o /tmp/tpc

   WHAT THIS PINS, and why it is a unit test rather than a board gate. `selector_prim_id`
   is consulted on every send whose selector is a Symbol, and it was an open-addressed
   table that never evicted, never grew, and probed `while (slot != NULL)` with no bound.
   Once every slot held a selector the next UNCACHED selector had no NULL to stop at: the
   reactor span forever in C, below the interpreter, at 100% CPU — no fault, no panic, no
   output, and the pulse mailbox filling behind it. That is the wedge #192, #213 and #222
   all describe, and on a C6 (PRIM_CACHE_SIZE=256) three different innocuous edits reached
   it within six check-ins while the host's 4096-slot table never filled in any suite.
   THE SIZE IS THE ONLY REASON THE HOST WAS BLIND, so this test takes the size away.

   A REGRESSION HERE HANGS RATHER THAN FAILS. That is the nature of the bug, so the runner
   gives this binary a timeout and treats expiry as failure; do not "fix" a timeout by
   raising it. */
#include "../src/oevm.h"
#include "../src/oimg.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>

int selector_prim_id(struct Symbol *sym);

/* dispatch.c's enum is file-local, and its FIRST member is pinned at 0 by its own comment
   ("PRIM_NONE (0) means 'not an intercepted primitive'"). Naming the literal here rather
   than exporting the whole enum keeps the test's dependency to the one fact it needs. */
#define PRIM_NOT_A_PRIM 0
void prim_cache_forensics(unsigned *used, unsigned *cap, uint32_t *evictions);

/* A fabricated interned selector. The classifier reads exactly two things — the POINTER
   (its hash and its identity) and symbol_name() — so a stack fixture is a faithful stand-in
   and needs no VM bootstrap. The padding keeps each fixture's address distinct in the
   `>> 4` the hash uses. */
struct FakeSym { struct Symbol s; char storage[24]; };

static void name_it(struct FakeSym *f, const char *nm) {
    f->s.base.vtable = NULL;
    f->s.length = strlen(nm);
    memcpy(f->s.name, nm, f->s.length + 1);
}

int main(void) {
    unsigned used = 0, cap = 0;
    uint32_t evictions = 0;
    prim_cache_forensics(&used, &cap, &evictions);
    assert(used == 0);
    assert(cap == 8);   /* the -D took effect; without it this test proves nothing */

    /* Fill every slot with selectors that are NOT primitives, which is the ordinary case:
       an app's own method names outnumber the prim list many times over. */
    static struct FakeSym filler[8];
    char nm[24];
    for (int i = 0; i < 8; i++) {
        snprintf(nm, sizeof nm, "appSelector%d:", i);
        name_it(&filler[i], nm);
        assert(selector_prim_id(&filler[i].s) == PRIM_NOT_A_PRIM);
    }
    prim_cache_forensics(&used, &cap, &evictions);
    assert(used == cap);   /* full — the state that used to be a hang */

    /* THE REGRESSION ITSELF: one more never-seen selector, against a full table.
       Before the fix this call does not return. */
    static struct FakeSym overflow_sym;
    name_it(&overflow_sym, "oneTooMany:");
    assert(selector_prim_id(&overflow_sym.s) == PRIM_NOT_A_PRIM);

    /* And the cache must still CLASSIFY correctly while full — degrading to a slower
       lookup is acceptable, answering the wrong primitive id is not. */
    static struct FakeSym real_prim;
    name_it(&real_prim, "basicAt:");
    int pid = selector_prim_id(&real_prim.s);
    assert(pid != PRIM_NOT_A_PRIM);
    assert(selector_prim_id(&real_prim.s) == pid);   /* stable across a re-ask */

    /* A full table must never grow past its bound, however many selectors arrive. */
    static struct FakeSym more[64];
    for (int i = 0; i < 64; i++) {
        snprintf(nm, sizeof nm, "late%d:", i);
        name_it(&more[i], nm);
        (void)selector_prim_id(&more[i].s);
        prim_cache_forensics(&used, &cap, &evictions);
        assert(used <= cap);
    }

    /* The degraded regime must be REPORTED, not inferred: a table that silently began
       throwing entries away is how a slowdown hides. */
    assert(evictions > 0);

    printf("test_prim_cache: PASSED (table %u/%u, %u evictions, no spin at capacity)\n",
           used, cap, (unsigned)evictions);
    return 0;
}
