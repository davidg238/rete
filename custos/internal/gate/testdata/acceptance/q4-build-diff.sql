-- §10 Q4: what changed between builds on the same node — boot × obs_build.
-- One row per (node, build) in the order the node ran them: its boots, its
-- span, the manifest pin and board it asserted, and the GC envelope's floor.
-- Each source is grouped once and joined: a correlated subquery over the
-- gc_envelope view re-runs the whole gc_cycle grouping per row, which on the
-- real store did not finish in 11 min.
WITH bb AS (
  SELECT node_id, build,
         COUNT(*)      AS boots,
         MIN(boot_id)  AS first_boot,
         MAX(boot_id)  AS last_boot,
         MIN(first_ms) AS from_ms,
         MAX(last_ms)  AS to_ms
    FROM boot
   WHERE build IS NOT NULL
   GROUP BY node_id, build
), ob AS (
  SELECT node_id, build, hex(man) AS man, board,
         ROW_NUMBER() OVER (PARTITION BY node_id, build ORDER BY ts_ms DESC) AS k
    FROM obs_build
), env AS MATERIALIZED (
  SELECT node_id, build, MIN(lfb_max) AS envelope_floor
    FROM gc_envelope
   GROUP BY node_id, build
)
SELECT bb.node_id, bb.build, bb.boots, bb.first_boot, bb.last_boot, bb.from_ms, bb.to_ms,
       ob.man, ob.board, env.envelope_floor
  FROM bb
  LEFT JOIN ob  ON ob.node_id = bb.node_id AND ob.build = bb.build AND ob.k = 1
  LEFT JOIN env ON env.node_id = bb.node_id AND env.build = bb.build
 ORDER BY bb.node_id, bb.from_ms
