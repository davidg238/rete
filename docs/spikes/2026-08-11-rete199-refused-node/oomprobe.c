/* rete#199 diagnostic — SCRATCH, not for the tree.
 *
 * Rides two seams the VM already exports, so no tracked file is edited:
 *   - oevm_millis  : the overridable clock, called constantly by the reactor.
 *                    Used here purely as a sampling tick for a heap trend.
 *   - oevm_oom_handler : the terminal OOM hook. Used to dump the arena's state
 *                    and the nsl frame chain at the exact fatal allocation.
 *
 * The question it answers: does the refused node die of a LEAK (live_bytes
 * climbing to the ceiling) or of FRAGMENTATION (live_bytes moderate but
 * largest_free_block falling below the 4104 B the ByteBuilder needs)?
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "oevm.h"

extern oop (*oevm_oom_handler)(size_t);
extern struct MethodContext *current_frame;
extern const uint8_t *oimg_blob_base;

static uint64_t (*prev_millis)(void);
static uint64_t last_sample_ms;

static void dump_stats(const char *why, uint64_t now) {
    struct OevmHeapStats s = oevm_heap_stats();
    fprintf(stderr,
            "probe %s t=%llu live=%zu high=%zu largest_free=%zu cycles=%zu\n",
            why, (unsigned long long)now,
            s.live_bytes, s.high_water, s.largest_free_block, s.gc_cycles);
    fflush(stderr);
}

static uint64_t probe_millis(void) {
    uint64_t now = prev_millis();
    if (now - last_sample_ms >= 2000) { last_sample_ms = now; dump_stats("tick", now); }
    return now;
}

static oop probe_oom(size_t size) {
    struct MethodContext *f;
    int i;
    fprintf(stderr, "\nprobe FATAL request=%zu\n", size);
    dump_stats("fatal", prev_millis ? prev_millis() : 0);
    fprintf(stderr, "probe frames (CompiledMethod offsets into the .oimg blob):\n");
    f = current_frame;
    for (i = 0; f != NULL && i < 24; i++) {
        struct Closure *cl = (struct Closure *)f->closure;
        if (cl != NULL && cl->entry_point != NULL) {
            fprintf(stderr, "probe frame %d cm_off %ld\n", i,
                    (long)((const uint8_t *)cl->entry_point - oimg_blob_base));
        }
        f = (struct MethodContext *)f->prev_frame;
    }
    fflush(stderr);
    abort();
    return NULL;
}

__attribute__((constructor))
static void probe_install(void) {
    prev_millis = oevm_millis;
    oevm_millis = probe_millis;
    oevm_oom_handler = probe_oom;
}
