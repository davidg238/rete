/* Unit test for the global method lookup cache (src/dispatch.c).
   Build (must define OEVM_CACHE_STATS so the stats accessor exists):
     gcc -std=c99 -Isrc -DOEVM_CACHE_STATS tests/test_method_cache.c \
         src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/solidified_image.c \
         -lm -o /tmp/tmc && /tmp/tmc
   Selectors/methods are matched by pointer identity, so we use distinct
   heap pointers as fake symbols/methods — no Symbol layout needed. */
#include "../src/oevm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

extern oop VtableVtable;

/* Build a RAM vtable (not in the image blob) with the given bindings. */
static struct Vtable *mk_vtable(struct Binding *bindings, size_t n) {
    struct Vtable *vt = calloc(1, sizeof(struct Vtable));
    vt->base.vtable = VtableVtable;
    vt->delegate = NULL;
    vt->bindings = bindings;
    vt->capacity = n;
    vt->size = n;
    vt->is_nested = 0;
    return vt;
}

int main(void) {
    /* What VM init does: a lookup tells a bytecode method from any other by this vtable. */
    extern oop BytecodeClosureVtable;
    extern const struct Vtable Vtable_BytecodeClosureVtable;
    BytecodeClosureVtable = (oop)&Vtable_BytecodeClosureVtable;
    /* Two distinct "selectors" and "methods" as opaque pointers. */
    int selA, selB;
    struct Object methA = {0}, methB = {0};   /* read as objects: a lookup asks a match its level */
    struct Binding bindings[2] = {
        { .symbol = (oop)&selA, .method = (oop)&methA },
        { .symbol = (oop)&selB, .method = (oop)&methB },
    };
    struct Vtable *vt = mk_vtable(bindings, 2);

    method_cache_flush();
    struct MethodCacheStats s0 = method_cache_stats();

    /* First lookup of selA: MISS then FILL. */
    oop m = NULL, dv = NULL;
    oop r = lookup_method_cached((oop)vt, (oop)&selA, &m, &dv, LOOKUP_ORDINARY);
    assert(r == (oop)vt);
    assert(m == (oop)&methA);
    assert(dv == (oop)vt);
    struct MethodCacheStats s1 = method_cache_stats();
    assert(s1.misses == s0.misses + 1);
    assert(s1.fills  == s0.fills + 1);

    /* Second lookup of selA: HIT. */
    m = NULL; dv = NULL;
    r = lookup_method_cached((oop)vt, (oop)&selA, &m, &dv, LOOKUP_ORDINARY);
    assert(r == (oop)vt && m == (oop)&methA);
    struct MethodCacheStats s2 = method_cache_stats();
    assert(s2.hits == s1.hits + 1);

    /* Unknown selector: MISS, returns NULL, NOT cached (no fill). */
    int selUnknown;
    m = (oop)1; dv = (oop)1;
    r = lookup_method_cached((oop)vt, (oop)&selUnknown, &m, &dv, LOOKUP_ORDINARY);
    assert(r == NULL && m == NULL && dv == NULL);  /* out_defining_vt cleared on DNU */
    struct MethodCacheStats s3 = method_cache_stats();
    assert(s3.fills == s2.fills);   /* misses are never cached */

    /* Flush clears: a subsequent lookup of selA must MISS again. */
    method_cache_flush();
    struct MethodCacheStats s4 = method_cache_stats();
    assert(s4.flushes == s3.flushes + 1);
    m = NULL; dv = NULL;
    r = lookup_method_cached((oop)vt, (oop)&selA, &m, &dv, LOOKUP_ORDINARY);
    assert(r == (oop)vt && m == (oop)&methA);
    struct MethodCacheStats s5 = method_cache_stats();
    assert(s5.misses == s4.misses + 1);   /* was flushed, so miss not hit */

    /* bind:to: must flush so a replaced method is never served stale. */
    extern oop primitive_vtable_bind(oop receiver, int argc, oop *args);
    method_cache_flush();
    /* Fill the cache for selA -> methA. */
    m = NULL; dv = NULL;
    lookup_method_cached((oop)vt, (oop)&selA, &m, &dv, LOOKUP_ORDINARY);
    assert(m == (oop)&methA);
    struct MethodCacheStats sb0 = method_cache_stats();
    /* Replace selA's method via the real bind path. */
    struct Object methA2 = {0};
    oop bind_args[2] = { (oop)&selA, (oop)&methA2 };
    primitive_vtable_bind((oop)vt, 2, bind_args);
    struct MethodCacheStats sb1 = method_cache_stats();
    assert(sb1.flushes == sb0.flushes + 1);   /* bind flushed the cache */
    /* Next lookup must return the NEW method, not the cached old one. */
    m = NULL; dv = NULL;
    r = lookup_method_cached((oop)vt, (oop)&selA, &m, &dv, LOOKUP_ORDINARY);
    assert(r == (oop)vt && m == (oop)&methA2);

    /* rete#372: levels. A protected method answers a send to self, never an ordinary send, and the
       self send's cache entry (keyed selector | 1) is never handed to the ordinary one. A private
       method answers only LOOKUP_ANY (OP_SEND_PRIVATE); the walk passes it by otherwise. */
    int selP, selQ;
    struct CompiledMethod cmP = {.access = ACCESS_PROTECTED}, cmQ = {.access = ACCESS_PRIVATE};
    struct Closure clP = {.base = {BytecodeClosureVtable}, .entry_point = &cmP};
    struct Closure clQ = {.base = {BytecodeClosureVtable}, .entry_point = &cmQ};
    struct Binding lv[2] = {
        { .symbol = (oop)&selP, .method = (oop)&clP },
        { .symbol = (oop)&selQ, .method = (oop)&clQ },
    };
    struct Vtable *lvt = mk_vtable(lv, 2);
    method_cache_flush();
    m = NULL; dv = NULL;
    assert(lookup_method_cached((oop)lvt, (oop)&selP, &m, &dv, LOOKUP_ORDINARY) == NULL && m == NULL);
    assert(lookup_method_cached((oop)lvt, (oop)&selP, &m, &dv, LOOKUP_SELF) == (oop)lvt && m == (oop)&clP);
    struct MethodCacheStats sl0 = method_cache_stats();
    assert(lookup_method_cached((oop)lvt, (oop)&selP, &m, &dv, LOOKUP_SELF) == (oop)lvt);   /* a hit */
    assert(method_cache_stats().hits == sl0.hits + 1);
    m = NULL;
    assert(lookup_method_cached((oop)lvt, (oop)&selP, &m, &dv, LOOKUP_ORDINARY) == NULL && m == NULL);
    assert(lookup_method_cached((oop)lvt, (oop)&selQ, &m, &dv, LOOKUP_SELF) == NULL);
    assert(lookup_method_cached((oop)lvt, (oop)&selQ, &m, &dv, LOOKUP_ORDINARY) == NULL);
    assert(lookup_defining_vtable((oop)lvt, (oop)&selQ, &m, LOOKUP_ANY) == (oop)lvt && m == (oop)&clQ);

    printf("test_method_cache: OK\n");
    return 0;
}
