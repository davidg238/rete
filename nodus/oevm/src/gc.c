#include "oevm.h"
#include "frame_abi.h"
#include "platform_backend.h"   /* platform_console_write: the OOM message must reach the console on every backend */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* Heap Header prepended to every general-object allocation. The `next` field
   links FREE blocks within their size-class bin (Task 2); it is unused while a
   block is live. */
struct HeapHeader {
    struct HeapHeader *next;
    size_t size;            /* payload capacity, rounded up to sizeof(oop)      */
    int marked;
};

/* ---- Fixed managed arena (W4) ---------------------------------------- */
#ifndef OEVM_HEAP_SIZE
#define OEVM_HEAP_SIZE (16u * 1024u * 1024u)   /* host default; -D override for embedded */
#endif

/* C99 max-aligned backing store (no C11 _Alignas / max_align_t). */
typedef union { long double ld; void *p; long long ll; } oevm_align_cell;
#define OEVM_HEAP_CELLS (((size_t)OEVM_HEAP_SIZE + sizeof(oevm_align_cell) - 1) / sizeof(oevm_align_cell))
static oevm_align_cell oevm_heap_store[OEVM_HEAP_CELLS];

static uint8_t *heap_base = NULL;   /* = oevm_heap_store; NULL until heap_init */
static uint8_t *heap_top  = NULL;   /* bump cursor */
static uint8_t *heap_end  = NULL;   /* one past the arena */

/* Alloc bitmap: 1 bit per sizeof(oop) granule, set at each live payload start. */
#define OEVM_GRANULE      (sizeof(oop))
#define OEVM_GRANULE_BITS (OEVM_HEAP_SIZE / OEVM_GRANULE)
static uint8_t heap_bitmap[OEVM_GRANULE_BITS / 8u + 1u];

static void bm_set(size_t g)   { heap_bitmap[g >> 3] |= (uint8_t)(1u << (g & 7u)); }
static void bm_clear(size_t g) { heap_bitmap[g >> 3] &= (uint8_t)~(1u << (g & 7u)); }
static int  bm_test(size_t g)  { return (heap_bitmap[g >> 3] >> (g & 7u)) & 1u; }
static size_t bm_granule(oop obj) { return (size_t)((uint8_t *)obj - heap_base) / OEVM_GRANULE; }

/* Flash ROM boundaries (can be updated dynamically for XIP simulation) */
uintptr_t flash_rom_start = 0;
uintptr_t flash_rom_end = 0;

/* Segregated free-lists. Bin k (k < FREE_BIN_COUNT-1) holds blocks of capacity
   exactly k*sizeof(oop). The last bin is a catch-all for larger blocks, served
   first-fit by capacity. */
#define FREE_BIN_COUNT 64
static struct HeapHeader *free_bins[FREE_BIN_COUNT];

/* ---- Heap telemetry (G3/M2, https://github.com/davidg238/nsl-tuvm/issues/3) --------------------------------
   Fragmentation early-warning counters for the fixed embedded arena. Kept as
   plain static counters (not gated behind a -D flag): the whole point is that
   firmware can call oevm_heap_stats() at any time to see how close the arena
   is to a fragmentation OOM. live_bytes/high_water/gc_cycles are maintained
   incrementally (a couple of integer ops on the hot alloc path, a running sum
   during the already-mandatory sweep walk); largest_free_block is computed on
   demand inside oevm_heap_stats() itself, since stats are read rarely and
   walking the free bins is not something the hot alloc path should ever pay
   for. See heap_init() for the counter reset and gc_collect()'s sweep phase
   for the live_bytes recompute. */
static size_t heap_live_bytes = 0;   /* header+payload bytes live as of the last completed sweep, plus allocations since */
static size_t heap_high_water = 0;   /* max live_bytes ever observed (monotonic since boot) */
static size_t heap_gc_cycles  = 0;   /* completed collections since boot */

/* rete#190: C-heap accounting. NOT the GC arena — that is heap_live_bytes above.
   The arena is statically sized and was never under pressure; the leak was here. */
static struct OevmCHeapStats cheap;

/* The size header. A union with uint64_t rather than a bare size_t: malloc returns
   8-aligned storage, and on a 32-bit target (RV32, the C6) a 4-byte header would
   hand back 4-aligned payloads. That misaligns every double stored through the
   pointer and faults on the TARGET ONLY — invisible on the 64-bit host, which is
   the worst possible failure shape. C99 has no max_align_t, hence uint64_t. */
union CHeapBlock {
    size_t   size;
    uint64_t _align;
};

void *oevm_malloc(size_t n) {
    union CHeapBlock *b = malloc(sizeof(union CHeapBlock) + n);
    if (b == NULL) { cheap.fails++; return NULL; }
    b->size = n;
    cheap.allocs++;
    MPROF_ALLOC(MPROF_HEAP_CHEAP, n);
    cheap.out_bytes += n;
    if (cheap.out_bytes > cheap.out_high) cheap.out_high = cheap.out_bytes;
    return (void *)(b + 1);
}

void oevm_free(void *p) {
    if (p == NULL) return;
    union CHeapBlock *b = ((union CHeapBlock *)p) - 1;
    cheap.out_bytes -= b->size;
    cheap.frees++;
    free(b);
}

void *oevm_realloc(void *p, size_t n) {
    if (p == NULL) return oevm_malloc(n);
    union CHeapBlock *b = ((union CHeapBlock *)p) - 1;
    size_t old = b->size;
    union CHeapBlock *nb = realloc(b, sizeof(union CHeapBlock) + n);
    if (nb == NULL) { cheap.fails++; return NULL; }   /* `b` is still valid and still counted */
    nb->size = n;
    /* written as subtract-then-add, NOT `+= n - old`: the latter relies on size_t
       wraparound when shrinking, which is correct but reads as a bug */
    cheap.out_bytes = cheap.out_bytes - old + n;
    if (cheap.out_bytes > cheap.out_high) cheap.out_high = cheap.out_bytes;
    cheap.allocs++;
    /* The NEW size, attributed as a fresh allocation: the column is cumulative
       "bytes caused", not a live-set delta (design D2). */
    MPROF_ALLOC(MPROF_HEAP_CHEAP, n);
    return (void *)(nb + 1);
}

