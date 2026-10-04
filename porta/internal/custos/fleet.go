// Copyright (c) 2026 Ekorau LLC

package custos

// fleet.go — the fleet page: what the nodes are doing right now, from porta's
// live store through the gate's `live` binding (first-cut design addendum
// 2026-10-04).
//
// The network surface is a FIXED set of named queries. There is no route that
// takes SQL: a name selects a statement authored here, so what a browser can
// ask is exactly this table. G5 is deliberately silent — everything runs inside
// Tailscale, and the allowlist is the perimeter (David, 2026-10-04).
//
// The page itself is a static file that renders the JSON, so the machine
// surface is primary and the human view is a rendering of it (§9.3).

import (
	_ "embed"
	"encoding/json"
	"fmt"
	"net/http"
	"time"

	"github.com/davidg238/porta/internal/gate"
)

// fleetQueries are the only statements the network surface runs.
var fleetQueries = map[string]string{
	"liveness": `SELECT node_id, role, verdict, reason, verdict_ms, last_seen_ms, boot_id, last_addr
	               FROM liveness ORDER BY node_id`,
	"convergence": `SELECT node_id, key, goal, obs, converged, goal_ms, obs_ms, obs_stale
	                  FROM convergence ORDER BY node_id, key`,
}

//go:embed fleet.html
var fleetPage []byte

// fleetAnswer is the gate's answer plus the gateway's clock, so a page renders
// ages against the clock that stamped them rather than the browser's.
type fleetAnswer struct {
	NowMs int64 `json:"now_ms"`
	*gate.Result
}

// openFleet opens the live binding over porta's store.
func openFleet(store, descriptors string) (*gate.Gate, error) {
	if descriptors == "" {
		return nil, fmt.Errorf("custos: --store needs --descriptors (protocol/descriptors/records.yaml)")
	}
	d, err := gate.LoadDescriptors(descriptors)
	if err != nil {
		return nil, fmt.Errorf("custos: descriptors: %w", err)
	}
	return gate.OpenLive(store, d)
}

func mountFleet(mux *http.ServeMux, g *gate.Gate) {
	mux.HandleFunc("GET /fleet", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		w.Write(fleetPage)
	})
	mux.HandleFunc("GET /q/{name}", func(w http.ResponseWriter, r *http.Request) {
		sql, ok := fleetQueries[r.PathValue("name")]
		if !ok {
			http.NotFound(w, r)
			return
		}
		res, err := g.Query(r.Context(), gate.Query{SQL: sql, Deadline: 10 * time.Second})
		if err != nil {
			http.Error(w, err.Error(), http.StatusInternalServerError)
			return
		}
		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("Cache-Control", "no-store")
		json.NewEncoder(w).Encode(fleetAnswer{NowMs: time.Now().UnixMilli(), Result: res})
	})
}
