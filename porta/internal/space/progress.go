// Copyright (c) 2026 Ekorau LLC

// internal/space/progress.go
package space

import (
	"database/sql"

	"github.com/davidg238/porta/internal/cval"
)

// Sample is one reading of a node's progress counter: obs/health's `wak` (the
// bridge's tick count, one per declared period) and the moment porta stored it.
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

// LatestSample reads wak out of the node's obs/health cell. ok is false when
// there is no cell, or the record carries no integer wak — never an error, for
// DeclaredLiveness' reason: one garbled record must not cost the fleet its sweep.
func (s *Store) LatestSample(nodeID string) (Sample, bool, error) {
	var raw []byte
	var at int64
	err := s.db.QueryRow(
		`SELECT value_cbor, ts_ms FROM cells WHERE node_id = ? AND path = ?`,
		nodeID, PathHealth).Scan(&raw, &at)
	if err == sql.ErrNoRows {
		return Sample{}, false, nil
	}
	if err != nil {
		return Sample{}, false, err
	}
	v, err := cval.Decode(raw)
	if err != nil {
		return Sample{}, false, nil
	}
	rec, ok := v.(cval.Record)
	if !ok || rec.Tag != "Health" {
		return Sample{}, false, nil
	}
	for _, p := range rec.Fields {
		if k, ok := p.K.(cval.Symbol); ok && k == "wak" {
			if n, ok := p.V.(int64); ok {
				return Sample{Wak: n, AtMs: at}, true, nil
			}
		}
	}
	return Sample{}, false, nil
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
