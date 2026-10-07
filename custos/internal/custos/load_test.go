// Copyright (c) 2026 Ekorau LLC

package custos

import (
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"strings"
	"sync"
	"testing"

	"github.com/davidg238/rete/custos/internal/debug"
	"github.com/davidg238/rete/custos/internal/load"
	"github.com/davidg238/rete/custos/internal/oimg"
)

// fakeTarget loads whatever it is given, or fails with err; hold blocks it.
type fakeTarget struct {
	err  error
	hold chan struct{}
	got  []byte
}

func (f *fakeTarget) Load(path string) (load.Result, error) {
	if f.hold != nil {
		<-f.hold
	}
	f.got, _ = os.ReadFile(path)
	if f.err != nil {
		return load.Result{}, f.err
	}
	img, err := oimg.Read(path)
	if err != nil {
		return load.Result{}, err
	}
	return load.Result{Image: path, Size: img.Size, SHA256: img.SHA256, BuildID: "B1", Offset: 0x320000}, nil
}

func fixtureImage(t *testing.T) []byte {
	t.Helper()
	b, err := os.ReadFile(strings.TrimSuffix(fixtureMap(t), ".map.json"))
	if err != nil {
		t.Fatal(err)
	}
	return b
}

func loadServer(t *testing.T, tg load.Target, held *cable, busy *sync.Mutex) *httptest.Server {
	t.Helper()
	mux := http.NewServeMux()
	if err := mountLoad(mux, []string{""}, tg, held, busy); err != nil {
		t.Fatal(err)
	}
	srv := httptest.NewServer(mux)
	t.Cleanup(srv.Close)
	return srv
}

func post(t *testing.T, url string, body []byte) (int, string) {
	t.Helper()
	resp, err := http.Post(url, "application/octet-stream", strings.NewReader(string(body)))
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	b, _ := io.ReadAll(resp.Body)
	return resp.StatusCode, string(b)
}

func TestLoadAnswersTheProofAndKeepsItAsLast(t *testing.T) {
	tg := &fakeTarget{}
	srv := loadServer(t, tg, nil, &sync.Mutex{})
	if r, _ := http.Get(srv.URL + "/load/last"); r.StatusCode != 404 {
		t.Fatalf("last before a load: %d", r.StatusCode)
	}
	img := fixtureImage(t)
	code, body := post(t, srv.URL+"/load?name=dir/Fixture.oimg", img)
	if code != 200 {
		t.Fatalf("%d %s", code, body)
	}
	var l Load
	if err := json.Unmarshal([]byte(body), &l); err != nil {
		t.Fatal(err)
	}
	// The name is the upload's base name, never custos's own temp path.
	if l.Result.Image != "Fixture.oimg" || l.Result.BuildID != "B1" || len(l.Result.SHA256) != 64 || l.Debug != "" {
		t.Fatalf("%+v", l)
	}
	if string(tg.got) != string(img) {
		t.Fatal("the target was not given the uploaded bytes")
	}
	r, _ := http.Get(srv.URL + "/load/last")
	var last Load
	json.NewDecoder(r.Body).Decode(&last)
	if last.Result.SHA256 != l.Result.SHA256 {
		t.Fatalf("last %+v", last)
	}
}

func TestLoadRefusesANonImageBeforeTheTarget(t *testing.T) {
	tg := &fakeTarget{}
	released := false
	held := &cable{release: func() error { released = true; return nil }, reopen: func(string) string { return "" }}
	srv := loadServer(t, tg, held, &sync.Mutex{})
	code, body := post(t, srv.URL+"/load", []byte("not an image at all, long enough"))
	if code != 400 || !strings.Contains(body, "not an .oimg") || tg.got != nil || released {
		t.Fatalf("%d %q got=%v released=%v", code, body, tg.got != nil, released)
	}
}

func TestLoadAnswers502WithTheTargetsReason(t *testing.T) {
	srv := loadServer(t, &fakeTarget{err: errors.New("the target refused the program: oimg_load_blob: bad magic")}, nil, &sync.Mutex{})
	code, body := post(t, srv.URL+"/load", fixtureImage(t))
	if code != 502 || !strings.Contains(body, "bad magic") {
		t.Fatalf("%d %q", code, body)
	}
	if r, _ := http.Get(srv.URL + "/load/last"); r.StatusCode != 404 {
		t.Fatal("a failed load must not become last")
	}
}

func TestLoadWhileTheCableIsBusyAnswers409(t *testing.T) {
	var busy sync.Mutex
	busy.Lock() // a debug command, or a capture, holds the cable
	defer busy.Unlock()
	srv := loadServer(t, &fakeTarget{}, nil, &busy)
	if code, body := post(t, srv.URL+"/load", fixtureImage(t)); code != 409 {
		t.Fatalf("%d %q", code, body)
	}
}

