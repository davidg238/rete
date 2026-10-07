// Copyright (c) 2026 Ekorau LLC

package custos

// debug.go — the debug page (rete#340): `custos debug`'s session over one board
// cable, held from start-up, as a page. The page sends the CLI's own command
// lines and reads the session's State; it parses no prose.
//
//	GET  /debug                 the page
//	GET  /debug/source          the app's lines, and which have code
//	GET  /debug/state?after=N   the state once its Seq passes N (long-poll, 25 s)
//	POST /debug/do              one command line, as typed at `custos debug`;
//	                            answers {OK, Error, State}
//	POST /debug/patch?method=V>>sel  the whole edited .nsl as the body: one
//	                            method compiled against the running image and
//	                            installed (peer design D17); answers as /debug/do
//
// One command at a time: a second while one is in flight answers 409 rather
// than queueing, as the profile page does. A load on the same cable (load.go)
// restarts the session over the new program's map, re-read from --debug-map.

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
	"github.com/davidg238/rete/custos/internal/nsl"
)

//go:embed debug.html
var debugPage []byte

// pollWait is how long /debug/state holds a request with nothing new.
const pollWait = 25 * time.Second

type SourceLine struct {
	Text string
	Code bool // a break here lands on this line
}

type Source struct {
	File  string
	Lines []SourceLine
}

// Done is /debug/do's answer: whether the command did what it says, why not
// (as the CLI said it, or the VM's dbg:error line), and the state after it.
type Done struct {
	OK    bool
	Error string
	State debug.State
}

// mountDebug serves s. busy is the cable's: a load on the same cable holds it.
func mountDebug(mux *http.ServeMux, prefixes []string, s *debug.Session, busy *sync.Mutex) {
	answer := func(w http.ResponseWriter, v any) {
		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("Cache-Control", "no-store")
		json.NewEncoder(w).Encode(v)
	}
	handle(mux, prefixes, "GET /debug", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		w.Write(debugPage)
	})
	handle(mux, prefixes, "GET /debug/source", func(w http.ResponseWriter, r *http.Request) {
		// Built per request: a load on the cable restarts the session on a new map.
		sm := s.SourceMap()
		code := sm.CodeLines()
		src := Source{File: sm.Source, Lines: []SourceLine{}}
		for i, t := range sm.Text() {
			src.Lines = append(src.Lines, SourceLine{t, code[i+1]})
		}
		answer(w, src)
	})
	handle(mux, prefixes, "GET /debug/state", func(w http.ResponseWriter, r *http.Request) {
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
	handle(mux, prefixes, "POST /debug/do", func(w http.ResponseWriter, r *http.Request) {
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
		res := s.Do(strings.TrimSpace(string(body))) // quit has nothing to leave: the cable stays held
		answer(w, Done{res.Err == "", res.Err, s.State(-1, 0)})
	})

	handle(mux, prefixes, "POST /debug/patch", func(w http.ResponseWriter, r *http.Request) {
		method := r.URL.Query().Get("method")
		if method == "" {
			http.Error(w, "custos: name the method, ?method=<Vtable>>selector", http.StatusBadRequest)
			return
		}
		edited, err := io.ReadAll(io.LimitReader(r.Body, 1<<20))
		if err != nil {
			http.Error(w, err.Error(), http.StatusBadRequest)
			return
		}
		if !busy.TryLock() {
			http.Error(w, "custos: a command is already running", http.StatusConflict)
			return
		}
		defer busy.Unlock()
		sm := s.SourceMap()
		text, err := nsl.Patch(sm.MapPath, sm.SourcePath, edited, method)
		if err != nil {
			// A refusal (a new constant, a block, another image) is the answer, not
			// a server fault: the VM was not touched.
			answer(w, Done{false, err.Error(), s.State(-1, 0)})
			return
		}
		res := s.Patch(text)
		answer(w, Done{res.Err == "", res.Err, s.State(-1, 0)})
	})
}
