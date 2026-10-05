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

	"github.com/davidg238/rete/custos/internal/gate"
)

// namedQuery is one statement the network surface runs, and the URL parameters
// bound to its placeholders, in order. Every parameter is required.
type namedQuery struct {
	sql    string
	params []string
}

// fleetQueries are the only statements the network surface runs.
var fleetQueries = map[string]namedQuery{
	"liveness": {sql: `SELECT node_id, role, verdict, reason, verdict_ms, last_seen_ms, boot_id, last_addr,
	                          board, retired_into
	                     FROM liveness ORDER BY node_id`},
	"convergence": {sql: `SELECT node_id, key, goal, obs, converged, goal_ms, obs_ms, obs_stale
	                        FROM convergence ORDER BY node_id, key`},
	"node": {sql: `SELECT node_id, path, value, direction, seq, boot_id, ts_ms, stale, actor, reason
	                 FROM cells WHERE node_id = ? ORDER BY path`, params: []string{"id"}},
}

//go:embed fleet.html
var fleetPage []byte

//go:embed node.html
var nodePage []byte

//go:embed fleet.css
var fleetCSS []byte

//go:embed fleet.js
var fleetJS []byte

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
	for path, f := range map[string]struct {
		ct   string
		body []byte
	}{
		"/fleet.css": {"text/css; charset=utf-8", fleetCSS},
		"/fleet.js":  {"text/javascript; charset=utf-8", fleetJS},
	} {
		mux.HandleFunc("GET "+path, func(w http.ResponseWriter, r *http.Request) {
			w.Header().Set("Content-Type", f.ct)
			w.Write(f.body)
		})
	}
	mux.HandleFunc("GET /node/{id}", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		w.Write(nodePage)
	})
	mux.HandleFunc("GET /q/{name}", func(w http.ResponseWriter, r *http.Request) {
		nq, ok := fleetQueries[r.PathValue("name")]
		if !ok {
			http.NotFound(w, r)
			return
		}
		args := make([]any, len(nq.params))
		for i, p := range nq.params {
			v := r.URL.Query().Get(p)
			if v == "" {
				http.Error(w, fmt.Sprintf("custos: /q/%s needs ?%s=", r.PathValue("name"), p), http.StatusBadRequest)
				return
			}
			args[i] = v
		}
		res, err := g.Query(r.Context(), gate.Query{SQL: nq.sql, Args: args, Deadline: 10 * time.Second})
		if err != nil {
			http.Error(w, err.Error(), http.StatusInternalServerError)
			return
		}
		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("Cache-Control", "no-store")
		json.NewEncoder(w).Encode(fleetAnswer{NowMs: time.Now().UnixMilli(), Result: res})
	})
}
