// Copyright (c) 2026 Ekorau LLC

package custos

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"testing"

	"github.com/davidg238/rete/custos/internal/profile"
)

func fixtureMap(t *testing.T) string {
	t.Helper()
	_, here, _, _ := runtime.Caller(0)
	return filepath.Join(filepath.Dir(here), "..", "..", "..", "protocol", "fixtures", "image", "Fixture.oimg.map.json")
}

// echoNode answers `profile <ms>` with one window over the fixture's main:args:.
type echoNode struct{ onLine func(string) }

func (e echoNode) Send(line string) error {
	go func() {
		e.onLine("mprof[window]: methods=1 activations=5 overflow=0 cap=256")
		e.onLine("mprof: 3992 5")
		e.onLine("mprof[window]: end")
	}()
	return nil
}

func profileServer(t *testing.T) *httptest.Server {
	t.Helper()
	mux := http.NewServeMux()
	mountProfile(mux, []string{""}, fixtureMap(t), func(onLine func(string), onEnd func()) (profile.Sender, error) {
		return echoNode{onLine}, nil
	}, nil, &sync.Mutex{})
	srv := httptest.NewServer(mux)
	t.Cleanup(srv.Close)
	return srv
}

func TestProfileHasNoCaptureUntilOneIsTaken(t *testing.T) {
	srv := profileServer(t)
	r, _ := http.Get(srv.URL + "/profile/last")
	if r.StatusCode != http.StatusNotFound {
		t.Fatalf("status %d", r.StatusCode)
	}
	r, _ = http.Get(srv.URL + "/profile")
	if r.StatusCode != 200 || !strings.HasPrefix(r.Header.Get("Content-Type"), "text/html") {
		t.Fatalf("page: %d %s", r.StatusCode, r.Header.Get("Content-Type"))
	}
}

func TestProfileCaptureRanksAndIsKeptAsLast(t *testing.T) {
	srv := profileServer(t)
	r, err := http.Post(srv.URL+"/profile/capture?ms=50", "", nil)
	if err != nil || r.StatusCode != 200 {
		t.Fatalf("capture: %v %d", err, r.StatusCode)
	}
	for _, path := range []string{"/profile/capture", "/profile/last"} {
		if path == "/profile/last" {
			r, _ = http.Get(srv.URL + path)
		}
		var c Capture
		if err := json.NewDecoder(r.Body).Decode(&c); err != nil {
			t.Fatal(err)
		}
		if c.Ms != 50 || len(c.Report.Rows) != 1 || c.Report.Rows[0].Name != "Fixture>>main:args:" || c.Report.Rows[0].Pct != 100 {
			t.Fatalf("%s: %+v", path, c)
		}
	}
}

func TestProfileCaptureNeedsAPositiveMs(t *testing.T) {
	srv := profileServer(t)
	for _, q := range []string{"", "?ms=0", "?ms=x"} {
		r, _ := http.Post(srv.URL+"/profile/capture"+q, "", nil)
		if r.StatusCode != http.StatusBadRequest {
			t.Fatalf("%q: %d", q, r.StatusCode)
		}
	}
}

// D15: a capture on a board whose debug page holds the cable borrows it, and
// gives it back unchanged (resume, not a restart).
func TestACaptureBorrowsTheDebugCable(t *testing.T) {
	var order []string
	held := &cable{
		release: func() error { order = append(order, "release"); return nil },
		reopen:  func(string) string { order = append(order, "reopen"); return "" },
		resume:  func() error { order = append(order, "resume"); return nil },
	}
	mux := http.NewServeMux()
	mountProfile(mux, []string{""}, fixtureMap(t), func(onLine func(string), onEnd func()) (profile.Sender, error) {
		order = append(order, "capture")
		return echoNode{onLine}, nil
	}, held, &sync.Mutex{})
	srv := httptest.NewServer(mux)
	defer srv.Close()
	r, err := http.Post(srv.URL+"/profile/capture?ms=10", "", nil)
	if err != nil || r.StatusCode != 200 {
		t.Fatalf("%v %v", err, r.StatusCode)
	}
	if strings.Join(order, ",") != "release,capture,resume" {
		t.Fatalf("order %v", order)
	}
}