struct OevmCHeapStats oevm_cheap_stats(void) { return cheap; }

/* Called from gc_allocate's two success paths (fresh bump, free-bin reuse)
   with `need` = sizeof(struct HeapHeader) + rounded payload — the exact bytes
   that just became live. Two integer ops: the hot-path budget this brief
   requires. */
static void heap_stats_on_alloc(size_t need) {
    /* The arena's single accounting choke point, so it is also the single
       attribution point (design 2026-08-12, D3). `need` includes the header and
       the rounding, which is what the caller actually consumed. */
    MPROF_ALLOC(MPROF_HEAP_ARENA, need);
    heap_live_bytes += need;
    if (heap_live_bytes > heap_high_water) heap_high_water = heap_live_bytes;
}

static size_t bin_index(size_t rounded) {
    size_t k = rounded / OEVM_GRANULE;
    return (k < (FREE_BIN_COUNT - 1)) ? k : (FREE_BIN_COUNT - 1);
}

/* Initializes (or resets) the arena — call once at startup, or before a test
   for isolation. Not a no-op if already initialized. */
void heap_init(void) {
    heap_base = (uint8_t *)oevm_heap_store;
    heap_top  = heap_base;
    heap_end  = heap_base + OEVM_HEAP_SIZE;
    memset(heap_bitmap, 0, sizeof heap_bitmap);
    for (size_t i = 0; i < FREE_BIN_COUNT; i++) free_bins[i] = NULL;
    heap_live_bytes = 0;
    heap_high_water = 0;
    heap_gc_cycles  = 0;
    /* Model A/B parity guard: the arena must not overlap the read-only image
       ranges, or mark()'s is_read_only/is_in_blob fences would mis-classify it.
       Ranges may be unset (0/NULL) at first init; only check when set.
       Note: the oimg blob is malloc-backed and cannot overlap the BSS arena,
       so only the flash ROM range needs checking here. */
    if (flash_rom_end > flash_rom_start) {
        uintptr_t a0 = (uintptr_t)heap_base, a1 = (uintptr_t)heap_end;
        if (!(a1 <= flash_rom_start || a0 >= flash_rom_end)) {
            fprintf(stderr, "VM GC Error: arena overlaps flash ROM range\n");
            abort();
        }
    }
}

/* Overridable out-of-memory handler. Default aborts; only reached by
   gc_allocate after a full collection still cannot satisfy the request.
   The message goes through platform_console_write — the seam every backend
   implements (and the one a wire logger would hook) — because stderr is NOT
   wired to the console on Zephyr: a boot OOM printed there dies invisibly
   while OpenThread keeps the node attached, which is exactly how the dongle
   passed as "attached but silent" through rungs 7-8. Host builds keep the
   stderr copy so shell redirections/expectations are unchanged. */
static oop default_oom(size_t size) {
    char msg[112];
    int n = snprintf(msg, sizeof msg,
                     "VM GC Error: Out of Memory (requested %zu, used %td of %u)\n",
                     size, (ptrdiff_t)(heap_top - heap_base), (unsigned)OEVM_HEAP_SIZE);
    if (n > 0) {
        platform_console_write(msg, (size_t)n);
        platform_console_flush();
    }
#ifndef __ZEPHYR__
    fputs(msg, stderr);
#endif
    abort();
    return NULL; /* unreached */
}
oop (*oevm_oom_handler)(size_t) = default_oom;

/* Set while gc_collect runs, so an allocation during collection (none today)
   cannot recursively trigger another collection. */
static int gc_in_progress = 0;

/* External VM Stack reference for root scanning */
extern oop *vm_stack;
extern size_t vm_stack_ptr;
extern struct MethodContext *current_frame;

/* Check if an oop resides in the read-only Flash/ROM range */
int is_read_only(oop ptr) {
    if (IS_SMI(ptr)) return 1;
    if (ptr == NULL) return 1;
    uintptr_t addr = (uintptr_t)ptr;
    if (addr >= flash_rom_start && addr < flash_rom_end) {
        return 1;
    }
    return 0;
}

/* O(1): range-check against the arena, require sizeof(oop) alignment, then
   confirm the bitmap marks this exact word as a live payload start. */
static OEVM_HOT2 int is_valid_heap_object(oop ptr) {
    if (heap_base == NULL) return 0;   /* M1: guard against NULL before heap_init() */
    uint8_t *p = (uint8_t *)ptr;
    if (p < heap_base + sizeof(struct HeapHeader) || p >= heap_top) return 0;
    if (((uintptr_t)p % OEVM_GRANULE) != 0) return 0;
    return bm_test((size_t)(p - heap_base) / OEVM_GRANULE);
}

/* Test-only introspection: is `ptr` a live payload start in the arena? */
int gc_is_live(oop ptr) { return is_valid_heap_object(ptr); }

/* Forward declaration: gc_collect is defined later in this file, but called
   from gc_allocate's exhaustion path before we get there. */
void gc_collect(void);

/* Allocation order is exact-bin → bump → cross-bin first-fit. The first two are
   O(1) and carry steady-state allocation; the cross-bin scan runs only when the
   exact bin missed AND bump is exhausted, so a contiguous run of dead space that
   the sweep coalesced is not stranded behind a size-class mismatch (the 32-bit
   Perfstex fragmentation OOM). Coalescing happens in the sweep below; the sweep
   also returns a trailing dead run to the bump pointer to keep bump the hot path. */

