// Copyright (c) 2026 Ekorau LLC

// internal/space/progress.go
package space

import "database/sql"

// Sample is one reading of a node's progress counter: the count the node sent
// in a pull (`#wak`, one per check-in) and the moment porta heard it (rete#272 D3.3).
type Sample struct {
	Wak  int64
	AtMs int64
}

// Mark is the sample the previous sweep judged from, and the telCad it was
// judged under. The next window runs from here to the latest sample (rete#272;
// the model's `markAge`/`seenDelta`, docs/spikes/2026-08-27-liveness-model).
type Mark struct {
	Sample
	CadMs int64
}

// MarkOf reads the node's mark. ok is false while any of the three is NULL.
func (s *Store) MarkOf(nodeID string) (Mark, bool, error) {
	var w, at, cad sql.NullInt64
	err := s.db.QueryRow(
		`SELECT mark_wak, mark_ms, mark_cad FROM nodes WHERE id = ?`, nodeID).Scan(&w, &at, &cad)
	if err == sql.ErrNoRows {
		return Mark{}, false, nil
	}
	if err != nil {
		return Mark{}, false, err
	}
	if !w.Valid || !at.Valid || !cad.Valid {
		return Mark{}, false, nil
	}
	return Mark{Sample: Sample{Wak: w.Int64, AtMs: at.Int64}, CadMs: cad.Int64}, true, nil
}

// SetMark moves the node's mark.
func (s *Store) SetMark(nodeID string, m Mark) error {
	_, err := s.db.Exec(
		`UPDATE nodes SET mark_wak = ?, mark_ms = ?, mark_cad = ? WHERE id = ?`,
		m.Wak, m.AtMs, m.CadMs, nodeID)
	return err
}

// ForgetMark drops the node's mark. A verdict past the budget does this: the
// conversation the mark belonged to is over, and a node that returns is judged
// from its first new sample, never across the silence (rete#272 D3.3).
func (s *Store) ForgetMark(nodeID string) error {
	_, err := s.db.Exec(
		`UPDATE nodes SET mark_wak = NULL, mark_ms = NULL, mark_cad = NULL WHERE id = ?`, nodeID)
	return err
}
