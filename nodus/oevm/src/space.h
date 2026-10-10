#ifndef OEVM_SPACE_H
#define OEVM_SPACE_H
/* Milestone C Task 1: the degenerate (exact-channel) TupleSpace C substrate —
   design doc docs/design/2026-07-06-milestone-c-channels-reactor-design.md
   section 6 ("the C seam carries data, not control"). This module owns:
     (1) the exact-match channel registry (path string -> small int id),
     (2) one node-wide FIFO event queue (D-C1),
     (3) enqueue/dequeue/take operations over that queue,
     (4) the owner-tagged reaction list storage per channel (section 6 point 2 —
         the tag is bought now for D's future "drop all reactions owned by job
         J" restart sweep; this task exposes no unregister/sweep verb).
   Every decision (value validation, capability prefix confinement, the
   reactor loop, registration policy) stays in nsl; this file is the storage +
   the ISR-callable post seam (space_post, spec section 6 point 1) only.

   Exact-channel dispatch (P1): a path is looked up/interned as an opaque
   whole string — no segment/prefix matching happens here. Capability prefix
   confinement (A1) is checked by nsl before it ever resolves to a full path
   string that reaches this module.

   Static capacities (overridable via -D, embedded-arena style): a fixed
   channel table, path buffer, ring-buffer queue depth, and per-channel
   reaction slots. Exhaustion of any of them is a Tier-2 crash with a FROZEN
   string (David-approved 2026-07-06, design doc section 7; the path-buffer
   one ratified post-Task-1 review, spec updated in nsl-tuvm 70aa2a4):
     "channel registry full", "channel queue full", "channel reactions full",
     "channel path too long".
*/
#include "oevm.h"
#include <stddef.h>

/* 64 since rete#233 (2026-10-10): the C6 bridge, with its debug and ota paths, held 32
   distinct paths, and the 33rd — `goal/apps/main` arriving for the first time — was an
   `abort()` on the board (c6spare, boot 40). The registry is static, so the cost is RAM
   at link time, printed by the footprint gate, not guessed here. */
#ifndef OEVM_SPACE_MAX_CHANNELS
#define OEVM_SPACE_MAX_CHANNELS 64
#endif
#ifndef OEVM_SPACE_MAX_PATH
#define OEVM_SPACE_MAX_PATH 64
#endif
#ifndef OEVM_SPACE_QUEUE_DEPTH
#define OEVM_SPACE_QUEUE_DEPTH 64
#endif
#ifndef OEVM_SPACE_MAX_REACTIONS
#define OEVM_SPACE_MAX_REACTIONS 8
#endif

/* Find-or-create the channel registered for the exact path [path, path+len).
   Returns the same small int id for the same path every time (registry id);
   a different path (including a prefix/suffix collision like "sensor" vs
   "sensors") always gets a distinct id. Aborts `channel path too long` when
   len >= OEVM_SPACE_MAX_PATH (checked first — a too-long path is invalid
   regardless of table occupancy); aborts `channel registry full` on the
   (OEVM_SPACE_MAX_CHANNELS+1)th distinct path. */
int space_intern(const char *path, size_t len);

/* How many distinct paths the registry holds right now. rete#233 T10: the MagTag build
   sets OEVM_SPACE_MAX_CHANNELS from this number, printed at every deep sleep — the
   knob is sized by a count, never by a guess. */
int space_channel_count(void);

/* The ISR-pulse seam (design doc section 6 point 1): enqueue (channel, value)
   at the FIFO tail, callable from C without entering nsl. Interns the path if
   it is new (a post to a channel with no reactions yet is a legal transient
   signal, P4 — dropped only at dispatch time, not here). A full ring NEVER
   aborts (tuvm#20, G2): the post is shed per the channel's kind — a CELL's
   poke coalesces into its own newest queued event (the value slot was already
   written by space_put), a STREAM's post is dropped newest-first — and every
   shed post is counted (space_overruns below). abort() in this module now
   means only structural invariants (registry full, path too long, reactions
   full, kind mismatch), never a load response. */
void space_post(const char *path, size_t len, oop value);

/* Like space_post but the channel is already interned to an id (the ISR-pulse
   mailbox stores ids, not path strings). Same tuvm#20 overflow contract. */
void space_post_id(int chan_id, oop value);