func TestLoadReleasesAndReopensTheDebugCable(t *testing.T) {
	var order []string
	held := &cable{
		release: func() error { order = append(order, "release"); return nil },
		reopen:  func(sha string) string { order = append(order, "reopen "+sha[:8]); return "restarted" },
	}
	srv := loadServer(t, &fakeTarget{}, held, &sync.Mutex{})
	code, body := post(t, srv.URL+"/load", fixtureImage(t))
	if code != 200 || !strings.Contains(body, `"Debug":"restarted"`) {
		t.Fatalf("%d %s", code, body)
	}
	if len(order) != 2 || order[0] != "release" || !strings.HasPrefix(order[1], "reopen ") {
		t.Fatalf("order %v", order)
	}
}

func TestLoadReopensTheDebugCableAfterAFailedLoad(t *testing.T) {
	reopened := "-"
	held := &cable{release: func() error { return nil }, reopen: func(sha string) string { reopened = sha; return "restarted" }}
	srv := loadServer(t, &fakeTarget{err: errors.New("esptool: exit status 2")}, held, &sync.Mutex{})
	if code, _ := post(t, srv.URL+"/load", fixtureImage(t)); code != 502 || reopened != "" {
		t.Fatalf("code %d reopened %q: the cable must be reopened, with no sha", code, reopened)
	}
}

func TestRestartStartsTheSessionOverWithSeqRising(t *testing.T) {
	sm, err := debug.LoadSourceMap(fixtureMap(t), "")
	if err != nil {
		t.Fatal(err)
	}
	s := debug.NewSession(sm, io.Discard)
	s.Attach(&stubVM{s, nil})
	s.OnLine("dbg:ready")
	before := s.State(-1, 0)
	if before.Run != "boot" {
		t.Fatalf("%+v", before)
	}
	s.Restart(sm)
	after := s.State(-1, 0)
	if after.Run != "unknown" || after.Seq <= before.Seq {
		t.Fatalf("before %d after %+v", before.Seq, after)
	}
	s.Stale("the board now runs another program")
	if st := s.State(-1, 0); st.Run != "stale" || !s.Gone() {
		t.Fatalf("%+v", st)
	}
	if res := s.Do("continue"); res.Err == "" {
		t.Fatal("a stale session must refuse commands")
	}
}

// closer is a cable line that counts its closes.
type closer struct {
	*stubVM
	closes *int
}

func (c closer) Close() error { *c.closes++; return nil }

func TestDebugCableRestartsGoesStaleAndComesBack(t *testing.T) {
	sm, err := debug.LoadSourceMap(fixtureMap(t), "")
	if err != nil {
		t.Fatal(err)
	}
	s := debug.NewSession(sm, io.Discard)
	closes, opens := 0, 0
	open := func() (debug.Line, error) { opens++; return closer{&stubVM{s, nil}, &closes}, nil }
	line, _ := open()
	s.Attach(line)
	c := newDebugCable(s, line, open, func() (*debug.SourceMap, error) { return sm, nil })

	c.release()
	if got := c.reopen(sm.SHA256()); got != "restarted" || closes != 1 || opens != 2 || s.Gone() {
		t.Fatalf("fits: %q closes %d opens %d gone %v", got, closes, opens, s.Gone())
	}
	c.release()
	if got := c.reopen(strings.Repeat("f", 64)); !strings.Contains(got, "rebuild") || opens != 2 || s.State(-1, 0).Run != "stale" {
		t.Fatalf("stale: %q opens %d run %s", got, opens, s.State(-1, 0).Run)
	}
	c.release() // the stale session left the cable closed: nothing to close twice
	if closes != 2 {
		t.Fatalf("closes %d: a closed cable was closed again", closes)
	}
	if got := c.reopen(sm.SHA256()); got != "restarted" || opens != 3 || s.Gone() {
		t.Fatalf("back: %q opens %d gone %v", got, opens, s.Gone())
	}
}

// A capture's resume gives a released cable back without restarting the
// session; when the cable is still held, it opens nothing.
func TestDebugCableResumeReattachesWithoutARestart(t *testing.T) {
	sm, err := debug.LoadSourceMap(fixtureMap(t), "")
	if err != nil {
		t.Fatal(err)
	}
	s := debug.NewSession(sm, io.Discard)
	closes, opens := 0, 0
	open := func() (debug.Line, error) { opens++; return closer{&stubVM{s, nil}, &closes}, nil }
	line, _ := open()
	s.Attach(line)
	s.OnLine("dbg:ready")
	c := newDebugCable(s, line, open, func() (*debug.SourceMap, error) { return sm, nil })
	if c.resume(); opens != 1 {
		t.Fatalf("a held cable was opened again: opens %d", opens)
	}
	c.release()
	if err := c.resume(); err != nil || opens != 2 || closes != 1 {
		t.Fatalf("resume: %v opens %d closes %d", err, opens, closes)
	}
	if st := s.State(-1, 0); st.Run != "boot" {
		t.Fatalf("resume restarted the session: %+v", st)
	}
}
