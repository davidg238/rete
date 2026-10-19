/* Milestone C Task 1: exact-channel registry + FIFO event queue + owner-tagged
   reaction storage + C-callable post. See src/space.h for the module contract
   and docs/design/2026-07-06-milestone-c-channels-reactor-design.md
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
#include "platform.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* Milestone D Task 2: build the channel-path String for the crash record
   (Space>>drain needs it to post `Record tag: #crash ... with: #channel ->
   chPath` and to run the sys/-loop guard). Same local-ctor idiom as
   udp_make_string (src/udp.c) / the file-static make_string in dispatch.c —
   gc_allocate(StringVtable, ...) directly, since dispatch.c's make_string is
   file-static and not reachable here. */
extern oop StringVtable;

oop space_make_string_pub(const char *bytes, size_t len) {
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
    /* Rung 9 (tuvm#32 option A). `kind` is set by the first WRITE — post makes a
       stream, put makes a cell — and `value` is the cell's slot. `kind == CELL`
       is itself the has-a-value flag: only space_put sets it, and space_put always
       stores a value. So a cell written `nil` (value == NULL, kind == CELL) is
       distinguishable from a name nobody ever wrote (kind == UNSET) — which is
       what lets Channel>>peek:ifAbsent: be correct without waiting on tuvm#21. */
    oop value;
    uint8_t kind;
    /* tuvm#20 (G2): posts to this channel shed by a full ring — cumulative,
       never reset (a drain empties the ring; it does not rewrite history). */
    uint32_t overruns;
    /* Space-sync (design D5): the counter value at this channel's last write.
       0 means never written — interning is not a write, exactly as `kind`
       records. This is etcd's mod_revision to `seq_counter`'s revision. */
    uint32_t last_seq;
};

struct SpaceEvent {
    int chan;   /* index into channels[]; only meaningful while live */
    oop value;
    int live;
};

static struct SpaceChannel channels[OEVM_SPACE_MAX_CHANNELS];
static int n_channels = 0;

int space_channel_count(void) { return n_channels; }

static struct SpaceEvent queue[OEVM_SPACE_QUEUE_DEPTH];
static size_t q_head = 0;    /* oldest occupied ring slot (live, whenever count > 0) */
static size_t q_count = 0;   /* occupied ring slots (see file-header note on staleness) */

/* tuvm#20 (G2): posts shed by a full ring, node-wide — the sum of every
   channel's `overruns`, kept incrementally so the empty-path read in
   space_overruns is O(1). */
static uint32_t total_overruns = 0;

/* tuvm#24: monotonic max of q_count ever observed. NOTE this counts ring SLOTS
   — q_count includes lazily-dead not-yet-compacted slots (see the file-header
   staleness note) — so it measures ring-slot PRESSURE (slots unavailable to a
   burst, the number G2 depth-sizing needs), not logical queue depth. */
static uint32_t q_high_water = 0;

/* Space-sync (design D5): ONE monotonic counter for the whole space, stamped on
   every write, and bumpable by space_stamp for a captured stream entry. One
   counter and not one per path, because the exchange's cursor is a scalar —
   see the space.h header for the full argument. */
static uint32_t seq_counter = 0;

/* Count one shed post — and say so ONCE per channel: the first overrun prints
   a single line so a console human sees the onset without being flooded;
   thereafter the counters are the record (G2 "never silent", pull side via
   space_overruns / prim 108; the push side — the periodic sys/metrics
   record — is tuvm#24). */
static void note_overrun(struct SpaceChannel *c) {
    c->overruns++;
    total_overruns++;
    if (c->overruns == 1) {
        fprintf(stderr, "channel overrun: %.*s\n", (int)c->len, c->path);
    }
}

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
    c->value = NULL;
    c->kind = SPACE_KIND_UNSET;   /* interning is not a write: `whenever:` on a
                                     name nobody has written leaves it UNSET */
    c->overruns = 0;
    c->last_seq = 0;              /* interning is not a write */
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

