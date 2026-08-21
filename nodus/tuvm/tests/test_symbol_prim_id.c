/* rete#226 — a Symbol carries its own primitive id, so nothing has to look one up.
   Build (Model A link set, same as test_debugger.c):
     gcc -std=c99 -Isrc tests/test_symbol_prim_id.c <ENG_A> -lm -o /tmp/tspi

   WHAT THIS REPLACED, and why the replacement is the point. rete#222 was an unbounded
   probe in a selector->prim-id CACHE: 256 slots on the C6, 4096 on the host, never
   evicting, never growing. Bounding the probe stopped the hang; it did not stop the table
   from filling ~17 s into every boot, after which every miss cost a linear scan of all 154
   PRIM_NAMES. The table is gone now. Classification is a pure property of the selector,
   decided ONCE where the Symbol is made, so a send reads a field.

   The old test asserted the cache did not spin at capacity. There is no capacity to be at,
   so what is asserted here instead is the invariant that makes the cache unnecessary:
   EVERY Symbol the VM creates knows its id before anyone asks. A Symbol that reaches a
   send with an unset id is the bug this file exists to catch, and it would present as a
   primitive silently ceasing to be intercepted. */
#include "../src/oevm.h"
#include "../src/oimg.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>

int selector_prim_id(struct Symbol *sym);
oop symbol_new(const char *bytes, size_t len);

/* dispatch.c's enum is file-local and its first member is pinned at 0 by its own comment
   ("PRIM_NONE (0) means 'not an intercepted primitive'"). */
#define PRIM_NOT_A_PRIM 0

int main(void) {
    /* 1. A selector that names a primitive knows so AT CREATION — not at first send.
          Read straight off the struct, because that is the whole claim: no lookup ran. */
    oop a = symbol_new("basicAt:", strlen("basicAt:"));
    assert(a != NULL);
    int pid = ((struct Symbol *)a)->prim_id;
    assert(pid != PRIM_NOT_A_PRIM);
    assert(selector_prim_id((struct Symbol *)a) == pid);

    /* 2. An ordinary app selector is decided too — "not a primitive" is an ANSWER here,
          not the absence of one, which is what lets the send path trust the field. */
    oop b = symbol_new("appSelector:", strlen("appSelector:"));
    assert(((struct Symbol *)b)->prim_id == PRIM_NOT_A_PRIM);
    assert(selector_prim_id((struct Symbol *)b) == PRIM_NOT_A_PRIM);

    /* 3. Two Symbols with the same name are independent objects — symbols are NOT
          interned (`platform asSymbol:` mints a fresh one per CBOR tag-39 decode, tuvm#75)
          — and must agree. Under the old pointer-keyed cache these were two cache entries;
          now the answer travels with each object. */
    oop a2 = symbol_new("basicAt:", strlen("basicAt:"));
    assert(a2 != a);
    assert(((struct Symbol *)a2)->prim_id == pid);

    /* 4. Scale is a non-event. Ten thousand distinct selectors, far past the 256 slots
          that used to be a hang and the 4096 that hid it from every host suite, each
          answered from its own object with no shared table to fill. */
    for (int i = 0; i < 10000; i++) {
        char nm[32];
        snprintf(nm, sizeof nm, "sel%d:", i);
        oop s = symbol_new(nm, strlen(nm));
        assert(s != NULL);
        assert(((struct Symbol *)s)->prim_id == PRIM_NOT_A_PRIM);
    }
    /* and the first one still answers, having been evicted by nothing */
    assert(((struct Symbol *)a)->prim_id == pid);

    /* 5. The name still round-trips: prim_id shares the header with `length`, so a
          narrowing that ate a byte of the name would show up here first. */
    const char *long_name = "aRatherLongKeywordSelector:withAnother:andMore:";
    oop c = symbol_new(long_name, strlen(long_name));
    assert(strcmp(symbol_name((struct Symbol *)c), long_name) == 0);
    assert(((struct Symbol *)c)->length == strlen(long_name));

    printf("test_symbol_prim_id: PASSED (id decided at creation, no table)\n");
    return 0;
}
