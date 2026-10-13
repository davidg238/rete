// Copyright (c) 2026 Ekorau LLC

package custos

// portaprofile.go — a porta node's console and profile (rete#415, design
// 2026-10-12-415). The console: custos binds a mesh sink on a port of its own,
// and writes goal/console = Console{addr: --advertise, port} through porta, so
// the node tees its console here, whichever custos asked last. The profile: the
// request is a goal/profile cell, Profile{id, ms}, through porta, and the rows
// come back by that tee, never on the space (RECORDS §4.5). The cell is one
// request, named by a new id: the node serves it once, and carries a window that
// deep sleep cuts to its next boot (design Q8/Q9). The POST answers at the write,
// Pending, because the window opens at the node's next check-in (design Q3, as
// load's Q2); /profile/last answers the pending request until the dump's end line
// arrives, then the cable's Capture.
//
//	GET  /profile               the page
//	GET  /profile/last          the last request: Pending, or its capture (404 before the first)
//	POST /profile/capture?ms=N  write goal/profile = Profile{id, N} through porta; answer {Pending, Seq} at once

import (
	"context"
	"encoding/json"
	"fmt"
	"net"
	"net/http"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/davidg238/rete/custos/internal/console"
	"github.com/davidg238/rete/custos/internal/imagemap"
	"github.com/davidg238/rete/custos/internal/load"
	"github.com/davidg238/rete/custos/internal/profile"
	"github.com/davidg238/rete/protocol/cval"
)

// portaConsole is a porta node's teed console: the sink it lands in, and the
// goal/console value that points the node at it.
type portaConsole struct {
	sink *console.Sink
	tee  cval.Record
}

// consoleRecord is goal/console's value, Console{addr, port} (MANIFEST.cddl).
func consoleRecord(addr string, port int) cval.Record {
	return cval.Record{Tag: "Console", Fields: cval.Map{
		{K: cval.Symbol("addr"), V: addr},
		{K: cval.Symbol("port"), V: int64(port)},
	}}
}

// mountPortaConsole binds the node's sink, serves its console verbs, and tells
// the node where to tee. A porta that cannot be reached at start-up is noted in
// the console's own log, not fatal: every profile request writes the cell again.
func mountPortaConsole(ctx context.Context, mux *http.ServeMux, prefixes []string, b Board, p load.Porta, root, advertise string) (*portaConsole, error) {
	if ip := net.ParseIP(advertise); ip == nil || ip.To4() != nil {
		return nil, fmt.Errorf("--advertise %q: the node tees over IPv6 only (log_udp.c), so give a literal IPv6 address", advertise)
	}
	lock, err := console.Lock(root, b.Name)
	if err != nil {
		return nil, err
	}
	s := &console.Sink{Store: &console.Store{Root: root, Board: b.Name, Budget: console.DefaultBudget},
		Addr: "[::]:0", Lock: lock}
	conn, err := s.Listen()
	if err != nil {
		lock.Close()
		return nil, err
	}
	go s.Run(ctx, conn)
	mountConsole(mux, prefixes, s.Store, s.Health, time.Now)
	pc := &portaConsole{sink: s, tee: consoleRecord(advertise, conn.LocalAddr().(*net.UDPAddr).Port)}
	if _, err := p.Write("goal/console", pc.tee, "custos console"); err != nil {
		s.Store.Note("goal/console not written at start-up: "+err.Error(), time.Now())
	}
	return pc, nil
}

// portaLine is a capture's request half on a porta node: the request is the
// node's goal/profile cell, written through porta, and the seq porta answers is
// handed to the POST waiting on sent. Close ends the capture's view of the sink.
type portaLine struct {
	p      load.Porta
	req    cval.Record
	cancel func()
	sent   chan<- portaSent
}

// profileRecord is goal/profile's value, Profile{id, ms} (MANIFEST.cddl; keys in
// wire order).
func profileRecord(id int64, ms int) cval.Record {
	return cval.Record{Tag: "Profile", Fields: cval.Map{
		{K: cval.Symbol("id"), V: id},
		{K: cval.Symbol("ms"), V: int64(ms)},
	}}
}

