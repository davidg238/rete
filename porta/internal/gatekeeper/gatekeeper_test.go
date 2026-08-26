// Copyright (c) 2026 Ekorau LLC

package gatekeeper

import (
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func newTestDocs(t *testing.T) string {
	t.Helper()
	dir := t.TempDir()
	if err := os.MkdirAll(filepath.Join(dir, "status"), 0o755); err != nil {
		t.Fatal(err)
	}
	html := "<title>rete dev status</title>"
	if err := os.WriteFile(filepath.Join(dir, "status", "dev-status.html"), []byte(html), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "next-steps-overview.md"), []byte("# next\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	return dir
}

func TestRootAndDevLandOnTheStatusPage(t *testing.T) {
	h, err := Handler(Config{Docs: newTestDocs(t)})
	if err != nil {
		t.Fatal(err)
	}
	srv := httptest.NewServer(h)
	defer srv.Close()
	for _, path := range []string{"/", "/dev"} {
		resp, err := srv.Client().Get(srv.URL + path)
		if err != nil {
			t.Fatal(err)
		}
		body, _ := io.ReadAll(resp.Body)
		resp.Body.Close()
		if resp.StatusCode != http.StatusOK || !strings.Contains(string(body), "rete dev status") {
			t.Fatalf("GET %s: status %d body %q — did not land on the status page", path, resp.StatusCode, body)
		}
	}
}

func TestMarkdownServesAsReadableText(t *testing.T) {
	h, err := Handler(Config{Docs: newTestDocs(t)})
	if err != nil {
		t.Fatal(err)
	}
	srv := httptest.NewServer(h)
	defer srv.Close()
	resp, err := srv.Client().Get(srv.URL + "/dev/next-steps-overview.md")
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	if ct := resp.Header.Get("Content-Type"); !strings.HasPrefix(ct, "text/plain") {
		t.Fatalf("markdown served as %q, want text/plain so a browser renders it", ct)
	}
}

// The entry point FRONTS porta's operator surfaces: /health and /manifest are
// proxied to the sheltered porta HTTP listener, so the status page's relative
// links keep working when read through gatekeeper.
func TestHealthAndManifestProxyToPorta(t *testing.T) {
	backend := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Write([]byte("porta says " + r.URL.Path))
	}))
	defer backend.Close()
	h, err := Handler(Config{Docs: newTestDocs(t), PortaHTTP: backend.URL})
	if err != nil {
		t.Fatal(err)
	}
	srv := httptest.NewServer(h)
	defer srv.Close()
	for _, path := range []string{"/health", "/manifest"} {
		resp, err := srv.Client().Get(srv.URL + path)
		if err != nil {
			t.Fatal(err)
		}
		body, _ := io.ReadAll(resp.Body)
		resp.Body.Close()
		if string(body) != "porta says "+path {
			t.Fatalf("GET %s: not proxied to porta; got %q", path, body)
		}
	}
}

// Without a porta address the proxy routes answer 502, not 404 — the surface
// exists and names its missing backend rather than pretending the path does not.
func TestProxyWithoutBackendAnswers502(t *testing.T) {
	h, err := Handler(Config{Docs: newTestDocs(t)})
	if err != nil {
		t.Fatal(err)
	}
	srv := httptest.NewServer(h)
	defer srv.Close()
	resp, err := srv.Client().Get(srv.URL + "/health")
	if err != nil {
		t.Fatal(err)
	}
	resp.Body.Close()
	if resp.StatusCode != http.StatusBadGateway {
		t.Fatalf("GET /health with no porta backend: status %d, want 502", resp.StatusCode)
	}
}
