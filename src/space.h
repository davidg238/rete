#ifndef OEVM_SPACE_H
#define OEVM_SPACE_H
/* Milestone C Task 1: the degenerate (exact-channel) TupleSpace C substrate —
   design doc docs/superpowers/specs/2026-07-06-milestone-c-channels-reactor-design.md
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

#ifndef OEVM_SPACE_MAX_CHANNELS
#define OEVM_SPACE_MAX_CHANNELS 32
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

/* The ISR-pulse seam (design doc section 6 point 1): enqueue (channel, value)
   at the FIFO tail, callable from C without entering nsl. Interns the path if
   it is new (a post to a channel with no reactions yet is a legal transient
   signal, P4 — dropped only at dispatch time, not here). Aborts
   `channel queue full` when the ring is at capacity. */
void space_post(const char *path, size_t len, oop value);

/* Remove-if-present-now (design doc section 3, D-C2): the oldest
   queued-but-undispatched value on exactly this channel, removed from the
   queue. Answers NULL (nil) if the channel has never been posted to, or has
   no undispatched event, without creating a registry entry as a side effect.
   Deliberate asymmetry: an over-length path (len >= OEVM_SPACE_MAX_PATH) also
   answers NULL here rather than aborting — it can never have been interned,
   so absent-is-nil is the correct answer for a query verb — whereas the same
   over-length path reaching space_intern via space_post/space_register
   (out:/whenever:) aborts `channel path too long`. */
oop space_take(const char *path, size_t len);

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
   if the queue is empty; else a fresh 2-slot heap Array {value,
   blocksSnapshotArray}, where blocksSnapshotArray is a fresh Array of the
   channel's reaction blocks snapshotted AT THIS MOMENT (D-C5), in
   registration order (D-C3). */
oop space_dequeue(void);

/* GC root trace: marks every queued (still-live) event's value oop, and every
   registered reaction's block + owner oop, across every channel. Called from
   gc.c's root scan beside the other builtin-vtable roots. */
void space_gc_trace(void);

#endif /* OEVM_SPACE_H */