/* Carve `rounded` payload bytes out of an already-unlinked free block `h`,
   splitting any reusable remainder back onto its size-class bin, and return it
   live. The remainder is only split off when it can hold a header + one granule;
   otherwise the slack stays with the allocation (bounded internal slack). */
static OEVM_HOT2 oop finish_reuse(struct HeapHeader *h, oop vtable, size_t rounded) {
    size_t leftover = h->size - rounded;
    if (leftover >= sizeof(struct HeapHeader) + OEVM_GRANULE) {
        struct HeapHeader *rem =
            (struct HeapHeader *)((uint8_t *)(h + 1) + rounded);
        rem->size = leftover - sizeof(struct HeapHeader);
        rem->marked = 0;
        size_t rbi = bin_index(rem->size);
        rem->next = free_bins[rbi];
        free_bins[rbi] = rem;
        h->size = rounded;
    }
    h->next = NULL;
    h->marked = 0;
    oop obj = (oop)(h + 1);
    bm_set(bm_granule(obj));
    obj->vtable = vtable;
    return obj;
}

/* One allocation attempt against the arena. Returns NULL if exact bin, bump, and
   the cross-bin first-fit all fail (caller decides whether to collect). */
static OEVM_HOT2 oop arena_try_alloc(oop vtable, size_t rounded, size_t need) {
    size_t bi = bin_index(rounded);
    /* 1. Exact size-class bin — the common reuse path. */
    struct HeapHeader **pp = &free_bins[bi];
    while (*pp != NULL) {
        if ((*pp)->size >= rounded) {
            struct HeapHeader *h = *pp;
            *pp = h->next;
            return finish_reuse(h, vtable, rounded);
        }
        pp = &(*pp)->next;
    }
    /* 2. Bump — kept ahead of the cross-bin scan so steady-state allocation
          never pays the bin walk. */
    if (heap_top + need <= heap_end) {
        struct HeapHeader *header = (struct HeapHeader *)heap_top;
        heap_top += need;
        header->next = NULL;
        header->size = rounded;
        header->marked = 0;
        oop obj = (oop)(header + 1);
        bm_set(bm_granule(obj));
        obj->vtable = vtable;
        return obj;
    }
    /* 3. Cross-bin first-fit + split: only when the exact bin missed and bump is
          exhausted. Reuses a larger freed/coalesced block. */
    for (size_t b = bi + 1; b < FREE_BIN_COUNT; b++) {
        pp = &free_bins[b];
        while (*pp != NULL) {
            if ((*pp)->size >= rounded) {
                struct HeapHeader *h = *pp;
                *pp = h->next;
                return finish_reuse(h, vtable, rounded);
            }
            pp = &(*pp)->next;
        }
    }
    return NULL;
}

OEVM_HOT2 oop gc_allocate(oop vtable, size_t size) {
    if (heap_base == NULL) heap_init();

    /* OEVM_GC_STRESS: collect before every allocation to expose GC correctness
       bugs early. Guarded by gc_in_progress to prevent recursive collection.
       Cached once at first allocation so getenv is not called on every object
       creation. */
    static int gc_stress_cached = -1;
    if (gc_stress_cached < 0)
        gc_stress_cached = (getenv("OEVM_GC_STRESS") != NULL) ? 1 : 0;
    if (!gc_in_progress && gc_stress_cached) gc_collect();

    size_t rounded = (size + (OEVM_GRANULE - 1)) & ~(size_t)(OEVM_GRANULE - 1);
    /* I2: zero-size guard — a zero request after rounding would produce a
       zero-capacity block; clamp to the minimum granule to keep headers safe. */
    if (rounded == 0) rounded = OEVM_GRANULE;
    size_t need = sizeof(struct HeapHeader) + rounded;

    oop obj = arena_try_alloc(vtable, rounded, need);
    if (obj != NULL) { heap_stats_on_alloc(need); return obj; }

    /* Exhausted: collect once and retry, unless already collecting. */
    if (!gc_in_progress) {
        gc_in_progress = 1;
        gc_collect();
        gc_in_progress = 0;
        obj = arena_try_alloc(vtable, rounded, need);
        if (obj != NULL) { heap_stats_on_alloc(need); return obj; }
    }

    /* Still no room after a full collection: terminal OOM policy. */
    return oevm_oom_handler(size);
}

/*
 * Float bump arena (P0 Phase 1).
 *
 * Float arithmetic produces huge numbers of short-lived boxed results; routing
 * each through gc_allocate() meant a full malloc + heap-list insert per op,
 * which dominated nbody/mandelbrot. float_alloc() instead hands out struct Float
 * cells by bumping a cursor within a slab, falling back to a fresh slab when one
 * fills — ~2 instructions per allocation.
 *
 * The arena is mark-swept by gc_collect (W2a): per-slab markbit arrays track
 * liveness, and float_sweep() rebuilds the float_freelist from all unmarked
 * allocated cells after each collection. Dead cells are reclaimed onto the
 * free-list and reused by subsequent float_alloc() calls. Slab structs
 * themselves are not returned to malloc — reclaim is at cell granularity only.
 */
#ifndef OEVM_FLOAT_ARENA_SLAB_CELLS
#define OEVM_FLOAT_ARENA_SLAB_CELLS 4096  /* ~64 KB/slab on host; -D lower for embedded (small newlib heap) */
#endif
#define FLOAT_ARENA_SLAB_CELLS OEVM_FLOAT_ARENA_SLAB_CELLS
/* markbits must cover every cell: round the bit-array up so a CELLS value that
 * is not a multiple of 8 (now possible via the -D knob) does not drop the last
 * partial byte (OOB mark write). Require >=8 cells so the array is non-empty. */
