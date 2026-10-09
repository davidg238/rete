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

func loadServer(t *testing.T, tg load.Target, held *around, busy *sync.Mutex) *httptest.Server {
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

func TestRestartStartsTheSessionOverWithSeqRising(t *testing.T) {
	sm, err := debug.LoadSourceMap(fixtureMap(t), "")
	if err != nil {
		t.Fatal(err)
	}
	s := debug.NewSession(sm, io.Discard)
	s.Attach(&stubVM{s, nil})
	s.OnLine("dbg:ready")
	before := s.State(-1, 0)
	if before.Run != "ready" {
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

func TestLoadRefusesANonImageBeforeTheTarget(t *testing.T) {
	tg := &fakeTarget{}
	lent := false
	held := &around{lend: func(fn func() error) error { lent = true; return fn() }, follow: func(string) string { return "" }}
	srv := loadServer(t, tg, held, &sync.Mutex{})
	code, body := post(t, srv.URL+"/load", []byte("not an image at all, long enough"))
	if code != 400 || !strings.Contains(body, "not an .oimg") || tg.got != nil || lent {
		t.Fatalf("%d %q got=%v lent=%v", code, body, tg.got != nil, lent)
	}
}

// D16: a load runs with the board's cable lent by its collector, and the debug
// page then follows the program the board reported.
func TestLoadLendsTheCableThenTheDebugPageFollows(t *testing.T) {
	var order []string
	tg := &fakeTarget{}
	held := &around{
		lend: func(fn func() error) error {
			order = append(order, "lend")
			err := fn()
			order = append(order, "back")
			return err
		},
		follow: func(sha string) string { order = append(order, "follow "+sha[:8]); return "restarted" },
	}
	srv := loadServer(t, tg, held, &sync.Mutex{})
	code, body := post(t, srv.URL+"/load", fixtureImage(t))
	if code != 200 || !strings.Contains(body, `"Debug":"restarted"`) || tg.got == nil {
		t.Fatalf("%d %s", code, body)
	}
	if len(order) != 3 || order[0] != "lend" || order[1] != "back" || !strings.HasPrefix(order[2], "follow ") {
		t.Fatalf("order %v", order)
	}
}

func TestLoadFollowsAfterAFailedLoadWithNoSha(t *testing.T) {
	followed := "-"
	held := &around{follow: func(sha string) string { followed = sha; return "restarted" }}
	srv := loadServer(t, &fakeTarget{err: errors.New("esptool: exit status 2")}, held, &sync.Mutex{})
	if code, _ := post(t, srv.URL+"/load", fixtureImage(t)); code != 502 || followed != "" {
		t.Fatalf("code %d followed %q: the debug page must follow, with no sha", code, followed)
	}
}

func TestFollowRestartsGoesStaleAndComesBack(t *testing.T) {
	sm, err := debug.LoadSourceMap(fixtureMap(t), "")
	if err != nil {
		t.Fatal(err)
	}
	s := debug.NewSession(sm, io.Discard)
	line := &stubVM{s, nil}
	s.Attach(line)
	f := follow(s, line, func() (*debug.SourceMap, error) { return sm, nil })
	if got := f(sm.SHA256()); got != "restarted" || s.Gone() {
		t.Fatalf("fits: %q gone %v", got, s.Gone())
	}
	if got := f(strings.Repeat("f", 64)); !strings.Contains(got, "rebuild") || s.State(-1, 0).Run != "stale" {
		t.Fatalf("stale: %q run %s", got, s.State(-1, 0).Run)
	}
	if res := s.Do("continue"); res.Err == "" {
		t.Fatal("a stale session must refuse commands")
	}
	if got := f(sm.SHA256()); got != "restarted" || s.Gone() {
		t.Fatalf("back: %q gone %v", got, s.Gone())
	}
	if res := s.Do("pause"); res.Err != "" {
		t.Fatalf("the restarted session cannot reach the VM: %s", res.Err)
	}
}