/* tuvm#20: posts shed by the full ring — cumulative per exact channel (never
   reset; a drain empties the ring, it does not rewrite history), or the
   node-wide total for len == 0. Never interns; a never-interned path reads 0,
   which is also the true count. Pull side of G2's "never silent" (prim 108
   `spaceOverruns:`); the periodic sys/metrics push side is tuvm#24. */
uint32_t space_overruns(const char *path, size_t len);

/* tuvm#24: monotonic max ring occupancy (q_count) ever observed — ring-slot
   pressure for G2 sizing, not logical depth (q_count counts lazily-dead
   not-yet-compacted slots too). Read by the sysStats prim. */
uint32_t space_queue_high_water(void);

/* Remove-if-present-now (design doc section 3, D-C2): the oldest
   queued-but-undispatched value on exactly this channel, removed from the
   queue. Returns the bare value, which MAY be nil (NULL) — a legal rank-0
   payload. Sets *found (when non-NULL) to 1 iff an event was pulled, 0 if the
   channel was empty or never posted: this is how a caller tells "took a nil"
   from "nothing queued" (tuvm#21). Never allocates (a drain loop stays
   stress-safe) and never interns; an over-length path answers not-found
   (found=0, NULL) rather than aborting — it can never have been interned, so
   absent is the correct answer for a query verb, whereas the same path
   reaching space_intern via space_post/space_register aborts. The nsl-visible
   Array-or-NULL wrap is materialized in the spaceTake: prim, not here. */
oop space_take(const char *path, size_t len, int *found);

/* Append a standing reaction (design doc section 6 point 2: owner-tagged for
   D's future restart sweep). Interns the path if new. Aborts
   `channel reactions full` on the (OEVM_SPACE_MAX_REACTIONS+1)th registration
   on one channel. Multiple registrations on one channel are fan-out (D-C3);
   duplicate (block, owner) pairs are two reactions, not deduped (D-C4). */
void space_register(const char *path, size_t len, oop block, oop owner);

/* Owner sweep (Milestone D Task 4 — the restart teardown verb): for every
   channel, remove every reaction whose stored owner oop is == to `owner`
   (raw pointer identity, matching the exact capability object), compacting
   each channel's reactions[] in place and decrementing n_reactions.
   Registration order is preserved among the survivors (D-C3). Used by the
   Supervisor (Task 5) to drop a crashed job's registrations before restart. */
void space_drop_reactions_owned_by(oop owner);

/* Dequeue the oldest queued event, node-wide FIFO order (D-C1). Answers NULL
   if the queue is empty; else a fresh 3-slot heap Array {value,
   blocksSnapshotArray, channelPath} (Milestone D widened this from 2 slots for
   crash records), where blocksSnapshotArray is a fresh Array of the channel's
   reaction blocks snapshotted AT THIS MOMENT (D-C5), in registration order
   (D-C3), and channelPath is the dequeued channel's path String. */
oop space_dequeue(void);

/* ---- Rung 9 (tuvm#32, option A): cells — a name that HAS a value ----------
   A channel is a STREAM (post/take: reading empties it) or a CELL (put/peek:
   it holds the latest value and reading does not remove it). The kind is set
   by the first WRITE and is fixed thereafter; `whenever:` works on both and
   sets no kind. Design: nsl-tuvm/docs/design/2026-07-13-rung9-cells-design.md

   Policy lives in nsl, exactly as this file's header says: `Channel>>put:value:`
   and `peek:ifAbsent:` check `space_kind` and raise a catchable nsl condition on
   a mismatch (the same division as checkPath:, whose capability confinement is
   also enforced in nsl). The abort below is an invariant net, not the error path. */
#define SPACE_KIND_UNSET  0
#define SPACE_KIND_STREAM 1
#define SPACE_KIND_CELL   2

/* The channel's kind, without creating a registry entry (asking about a name
   must not intern it — same rule space_take obeys). An over-length path can
   never have been interned, so it answers UNSET rather than aborting. */
int space_kind(const char *path, size_t len);

/* Write the cell: store `value` in the channel's value slot (interning the path
   if new), THEN space_post_id it, so `whenever:` reactions fire through the
   existing ring and reactor, unchanged. There is no second delivery mechanism —
   the only difference from a post is that the cell ALSO KEPT A COPY.
   Aborts `channel kind mismatch` if the path is already a stream: unreachable
   from safe nsl (the Channel guard raises first), so reaching it is a VM bug. */
