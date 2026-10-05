-- §10 Q3: did any node's GC arena grow over its last soak. Segmented by boot
-- and build (lfb's `segment`), one least-squares slope of the envelope per
-- segment, and NO slope for a segment under ~5 h — lfb's validity rule: a run
-- flat over 9.95 h yields "falling 7.1 sigma" and "rising 8.5 sigma" in
-- adjacent 1.5 h slices. x is hours from the segment's first cycle, so the
-- sums stay small enough for a double.
WITH c AS (
  SELECT node_id, boot_id, build, lfb_max AS y, first_ms, last_ms,
         (last_ms - MIN(first_ms) OVER (PARTITION BY node_id, boot_id)) / 3600000.0 AS x
    FROM gc_envelope
), s AS (
  SELECT node_id, boot_id, build, COUNT(*) AS n,
         (MAX(last_ms) - MIN(first_ms)) / 3600000.0 AS span_h,
         COUNT(*) * SUM(x * y) - SUM(x) * SUM(y)    AS num,
         COUNT(*) * SUM(x * x) - SUM(x) * SUM(x)    AS den
    FROM c
   GROUP BY node_id, boot_id, build
)
SELECT node_id, boot_id, build, n AS cycles, ROUND(span_h, 2) AS span_h,
       CASE WHEN span_h >= 5 AND den > 0 THEN ROUND(num / den, 1) END AS lfb_slope_per_h,
       CASE WHEN span_h < 5 THEN 'refused: under ~5 h, lfb validity rule' END AS refusal
  FROM s
 ORDER BY node_id, boot_id