void space_post_id(int chan_id, oop value) {
    struct SpaceChannel *c = &channels[chan_id];
    /* Rung 9: the first write fixes the kind. UNSET -> STREAM here; a CELL is
       left alone, because space_put sets CELL and then calls THIS function to
       fan the write out to `whenever:` reactions through the one reactor.
       Fixed BEFORE the full-ring check: a write the ring is about to shed
       still happened — declaring the kind is not contingent on delivery. */
    if (c->kind == SPACE_KIND_UNSET) {
        c->kind = SPACE_KIND_STREAM;
    }
    /* Space-sync (design D5): stamp the write. HERE rather than in space_put and
       space_post separately, because both of those reach this function —
       space_put writes the value slot and then calls us to fan out — so one
       stamp covers both and a cell write is not counted twice. Before the
       full-ring check for the same reason the kind fix is: a write the ring is
       about to shed still happened, and declaring it is not contingent on
       delivery. */
    c->last_seq = ++seq_counter;
    if (q_count >= OEVM_SPACE_QUEUE_DEPTH) {
        /* tuvm#20 — the G2 overflow policy, replacing the abort() that made a
           burst of events a whole-node kill (bypassing the crash boundary and
           supervisor entirely). The ring never kills the node; the policy
           derives from the kind, so it needs no per-source registration API:
             CELL   — the queued events are doorbell pokes and the value is
                      already in the slot (space_put wrote it before calling
                      here), so COALESCE: overwrite this cell's newest queued
                      poke with the new value. Under overload N puts degrade
                      to one firing with the latest value — G2's latest-value
                      case; 16_cells/04's fires-on-WRITE guarantee yields only
                      here, and the yield is counted.
             STREAM — events are distinct letters; merging them corrupts
                      meaning (imagine merged crash records). DROP NEWEST and
                      count: already-queued events keep causal order, a reader
                      sees history up to the overload point. (Drop-oldest
                      rejected — David, 2026-07-14: the crash that STARTED
                      the storm must not be the record lost.)
           Either way note_overrun makes it observable. abort() in this file
           now means only structural invariants, never a load response. */
        if (c->kind == SPACE_KIND_CELL) {
            for (size_t k = q_count; k > 0; k--) {
                size_t idx = (q_head + k - 1) % OEVM_SPACE_QUEUE_DEPTH;
                if (queue[idx].live && queue[idx].chan == chan_id) {
                    queue[idx].value = value;   /* coalesce: latest wins */
                    note_overrun(c);
                    return;
                }
            }
            /* No poke of this cell in the ring to coalesce into: shed the
               incoming one. The value is still in the slot — peek: answers
               it — so the cell's contract survives even total starvation. */
        }
        note_overrun(c);
        return;
    }
    size_t tail = (q_head + q_count) % OEVM_SPACE_QUEUE_DEPTH;
    queue[tail].chan = chan_id;
    /* nil (NULL) is a legal rank-0 value payload; the pull side distinguishes a
       stored nil from an empty channel via space_take's `found` out-param
       (tuvm#21), so it is stored here without ceremony. */
    queue[tail].value = value;
    queue[tail].live = 1;
    q_count++;
    if (q_count > q_high_water) q_high_water = (uint32_t)q_count;
}

void space_post(const char *path, size_t len, oop value) {
    space_post_id(space_intern(path, len), value);
}

oop space_take(const char *path, size_t len, int *found) {
    int chan = find_channel(path, len);
    if (chan < 0) { if (found) *found = 0; return NULL; }
    for (size_t k = 0; k < q_count; k++) {
        size_t idx = (q_head + k) % OEVM_SPACE_QUEUE_DEPTH;
        if (queue[idx].live && queue[idx].chan == chan) {
            oop value = queue[idx].value;
            queue[idx].live = 0;
            queue[idx].value = NULL;   /* drop the reference promptly for GC */
            if (idx == q_head) compact_head();
            if (found) *found = 1;
            return value;
        }
    }
    if (found) *found = 0;
    return NULL;
}

/* ---- Rung 9: cells (tuvm#32 option A) ------------------------------------
   A cell is a pigeonhole that HOLDS the latest letter; a stream is one you
   empty by reading. The three entry points below are storage only — the kind
   RULE ("wrong verb on an existing name is an error") is enforced in nsl by
   Channel>>put:value: / peek:ifAbsent:, which raise a catchable condition. That
   is the same division of labour as checkPath:, whose capability confinement is
   likewise enforced in nsl and not here (file header, and space.h). */

int space_kind(const char *path, size_t len) {
    int chan = find_channel(path, len);
    if (chan < 0) return SPACE_KIND_UNSET;   /* never interned == nobody ever wrote it */
    return (int)channels[chan].kind;
}

void space_put(const char *path, size_t len, oop value) {
    int chan = space_intern(path, len);
    struct SpaceChannel *c = &channels[chan];
    if (c->kind == SPACE_KIND_STREAM) {
        /* Unreachable from safe nsl: Channel>>put:value: checks space_kind and
           raises first. Reaching this means the guard was bypassed — a VM bug,
           which is exactly what this file's Tier-2 frozen-string aborts are for
           (cf. "channel registry full"). It is a net, not the error path. */
        fprintf(stderr, "channel kind mismatch\n");
        abort();
    }
    c->kind = SPACE_KIND_CELL;
    c->value = value;
    /* Then fan out through the EXISTING ring and reactor, so a `whenever:` on a
       cell fires on write with no second delivery mechanism and no change to
       Space>>drain. If nobody is registered, the event is drained and dropped
       (space_dequeue) — and the value stays in the slot. That case is not a
       leak; it is the entire reason a cell exists. */
    space_post_id(chan, value);
}