#if OEVM_FLOAT_ARENA_SLAB_CELLS < 8
#error "OEVM_FLOAT_ARENA_SLAB_CELLS must be >= 8"
#endif
#ifndef OEVM_FLOAT_ARENA_SLAB_BUDGET
#define OEVM_FLOAT_ARENA_SLAB_BUDGET 16   /* soft cap on total slabs (~1 MB host); -D lower for embedded */
#endif
struct FloatSlab {
    struct FloatSlab *next;
    struct Float cells[FLOAT_ARENA_SLAB_CELLS];
    uint8_t markbits[(FLOAT_ARENA_SLAB_CELLS + 7) / 8];   /* W2a: 1 bit/cell, rounded up */
};
static struct FloatSlab *float_slab = NULL;
/* Initialised at the slab capacity so the first allocation forces a slab. */
static size_t float_slab_used = FLOAT_ARENA_SLAB_CELLS;

/* W2a reclaim state. A dead cell stores the free-list next pointer overlaid on
   its unused `value` field. The [lo,hi) span across all slabs gives mark() an
   O(1) fast reject for the common non-float pointer. */
static struct Float *float_freelist = NULL;
static char *float_arena_lo = NULL;     /* min cell address across slabs */
static char *float_arena_hi = NULL;     /* one-past max cell address */

static size_t float_slab_count = 0;     /* slabs ever allocated (test hook) */

oop float_alloc(oop vtable) {
    struct Float *f;
    if (float_freelist != NULL) {
        f = float_freelist;
        /* unthread: next pointer was stored overlaid on value */
        float_freelist = *(struct Float **)&f->value;
    } else {
        if (float_slab_used >= FLOAT_ARENA_SLAB_CELLS) {
            /* W2a.1: at the soft cap, reclaim before growing (mirror gc_allocate). */
            if (!gc_in_progress && float_slab_count >= OEVM_FLOAT_ARENA_SLAB_BUDGET) {
                gc_in_progress = 1;
                gc_collect();                 /* float_sweep refills float_freelist */
                gc_in_progress = 0;
                if (float_freelist != NULL) { /* reclaim worked — draw and return, no growth */
                    struct Float *r = float_freelist;
                    float_freelist = *(struct Float **)&r->value;
                    r->base.vtable = vtable;
                    return (oop)r;
                }
                /* else: live set exceeds the cap — fall through and grow */
            }
            struct FloatSlab *slab = oevm_malloc(sizeof(struct FloatSlab));
            /* rete#190: this NULL is already checked below — the tuvm#15 reclaim
               path this `if (!slab)` guards was written before the oevm_malloc
               seam existed but already treats a failed allocation as survivable
               pressure, not a crash. Converting the raw malloc() to oevm_malloc()
               (Task 1) changed nothing here: reclaim-then-retry, and only abort
               (below) if the collection frees nothing — a vm.c:1751/:1794-style
               fatal abort, but reached only after trying to make the request
               succeed, because unlike those two sites this one has a cheap,
               already-designed recovery. */
            if (!slab) {
                /* tuvm#15: the pool is exhausted before we reached the slab budget
                   (small embedded newlib heap — the DK's 64 KB pool can't hold
                   OEVM_FLOAT_ARENA_SLAB_BUDGET slabs). The budget trigger above
                   therefore never fired, yet the existing slabs are full of dead
                   cells (float-heavy loops keep a tiny live set). Don't abort with
                   reclaimable garbage in hand: run one collection and, if it freed
                   any cells, draw from the refilled free-list instead of growing.
                   This makes reclaim driven by real memory pressure, not the budget,
                   so the arena self-bounds to whatever the pool actually allows. */
                if (!gc_in_progress) {
                    gc_in_progress = 1;
                    gc_collect();                 /* float_sweep refills float_freelist */
                    gc_in_progress = 0;
                    if (float_freelist != NULL) {
                        struct Float *r = float_freelist;
                        float_freelist = *(struct Float **)&r->value;
                        r->base.vtable = vtable;
                        return (oop)r;
                    }
                }
                /* collection freed nothing (live set genuinely exceeds the pool) —
                   terminal OOM. */
                fprintf(stderr, "VM Float Arena Error: Out of Memory\n");
                abort();
            }
            memset(slab->markbits, 0, sizeof slab->markbits);
            slab->next = float_slab;
            float_slab = slab;
            float_slab_used = 0;
            float_slab_count++;
            /* extend the address span to cover this slab's cells */
            {
                char *clo = (char *)&slab->cells[0];
                char *chi = (char *)&slab->cells[FLOAT_ARENA_SLAB_CELLS];
                if (float_arena_lo == NULL || clo < float_arena_lo) float_arena_lo = clo;
                if (float_arena_hi == NULL || chi > float_arena_hi) float_arena_hi = chi;
            }
        }
        f = &float_slab->cells[float_slab_used++];
        {
            size_t idx = (size_t)(f - float_slab->cells);
            float_slab->markbits[idx >> 3] &= (uint8_t)~(1u << (idx & 7));
        }
    }
    f->base.vtable = vtable;
    return (oop)f;
}

/* --- W2a test-only introspection (parallel to gc_is_live) --- */
size_t float_arena_slab_count(void) { return float_slab_count; }

/* W2a.1 test-only: the compiled slab budget, so bound tests adapt to -D overrides. */
size_t float_arena_slab_budget(void) { return OEVM_FLOAT_ARENA_SLAB_BUDGET; }

size_t float_arena_free_count(void) {
    size_t n = 0;
    for (struct Float *p = float_freelist; p != NULL;
         p = *(struct Float **)&p->value) n++;
    return n;
}

/* 1 if the cell is currently in use (not reclaimed). A cell is "dead" iff it is
   on the free-list. Pristine bump-tail cells are never passed here by tests. */
int float_is_live(oop ptr) {
    for (struct Float *p = float_freelist; p != NULL;
         p = *(struct Float **)&p->value) {
        if ((oop)p == ptr) return 0;
    }
    return 1;
}

