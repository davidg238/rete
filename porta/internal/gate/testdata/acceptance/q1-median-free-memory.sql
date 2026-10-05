-- §10 Q1: median free memory per node over its last month of data.
-- Read from gc_envelope (max lfb per GC cycle, warm-up discarded), never from
-- raw sys_metrics.lfb: lfb's validity rule says a point sample is INVALID.
-- "Last month" is the node's own last 30 days of cycles, not wall-clock now:
-- a snapshot's now is the moment it was taken.
-- The node's latest cycle is a window over one pass of the view: a correlated
-- MAX over gc_envelope re-runs the whole gc_cycle grouping per row, which on
-- the real store did not finish in 17 min.
WITH e AS (
  SELECT node_id, lfb_max, last_ms,
         MAX(last_ms) OVER (PARTITION BY node_id) AS latest_ms
    FROM gc_envelope
), w AS (
  SELECT node_id, lfb_max FROM e WHERE last_ms >= latest_ms - 30 * 86400000
), r AS (
  SELECT node_id, lfb_max,
         ROW_NUMBER() OVER (PARTITION BY node_id ORDER BY lfb_max) AS i,
         COUNT(*)     OVER (PARTITION BY node_id)                  AS n
    FROM w
)
SELECT node_id, AVG(lfb_max) AS median_lfb, MAX(n) AS cycles
  FROM r
 WHERE i IN ((n + 1) / 2, (n + 2) / 2)
 GROUP BY node_id
 ORDER BY node_id