// requestIDs names each request with a new id. The node holds it as a 31-bit SMI
// and serves an id once, by equality, so an id must differ from the last one the
// node served even across a custos restart: the clock's seconds below 2^30, and
// one more than the last when two requests share a second.
type requestIDs struct {
	mu   sync.Mutex
	last int64
}

func (r *requestIDs) next(now time.Time) int64 {
	r.mu.Lock()
	defer r.mu.Unlock()
	id := now.Unix() % (1 << 30)
	if id <= r.last {
		id = r.last + 1
	}
	r.last = id % (1 << 30)
	return r.last
}

type portaSent struct {
	seq int64
	err error
}

// Send writes the cell; the line, `profile <ms>`, is the cable's spelling of it.
func (l portaLine) Send(line string) error {
	seq, err := l.p.Write("goal/profile", l.req, "custos profile")
	l.sent <- portaSent{seq, err}
	return err
}

func (l portaLine) Close() error { l.cancel(); return nil }

// mountPortaProfile serves profile on a porta node whose console is teed to pc.
// A newer request supersedes a pending one (a sleepy node's request can be
// spent on a wake that never dumped); the older capture still ends at the next
// end line, and its answer is dropped.
func mountPortaProfile(mux *http.ServeMux, prefixes []string, mapPath string, p load.Porta, pc *portaConsole, busy *sync.Mutex) {
	var (
		mu   sync.Mutex
		gen  int
		last *Capture
		ids  requestIDs
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
			http.Error(w, "custos: a push or a profile request to this node is being written", http.StatusConflict)
			return
		}
		defer busy.Unlock()
		m, err := imagemap.Load(mapPath)
		if err != nil {
			http.Error(w, err.Error(), http.StatusInternalServerError)
			return
		}
		// A profile ranked against another program's map names the wrong methods
		// and looks exactly like a real one: refuse it. A node that has published no
		// obs/apps/main cannot be checked (a bare host run), so it is ranked with a
		// warning, as the debugger arms a node that knows no hash (rete#375).
		running, err := p.Running()
		if err != nil {
			http.Error(w, err.Error(), http.StatusBadGateway)
			return
		}
		var warn string
		switch {
		case running == "":
			warn = "UNCHECKED: the node has published no obs/apps/main, so custos cannot tell that the map is its program."
		case !strings.EqualFold(running, m.SHA256):
			http.Error(w, fmt.Sprintf("custos: the node runs sha256 %.12s and the map describes %.12s: rebuild, load, then profile",
				running, m.SHA256), http.StatusConflict)
			return
		}
		if _, err := p.Write("goal/console", pc.tee, "custos profile"); err != nil {
			http.Error(w, err.Error(), http.StatusBadGateway)
			return
		}
		pending := &Capture{Ms: ms, Map: mapPath, Pending: true}
		mu.Lock()
		gen++
		mine := gen
		last = pending
		mu.Unlock()
		sent := make(chan portaSent, 1)
		open := func(onLine func(string), onEnd func()) (profile.Sender, error) {
			return portaLine{p: p, req: profileRecord(ids.next(time.Now()), ms), cancel: pc.sink.Subscribe(onLine), sent: sent}, nil
		}
		go func() {
			text, err := profile.Capture(open, ms, 0)
			c := &Capture{CapturedMs: time.Now().UnixMilli(), Ms: ms, Map: mapPath}
			if err != nil {
				c.Error = err.Error()
			} else {
				c.Report = profile.NewReport(profile.Parse(text), m)
				if warn != "" {
					c.Report.Warnings = append([]string{warn}, c.Report.Warnings...)
				}
			}
			mu.Lock()
			if gen == mine {
				c.Seq = pending.Seq
				last = c
			}
			mu.Unlock()
		}()
		s := <-sent
		if s.err != nil {
			http.Error(w, s.err.Error(), http.StatusBadGateway)
			return
		}
		mu.Lock()
		pending.Seq = s.seq
		c := *pending
		mu.Unlock()
		answer(w, &c)
	})
}
