// Copyright (c) 2026 Ekorau LLC

// internal/space/sweep.go
package space

import (
	"bytes"
	"fmt"
	"time"

	"github.com/davidg238/porta/internal/cval"
)

// SweepTick is how often the verdict is re-formed. It is a floor on detection
// latency and sits below the shortest cadence in use.
const SweepTick = 30 * time.Second

// The two verdicts, pre-encoded once. The transition test compares these bytes
// against what is stored: values in the store are raw deterministic CBOR and
// are decoded only for display (D4.1), so decoding here to compare two symbols
// would break that rule for nothing.
var (
	livenessLive   = mustEncode(cval.Symbol("live"))
	livenessSilent = mustEncode(cval.Symbol("silent"))
)

func mustEncode(v any) []byte {
	b, err := cval.Encode(v)
	if err != nil {
		panic(fmt.Sprintf("space: encoding a constant failed: %v", err))
	}
	return b
}

// LivenessChange is one transition, returned so the caller can log it. A sweep
// that changed nothing returns an empty slice — which is the normal case.
type LivenessChange struct {
	NodeID    string
	Silent    bool
	CadenceMs int64
	SinceMs   int64 // how long since the node was last heard from, at the verdict
}

// Sweep forms one liveness verdict per node and writes gw/liveness ONLY where
// the verdict differs from the one already stored.
//
// Why a ticker and not a lazy read-time view (design §6.3): the failure this
// must catch is porta hearing NOTHING, and a system that evaluates only when
// read is silent exactly when everything is. The ticker is not a convenience.
//
// Why transition-only: it makes `porta log` a list of the times the fleet went
// quiet. A row every 30 s would bury the one moment that matters.
//
// A node with no cadence yet (fewer than MinExchanges arrivals) is SKIPPED, not
// defaulted — gw/liveness stays unwritten rather than carrying a guess.
func (s *Store) Sweep() ([]LivenessChange, error) {
	now := s.nowMs()
	rows, err := s.db.Query(`
		SELECT n.id, COALESCE(n.last_seen_ms, 0), COALESCE(c.value_cbor, x'')
		FROM nodes n
		LEFT JOIN cells c ON c.node_id = n.id AND c.path = ?
		ORDER BY n.id`, PathLiveness)
	if err != nil {
		return nil, err
	}
	type row struct {
		id       string
		lastSeen int64
		stored   []byte
	}
	var nodes []row
	for rows.Next() {
		var r row
		if err := rows.Scan(&r.id, &r.lastSeen, &r.stored); err != nil {
			rows.Close()
			return nil, err
		}
		nodes = append(nodes, r)
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return nil, err
	}

	var changes []LivenessChange
	for _, n := range nodes {
		cadence, ok, err := s.CadenceMs(n.id)
		if err != nil {
			return changes, err
		}
		if !ok {
			continue
		}
		if err := s.recordCadence(n.id, cadence); err != nil {
			return changes, err
		}
		since := now - n.lastSeen
		silent := since > SilenceFactor*cadence
		want := livenessLive
		verdict := "live"
		if silent {
			want, verdict = livenessSilent, "silent"
		}
		if bytes.Equal(n.stored, want) {
			continue
		}
		reason := fmt.Sprintf("%s: %d ms since last exchange, cadence %d ms", verdict, since, cadence)
		if err := s.WriteGateway(n.id, PathLiveness, want, reason); err != nil {
			return changes, err
		}
		changes = append(changes, LivenessChange{
			NodeID: n.id, Silent: silent, CadenceMs: cadence, SinceMs: since,
		})
	}
	return changes, nil
}

// recordCadence writes the inferred rate back to the column that has been in the
// schema, unused, since the rewrite. It is written on every sweep and not only
// on a transition: it is a measurement, not a verdict.
func (s *Store) recordCadence(nodeID string, cadence int64) error {
	// An UPDATE that matches nothing is not an error here: the node row came
	// from the sweep's own query one statement ago.
	_, err := s.db.Exec(`UPDATE nodes SET cadence_ms = ? WHERE id = ?`, cadence, nodeID)
	return err
}
