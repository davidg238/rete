// Copyright (c) 2026 Ekorau LLC

package portacli

import (
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// devDocsHandler serves the repo docs tree read-only under /dev/ — the dev
// status surface. The bare /dev redirects to the status page so an operator
// (or agent) can be told just "hit /dev".
func newDevDocsServer(t *testing.T, dir string) *httptest.Server {
	t.Helper()
	mux := http.NewServeMux()
	registerDevDocs(mux, dir)
	return httptest.NewServer(mux)
}

func TestDevDocsServesStatusPageAtSlashDev(t *testing.T) {
	dir := t.TempDir()
	if err := os.MkdirAll(filepath.Join(dir, "status"), 0o755); err != nil {
		t.Fatal(err)
	}
	html := "<title>rete dev status</title>"
	if err := os.WriteFile(filepath.Join(dir, "status", "dev-status.html"), []byte(html), 0o644); err != nil {
		t.Fatal(err)
	}
	srv := newDevDocsServer(t, dir)
	defer srv.Close()
	resp, err := srv.Client().Get(srv.URL + "/dev")
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	body, _ := io.ReadAll(resp.Body)
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("GET /dev: status %d", resp.StatusCode)
	}
	if string(body) != html {
		t.Fatalf("GET /dev did not land on the status page; got %q", body)
	}
}

func TestDevDocsServesMarkdownAsReadableText(t *testing.T) {
	dir := t.TempDir()
	md := "# next steps\n"
	if err := os.WriteFile(filepath.Join(dir, "next-steps-overview.md"), []byte(md), 0o644); err != nil {
		t.Fatal(err)
	}
	srv := newDevDocsServer(t, dir)
	defer srv.Close()
	resp, err := srv.Client().Get(srv.URL + "/dev/next-steps-overview.md")
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	body, _ := io.ReadAll(resp.Body)
	if string(body) != md {
		t.Fatalf("served bytes differ from the file; got %q", body)
	}
	// text/plain, not application/octet-stream or text/markdown — the point is
	// a browser DISPLAYS the document of record instead of downloading it.
	if ct := resp.Header.Get("Content-Type"); !strings.HasPrefix(ct, "text/plain") {
		t.Fatalf("markdown served as %q, want text/plain so a browser renders it", ct)
	}
}
