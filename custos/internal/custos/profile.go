// Copyright (c) 2026 Ekorau LLC

package custos

// profile.go — the profile page (rete#312 Task 6): the same capture and ranking
// as `custos profile -port`, on one board cable configured at start-up.
//
//	GET  /profile               the page
//	GET  /profile/last          the last capture, as JSON (404 before the first)
//	POST /profile/capture?ms=N  open a window for N ms, rank it, keep it as last
//
// The map is re-read on every capture, so a reflashed board is ranked against
// the map written beside its new image. One capture at a time: the cable is
// opened exclusively, and a second request answers 409 rather than queueing.

import (
	_ "embed"
	"encoding/json"
	"net/http"
	"strconv"
	"sync"
	"time"

	"github.com/davidg238/rete/custos/internal/imagemap"
	"github.com/davidg238/rete/custos/internal/profile"
)

//go:embed profile.html
var profilePage []byte

// Capture is one window, as the page receives it.
type Capture struct {
	CapturedMs int64
	Ms         int
	Map        string
	Report     profile.Report
	// A porta node's request (rete#415): Pending until the dump's end line
	// arrives by the console tee, Seq porta's for the goal/profile write, Error why a
	// capture that started could not finish.
	Pending bool   `json:",omitempty"`
	Seq     int64  `json:",omitempty"`
	Error   string `json:",omitempty"`
}

// mountProfile serves captures through open. busy is the board's: its other
// verbs hold it too.
func mountProfile(mux *http.ServeMux, prefixes []string, mapPath string, open func(onLine func(string), onEnd func()) (profile.Sender, error), busy *sync.Mutex) {
	var (
		mu   sync.Mutex
		last *Capture
	)
	answer := func(w http.ResponseWriter, c *Capture) {
		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("Cache-Control", "no-store")
		json.NewEncoder(w).Encode(c)
	}
	handle(mux, prefixes, "GET /profile", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		w.Write(profilePage)
	})
	handle(mux, prefixes, "GET /profile/last", func(w http.ResponseWriter, r *http.Request) {
		mu.Lock()
		c := last
		mu.Unlock()
		if c == nil {
			http.Error(w, "custos: no capture yet", http.StatusNotFound)
			return
		}
		answer(w, c)
	})
	handle(mux, prefixes, "POST /profile/capture", func(w http.ResponseWriter, r *http.Request) {
		ms, err := strconv.Atoi(r.URL.Query().Get("ms"))
		if err != nil || ms <= 0 {
			http.Error(w, "custos: /profile/capture needs ?ms= a positive window length", http.StatusBadRequest)
			return
		}
		if !busy.TryLock() {
			http.Error(w, "custos: the cable is busy (a capture or a load is running)", http.StatusConflict)
			return
		}
		defer busy.Unlock()
		m, err := imagemap.Load(mapPath)
		if err != nil {
			http.Error(w, err.Error(), http.StatusInternalServerError)
			return
		}
		text, err := profile.Capture(open, ms, time.Duration(ms)*time.Millisecond+10*time.Second)
		if err != nil {
			http.Error(w, err.Error(), http.StatusBadGateway)
			return
		}
		c := &Capture{CapturedMs: time.Now().UnixMilli(), Ms: ms, Map: mapPath, Report: profile.NewReport(profile.Parse(text), m)}
		mu.Lock()
		last = c
		mu.Unlock()
		answer(w, c)
	})
}
