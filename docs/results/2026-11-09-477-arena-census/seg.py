# rete#477 spike: size-segregated arena. Large blocks (rounded >= OEVM_SEG_LARGE) live in a region growing
# DOWN from heap_end; small blocks keep bins/big_free/bump growing UP. Applied to a scratch gc.c only.
import sys, re
p = sys.argv[1]; s = open(p).read()
def sub(old, new, count=1):
    global s
    n = s.count(old)
    assert n == count, (old[:60], n)
    s = s.replace(old, new)

sub("static uint8_t *heap_end  = NULL;   /* one past the arena */",
    """static uint8_t *heap_end  = NULL;   /* one past the arena */
#ifndef OEVM_SEG_LARGE
#define OEVM_SEG_LARGE 192
#endif
static uint8_t *large_lo = NULL;                 /* #477 spike: lowest large block; region is [large_lo, heap_end) */
static struct HeapHeader *large_free = NULL;     /* first-fit list of dead large-region runs */""")
sub("    heap_end  = heap_base + OEVM_HEAP_SIZE;\n",
    "    heap_end  = heap_base + OEVM_HEAP_SIZE;\n    large_lo = heap_end; large_free = NULL;\n")
sub("if (p < heap_base + sizeof(struct HeapHeader) || p >= heap_top) return 0;",
    "if (p < heap_base + sizeof(struct HeapHeader) || p >= heap_end) return 0;\n    if (p >= heap_top && p < large_lo + sizeof(struct HeapHeader)) return 0;")
sub("    if (heap_top + need <= heap_end) {", """#ifndef OEVM_SEG_RESERVE
#define OEVM_SEG_RESERVE 0
#endif
    if (heap_top + need <= large_lo && heap_top + need <= heap_end - OEVM_SEG_RESERVE) {""")

# large allocator, placed before gc_allocate
sub("OEVM_HOT oop gc_allocate(oop vtable, size_t size) {",
"""static oop large_try_alloc(oop vtable, size_t rounded, size_t need) {
    for (struct HeapHeader **pp = &large_free; *pp; pp = hh_next(*pp)) {
        struct HeapHeader *h = *pp;
        if (hh_size(h) < rounded) continue;
        *pp = *hh_next(h);
        size_t leftover = hh_size(h) - rounded;
        if (leftover >= sizeof(struct HeapHeader) + OEVM_GRANULE) {
            struct HeapHeader *rem = (struct HeapHeader *)((uint8_t *)(h + 1) + rounded);
            hh_init(rem, leftover - sizeof(struct HeapHeader));
            *hh_next(rem) = large_free; large_free = rem;
            hh_init(h, rounded);
        } else hh_clear_mark(h);
        oop obj = (oop)(h + 1); bm_set(bm_granule(obj)); obj->vtable = vtable; return obj;
    }
    if (large_lo - need >= heap_top) {
        large_lo -= need;
        struct HeapHeader *h = (struct HeapHeader *)large_lo; hh_init(h, rounded);
        oop obj = (oop)(h + 1); bm_set(bm_granule(obj)); obj->vtable = vtable; return obj;
    }
    return NULL;
}
static oop seg_try_alloc(oop vtable, size_t rounded, size_t need) {
    oop o;
    if (rounded >= OEVM_SEG_LARGE) { o = large_try_alloc(vtable, rounded, need); return o ? o : arena_try_alloc(vtable, rounded, need); }
    o = arena_try_alloc(vtable, rounded, need); return o ? o : large_try_alloc(vtable, rounded, need);
}
OEVM_HOT oop gc_allocate(oop vtable, size_t size) {""")
sub("oop obj = arena_try_alloc(vtable, rounded, need);", "oop obj = seg_try_alloc(vtable, rounded, need);")
sub("        obj = arena_try_alloc(vtable, rounded, need);", "        obj = seg_try_alloc(vtable, rounded, need);")

# the two non-sweep walks: also cover the large region
sub("""        while (cur < heap_top) {
            struct HeapHeader *h = (struct HeapHeader *)cur;
            if (hh_marked(h)) trace_one((oop)(h + 1));  /* re-enqueue unmarked children */
            cur += sizeof(struct HeapHeader) + hh_size(h);
        }""",
"""        for (int pass = 0; pass < 2; pass++) {
        uint8_t *lim = pass ? heap_end : heap_top; if (pass) cur = large_lo;
        while (cur < lim) {
            struct HeapHeader *h = (struct HeapHeader *)cur;
            if (hh_marked(h)) trace_one((oop)(h + 1));  /* re-enqueue unmarked children */
            cur += sizeof(struct HeapHeader) + hh_size(h);
        }
        }""")
sub("""        while (cur < heap_top) {
            struct HeapHeader *h = (struct HeapHeader *)cur;
            hh_clear_mark(h);
            cur += sizeof(struct HeapHeader) + hh_size(h);
        }""",
"""        for (int pass = 0; pass < 2; pass++) {
        uint8_t *lim = pass ? heap_end : heap_top; if (pass) cur = large_lo;
        while (cur < lim) {
            struct HeapHeader *h = (struct HeapHeader *)cur;
            hh_clear_mark(h);
            cur += sizeof(struct HeapHeader) + hh_size(h);
        }
        }""")
# sweep the large region right after the small one
sub("    heap_live_bytes = swept_live;\n",
"""    {   /* #477 spike: sweep the large region; a dead run at its low edge goes back to the gap */
        large_free = NULL;
        uint8_t *cur = large_lo;
        while (cur < heap_end) {
            struct HeapHeader *h = (struct HeapHeader *)cur;
            uint8_t *next = cur + sizeof(struct HeapHeader) + hh_size(h);
            if (hh_marked(h)) { SEG_CENSUS_LIVE(h); swept_live += sizeof(struct HeapHeader) + hh_size(h); cur = next; continue; }
            bm_clear(bm_granule((oop)(h + 1)));
            uint8_t *run_end = next;
            while (run_end < heap_end) {
                struct HeapHeader *h2 = (struct HeapHeader *)run_end;
                if (hh_marked(h2)) break;
                bm_clear(bm_granule((oop)(h2 + 1)));
                run_end += sizeof(struct HeapHeader) + hh_size(h2);
            }
            if (cur == large_lo) { large_lo = run_end; }
            else { hh_init(h, (size_t)(run_end - cur) - sizeof(struct HeapHeader)); *hh_next(h) = large_free; large_free = h; }
            cur = run_end;
        }
    }
    heap_live_bytes = swept_live;
""")
open(p, "w").write(s)
print("seg patch applied")
