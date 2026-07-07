/* Milestone C Task 1: exact-channel registry + FIFO event queue + owner-tagged
   reaction storage + C-callable post. See src/space.h for the module contract
   and docs/superpowers/specs/2026-07-06-milestone-c-channels-reactor-design.md
   section 6 for the design this implements.

   Storage shape (three static tables, no dynamic allocation — embedded-arena
   style, sized by the OEVM_SPACE_* macros in space.h):
     - `channels[]`: exact path string -> small int id, each with its own
       fixed reaction list (block, owner) pairs in registration order.
     - `queue[]`: one ring buffer of (channel id, value, live) events, shared
       by every channel (D-C1 — one FIFO, node-wide, not per-channel).

   Ring-buffer invariant maintained by every removal path (space_take,
   space_dequeue): after marking a slot dead, `compact_head()` advances `head`
   past any now-dead LEADING run so that whenever `count > 0`, queue[head] is
   always live. This makes space_dequeue's "scan from head for the oldest
   live event" O(1) (the invariant already put a live event there) while
   space_take's "scan for the oldest live event on one channel" still walks
   the ring, since the match it wants may not be at head (another channel's
   still-live event can sit in front of it). A slot taken out of order (by
   space_take, mid-ring) is simply left `live = 0` in place — freed later,
   lazily, once the ring's head walk reaches it — so `count` is "ring slots
   still occupied" rather than "events not yet dispatched"; a channel-queue
   overflow can therefore trigger slightly before OEVM_SPACE_QUEUE_DEPTH
   *live* events are outstanding if take:-consumed slots haven't been walked
   off the front yet. This is the ordinary, bounded behavior of a fixed-depth
   ring buffer (ring depth is a work-order sizing detail per the design doc),
   not a correctness bug: FIFO order and exact-match semantics are unaffected. */
#include "space.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* platform.c's indexable allocator (struct ObjectInstance: slots[0] = TAG_SMI
   length, slots[1..n] = elements, 1-based) — used both for the snapshot Array
   of a channel's reaction blocks and the outer {value, snapshot, channelPath}
   triple space_dequeue answers. Declared extern here rather than a shared
   header, mirroring src/udp.c's precedent (indexable_new is non-static in
   platform.c). */
extern oop indexable_new(intptr_t n);

/* mark_object: gc.c's exported single-oop mark entry point (used already by
   every test's gc_additional_roots_hook; here it is gc.c's OWN root scan that
   calls into this module, via space_gc_trace, so the direction is reversed —
   but the mark primitive is the same one). */
extern void mark_object(oop ptr);

/* Milestone D Task 2: build the channel-path String for the crash record
   (Space>>drain needs it to post `Record tag: #crash ... with: #channel ->
   chPath` and to run the sys/-loop guard). Same local-ctor idiom as
   udp_make_string (src/udp.c) / the file-static make_string in dispatch.c —
   gc_allocate(StringVtable, ...) directly, since dispatch.c's make_string is
   file-static and not reachable here. */
extern oop gc_allocate(oop vtable, size_t size);
extern oop StringVtable;

static oop space_make_string(const char *bytes, size_t len) {
    oop s = gc_allocate(StringVtable, sizeof(struct String) + len);
    struct String *str = (struct String *)s;
    str->length = len;
    if (len > 0) memcpy(str->bytes, bytes, len);
    return s;
}

struct SpaceReaction {
    oop block;
    oop owner;
};

struct SpaceChannel {
    char path[OEVM_SPACE_MAX_PATH];
    size_t len;
    struct SpaceReaction reactions[OEVM_SPACE_MAX_REACTIONS];
    int n_reactions;
};

struct SpaceEvent {
    int chan;   /* index into channels[]; only meaningful while live */
    oop value;
    int live;
};

static struct SpaceChannel channels[OEVM_SPACE_MAX_CHANNELS];
static int n_channels = 0;

static struct SpaceEvent queue[OEVM_SPACE_QUEUE_DEPTH];
static size_t q_head = 0;    /* oldest occupied ring slot (live, whenever count > 0) */
static size_t q_count = 0;   /* occupied ring slots (see file-header note on staleness) */

/* Find an existing channel for the exact path; -1 if none has ever been
   interned. Never creates an entry (space_take must not grow the registry
   just to discover "nothing was ever posted/registered here"). */
static int find_channel(const char *path, size_t len) {
    for (int i = 0; i < n_channels; i++) {
        if (channels[i].len == len && memcmp(channels[i].path, path, len) == 0) {
            return i;
        }
    }
    return -1;
}

int space_intern(const char *path, size_t len) {
    int existing = find_channel(path, len);
    if (existing >= 0) return existing;
    if (len >= OEVM_SPACE_MAX_PATH) {
        /* Static path buffer cannot hold len bytes. Dedicated frozen string
           (David-ratified post-Task-1 review; spec section 7 updated,
           nsl-tuvm 70aa2a4). Checked BEFORE registry occupancy: a too-long
           path is invalid regardless of whether a table slot is free, and
           the diagnosis must not shift to `channel registry full` just
           because the table happens to be full at the time. */
        fprintf(stderr, "channel path too long\n");
        abort();
    }
    if (n_channels >= OEVM_SPACE_MAX_CHANNELS) {
        fprintf(stderr, "channel registry full\n");
        abort();
    }
    int id = n_channels++;
    struct SpaceChannel *c = &channels[id];
    memcpy(c->path, path, len);
    c->len = len;
    c->n_reactions = 0;
    return id;
}

