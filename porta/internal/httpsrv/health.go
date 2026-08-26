// Copyright (c) 2026 Ekorau LLC

// internal/httpsrv/health.go
package httpsrv

import (
	"encoding/json"
	"net/http"

	"github.com/davidg238/porta/internal/serverstat"
)

// healthHandler returns 200 with a small JSON body summarizing the
// gateway's state for uptime checks and the future ops dashboard:
//
//	{"status":"ok","nodes":<int>}
//
// The endpoint reports the listener itself is healthy. A nil counter or a
// transient count failure renders nodes:-1 but still 200 — the HTTP listener
// is up even if the DB blip is in progress, and during the space-sync rewrite
// it is up before any store exists at all.
// statsFn returns the live stats holder (or nil) at request time, so a stats
// holder attached after New still enriches the response.
func healthHandler(nodes NodeCounter, statsFn func() *serverstat.Stats) http.HandlerFunc {
	return func(w http.ResponseWriter, _ *http.Request) {
		n := -1
		if nodes != nil {
			if c, err := nodes.CountNodes(); err == nil {
				n = c
			}
		}
		out := map[string]any{"status": "ok", "nodes": n}
		// Keep /health a cheap liveness probe: only the in-memory stats (version,
		// uptime, reject count) — no PRAGMA/stat. The heavy db metrics live in
		// /api/status.
		if statsFn != nil {
			if s := statsFn(); s != nil {
				snap := s.Snapshot()
				out["version"] = snap.Version
				out["uptime_s"] = snap.UptimeSeconds
				// rete#176: present only when degraded — probes keying on the
				// healthy body shape see nothing new. status stays "ok": the
				// exchange (what this endpoint probes) IS serving.
				if snap.BlocksPlaneDown != "" {
					out["blocks_plane_down"] = snap.BlocksPlaneDown
				}
			}
		}
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusOK)
		_ = json.NewEncoder(w).Encode(out)
	}
}
