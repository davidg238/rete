// Copyright (c) 2026 Ekorau LLC

package custos

// console.go — a collected board's console (peer design D16), as JSON:
//
//	GET  /log?since=<spec>    the lines since -<N>s, -<N>m or a time
//	GET  /log?tail=<N>        the last N lines (no parameter: 20)
//	GET  /runs                one row per boot: index, start, build id, line count
//	GET  /panics              one row per panic: its run, start, build id, coredump lines
//	GET  /health              the cable from memory: up, down (and why) or lent
//	POST /lend?s=<N>          close the cable for a flash; back after N s or on /return
//	POST /return              take a lent cable back now

import (
	"encoding/json"
	"errors"
	"net/http"
	"strconv"
	"sync"
	"time"

	"github.com/davidg238/rete/custos/internal/console"
)

// RunRow is one row of GET /runs.
type RunRow struct {
	Index   int
	Start   time.Time
	BuildID string
	Lines   int
}

// PanicRow is one row of GET /panics; Index numbers them from 1, oldest first.
type PanicRow struct {
	Index    int
	Run      int
	Start    time.Time
	BuildID  string
	Coredump int
}

func answerJSON(w http.ResponseWriter, code int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(code)
	json.NewEncoder(w).Encode(v)
}

func answerError(w http.ResponseWriter, code int, err error) {
	answerJSON(w, code, map[string]string{"Error": err.Error()})
}

// panics answers every panic in the store, numbered.
func panics(s *console.Store) ([]PanicRow, error) {
	lines, err := s.Read(time.Time{})
	if err != nil {
		return nil, err
	}
	var out []PanicRow
	for _, r := range console.Runs(lines) {
		for _, p := range console.Panics(r) {
			out = append(out, PanicRow{len(out) + 1, r.Index, p.Start, r.BuildID, len(p.Coredump)})
		}
	}
	return out, nil
}

func mountConsole(mux *http.ServeMux, prefixes []string, c *console.Cable, now func() time.Time) {
	s := c.Store
	handle(mux, prefixes, "GET /log", func(w http.ResponseWriter, r *http.Request) {
		q := r.URL.Query()
		var since time.Time
		tail := 20
		if spec := q.Get("since"); spec != "" {
			t, err := console.ResolveSince(spec, now())
			if err != nil {
				answerError(w, http.StatusBadRequest, err)
				return
			}
			since, tail = t, 0
		} else if n := q.Get("tail"); n != "" {
			v, err := strconv.Atoi(n)
			if err != nil || v < 1 {
				answerError(w, http.StatusBadRequest, errors.New("tail takes a count of lines, 1 or more"))
				return
			}
			tail = v
		}
		lines, err := s.Read(since)
		if err != nil {
			answerError(w, http.StatusInternalServerError, err)
			return
		}
		if tail > 0 && len(lines) > tail {
			lines = lines[len(lines)-tail:]
		}
		if lines == nil {
			lines = []console.Entry{}
		}
		answerJSON(w, http.StatusOK, lines)
	})
	handle(mux, prefixes, "GET /runs", func(w http.ResponseWriter, r *http.Request) {
		lines, err := s.Read(time.Time{})
		if err != nil {
			answerError(w, http.StatusInternalServerError, err)
			return
		}
		out := []RunRow{}
		for _, run := range console.Runs(lines) {
			out = append(out, RunRow{run.Index, run.Start, run.BuildID, len(run.Lines)})
		}
		answerJSON(w, http.StatusOK, out)
	})
	handle(mux, prefixes, "GET /panics", func(w http.ResponseWriter, r *http.Request) {
		out, err := panics(s)
		if err != nil {
			answerError(w, http.StatusInternalServerError, err)
			return
		}
		if out == nil {
			out = []PanicRow{}
		}
		answerJSON(w, http.StatusOK, out)
	})
	handle(mux, prefixes, "GET /health", func(w http.ResponseWriter, r *http.Request) {
		answerJSON(w, http.StatusOK, c.Health())
	})

	var (
		mu  sync.Mutex
		ret chan struct{} // non-nil while a lend from /lend holds the cable
	)
	handle(mux, prefixes, "POST /lend", func(w http.ResponseWriter, r *http.Request) {
		n, err := strconv.Atoi(r.URL.Query().Get("s"))
		if err != nil || n < 1 {
			answerError(w, http.StatusBadRequest, errors.New("lend takes s=<seconds>, 1 or more: how long until custos takes the cable back"))
			return
		}
		back := make(chan struct{})
		closed := make(chan struct{})
		result := make(chan error, 1)
		go func() {
			result <- c.Lend(func() error {
				mu.Lock()
				ret = back
				mu.Unlock()
				close(closed)
				select {
				case <-back:
				case <-time.After(time.Duration(n) * time.Second):
				}
				mu.Lock()
				ret = nil
				mu.Unlock()
				return nil
			})
		}()
		select {
		case <-closed:
			answerJSON(w, http.StatusOK, c.Health())
		case err := <-result:
			answerError(w, http.StatusConflict, err)
		}
	})
	handle(mux, prefixes, "POST /return", func(w http.ResponseWriter, r *http.Request) {
		mu.Lock()
		b := ret
		ret = nil
		mu.Unlock()
		if b == nil {
			answerError(w, http.StatusConflict, errors.New("the cable is not lent by /lend"))
			return
		}
		close(b)
		for deadline := time.Now().Add(2 * time.Second); c.Health().State == "lent" && time.Now().Before(deadline); time.Sleep(10 * time.Millisecond) {
		}
		answerJSON(w, http.StatusOK, c.Health())
	})
}
