// Copyright (c) 2026 Ekorau LLC

package custos

import (
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"runtime"
	"strings"
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
	mountProfile(mux, fixtureMap(t), func(onLine func(string), onEnd func()) (profile.Sender, error) {
		return echoNode{onLine}, nil
	})
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

func TestNoProfilePortNoProfileRoutes(t *testing.T) {
	h, err := Handler(Config{})
	if err != nil {
		t.Fatal(err)
	}
	srv := httptest.NewServer(h)
	defer srv.Close()
	r, _ := http.Get(srv.URL + "/profile")
	if r.StatusCode != http.StatusNotFound {
		t.Fatalf("status %d", r.StatusCode)
	}
}

func TestProfilePortWithoutAMapIsRefused(t *testing.T) {
	if _, err := Handler(Config{ProfilePort: "/dev/null"}); err == nil || !strings.Contains(err.Error(), "--profile-map") {
		t.Fatalf("err %v", err)
	}
}

func TestTheStylesheetIsServedWithoutAStore(t *testing.T) {
	h, err := Handler(Config{ProfilePort: "/dev/null", ProfileMap: fixtureMap(t)})
	if err != nil {
		t.Fatal(err)
	}
	srv := httptest.NewServer(h)
	defer srv.Close()
	for _, p := range []string{"/fleet.css", "/profile"} {
		if r, _ := http.Get(srv.URL + p); r.StatusCode != 200 {
			t.Fatalf("%s: %d", p, r.StatusCode)
		}
	}
}

// rete#364: a host VM is the profile page's board-free target, checked at
// start-up because each capture starts it afresh.
func TestProfileHostConfigIsChecked(t *testing.T) {
	m := fixtureMap(t)
	cases := map[string]Config{
		"one of":         {ProfilePort: "/dev/ttyX", ProfileHost: "/bin/true", ProfileMap: m},
		"--profile-map":  {ProfileHost: "/bin/true"},
		"--profile-host": {ProfileHost: "/nonexistent/oevm_img", ProfileMap: m},
	}
	for want, cfg := range cases {
		if _, err := Handler(cfg); err == nil || !strings.Contains(err.Error(), want) {
			t.Errorf("%+v: %v, want an error naming %q", cfg, err, want)
		}
	}
	wrong := filepath.Join(t.TempDir(), "wrong.oimg")
	os.WriteFile(wrong, []byte("not the image the map describes"), 0o644)
	if _, err := Handler(Config{ProfileHost: "/bin/true " + wrong, ProfileMap: m}); err == nil {
		t.Errorf("an image the map does not describe was accepted")
	}
}

// A host program that ends before its window closes answers 502 at once.
func TestAProfileHostThatEndsEarlyAnswers502(t *testing.T) {
	h, err := Handler(Config{ProfileHost: "/bin/true", ProfileMap: fixtureMap(t)})
	if err != nil {
		t.Fatal(err)
	}
	srv := httptest.NewServer(h)
	defer srv.Close()
	r, err := http.Post(srv.URL+"/profile/capture?ms=50", "", nil)
	if err != nil {
		t.Fatal(err)
	}
	b, _ := io.ReadAll(r.Body)
	if r.StatusCode != http.StatusBadGateway || !strings.Contains(string(b), "program ended before the window closed") {
		t.Fatalf("%d %s", r.StatusCode, b)
	}
}
