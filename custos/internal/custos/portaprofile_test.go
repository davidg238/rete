// Copyright (c) 2026 Ekorau LLC

package custos

import (
	"context"
	"encoding/json"
	"fmt"
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/davidg238/rete/custos/internal/imagemap"
	"github.com/davidg238/rete/protocol/cval"
)

// portaConsoleBoard is a custos serving one porta node with a console, against f.
func portaConsoleBoard(t *testing.T, f *fakePorta, b Board) *httptest.Server {
	t.Helper()
	ps := f.server(t)
	mux := http.NewServeMux()
	ctx, cancel := context.WithCancel(context.Background())
	t.Cleanup(cancel)
	if err := mountBoards(ctx, mux, []Board{b}, "", t.TempDir(), ps.URL, "::1"); err != nil {
		t.Fatal(err)
	}
	srv := httptest.NewServer(mux)
	t.Cleanup(srv.Close)
	return srv
}

// teeTarget decodes the goal/console custos wrote for node.
func (f *fakePorta) teeTarget(t *testing.T, node string) (string, int64) {
	t.Helper()
	f.mu.Lock()
	b := f.cells[node+"/goal/console"]
	f.mu.Unlock()
	v, err := cval.Decode(b)
	if err != nil {
		t.Fatalf("goal/console: %v", err)
	}
	r, ok := v.(cval.Record)
	if !ok || r.Tag != "Console" || len(r.Fields) != 2 {
		t.Fatalf("goal/console is %s, want Console{addr, port}", cval.Format(v))
	}
	return r.Fields[0].V.(string), r.Fields[1].V.(int64)
}

// rete#415: the whole path with porta and the node faked. Start-up tells the
// node where to tee; a request is a dbg/req line through porta, answered
// Pending; the dump, arriving by UDP at the address the cell named, completes
// /profile/last in the cable's shape.
func TestPortaProfileThroughTheTee(t *testing.T) {
	m, err := imagemap.Load(fixtureMap(t))
	if err != nil {
		t.Fatal(err)
	}
	f := &fakePorta{}
	srv := portaConsoleBoard(t, f, Board{Name: "bell", Node: "n1", Map: fixtureMap(t)})
	f.runs(t, "n1", m.SHA256)

	var infos []BoardInfo
	getJSON(t, srv.URL+"/boards", &infos)
	if got := strings.Join(infos[0].Verbs, " "); got != "log runs panics health load profile" {
		t.Fatalf("verbs %q", got)
	}
	addr, port := f.teeTarget(t, "n1")
	if addr != "::1" || port <= 0 {
		t.Fatalf("goal/console names [%s]:%d", addr, port)
	}

	r, err := http.Post(srv.URL+"/board/bell/profile/capture?ms=500", "", nil)
	if err != nil {
		t.Fatal(err)
	}
	var c Capture
	err = json.NewDecoder(r.Body).Decode(&c)
	r.Body.Close()
	if err != nil || r.StatusCode != 200 || !c.Pending || c.Seq == 0 {
		t.Fatalf("POST: %d %+v %v", r.StatusCode, c, err)
	}
	f.mu.Lock()
	req, _ := cval.Decode(f.cells["n1/goal/profile"])
	f.mu.Unlock()
	if r, ok := req.(cval.Record); !ok || r.Tag != "Profile" || len(r.Fields) != 2 ||
		r.Fields[0].V.(int64) <= 0 || r.Fields[1].V != int64(500) {
		t.Fatalf("goal/profile is %v, want Profile{id, ms: 500}", req)
	}
	getJSON(t, srv.URL+"/board/bell/profile/last", &c)
	if !c.Pending {
		t.Fatalf("/profile/last before the dump: %+v", c)
	}

	tx, err := net.Dial("udp", fmt.Sprintf("[::1]:%d", port))
	if err != nil {
		t.Fatal(err)
	}
	defer tx.Close()
	off := m.Offsets()[0]
	for _, l := range []string{
		"tick 41", // traffic before the dump is not part of it
		"mprof[window]: methods=1 activations=7 overflow=0 cap=256",
		fmt.Sprintf("mprof: %d 7", off),
		"mprof[window]: end",
	} {
		tx.Write([]byte(l + "\n"))
	}
	deadline := time.Now().Add(5 * time.Second)
	for c.Pending && time.Now().Before(deadline) {
		time.Sleep(20 * time.Millisecond)
		c = Capture{} // omitempty: a false Pending is absent, and must not keep the last true
		getJSON(t, srv.URL+"/board/bell/profile/last", &c)
	}
	if c.Pending || c.Error != "" || len(c.Report.Rows) != 1 || c.Report.Rows[0].Count != 7 || c.Report.Rows[0].Unknown {
		t.Fatalf("/profile/last after the dump: %+v", c)
	}
	if len(c.Report.Warnings) != 0 {
		t.Fatalf("warnings on a whole dump: %v", c.Report.Warnings)
	}
}

// A node running another program is refused before anything is written: its
// rows would be ranked against the wrong map and look like a real profile.
func TestPortaProfileRefusesAnotherProgramsMap(t *testing.T) {
	f := &fakePorta{}
	srv := portaConsoleBoard(t, f, Board{Name: "bell", Node: "n1", Map: fixtureMap(t)})
	f.runs(t, "n1", strings.Repeat("ab", 32))
	r, err := http.Post(srv.URL+"/board/bell/profile/capture?ms=500", "", nil)
	if err != nil {
		t.Fatal(err)
	}
	r.Body.Close()
	f.mu.Lock()
	_, wrote := f.cells["n1/goal/profile"]
	f.mu.Unlock()
	if r.StatusCode != http.StatusConflict || wrote {
		t.Fatalf("status %d, goal/profile written %v", r.StatusCode, wrote)
	}
}

// Without --advertise a porta node serves load only, and a map on it is refused
// at start-up rather than served as a profile that can never come back.
func TestPortaProfileNeedsAdvertise(t *testing.T) {
	f := &fakePorta{}
	ps := f.server(t)
	err := mountBoards(context.Background(), http.NewServeMux(), []Board{{Name: "bell", Node: "n1", Map: fixtureMap(t)}},
		"", t.TempDir(), ps.URL, "")
	if err == nil || !strings.Contains(err.Error(), "--advertise") {
		t.Fatalf("err %v", err)
	}
	err = mountBoards(context.Background(), http.NewServeMux(), []Board{{Name: "bell", Node: "n1"}},
		"", t.TempDir(), ps.URL, "127.0.0.1")
	if err == nil || !strings.Contains(err.Error(), "IPv6") {
		t.Fatalf("a v4 --advertise: err %v", err)
	}
}

// Each request is a new id, even two in one second and across a restart's clock:
// the node serves an id once, by equality, so a repeat would be ignored silently.
func TestPortaProfileIDsAreNew(t *testing.T) {
	var r requestIDs
	now := time.Unix(1_790_000_000, 0)
	a, b := r.next(now), r.next(now)
	if a == b || a <= 0 || b >= 1<<30 {
		t.Fatalf("ids %d, %d", a, b)
	}
	var fresh requestIDs // a restarted custos, a second later
	if c := fresh.next(now.Add(time.Second)); c == a {
		t.Fatalf("a restart reused %d", c)
	}
}
