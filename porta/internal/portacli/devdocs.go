// Copyright (c) 2026 Ekorau LLC

package portacli

import (
	"net/http"
	"strings"
)

// registerDevDocs mounts the dev status surface on mux: /dev/ serves the
// given docs directory read-only (the repo's docs/ tree — the status page,
// next-steps-overview.md, the design records), and the bare /dev lands on
// docs/status/dev-status.html so "hit /dev" is the whole instruction.
//
// This is a VIEW over files already on disk, refreshed by editing them — a
// status update must never require a gateway restart (a restart costs the
// nodes an outage). Markdown is served as text/plain so a browser displays
// the document of record instead of offering a download. GitHub stays the
// tracker of record; the page links out to it.
func registerDevDocs(mux *http.ServeMux, dir string) {
	if dir == "" {
		return
	}
	fs := http.FileServer(http.Dir(dir))
	mux.Handle("/dev/", http.StripPrefix("/dev/",
		http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			if strings.HasSuffix(r.URL.Path, ".md") {
				w.Header().Set("Content-Type", "text/plain; charset=utf-8")
			}
			fs.ServeHTTP(w, r)
		})))
	mux.HandleFunc("/dev", func(w http.ResponseWriter, r *http.Request) {
		http.Redirect(w, r, "/dev/status/dev-status.html", http.StatusFound)
	})
}
