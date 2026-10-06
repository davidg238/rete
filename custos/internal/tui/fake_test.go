// Copyright (c) 2026 Ekorau LLC

package tui

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strconv"
	"sync"
	"testing"
	"time"

	"github.com/davidg238/rete/custos/internal/custos"
	"github.com/davidg238/rete/custos/internal/debug"
)

type fake struct {
	mu   sync.Mutex
	st   debug.State
	cmds []string
	hold chan struct{} // when non-nil, /debug/do waits on it
}

func (f *fake) set(st debug.State) { f.mu.Lock(); f.st = st; f.mu.Unlock() }

func newFake(t *testing.T) (*fake, *httptest.Server) {
	f := &fake{st: debug.State{Seq: 1, Run: "boot"}}
	mux := http.NewServeMux()
	js := func(w http.ResponseWriter, v any) { json.NewEncoder(w).Encode(v) }
	mux.HandleFunc("GET /debug/source", func(w http.ResponseWriter, r *http.Request) {
		js(w, custos.Source{File: "/x/a.nsl", Lines: []custos.SourceLine{{Text: "one"}, {Text: "two", Code: true}, {Text: "three", Code: true}}})
	})
	mux.HandleFunc("GET /debug/state", func(w http.ResponseWriter, r *http.Request) {
		after, _ := strconv.Atoi(r.URL.Query().Get("after"))
		f.mu.Lock()
		st := f.st
		f.mu.Unlock()
		if st.Seq <= after {
			time.Sleep(50 * time.Millisecond)
		}
		js(w, st)
	})
	mux.HandleFunc("POST /debug/do", func(w http.ResponseWriter, r *http.Request) {
		b := make([]byte, 64)
		n, _ := r.Body.Read(b)
		if f.hold != nil {
			<-f.hold
		}
		f.mu.Lock()
		f.cmds = append(f.cmds, string(b[:n]))
		f.st.Seq++
		st := f.st
		f.mu.Unlock()
		js(w, custos.Done{OK: true, State: st})
	})
	srv := httptest.NewServer(mux)
	t.Cleanup(srv.Close)
	return f, srv
}
