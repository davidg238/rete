// Copyright (c) 2026 Ekorau LLC

package custos

import (
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"runtime"
	"strings"
	"testing"

	"github.com/davidg238/porta/internal/cval"
	"github.com/davidg238/porta/internal/space"
)

func repoDescriptors(t *testing.T) string {
	t.Helper()
	_, here, _, _ := runtime.Caller(0)
	return filepath.Join(filepath.Dir(here), "..", "..", "..", "protocol", "descriptors", "records.yaml")
}

// fleetStore is a one-node store with a commissioned role and a verdict, built
// through porta's own writer.
func fleetStore(t *testing.T) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), "store.db")
	var clock int64 = 1_700_000_000_000
	st, err := space.Open(path, func() int64 { clock += 1_000; return clock })
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { st.Close() })
	enc := func(v any) []byte {
		b, err := cval.Encode(v)
		if err != nil {
			t.Fatal(err)
		}
		return b
	}
	if _, err := st.Write("n1", "goal/name", cval.KindCell, enc("porch"), "david", "commission"); err != nil {
		t.Fatal(err)
	}
	if _, _, err := st.Ingest("n1", 3, []space.Entry{
		{Seq: 1, Path: "obs/name", Kind: cval.KindCell, Value: enc("porch")},
	}); err != nil {
		t.Fatal(err)
	}
	if err := st.WriteGateway("n1", space.PathLiveness, enc(cval.Symbol("live")), "live: fine"); err != nil {
		t.Fatal(err)
	}
	return path
}

func fleetServer(t *testing.T) *httptest.Server {
	t.Helper()
	h, err := Handler(Config{Docs: newTestDocs(t), Store: fleetStore(t), Descriptors: repoDescriptors(t)})
	if err != nil {
		t.Fatal(err)
	}
	srv := httptest.NewServer(h)
	t.Cleanup(srv.Close)
	return srv
}

type answer struct {
	Binding string   `json:"binding"`
	Columns []string `json:"columns"`
	Rows    [][]any  `json:"rows"`
}

func getAnswer(t *testing.T, srv *httptest.Server, path string) answer {
	t.Helper()
	resp, err := srv.Client().Get(srv.URL + path)
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	body, _ := io.ReadAll(resp.Body)
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("GET %s = %d: %s", path, resp.StatusCode, body)
	}
	if ct := resp.Header.Get("Content-Type"); !strings.HasPrefix(ct, "application/json") {
		t.Errorf("GET %s Content-Type = %q", path, ct)
	}
	var a answer
	if err := json.Unmarshal(body, &a); err != nil {
		t.Fatalf("GET %s: %v: %s", path, err, body)
	}
	return a
}

func column(t *testing.T, a answer, row int, name string) any {
	t.Helper()
	for i, c := range a.Columns {
		if c == name {
			return a.Rows[row][i]
		}
	}
	t.Fatalf("no column %q in %v", name, a.Columns)
	return nil
}

// TestFleetLivenessAnswersFromTheLiveStore: the page's main table is the gate's
// liveness view, served as the gate's own answer — columns, rows and the
// binding it came from.
func TestFleetLivenessAnswersFromTheLiveStore(t *testing.T) {
	a := getAnswer(t, fleetServer(t), "/q/liveness")
	if a.Binding != "live" || len(a.Rows) != 1 {
		t.Fatalf("liveness = %+v", a)
	}
	if column(t, a, 0, "verdict") != "live" || column(t, a, 0, "role") != "porch" {
		t.Errorf("liveness row = %v", a.Rows[0])
	}
}

func TestFleetConvergenceAnswers(t *testing.T) {
	a := getAnswer(t, fleetServer(t), "/q/convergence")
	if len(a.Rows) != 1 || column(t, a, 0, "converged") != float64(1) {
		t.Fatalf("convergence = %+v", a)
	}
}

// TestOnlyNamedQueriesAreServed: the network surface is a fixed set of named
// queries. There is no route that takes SQL, so an unknown name is a 404 and
// not an error from the query core.
func TestOnlyNamedQueriesAreServed(t *testing.T) {
	srv := fleetServer(t)
	for _, path := range []string{"/q/events", "/q/", "/q/liveness?sql=delete+from+cells"} {
		resp, err := srv.Client().Get(srv.URL + path)
		if err != nil {
			t.Fatal(err)
		}
		resp.Body.Close()
		want := http.StatusNotFound
		if strings.Contains(path, "?") {
			want = http.StatusOK // the parameter is ignored, never read
		}
		if resp.StatusCode != want {
			t.Errorf("GET %s = %d, want %d", path, resp.StatusCode, want)
		}
	}
}

// TestRootLandsOnTheFleetWhenAStoreIsConfigured: with a store, the front door
// opens on the fleet; the dev status page stays at /dev.
func TestRootLandsOnTheFleetWhenAStoreIsConfigured(t *testing.T) {
	srv := fleetServer(t)
	client := *srv.Client()
	client.CheckRedirect = func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }
	for path, want := range map[string]string{"/": "/fleet", "/dev": "/dev/status/dev-status.html"} {
		resp, err := client.Get(srv.URL + path)
		if err != nil {
			t.Fatal(err)
		}
		resp.Body.Close()
		if loc := resp.Header.Get("Location"); loc != want {
			t.Errorf("GET %s -> %q, want %q", path, loc, want)
		}
	}
	resp, err := srv.Client().Get(srv.URL + "/fleet")
	if err != nil {
		t.Fatal(err)
	}
	body, _ := io.ReadAll(resp.Body)
	resp.Body.Close()
	if resp.StatusCode != http.StatusOK || !strings.Contains(string(body), "/q/liveness") {
		t.Errorf("GET /fleet = %d, page does not read /q/liveness", resp.StatusCode)
	}
}

// TestNoStoreNoFleet: without --store custos is exactly the shell it was.
func TestNoStoreNoFleet(t *testing.T) {
	h, err := Handler(Config{Docs: newTestDocs(t)})
	if err != nil {
		t.Fatal(err)
	}
	srv := httptest.NewServer(h)
	defer srv.Close()
	resp, err := srv.Client().Get(srv.URL + "/q/liveness")
	if err != nil {
		t.Fatal(err)
	}
	resp.Body.Close()
	if resp.StatusCode != http.StatusNotFound {
		t.Errorf("/q/liveness without a store = %d", resp.StatusCode)
	}
}