/* W2a: 1 if ptr is a cell-aligned address inside some slab's cells[]. The
   [lo,hi) span fast-rejects the overwhelmingly common non-float pointer. */
static int is_in_float_arena(oop ptr) {
    char *p = (char *)ptr;
    if (p < float_arena_lo || p >= float_arena_hi) return 0;
    for (struct FloatSlab *s = float_slab; s != NULL; s = s->next) {
        char *base = (char *)&s->cells[0];
        char *end  = (char *)&s->cells[FLOAT_ARENA_SLAB_CELLS];
        if (p >= base && p < end) {
            size_t off = (size_t)(p - base);
            return (off % sizeof(struct Float)) == 0;   /* must be a cell start */
        }
    }
    return 0;
}

/* W2a: set the mark bit for the cell at ptr. Caller has verified
   is_in_float_arena(ptr). Floats are leaves, so nothing else to trace. */
static void float_mark(oop ptr) {
    char *p = (char *)ptr;
    for (struct FloatSlab *s = float_slab; s != NULL; s = s->next) {
        char *base = (char *)&s->cells[0];
        char *end  = (char *)&s->cells[FLOAT_ARENA_SLAB_CELLS];
        if (p >= base && p < end) {
            size_t idx = (size_t)(p - base) / sizeof(struct Float);
            s->markbits[idx >> 3] |= (uint8_t)(1u << (idx & 7));
            return;
        }
    }
}

/* W2a: rebuild the free-list from every unmarked allocated cell, then clear all
   bits for the next cycle. Mirrors the main heap's rebuild-free_bins-from-scratch
   sweep. Head slab is swept only over [0, float_slab_used); older slabs in full. */
static void float_sweep(void) {
    float_freelist = NULL;
    for (struct FloatSlab *s = float_slab; s != NULL; s = s->next) {
        size_t limit = (s == float_slab) ? float_slab_used : FLOAT_ARENA_SLAB_CELLS;
        for (size_t i = 0; i < limit; i++) {
            uint8_t bit = s->markbits[i >> 3] & (uint8_t)(1u << (i & 7));
            if (bit) {
                s->markbits[i >> 3] &= (uint8_t)~(1u << (i & 7));  /* live: clear for next cycle */
            } else {
                struct Float *cell = &s->cells[i];                 /* dead: thread onto free-list */
                *(struct Float **)&cell->value = float_freelist;
                float_freelist = cell;
            }
        }
    }
}

/* GH#4: explicit mark worklist replaces recursive marking so the mark phase
   uses O(1) C stack regardless of object-graph depth (Zephyr small stacks). */
#ifndef OEVM_MARK_STACK_SIZE
#define OEVM_MARK_STACK_SIZE 1024   /* entries; -D lower for embedded. ~8 KB/64-bit */
#endif
static oop    mark_stack[OEVM_MARK_STACK_SIZE];
static size_t mark_sp;            /* next free slot; 0 = empty */
static int    mark_overflowed;    /* a push was dropped this cycle → recovery rescan */

static void trace_one(oop ptr);   /* forward: traces one already-marked object */

/* Forward declaration of push-only mark */
static void mark(oop ptr);

/* Trace all pointer fields within an object */
static OEVM_HOT2 void trace_object(oop ptr, size_t size) {
    // Scan the object payload at word granularity
    oop *fields = (oop *)ptr;
    size_t count = size / sizeof(oop);
    for (size_t i = 0; i < count; i++) {
        mark(fields[i]);
    }
}

/* Classify a pointer, set its mark bit, and enqueue it for tracing. No
   recursion: children are reached when this object is later popped and traced. */
static OEVM_HOT2 void mark(oop ptr) {
    if (IS_SMI(ptr)) return;
    if (ptr == NULL) return;
    if (is_in_blob(ptr)) return; /* blob objects are immutable ROM: never marked, never traced */
    if (is_read_only(ptr)) return;

    if (is_in_float_arena(ptr)) {
        float_mark(ptr);        /* leaf object: set bit, no tracing, never enqueued */
        return;
    }

    if (!is_valid_heap_object(ptr)) {
        return; // Skip non-heap pointers (e.g. raw pointers or unaligned values)
    }

    struct HeapHeader *header = ((struct HeapHeader *)ptr) - 1;
    if (header->marked) {
        return; // Already visited
    }

    header->marked = 1;          /* set BEFORE the push: an overflowed object is
                                    still marked (never swept); only its edges defer */
    if (mark_sp < OEVM_MARK_STACK_SIZE) {
        mark_stack[mark_sp++] = ptr;
    } else {
        mark_overflowed = 1;     /* recovered by the heap-rescan pass */
    }
}

/* Trace one already-marked heap object: enqueue each of its child references.
   This is the per-vtable dispatch that used to live inside the recursive mark;
   every child is reached via mark() (an enqueue), never a recursive descent. */
