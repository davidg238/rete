// Copyright (c) 2026 Ekorau LLC

// internal/space/runs.go
package space

// The gateway's record of its own presence (rete#231 D6). The store must not
// keep asserting the last thing it heard: a verdict formed across an interval
// porta was not listening is not evidence about the node, and reporting it as
// one is the same defect as a gate that reads green while testing nothing —
// the record is not missing, it is positively wrong, and it is unfalsifiable
// from the store alone.

// StartRun opens this process's row. Called once, from `porta serve`, before
// the sweep goroutine starts.
func (s *Store) StartRun() error {
	now := s.nowMs()
	res, err := s.db.Exec(
		`INSERT INTO gw_runs (started_ms, alive_ms) VALUES (?, ?)`, now, now)
	if err != nil {
		return err
	}
	id, err := res.LastInsertId()
	if err != nil {
		return err
	}
	s.runID = id
	return nil
}

// MarkAlive stamps the open run. The sweep calls it at the TOP of every tick,
// BEFORE forming any verdict, and that order is load-bearing: a verdict formed
// at `now` against a run whose alive_ms is one tick behind would find its own
// window uncovered and report #unknown about a gateway that was plainly awake.
//
// A store with no open run (every read-only CLI path) is a no-op rather than an
// error: `porta get` does not claim to have been listening.
func (s *Store) MarkAlive() error {
	if s.runID == 0 {
		return nil
	}
	_, err := s.db.Exec(`UPDATE gw_runs SET alive_ms = ? WHERE id = ?`, s.nowMs(), s.runID)
	return err
}

// WasDeaf answers whether [fromMs, toMs] contains ANY moment porta was not
// known to be listening.
//
// It is deliberately a coverage question and not a gap question: the window is
// deaf unless the recorded runs cover every millisecond of it. That makes the
// honest answer the DEFAULT — a store that predates this table answers "deaf"
// for every window before its first run, which is exactly true of it, and no
// backfill is possible or attempted.
func (s *Store) WasDeaf(fromMs, toMs int64) (bool, error) {
	rows, err := s.db.Query(
		`SELECT started_ms, alive_ms FROM gw_runs ORDER BY started_ms`)
	if err != nil {
		return false, err
	}
	defer rows.Close()
	cursor := fromMs
	for rows.Next() {
		var started, alive int64
		if err := rows.Scan(&started, &alive); err != nil {
			return false, err
		}
		if alive < cursor {
			continue // a run that ended before the window opened
		}
		if started > cursor {
			return true, nil // an uncovered gap at the head of what is left
		}
		cursor = alive
		if cursor >= toMs {
			return false, nil
		}
	}
	if err := rows.Err(); err != nil {
		return false, err
	}
	return cursor < toMs, nil
}