/* Advance q_head past any dead slots now at the front, reclaiming their ring
   space. Preserves the invariant "count > 0 implies queue[head] is live". */
static void compact_head(void) {
    while (q_count > 0 && !queue[q_head].live) {
        q_head = (q_head + 1) % OEVM_SPACE_QUEUE_DEPTH;
        q_count--;
    }
}

void space_post(const char *path, size_t len, oop value) {
    int chan = space_intern(path, len);
    if (q_count >= OEVM_SPACE_QUEUE_DEPTH) {
        fprintf(stderr, "channel queue full\n");
        abort();
    }
    size_t tail = (q_head + q_count) % OEVM_SPACE_QUEUE_DEPTH;
    queue[tail].chan = chan;
    /* nil (NULL) is a legal rank-0 value payload here — but it is
       indistinguishable from "nothing queued" at the take:/dequeue read
       side (both answer NULL); callers cannot tell a stored nil apart from
       an empty channel. */
    queue[tail].value = value;
    queue[tail].live = 1;
    q_count++;
}

oop space_take(const char *path, size_t len) {
    int chan = find_channel(path, len);
    if (chan < 0) return NULL;
    for (size_t k = 0; k < q_count; k++) {
        size_t idx = (q_head + k) % OEVM_SPACE_QUEUE_DEPTH;
        if (queue[idx].live && queue[idx].chan == chan) {
            oop value = queue[idx].value;
            queue[idx].live = 0;
            queue[idx].value = NULL;   /* drop the reference promptly for GC */
            if (idx == q_head) compact_head();
            return value;
        }
    }
    return NULL;
}

void space_register(const char *path, size_t len, oop block, oop owner) {
    int chan = space_intern(path, len);
    struct SpaceChannel *c = &channels[chan];
    if (c->n_reactions >= OEVM_SPACE_MAX_REACTIONS) {
        fprintf(stderr, "channel reactions full\n");
        abort();
    }
    c->reactions[c->n_reactions].block = block;
    c->reactions[c->n_reactions].owner = owner;
    c->n_reactions++;
}

oop space_dequeue(void) {
    if (q_count == 0) return NULL;
    /* Ring invariant: queue[q_head] is live whenever q_count > 0. */
    size_t idx = q_head;
    int chan = queue[idx].chan;
    oop value = queue[idx].value;
    struct SpaceChannel *c = &channels[chan];

    /* GC-hazard discipline (Milestone C Task 1 review fix; widened Milestone D
       Task 2 for the new channel-path String): this collector is PRECISE — a
       heap oop held only in a C local has NO root, and every indexable_new/
       space_make_string call below can collect (gc_allocate collects on
       exhaustion, and before EVERY allocation under OEVM_GC_STRESS — the
       condition the boot-gc-asan gate runs under). Measures, all load-bearing:
         (1) the queue slot is NOT cleared until after every allocation and
             slot-fill completes — the still-live slot keeps `value` rooted
             via space_gc_trace across any collection in between;
         (2) `snapshot`, then `path_string`, are each pushed as a gc temp root
             as soon as they exist and stay pinned across every subsequent
             allocation up to and including `result`, using the save/restore
             idiom (dispatch.c context_alloc, its Task-2 review carry-forward):
             a caller up the prim-dispatch path may already hold temp roots,
             so restore the count rather than clearing the whole set. (The
             reaction blocks copied into the snapshot need no extra pinning —
             the channel table itself keeps them rooted via space_gc_trace.) */
    extern void gc_temp_root_push(oop o);
    extern int gc_temp_roots_count;
    int saved = gc_temp_roots_count;

    /* Snapshot the channel's CURRENT reaction list, taken now (at dequeue),
       not at post time — D-C5: registrations made after this event was
       posted but before it is dequeued are visible; registrations made
       during this event's own fan-out are not (they land in the channel's
       list only after this snapshot already copied it). */
    oop snapshot = indexable_new(c->n_reactions);
    struct ObjectInstance *si = (struct ObjectInstance *)snapshot;
    for (int i = 0; i < c->n_reactions; i++) {
        si->slots[i + 1] = c->reactions[i].block;
    }
    gc_temp_root_push(snapshot);

    /* Milestone D Task 2: the channel's path, as an nsl String, for
       Space>>drain's crash record (`Record ... with: #channel -> chPath`) and
       its sys/-loop guard. Built from the same `path`/`len` the registry
       already holds per channel (space.c:53-ish, struct SpaceChannel). */
    oop path_string = space_make_string(c->path, c->len);
    gc_temp_root_push(path_string);

    oop result = indexable_new(3);
    gc_temp_roots_count = saved;

    struct ObjectInstance *ri = (struct ObjectInstance *)result;
    ri->slots[1] = value;
    ri->slots[2] = snapshot;
    ri->slots[3] = path_string;   /* channel path for crash records */

    /* Only now — with value and snapshot both reachable from `result` —
       remove the event from the queue. */
    queue[idx].live = 0;
    queue[idx].value = NULL;
    compact_head();
    return result;
}

void space_gc_trace(void) {
    for (size_t k = 0; k < q_count; k++) {
        size_t idx = (q_head + k) % OEVM_SPACE_QUEUE_DEPTH;
        if (queue[idx].live) mark_object(queue[idx].value);
    }
    for (int i = 0; i < n_channels; i++) {
        struct SpaceChannel *c = &channels[i];
        for (int r = 0; r < c->n_reactions; r++) {
            mark_object(c->reactions[r].block);
            mark_object(c->reactions[r].owner);
        }
    }
}
