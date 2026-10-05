// Copyright (c) 2026 Ekorau LLC

package portacli

import (
	"crypto/sha256"
	"encoding/hex"
	"io"
	"net/http/httptest"
	"path/filepath"
	"runtime"
	"testing"

	"github.com/davidg238/rete/protocol/manifest"
)

func TestManifestHandlerServesFileAndHash(t *testing.T) {
	_, this, _, _ := runtime.Caller(0)
	p := filepath.Join(filepath.Dir(this), "..", "..", "..", "protocol", "MANIFEST.cddl")
	tbl, err := manifest.Load(p)
	if err != nil {
		t.Fatal(err)
	}
	srv := httptest.NewServer(manifestHandler(tbl))
	defer srv.Close()
	resp, err := srv.Client().Get(srv.URL + "/manifest")
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	body, _ := io.ReadAll(resp.Body)
	sum := sha256.Sum256(body)
	if got := resp.Header.Get("X-Manifest-SHA256"); got != hex.EncodeToString(sum[:]) {
		t.Fatalf("header hash %s does not match body hash", got)
	}
	if string(body) != string(tbl.Raw) {
		t.Fatal("served bytes differ from the loaded file")
	}
}

func TestManifestHandlerNilTable404s(t *testing.T) {
	srv := httptest.NewServer(manifestHandler(nil))
	defer srv.Close()
	resp, err := srv.Client().Get(srv.URL + "/manifest")
	if err != nil {
		t.Fatal(err)
	}
	resp.Body.Close()
	if resp.StatusCode != 404 {
		t.Fatalf("manifest-less server answered %d, want 404", resp.StatusCode)
	}
}