static OEVM_HOT2 void trace_one(oop ptr) {
    extern oop VtableVtable;
    extern oop FloatVtable;
    extern oop StringVtable;
    extern oop RawBytesVtable;
    extern oop SymbolVtable;
    extern oop StackVtable;
    extern oop ContextVtable;
    extern oop g_active_stack;

    if (ptr->vtable == VtableVtable) {
        struct Vtable *vt = (struct Vtable *)ptr;
        mark(vt->delegate);
        if (vt->bindings != NULL) {
            for (size_t i = 0; i < vt->size; i++) {
                mark(vt->bindings[i].symbol);
                mark(vt->bindings[i].method);
            }
        }
    } else if (StringVtable != NULL && ptr->vtable == StringVtable) {
        mark(ptr->vtable);          /* leaf: inline bytes are NOT oops — do not scan */
    } else if (RawBytesVtable != NULL && ptr->vtable == RawBytesVtable) {
        /* RawBytes (Milestone B Task 3): reuses struct String's C layout —
           same leaf reasoning as StringVtable above: the inline bytes are
           NOT oops and must not be scanned by the generic word-count
           fallback (trace_object), which would otherwise misinterpret raw
           byte content as pointers. */
        mark(ptr->vtable);
    } else if (SymbolVtable != NULL && ptr->vtable == SymbolVtable) {
        /* rete#190 stage 2: same leaf reasoning as StringVtable/RawBytesVtable
           above — the inline `name` bytes are NOT oops. Before the bytes moved
           inline, Symbol fell through to the generic word-count fallback, which
           scanned a single const char * harmlessly; with inline bytes that
           fallback would misinterpret raw symbol text as pointers. */
        mark(ptr->vtable);
    } else if (FloatVtable != NULL && ptr->vtable == FloatVtable) {
        mark(ptr->vtable);
    } else if (StackVtable != NULL && ptr->vtable == StackVtable) {
        /* Stack (L1 Task 2/3): a first-class heap object holding a region of
           MethodContext frames. The ACTIVE stack (g_active_stack) is skipped
           here — its sp/current_off are stale (only synced on fiber switch;
           main never switches), and its live frames are already covered by
           scan_vm_stack(), which walks via current_frame/vm_stack_ptr (the
           live cursor). (Task 6 removed the Fiber wrapper; g_active_stack IS
           now the sole execution context, rooted directly in gc_collect.)
           For suspended (non-active) Stacks this precise walk is exercised
           starting in Task 4; today the active main Stack is the only Stack
           in existence, so the walk body below is still dormant. prev_frame
           is a base-relative SMI (frame_abi.h), never marked directly. */
        mark(ptr->vtable);
        if (ptr != g_active_stack) {
            struct Stack *st = (struct Stack *)ptr;
            struct MethodContext *frame = st->sp ?
                (struct MethodContext *)(st->data + st->current_off) : NULL;
            struct MethodContext *higher = NULL;
            while (frame != NULL) {
                mark(frame->vtable);
                mark(frame->home_ctx);   /* relative SMI: mark() no-ops it (frame lives in the marked Stack) */

                oop *regs = frame->regs;
                oop *end = higher ? (oop *)higher : (st->data + st->sp);
                for (oop *r = regs; r < end; r++) mark(*r);

                higher = frame;
                frame = frame_link_decode(st->data, frame->prev_frame);
            }
        }
    } else if (ContextVtable != NULL && ptr->vtable == ContextVtable) {
        /* Context (deferred-lift Phase 1, Task 1): a heap temp-vector of
           captured locals. LIVE (decision 2026-07-04) — allocated by the
           compiler-emitted OP_MK_CONTEXT for escaping captures, so this
           tracer must be precise (same reasoning as the Stack branch above):
           the flexible slots[] array is NOT covered by the generic
           trace_object word-count fallback. */
        struct Context *c = (struct Context *)ptr;
        mark(c->base.vtable);
        mark(c->outer);
        for (uint32_t i = 0; i < c->num_slots; i++) mark(c->slots[i]);
    } else {
        // Trace the object's fields word-by-word (enqueues each pointer).
        struct HeapHeader *header = ((struct HeapHeader *)ptr) - 1;
        trace_object(ptr, header->size);
    }
}

/* rete#222 — the collector's own state, for the wedge detector's forensics dump. The
   reactor spinning at 100% CPU with no send completing is exactly what an unterminating
   mark phase would look like from outside, and nothing else in the tree can tell that
   apart from an interpreter loop. Three statics, read-only, no lock: a torn read of a
   wedged collector is still the answer, and taking a lock from the observer thread to
   read them would be a new way to hang. */
void gc_forensics(int *in_progress, unsigned *msp, int *overflowed) {
    if (in_progress) *in_progress = gc_in_progress;
    if (msp)         *msp = (unsigned)mark_sp;
    if (overflowed)  *overflowed = mark_overflowed;
}

/* Pop-and-trace until the worklist drains. */
static OEVM_HOT2 void drain_mark_stack(void) {
    while (mark_sp > 0) {
        oop ptr = mark_stack[--mark_sp];
        trace_one(ptr);            /* may enqueue more via mark() */
    }
}

/* Drain the worklist, then — if any push was dropped — re-trace every marked
   object via a linear arena walk so dropped children get re-enqueued; repeat
   until a full pass overflows nothing. Each round marks >=1 new object, so it
   terminates (bounded by the object count). No overflow → one drain, no rescan. */
static void mark_drain_with_recovery(void) {
    drain_mark_stack();
    while (mark_overflowed) {
        mark_overflowed = 0;
        uint8_t *cur = heap_base;
        while (cur < heap_top) {
            struct HeapHeader *h = (struct HeapHeader *)cur;
            if (h->marked) trace_one((oop)(h + 1));  /* re-enqueue unmarked children */
            cur += sizeof(struct HeapHeader) + h->size;
        }
        drain_mark_stack();        /* may set mark_overflowed again */
    }
}

/* Walk the active sequential stack frames and mark roots.
   As of L1 Task 3, current_frame and every frame->home_ctx are interior
   pointers INTO g_active_stack's data[] payload, not heap-object starts.
   mark() exact-start-validates via the alloc bitmap, so marking an interior
   pointer here safely no-ops — it neither corrupts nor (more importantly)
   loses anything: every frame on this chain is reachable via this walk, and
   the frame STRUCT memory itself is kept alive transitively because it lives
   inside g_active_stack, which is marked as a root above. */
