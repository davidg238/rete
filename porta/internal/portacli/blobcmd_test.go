// Copyright (c) 2026 Ekorau LLC

// internal/portacli/blobcmd_test.go
package portacli

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/davidg238/porta/internal/cval"
)

// runRoot builds a fresh root command tree, runs it with args, and discards
// output — for setup calls (e.g. `key gen`) whose stdout the test doesn't
// need to inspect.
func runRoot(t *testing.T, args ...string) error {
	t.Helper()
	_, err := runRootCapture(t, args...)
	return err
}

// runRootCapture builds a fresh root command tree, runs it with args, and
// returns everything written to stdout/stderr.
func runRootCapture(t *testing.T, args ...string) (string, error) {
	t.Helper()
	var buf bytes.Buffer
	cmd := NewRootCmd()
	cmd.SetOut(&buf)
	cmd.SetErr(&buf)
	cmd.SetArgs(args)
	err := cmd.Execute()
	return buf.String(), err
}

// containsLine reports whether out has a line exactly equal to want.
func containsLine(out, want string) bool {
	for _, line := range strings.Split(out, "\n") {
		if line == want {
			return true
		}
	}
	return false
}

// appRecord is the pure builder blob put uses; test it directly.
func TestAppRecordEncodesTheCatalogueShape(t *testing.T) {
	h := sha256.Sum256([]byte("img"))
	sig := make([]byte, 64)
	raw, err := appRecord(h[:], 3, sig)
	if err != nil {
		t.Fatal(err)
	}
	v, err := cval.Decode(raw)
	if err != nil {
		t.Fatal(err)
	}
	r, ok := v.(cval.Record)
	if !ok || r.Tag != "App" {
		t.Fatalf("want Record App, got %#v", v)
	}
	var img cval.Record
	var gotSig cval.Bytes
	for _, p := range r.Fields {
		switch p.K {
		case cval.Symbol("image"):
			img = p.V.(cval.Record)
		case cval.Symbol("sig"):
			gotSig = p.V.(cval.Bytes)
		}
	}
	if img.Tag != "BlobRef" || len(gotSig) != 64 {
		t.Fatalf("shape wrong: %#v", r)
	}
}

func TestBlobPutStoresSignsAndPrints(t *testing.T) {
	dir := t.TempDir()
	dbPath = filepath.Join(dir, "porta.db") // package-level flag var, reset for this test
	priv, pub := keyPaths(dbPath)
	_ = pub
	if err := os.WriteFile(filepath.Join(dir, "app.oimg"), []byte("fake image"), 0o644); err != nil {
		t.Fatal(err)
	}
	// generate keys first
	if err := runRoot(t, "--db", dbPath, "key", "gen"); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(priv); err != nil {
		t.Fatal("key gen wrote nothing")
	}
	out, err := runRootCapture(t, "--db", dbPath, "blob", "put", filepath.Join(dir, "app.oimg"))
	if err != nil {
		t.Fatal(err)
	}
	sum := sha256.Sum256([]byte("fake image"))
	if !containsLine(out, "hash "+hex.EncodeToString(sum[:])) {
		t.Fatalf("hash line missing in:\n%s", out)
	}
	if _, err := os.Stat(filepath.Join(dir, "blobs", hex.EncodeToString(sum[:]))); err != nil {
		t.Fatal("blob not stored under blobs/")
	}
}