void space_put(const char *path, size_t len, oop value);

/* The cell's value; NULL if the path is not a cell. Non-destructive, and it does
   not intern. NOTE: a cell that was written `nil` answers NULL here too — which
   is NOT the tuvm#21 ambiguity, because `space_kind` already told the caller the
   difference between "wrote nil" (CELL) and "never written" (UNSET). That is why
   Channel>>peek:ifAbsent: guards on the kind and not on the value. */
oop space_peek(const char *path, size_t len);

/* ---- Space-sync (porta space-sync rewrite, design D5) ---------------------
   Replication needs three things the raw event queue never had: a latest-value
   slot per cell (rung 9 gave that), ONE monotonic counter per space, and a
   changed-since-cursor query. This is the second and third.

   ONE counter per space, not one per path (D5, resolving a PROTOCOL-NSL
   §3/§5.1 contradiction): a per-path seq would make a cursor a VECTOR over
   every path, and the exchange's cursor is a scalar. Each channel additionally
   records the counter value of its own last write — exactly etcd's
   revision / mod_revision split.

   The counter is stamped inside space_post_id, which is on the path of BOTH
   writes (space_put fans out through it), so a cell write is stamped once and
   not twice. It is stamped BEFORE the full-ring check, for the same reason the
   kind is: a write the ring is about to shed still happened.

   uint32 wraps after 4.29e9 writes. At the fastest cadence this stack has ever
   run that is decades; a node that reached it would re-pull from a cursor it
   has already passed, which the exchange absorbs as an ordinary full replay. */
uint32_t space_seq(void);

/* Bump the counter and answer the new value. The bridge stamps a CAPTURED
   stream entry with this: a stream's value is not retained (space_dequeue kills
   the event on delivery), so it is captured by a reaction, and reading the
   counter instead of bumping it would let two captures with no intervening
   write share one seq. The gateway drops `seq <= high-water` as a duplicate, so
   that is silent LOSS, not a repeated delivery. Hence a bump. */
uint32_t space_stamp(void);

/* The counter value at that channel's last write; 0 if it has never been
   written (interning is not a write, exactly as `kind` records). */
uint32_t space_last_seq_of(int id);

/* Fill `ids` with the ids of CELL channels whose last write is newer than
   `cursor`, at or under `prefix`, ASCENDING BY last_seq, at most `max` of them;
   answers how many were written.

   CELLS ONLY: a stream's last_seq advances on every post, but its value is
   killed on delivery, so returning one would publish a stale-or-nil value under
   a fresh seq. Streams reach the wire by reaction capture instead.

   Prefix matching is SEGMENT-ALIGNED, the A1 rule `Channel>>resolve:` already
   obeys for whole paths: "obs" matches "obs" and "obs/health", never "obscure".
   plen == 0 matches everything (the root capability). Never interns — asking
   about names must not create one (space_take's rule).

   ASCENDING is load-bearing, not tidiness: the caller sends a PREFIX of this
   list and advances its cursor to the last entry it sent, so any other order
   would strand every entry it skipped. */
int space_changed_since(uint32_t cursor, const char *prefix, size_t plen,
                        int *ids, int max);

/* Read the row the scan named. `space_path_of` answers a pointer INTO the
   registry's static path buffer — NOT NUL-terminated, so take the length. It
   stays valid for the life of the process: channels are never removed. */
const char *space_path_of(int id, size_t *len_out);
int space_kind_of(int id);
oop space_value_of(int id);

/* The registry's own String constructor, exported for the spaceChangedSince:
   prim so a path String is built exactly the way space_dequeue builds one —
   one allocation idiom, one place to review for GC hazards. */
oop space_make_string_pub(const char *bytes, size_t len);

/* GC root trace: marks every queued (still-live) event's value oop, every
   registered reaction's block + owner oop, and EVERY CELL'S VALUE, across every
   channel. Called from gc.c's root scan beside the other builtin-vtable roots.
   The cell values are the rung-9 addition and they are reachable from nothing
   else in the heap — miss them and a live setpoint is collected. */
void space_gc_trace(void);

#endif /* OEVM_SPACE_H */
