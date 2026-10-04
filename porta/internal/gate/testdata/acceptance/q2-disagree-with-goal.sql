-- §10 Q2: which nodes disagree with their goal/ tree right now (live binding).
SELECT node_id, key, goal, obs, obs_stale
  FROM convergence
 WHERE NOT converged
 ORDER BY node_id, key
