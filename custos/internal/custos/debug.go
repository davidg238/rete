// Copyright (c) 2026 Ekorau LLC

package custos

// debug.go — the debug page (rete#340): `custos debug`'s session over one board
// cable, held from start-up, as a page. The page sends the CLI's own command
// lines and reads the session's State; it parses no prose.
//
//	GET  /debug                 the page
//	GET  /debug/source          the app's lines, and which have code
//	GET  /debug/state?after=N   the state once its Seq passes N (long-poll, 25 s)
//	POST /debug/do              one command line, as typed at `custos debug`
//
// One command at a time: a second while one is in flight answers 409 rather
// than queueing, as the profile page does.

import (
	_ "embed"
	"encoding/json"
	"io"
	"net/http"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/davidg238/rete/custos/internal/debug"
)

//go:embed debug.html
var debugPage []byte

// pollWait is how long /debug/state holds a request with nothing new.
const pollWait = 25 * time.Second

type sourceLine struct {
	Text string
	Code bool // a break here lands on this line
}

type source struct {
	File  string
	Lines []sourceLine
}

func mountDebug(mux *http.ServeMux, s *debug.Session, sm *debug.SourceMap) {
	var busy sync.Mutex
	code := sm.CodeLines()
	src := source{File: sm.Source, Lines: []sourceLine{}}
	for i, t := range sm.Text() {
		src.Lines = append(src.Lines, sourceLine{t, code[i+1]})
	}
	answer := func(w http.ResponseWriter, v any) {
		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("Cache-Control", "no-store")
		json.NewEncoder(w).Encode(v)
	}
	mux.HandleFunc("GET /debug", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		w.Write(debugPage)
	})
	mux.HandleFunc("GET /debug/source", func(w http.ResponseWriter, r *http.Request) {
		answer(w, src)
	})
	mux.HandleFunc("GET /debug/state", func(w http.ResponseWriter, r *http.Request) {
		after := -1
		if a := r.URL.Query().Get("after"); a != "" {
			n, err := strconv.Atoi(a)
			if err != nil {
				http.Error(w, "custos: /debug/state?after= takes a Seq", http.StatusBadRequest)
				return
			}
			after = n
		}
		answer(w, s.State(after, pollWait))
	})
	mux.HandleFunc("POST /debug/do", func(w http.ResponseWriter, r *http.Request) {
		body, err := io.ReadAll(io.LimitReader(r.Body, 4096))
		if err != nil {
			http.Error(w, err.Error(), http.StatusBadRequest)
			return
		}
		if !busy.TryLock() {
			http.Error(w, "custos: a command is already running", http.StatusConflict)
			return
		}
		defer busy.Unlock()
		s.Do(strings.TrimSpace(string(body))) // quit has nothing to leave: the cable stays held
		answer(w, s.State(-1, 0))
	})
}
