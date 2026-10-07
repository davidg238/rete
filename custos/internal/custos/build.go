// Copyright (c) 2026 Ekorau LLC

package custos

// The build and run verbs (peer design D18, rete#314):
//
//	GET  /build                          the page
//	POST /build?for=<board>&name=F.nsl   the .nsl as the body: compiled with the
//	                                     board's layout; answers Built
//	GET  /build/last.oimg                the last build's image, for POST /load
//	GET  /build/last.oimg.map.json       its map
//	POST /run?for=<host board>&name=F.nsl  the .nsl as the body: built for the
//	                                     host VM and run once; answers Ran
//
// Build and load stay two verbs: the agent POSTs the image to /load (D14). Run is
// host-only: on a board, load already boots the program. A compile error answers
// 422 with the compiler's words; a run that outlives RunTimeout is stopped and
// says so.

import (
	"bytes"
	"context"
	"crypto/sha256"
	_ "embed"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"time"

	"github.com/davidg238/rete/custos/internal/load"
	"github.com/davidg238/rete/custos/internal/nsl"
)

//go:embed build.html
var buildPage []byte

// RunTimeout bounds a run (docs/development.md §4: a bounded run time).
const RunTimeout = 30 * time.Second

// Built is /build's answer.
type Built struct {
	Board  string
	Image  string // the image's name: F.oimg for F.nsl
	SHA256 string
	Size   int64
}

// Ran is /run's answer: what the program printed (stdout and stderr, in order)
// and how it ended.
type Ran struct {
	Board    string
	Image    string
	SHA256   string
	Output   []string
	Exit     int  // the VM's exit status; -1 when it was stopped
	TimedOut bool // stopped at RunTimeout
}

// Failed is a verb's refusal as JSON: the compiler's own words when it refused.
type Failed struct {
	Error string
}

func mountBuild(mux *http.ServeMux, boards []Board) {
	byName := map[string]Board{}
	for _, b := range boards {
		byName[b.Name] = b
	}
	var mu sync.Mutex // one build at a time; the last build is shared
	dir, _ := os.MkdirTemp("", "custos-build-")
	var last string // the last build's .oimg path, "" before one

	answer := func(w http.ResponseWriter, code int, v any) {
		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("Cache-Control", "no-store")
		w.WriteHeader(code)
		json.NewEncoder(w).Encode(v)
	}
	// board picks the target: ?for=, or the only board that can take the verb.
	board := func(r *http.Request, hostOnly bool) (Board, error) {
		if n := r.URL.Query().Get("for"); n != "" {
			b, ok := byName[n]
			if !ok {
				return Board{}, fmt.Errorf("no board %q (GET /boards)", n)
			}
			if hostOnly && b.Host == "" {
				return Board{}, fmt.Errorf("%s is not a --host board: run is host-only (on a board, load boots the program)", n)
			}
			return b, nil
		}
		var fit []Board
		for _, b := range boards {
			if (b.Host != "" || !hostOnly) && b.UDP == 0 {
				fit = append(fit, b)
			}
		}
		if len(fit) != 1 {
			return Board{}, errors.New("name the board, ?for=<name> (GET /boards)")
		}
		return fit[0], nil
	}
	// compile writes the body as name in a fresh directory, so the map's files[0]
	// is name, and compiles it for b; answers the .oimg path.
	compile := func(r *http.Request, b Board) (string, int, error) {
		name := filepath.Base(r.URL.Query().Get("name"))
		if !strings.HasSuffix(name, ".nsl") || name == ".nsl" {
			return "", http.StatusBadRequest, errors.New("name the source, ?name=F.nsl")
		}
		src, err := io.ReadAll(io.LimitReader(r.Body, 1<<20))
		if err != nil {
			return "", http.StatusBadRequest, err
		}
		d, err := os.MkdirTemp(dir, "b-")
		if err != nil {
			return "", http.StatusInternalServerError, err
		}
		path := filepath.Join(d, name)
		if err := os.WriteFile(path, src, 0o644); err != nil {
			return "", http.StatusInternalServerError, err
		}
		layout := nsl.BoardLayout(load.ESP32C6.Chip)
		if b.Host != "" {
			layout = nsl.HostLayout(strings.Fields(b.Host)[0])
		}
		out := strings.TrimSuffix(path, ".nsl") + ".oimg"
		if err := nsl.Compile(path, out, layout); err != nil {
			var ref nsl.Refused
			if errors.As(err, &ref) {
				return "", http.StatusUnprocessableEntity, err
			}
			return "", http.StatusInternalServerError, err
		}
		return out, 0, nil
	}
	sum := func(path string) (string, int64) {
		b, _ := os.ReadFile(path)
		h := sha256.Sum256(b)
		return hex.EncodeToString(h[:]), int64(len(b))
	}

	mux.HandleFunc("GET /build", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		w.Write(buildPage)
	})
	mux.HandleFunc("POST /build", func(w http.ResponseWriter, r *http.Request) {
		b, err := board(r, false)
		if err != nil {
			answer(w, http.StatusBadRequest, Failed{err.Error()})
			return
		}
		mu.Lock()
		defer mu.Unlock()
		out, code, err := compile(r, b)
		if err != nil {
			answer(w, code, Failed{err.Error()})
			return
		}
		last = out
		sha, size := sum(out)
		answer(w, http.StatusOK, Built{b.Name, filepath.Base(out), sha, size})
	})
	for _, suffix := range []string{"", ".map.json"} {
		suffix := suffix
		mux.HandleFunc("GET /build/last.oimg"+suffix, func(w http.ResponseWriter, r *http.Request) {
			mu.Lock()
			path := last
			mu.Unlock()
			if path == "" {
				http.Error(w, "custos: nothing built yet", http.StatusNotFound)
				return
			}
			w.Header().Set("Content-Disposition", fmt.Sprintf("attachment; filename=%q", filepath.Base(path)+suffix))
			http.ServeFile(w, r, path+suffix)
		})
	}
	mux.HandleFunc("POST /run", func(w http.ResponseWriter, r *http.Request) {
		b, err := board(r, true)
		if err != nil {
			answer(w, http.StatusBadRequest, Failed{err.Error()})
			return
		}
		mu.Lock()
		defer mu.Unlock()
		out, code, err := compile(r, b)
		if err != nil {
			answer(w, code, Failed{err.Error()})
			return
		}
		sha, _ := sum(out)
		ran := Ran{Board: b.Name, Image: filepath.Base(out), SHA256: sha}
		argv := append(strings.Fields(b.Host), out)
		ctx, cancel := context.WithTimeout(r.Context(), RunTimeout)
		defer cancel()
		cmd := exec.CommandContext(ctx, argv[0], argv[1:]...)
		var buf bytes.Buffer
		cmd.Stdout, cmd.Stderr = &buf, &buf
		err = cmd.Run()
		ran.Output = strings.Split(strings.TrimRight(buf.String(), "\n"), "\n")
		var ee *exec.ExitError
		switch {
		case ctx.Err() == context.DeadlineExceeded:
			ran.Exit, ran.TimedOut = -1, true
		case errors.As(err, &ee):
			ran.Exit = ee.ExitCode()
		case err != nil:
			answer(w, http.StatusInternalServerError, Failed{err.Error()})
			return
		}
		answer(w, http.StatusOK, ran)
	})
}