static OEVM_HOT2 void scan_vm_stack(void) {
    struct MethodContext *frame = current_frame;
    struct MethodContext *prev_higher_frame = NULL;

    while (frame != NULL) {
        // Trace the header fields of the MethodContext
        mark(frame->vtable);
        mark(frame->home_ctx);   /* relative SMI: mark() no-ops it (frame lives in the marked Stack) */

        // Deduce the register count based on relative stack offsets
        size_t reg_count = 0;
        if (prev_higher_frame != NULL) {
            // End of current frame's registers is the start of the next allocated frame
            reg_count = (oop *)prev_higher_frame - frame->regs;
        } else {
            // For the top-most frame, registers extend to the current stack pointer
            reg_count = &vm_stack[vm_stack_ptr] - frame->regs;
        }

        // Trace all active registers in this frame
        for (size_t i = 0; i < reg_count; i++) {
            mark(frame->regs[i]);
        }

        prev_higher_frame = frame;
        frame = frame_link_decode(vm_stack, frame->prev_frame);
    }
}

/* Custom roots callback hook */
void (*gc_additional_roots_hook)(void) = NULL;

/* Exported mark function for custom roots */
void mark_object(oop ptr) {
    mark(ptr);
}

/* Precise temp-root set for the boot drivers (oevm#21). run_solidified_entry_point
   (Path A) and oimg_bootstrap (Path B) hold the freshly-allocated boot instances
   (kernel/entry/supervisor + capability wrappers) in C locals across a sequence of
   sends, each of which can allocate and (under GC_STRESS) collect. With no
   conservative C-stack scan, a C local is not a GC root at all, so such a local
   would be swept and the reused slot corrupts dispatch (DNU / SEGV). Each boot
   driver pushes its live oops here so they are PRECISE roots for the whole boot,
   then clears the set. Empty
   (zero overhead) once boot completes. */
#define GC_TEMP_ROOTS_MAX 16
oop gc_temp_roots[GC_TEMP_ROOTS_MAX];
int gc_temp_roots_count = 0;

void gc_temp_root_push(oop o) {
    if (gc_temp_roots_count < GC_TEMP_ROOTS_MAX) {
        gc_temp_roots[gc_temp_roots_count++] = o;
    } else {
        /* Boot pins a small, fixed number of instances; overflow means the boot
           driver outgrew the budget. Fail loud rather than silently drop a root. */
        fprintf(stderr, "VM Error: gc_temp_roots overflow (>%d)\n", GC_TEMP_ROOTS_MAX);
        abort();
    }
}

void gc_temp_roots_clear(void) {
    gc_temp_roots_count = 0;
}

