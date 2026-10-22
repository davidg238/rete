// Copyright (c) 2026 Ekorau LLC

package custos

// portaload.go — load on a porta node (rete#398): the same three routes as a
// cable's load, but the image goes to porta, which signs it and holds it as the
// node's goal until the node next asks. The POST answers at the goal write
// (design 2026-10-12-398 Q2) in the cable's Load shape, Push set; /load/last
// re-reads what the node runs on every call, so an agent polls it until
// Push.Converged.
//
//	GET  /load              the page
//	GET  /load/last         the last push, the node re-read (404 before the first)
//	POST /load?name=F.oimg  the image as the body; push it

import (
	"encoding/json"
	"net/http"
	"os"
	"sync"
	"time"

	"github.com/davidg238/rete/custos/internal/load"
)

// follow is the debug page's (portadebug.go), told the pushed image's sha; nil
// when the board serves no debug page.
func mountPortaLoad(mux *http.ServeMux, prefixes []string, p load.Porta, busy *sync.Mutex, follow func(sha string) string) error {
	dir, err := os.MkdirTemp("", "custos-push-")
	if err != nil {
		return err
	}
	var (
		mu   sync.Mutex
		last *Load
	)
	answer := func(w http.ResponseWriter, l Load) {
		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("Cache-Control", "no-store")
		json.NewEncoder(w).Encode(l)
	}
	handle(mux, prefixes, "GET /load", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		w.Write(loadPage)
	})
	handle(mux, prefixes, "GET /load/last", func(w http.ResponseWriter, r *http.Request) {
		mu.Lock()
		l := last
		mu.Unlock()
		if l == nil {
			http.Error(w, "custos: no load yet", http.StatusNotFound)
			return
		}
		push, err := p.Refresh(*l.Push)
		if err != nil {
			http.Error(w, err.Error(), http.StatusBadGateway)
			return
		}
		again := *l
		again.Push = &push
		answer(w, again)
	})
	handle(mux, prefixes, "POST /load", func(w http.ResponseWriter, r *http.Request) {
		if !busy.TryLock() {
			http.Error(w, "custos: a push to this node is running", http.StatusConflict)
			return
		}
		defer busy.Unlock()
		path, _, ok := upload(w, r, dir)
		if !ok {
			return
		}
		push, err := p.Push(path)
		if err != nil {
			http.Error(w, err.Error(), http.StatusBadGateway)
			return
		}
		l := &Load{LoadedMs: time.Now().UnixMilli(), Result: load.Result{Image: push.Image, Size: push.Size, SHA256: push.SHA256}, Push: &push}
		if follow != nil {
			l.Debug = follow(push.SHA256)
		}
		mu.Lock()
		last = l
		mu.Unlock()
		answer(w, *l)
	})
	return nil
}
