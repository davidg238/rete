// Copyright (c) 2026 Ekorau LLC

package custos

// load.go — the load page (rete#367, peer design D14): an .oimg into a board's
// program region over one cable, or run once on a host VM, configured at
// start-up.
//
//	GET  /load              the page
//	GET  /load/last         the last load, as JSON (404 before the first)
//	POST /load?name=F.oimg  the image as the body; load it, keep it as last
//
// The answer is the proof: the program's sha256 as the target reported it at
// boot, and the firmware's build-id. One load at a time; a second answers 409.

import (
	_ "embed"
	"encoding/json"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"sync"
	"time"

	"github.com/davidg238/rete/custos/internal/load"
	"github.com/davidg238/rete/custos/internal/oimg"
)

//go:embed load.html
var loadPage []byte

// maxImage bounds an upload: well over any program region (the C6's is 512 KiB).
const maxImage = 16 << 20

// Load is one load, as the page receives it. Debug is what became of the debug
// page on the same cable: "" (none), "restarted", or why it is stale.
type Load struct {
	LoadedMs int64
	Result   load.Result
	Debug    string
}

// cable is what another verb must hold of the debug page on its board's cable:
// release closes the session's line and answers once its reader has ended;
// reopen restarts the session over the board's new program after a load (sha ""
// after a failed one); resume gives a released session its cable back,
// unchanged, after a capture.
type cable struct {
	release func() error
	reopen  func(sha string) string
	resume  func() error
}

func mountLoad(mux *http.ServeMux, prefixes []string, t load.Target, held *cable, busy *sync.Mutex) error {
	dir, err := os.MkdirTemp("", "custos-load-")
	if err != nil {
		return err
	}
	var (
		mu   sync.Mutex
		last *Load
	)
	answer := func(w http.ResponseWriter, l *Load) {
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
		answer(w, l)
	})
	handle(mux, prefixes, "POST /load", func(w http.ResponseWriter, r *http.Request) {
		body, err := io.ReadAll(io.LimitReader(r.Body, maxImage+1))
		if err != nil {
			http.Error(w, err.Error(), http.StatusBadRequest)
			return
		}
		if len(body) > maxImage {
			http.Error(w, "custos: an image is at most 16 MiB", http.StatusRequestEntityTooLarge)
			return
		}
		name := filepath.Base(r.URL.Query().Get("name"))
		if name == "." || name == "/" {
			name = "upload.oimg"
		}
		if !busy.TryLock() {
			http.Error(w, "custos: the cable is busy (a load or a debug command is running)", http.StatusConflict)
			return
		}
		defer busy.Unlock()
		path := filepath.Join(dir, name)
		if err := os.WriteFile(path, body, 0o644); err != nil {
			http.Error(w, err.Error(), http.StatusInternalServerError)
			return
		}
		if _, err := oimg.Read(path); err != nil {
			http.Error(w, err.Error(), http.StatusBadRequest)
			return
		}
		if held != nil {
			if err := held.release(); err != nil {
				http.Error(w, "custos: releasing the debug page's cable: "+err.Error(), http.StatusInternalServerError)
				return
			}
		}
		res, err := t.Load(path)
		debug := ""
		if held != nil {
			debug = held.reopen(res.SHA256)
		}
		if err != nil {
			http.Error(w, err.Error(), http.StatusBadGateway)
			return
		}
		res.Image = name
		l := &Load{time.Now().UnixMilli(), res, debug}
		mu.Lock()
		last = l
		mu.Unlock()
		answer(w, l)
	})
	return nil
}
