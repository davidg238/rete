-- §10 Q5: which nodes are stale, and why (live binding). porta's own verdict
-- and reason (addendum D1), never re-derived; NULL is "not judged yet".
SELECT node_id, role, verdict, reason, verdict_ms, last_seen_ms
  FROM liveness
 WHERE verdict IS NULL OR verdict <> 'live'
 ORDER BY node_id
