// Copyright (c) 2026 Ekorau LLC

package gate

// history.go — design §4.3's history views, offered on `snap` only. Each needs
// history the `live` binding deliberately does not hold (no events; exchanges
// capped at 24 h per node), so offered there it would answer from a truncated
// slice and read exactly like a whole one.
//
// They are plain SQL over the logical tables, authored here like the descriptor
// table and planned like any statement: the authorizer reports the base columns
// underneath, and only those are materialized.

import "fmt"

// viewBoot is the segmentation unit (rete#208): one row per (node, boot), its
// span from the node's own requests, and the build it ran. Boot 0 is excluded:
// it is "no persistent identity", not one boot, and grouping it would merge
// every unidentified run into one.
var viewBoot = viewDef{"boot", fmt.Sprintf(`
SELECT x.node_id       AS node_id,
       x.boot_id       AS boot_id,
       MIN(x.ts_ms)    AS first_ms,
       MAX(x.ts_ms)    AS last_ms,
       COUNT(*)        AS exchanges,
       (SELECT b.build FROM %[1]s b
         WHERE b.node_id = x.node_id AND b.boot_id = x.boot_id
         ORDER BY b.ts_ms DESC LIMIT 1) AS build
  FROM exchanges x
 WHERE x.boot_id <> 0
 GROUP BY x.node_id, x.boot_id`, TableName("obs/build"))}

// viewGCCycle is rete#153's unit: one row per GC cycle of one boot, carrying
// the per-cycle maximum of lfb (the only readable form of it) and minimum of
// lb. gcc is cumulative since boot, so (node, boot, gcc) is one cycle — which
// is why boot 0 is excluded, as in viewBoot: it is every boot recorded before
// the store kept boot ids, gcc restarts in each, and grouping them would merge
// unrelated cycles into one (found on the real store, addendum D5).
var viewGCCycle = viewDef{"gc_cycle", fmt.Sprintf(`
SELECT m.node_id     AS node_id,
       m.boot_id     AS boot_id,
       (SELECT b.build FROM %[1]s b
         WHERE b.node_id = m.node_id AND b.boot_id = m.boot_id
         ORDER BY b.ts_ms DESC LIMIT 1) AS build,
       m.gcc         AS gcc,
       MAX(m.lfb)    AS lfb_max,
       MIN(m.lb)     AS lb_min,
       COUNT(*)      AS samples,
       MIN(m.ts_ms)  AS first_ms,
       MAX(m.ts_ms)  AS last_ms
  FROM %[2]s m
 WHERE m.gcc IS NOT NULL AND m.boot_id <> 0
 GROUP BY m.node_id, m.boot_id, m.gcc`, TableName("obs/build"), TableName("sys/metrics"))}

// gcWarmupCycles is the "discard the first ~5 cycles of a boot" of lfb's
// validity rule (protocol/descriptors/records.yaml, rete#153). It is that rule's
// number, not a new tunable; when the rule changes, this follows it.
const gcWarmupCycles = 5

// viewGCEnvelope is the gauge itself: gc_cycle without a boot's warm-up. It
// filters on gcc rather than on a row number because gcc counts from boot — a
// window that starts mid-boot must not have its first sampled cycles discarded
// as if they were the boot's.
var viewGCEnvelope = viewDef{"gc_envelope", fmt.Sprintf(`
SELECT * FROM gc_cycle WHERE gcc > %d`, gcWarmupCycles)}

// viewRole is the commissioning history (first-cut §5.2): each goal/name write
// from when it was written until the next one for that node. The current role
// has no end.
var viewRole = viewDef{"role", fmt.Sprintf(`
SELECT g.value   AS role,
       g.node_id AS node_id,
       g.ts_ms   AS from_ms,
       LEAD(g.ts_ms) OVER (PARTITION BY g.node_id ORDER BY g.ts_ms, g.seq) AS to_ms
  FROM %s g`, TableName("goal/name"))}