oop space_peek(const char *path, size_t len) {
    int chan = find_channel(path, len);
    if (chan < 0) return NULL;
    if (channels[chan].kind != SPACE_KIND_CELL) return NULL;
    return channels[chan].value;
}

/* tuvm#20: shed-post count for one exact channel; len == 0 answers the node
   total. Does NOT intern (space_take's rule: asking about a name must not
   create it) — a never-interned path reads 0, which is also the true count. */
uint32_t space_overruns(const char *path, size_t len) {
    if (len == 0) return total_overruns;
    int chan = find_channel(path, len);
    if (chan < 0) return 0;
    return channels[chan].overruns;
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

/* Owner sweep (Milestone D Task 4): compact every channel's reactions[] in
   place, dropping every reaction whose owner is == the given capability.
   Read-index rd scans every reaction; write-index w only advances past
   survivors, so shifting reactions[rd] down to reactions[w] preserves
   registration order among what remains (D-C3). No GC hazard: this only
   moves existing SpaceReaction structs within the static channels[] array —
   no allocation happens here. */
void space_drop_reactions_owned_by(oop owner) {
    for (int c = 0; c < n_channels; c++) {
        struct SpaceChannel *ch = &channels[c];
        int w = 0;
        for (int rd = 0; rd < ch->n_reactions; rd++) {
            if (ch->reactions[rd].owner != owner) {
                if (w != rd) ch->reactions[w] = ch->reactions[rd];
                w++;
            }
        }
        ch->n_reactions = w;
    }
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
    oop path_string = space_make_string_pub(c->path, c->len);
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
        /* Rung 9: a cell's value is a GC root. It is reachable from NOTHING else
           in the heap — that is the whole point of a cell: it outlives the job
           that wrote it, and outlives the queued event that announced it (which
           space_dequeue kills on delivery). Miss this line and a running plant
           gets a collected setpoint. Guarded by nodus/tests/15_cells/05_gc_root. */
        if (c->kind == SPACE_KIND_CELL && c->value) mark_object(c->value);
    }
}

uint32_t space_queue_high_water(void) { return q_high_water; }

/* ---- Space-sync (design D5) -----------------------------------------------
   The counter, the per-channel last_seq readers, and the prefix-scoped
   changed-since scan that backs `Channel>>changedSince:max:`. Storage only, as
   ever: WHICH prefix a caller may scan is capability policy and is resolved in
   nsl before it reaches here, exactly as checkPath: confines the write verbs. */

uint32_t space_seq(void)   { return seq_counter; }
uint32_t space_stamp(void) { return ++seq_counter; }

uint32_t space_last_seq_of(int id) { return channels[id].last_seq; }
int      space_kind_of(int id)     { return (int)channels[id].kind; }
oop      space_value_of(int id)    { return channels[id].value; }

const char *space_path_of(int id, size_t *len_out) {
    if (len_out) *len_out = channels[id].len;
    return channels[id].path;
}

/* Segment-aligned prefix test — the A1 rule Channel>>resolve: enforces for whole
   paths, restated here because this scan takes a PREFIX: "obs" is at-or-above
   "obs" and "obs/health" and is NOT above "obscure". */
static int under_prefix(const struct SpaceChannel *c, const char *prefix, size_t plen) {
    if (plen == 0) return 1;                        /* the root capability */
    if (c->len < plen) return 0;
    if (memcmp(c->path, prefix, plen) != 0) return 0;
    return c->len == plen || c->path[plen] == '/';
}

int space_changed_since(uint32_t cursor, const char *prefix, size_t plen,
                        int *ids, int max) {
    int n = 0;
    /* `taken` is the last seq already emitted, so each pass picks the smallest
       last_seq strictly above it: a selection walk, ascending. Seqs are unique
       across channels (one counter), so there are no ties to break. n_channels
       is bounded by OEVM_SPACE_MAX_CHANNELS, so the quadratic
       walk is bounded by a small constant and needs no scratch allocation —
       which also keeps this callable from a drain-time reaction. */
    uint32_t taken = cursor;
    while (n < max) {
        int best = -1;
        for (int i = 0; i < n_channels; i++) {
            struct SpaceChannel *c = &channels[i];
            if (c->kind != SPACE_KIND_CELL) continue;
            if (c->last_seq <= taken) continue;
            if (!under_prefix(c, prefix, plen)) continue;
            if (best < 0 || c->last_seq < channels[best].last_seq) best = i;
        }
        if (best < 0) break;
        ids[n++] = best;
        taken = channels[best].last_seq;
    }
    return n;
}