/* Trigger a Mark-and-Sweep Garbage Collection cycle */
OEVM_HOT2 void gc_collect(void) {
    // 1. Reset mark tags: linear walk of the arena
    if (heap_base == NULL) heap_init();

    /* Invalidate the global method cache: it holds oop keys/values that are not
       GC roots; after a non-moving sweep + slot reuse a stale entry could alias a
       reused address and produce a spurious cache hit dispatching the wrong method.
       GC is rare (exhaustion-triggered), so flushing here is cheap. */
    extern void method_cache_flush(void);
    method_cache_flush();
    mark_sp = 0;
    mark_overflowed = 0;
    {
        uint8_t *cur = heap_base;
        while (cur < heap_top) {
            struct HeapHeader *h = (struct HeapHeader *)cur;
            h->marked = 0;
            cur += sizeof(struct HeapHeader) + h->size;
        }
    }

    // 2. Scan roots: active stack frames
    scan_vm_stack();

    // 3. Scan roots: global static vtable variables
    extern oop IntegerVtable;
    extern oop FloatVtable;
    extern oop MethodContextVtable;
    extern oop SymbolVtable;
    extern oop StringVtable;
    extern oop RawBytesVtable;
    extern oop VtableVtable;
    extern oop PrimitiveClosureVtable;
    extern oop BytecodeClosureVtable;
    extern oop Symbol_value;

    mark(IntegerVtable);
    mark(FloatVtable);
    mark(MethodContextVtable);
    mark(SymbolVtable);
    mark(StringVtable);
    mark(RawBytesVtable);
    mark(VtableVtable);
    mark(PrimitiveClosureVtable);
    mark(BytecodeClosureVtable);
    mark(Symbol_value);

    // Milestone C Task 1: the channel-space C substrate (src/space.c) holds
    // queued event values + registered reaction blocks/owners in its own
    // static tables, outside every other root set scanned here (not on the
    // VM stack, not a global vtable/singleton) — trace them explicitly.
    extern void space_gc_trace(void);
    space_gc_trace();

    /* M6 C-boundary handle table (src/handles.c): live entries are GC roots held
       only in that static table, outside every other root set — trace them. */
    extern void handle_gc_trace(void);
    handle_gc_trace();

    // Mark the execution-context root. Task 6 removed the Fiber wrapper: the running
    // context is now exactly g_active_stack (this Stack heap object) — its live
    // frames/registers are traced precisely by scan_vm_stack() below (via
    // current_frame/vm_stack_ptr), and marking the Stack object itself here keeps
    // it from being swept.
    extern oop StackVtable;
    extern oop ContextVtable;
    extern oop g_active_stack;
    mark(StackVtable);
    mark(ContextVtable);
    mark(g_active_stack);

    // Mark debugger hook globals (dispatch.c) — NOT in the list above, but
    // persist across GC cycles after primitive_register_debugger_hook returns.
    // mark() is a no-op on NULL, so this is safe whether or not they are set.
    extern oop debugger_hook;
    extern oop Symbol_debug_;
    mark(debugger_hook);
    mark(Symbol_debug_);
#ifndef OEVM_NO_DEBUG_HOOKS
    // rete#313: a replaced method's closure is held only by the patch tables.
    for (int i = 0; i < method_override_count; i++) mark(method_overrides[i].method);
    for (int i = 0; i < method_patch_count; i++) { mark(method_patches[i].from); mark(method_patches[i].to); }
#endif

    // Mark lazy-allocated dispatch globals (dispatch.c) — these are allocated
    // once on first use and stored only in these globals, never on the VM stack.
    // Under Model A (-DOEVM_APPLIANCE), Symbol_accessViolation is lazily
    // heap-allocated (not a data-segment const); without this mark it would be
    // swept on the next collection → use-after-free.
    extern oop Symbol_accessViolation;
    mark(Symbol_accessViolation);

    // Milestone D: the in-flight raised condition (self error:'s argument),
    // live between vm_raise's longjmp and vm_protected_fire's handler reading
    // it — held only in this global, not on the VM stack. mark() is a no-op
    // on NULL, so this is safe whether or not a fire is in progress.
    extern oop g_raised_condition;
    mark(g_raised_condition);

    // Milestone E Task 1: the captured Kernel module instance (dispatch.c stashes
    // it at main:args: entry). The error:/error:tag: prims send it
    // crashRecordTag:text: to build the in-flight Record, so it must outlive any
    // GC between app entry and a raise. mark() is a no-op on NULL (pre-capture).
    extern oop g_kernel_instance;
    mark(g_kernel_instance);

    /* oevm#21: the in-flight boot receiver/args, live across the lazy exec-context
       bootstrap before they are written into a GC-scanned frame. With no
       conservative C-stack scan these C locals have no other root; mark them here
       so a GC_STRESS collection during bootstrap cannot sweep the boot entry
       instance. All NULL/0 outside the boot window (no-ops). */
    extern oop g_boot_inflight_receiver;
    extern oop *g_boot_inflight_args;
    extern int g_boot_inflight_argc;
    mark(g_boot_inflight_receiver);
    for (int i = 0; i < g_boot_inflight_argc; i++) {
        mark(g_boot_inflight_args[i]);
    }

    /* oevm#21: boot-driver temp roots (instances held across the boot send sequence). */
    extern oop gc_temp_roots[];
    extern int gc_temp_roots_count;
    for (int i = 0; i < gc_temp_roots_count; i++) {
        mark(gc_temp_roots[i]);
    }

    // Call custom roots hook if registered
    if (gc_additional_roots_hook != NULL) {
        gc_additional_roots_hook();
    }

    /* GH#4: all roots are now enqueued — trace the live graph iteratively
       (with overflow recovery) instead of via the old recursive mark. */
    mark_drain_with_recovery();

    // 4. Sweep phase: linear walk; reclaim unmarked blocks, COALESCING physically
    // adjacent dead runs into one free block so a contiguous span of dead space is
    // not stranded as many small per-size-class blocks (the 32-bit Perfstex OOM).
    // Reset every bin FIRST: dead blocks remain in [heap_base, heap_top) and are
    // re-walked on every collection, so without this reset they would be pushed
    // onto a bin a second time, self-cycling the list and aliasing reused addresses.
    for (size_t i = 0; i < FREE_BIN_COUNT; i++) free_bins[i] = NULL;
    /* Telemetry: recompute live_bytes from this sweep's marked survivors
       rather than subtracting per freed object — least invasive against the
       existing walk, and exact by construction. high_water is untouched here:
       a sweep only removes garbage, so live_bytes can only fall or hold,
       never rise past a previously recorded peak. */
    size_t swept_live = 0;
    {
        uint8_t *cur = heap_base;
        while (cur < heap_top) {
            struct HeapHeader *h = (struct HeapHeader *)cur;
            uint8_t *next = cur + sizeof(struct HeapHeader) + h->size;
            if (h->marked) { swept_live += sizeof(struct HeapHeader) + h->size; cur = next; continue; }
            /* Dead: clear its live bit, then extend the run over every following
               consecutive dead block. */
            bm_clear(bm_granule((oop)(h + 1)));
            uint8_t *run_end = next;
            while (run_end < heap_top) {
                struct HeapHeader *h2 = (struct HeapHeader *)run_end;
                if (h2->marked) break;
                bm_clear(bm_granule((oop)(h2 + 1)));
                run_end += sizeof(struct HeapHeader) + h2->size;
            }
            if (run_end >= heap_top) {
                /* Trailing dead run: hand it back to the bump allocator so the
                   common case stays O(1) bump allocation (no bin walk), and so
                   coalesced tail space is immediately reusable at any size. */
                heap_top = cur;
            } else {
                /* Interior dead run: one coalesced free block spanning [cur,run_end). */
                h->size = (size_t)(run_end - cur) - sizeof(struct HeapHeader);
                h->marked = 0;
                size_t bi = bin_index(h->size);
                h->next = free_bins[bi];
                free_bins[bi] = h;
            }
            cur = run_end;
        }
    }
    heap_live_bytes = swept_live;
    heap_gc_cycles++;

    /* W2a: reclaim dead Float cells (separate malloc-backed arena). */
    float_sweep();
}

/* Heap telemetry accessor (G3/M2, https://github.com/davidg238/nsl-tuvm/issues/3). largest_free_block is computed
   here, on demand, rather than maintained per-allocation: stats are read
   rarely, so the walk cost belongs here, not on the hot alloc path. Walks
   every free bin fully (including the catch-all last bin, which holds mixed
   sizes) rather than assuming per-bin uniformity, so the result is correct
   regardless of how split remainders landed in the bins. */
struct OevmHeapStats oevm_heap_stats(void) {
    struct OevmHeapStats s;
    s.heap_size = OEVM_HEAP_SIZE;
    s.live_bytes = heap_live_bytes;
    s.high_water = heap_high_water;
    s.gc_cycles = heap_gc_cycles;

    size_t largest = 0;
    for (size_t i = 0; i < FREE_BIN_COUNT; i++) {
        for (struct HeapHeader *h = free_bins[i]; h != NULL; h = h->next) {
            if (h->size > largest) largest = h->size;
        }
    }
    size_t bump_remaining = (size_t)(heap_end - heap_top);
    if (bump_remaining > largest) largest = bump_remaining;
    s.largest_free_block = largest;

    return s;
}