// viewDeviceConflict is §6: one id whose boots overlap in time is two boards —
// a reboot's boots follow one another, a second board's interleave. Strict
// inequalities, so a boot that ends in the same millisecond the next begins is
// a reboot, not a conflict.
var viewDeviceConflict = viewDef{"device_conflict", `
SELECT a.node_id                  AS node_id,
       a.boot_id                  AS boot_a,
       b.boot_id                  AS boot_b,
       MAX(a.first_ms, b.first_ms) AS overlap_from,
       MIN(a.last_ms, b.last_ms)   AS overlap_to
  FROM boot a
  JOIN boot b
    ON b.node_id = a.node_id AND a.boot_id < b.boot_id
   AND a.first_ms < b.last_ms AND b.first_ms < a.last_ms`}

// viewDevice is §5.3's derived lineage. Two ids are one board when either
//
//   - boot: the persisted boot counter runs on across them — one id's boot k
//     hands over to the other's boot k+1 — under three conditions, each of
//     which the real store showed to be necessary (addendum D5). (1) Neither
//     side is an ordinary reboot: the old id has no boot k+1 of its own, and
//     the new id no boot k of its own. (2) The two are each other's NEAREST
//     candidate: the new boot is the earliest k+1 to start after the old one
//     ends, and the old boot the latest k to end before the new one starts.
//     Counters reset on reflash and stay small, so "some k+1 somewhere later"
//     matches boards weeks apart — on 2026-10-04 it chained all thirteen ids
//     into one device. (3) No boot of one id ever overlaps a boot of the
//     other: §5.3's invariant, non-overlapping is one board, overlapping two.
//   - board: the later id asserts the earlier as its bench board in obs/build
//     and was first heard after the earlier fell silent (addendum D4).
//
// The links are closed transitively, and each component is named by its most
// recently heard id — the board's current name. An id with no links is its own
// device.
var viewDevice = viewDef{"device", fmt.Sprintf(`
WITH RECURSIVE
cand(p_node, p_boot, p_last, q_node, q_boot, q_first) AS (
  SELECT p.node_id, p.boot_id, p.last_ms, q.node_id, q.boot_id, q.first_ms
    FROM boot p
    JOIN boot q ON q.boot_id = p.boot_id + 1 AND q.node_id <> p.node_id AND p.last_ms <= q.first_ms
   WHERE NOT EXISTS (SELECT 1 FROM boot o WHERE o.node_id = p.node_id AND o.boot_id = p.boot_id + 1)
     AND NOT EXISTS (SELECT 1 FROM boot o WHERE o.node_id = q.node_id AND o.boot_id = q.boot_id - 1)
     AND NOT EXISTS (
       SELECT 1 FROM boot r JOIN boot s
         ON r.node_id = p.node_id AND s.node_id = q.node_id
        AND r.first_ms < s.last_ms AND s.first_ms < r.last_ms)
),
link(a, b) AS (
  SELECT c.p_node, c.q_node
    FROM cand c
   WHERE NOT EXISTS (SELECT 1 FROM cand c2
                      WHERE c2.p_node = c.p_node AND c2.p_boot = c.p_boot AND c2.q_first < c.q_first)
     AND NOT EXISTS (SELECT 1 FROM cand c3
                      WHERE c3.q_node = c.q_node AND c3.q_boot = c.q_boot AND c3.p_last > c.p_last)
  UNION
  SELECT n.id, b.node_id
    FROM nodes n
    JOIN %[1]s b ON b.board = n.id AND b.node_id <> n.id
    JOIN nodes sn ON sn.id = b.node_id
   WHERE sn.first_seen_ms >= n.last_seen_ms
),
reach(node_id, peer) AS (
  SELECT id, id FROM nodes
  UNION
  SELECT r.node_id, l.b FROM reach r JOIN link l ON l.a = r.peer
  UNION
  SELECT r.node_id, l.a FROM reach r JOIN link l ON l.b = r.peer
)
SELECT r.node_id AS node_id,
       (SELECT r2.peer FROM reach r2 JOIN nodes pn ON pn.id = r2.peer
         WHERE r2.node_id = r.node_id
         ORDER BY pn.last_seen_ms DESC, r2.peer LIMIT 1) AS device,
       COUNT(*) AS ids
  FROM reach r
 GROUP BY r.node_id`, TableName("obs/build"))}

// historyViews are offered on `snap`, in dependency order: CREATE VIEW checks
// the tables a body names, so a view is created after the views it reads.
var historyViews = []viewDef{viewBoot, viewGCCycle, viewGCEnvelope, viewRole, viewDeviceConflict, viewDevice}
