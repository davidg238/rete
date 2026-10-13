// Copyright (c) 2026 Ekorau LLC

package portacli

import (
	"bytes"
	"crypto/ed25519"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"testing"

	"github.com/davidg238/porta/internal/blob"
	"github.com/davidg238/porta/internal/space"
	"github.com/davidg238/rete/protocol/cval"
)

// opsServer is porta's ops routes over a fresh store with a fresh key.
func opsServer(t *testing.T) (*httptest.Server, string) {
	t.Helper()
	db := filepath.Join(t.TempDir(), "porta.db")
	priv, pub := keyPaths(db)
	if err := blob.GenerateKeys(priv, pub); err != nil {
		t.Fatal(err)
	}
	st, err := space.Open(db, nowMs)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { st.Close() })
	mux := http.NewServeMux()
	mountOps(mux, st, db)
	srv := httptest.NewServer(mux)
	t.Cleanup(srv.Close)
	return srv, db
}

func do(t *testing.T, srv *httptest.Server, method, path string, body []byte) (*http.Response, []byte) {
	t.Helper()
	req, err := http.NewRequest(method, srv.URL+path, bytes.NewReader(body))
	if err != nil {
		t.Fatal(err)
	}
	resp, err := srv.Client().Do(req)
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	b, _ := io.ReadAll(resp.Body)
	return resp, b
}

// A push over HTTP is the CLI's push: the blob stored and signed by porta's
// key, its record written as the goal, and the same bytes read back.
func TestOpsBlobThenGoalThenRead(t *testing.T) {
	srv, db := opsServer(t)
	img := []byte("an image")
	resp, body := do(t, srv, "POST", "/blob", img)
	if resp.StatusCode != 200 {
		t.Fatalf("POST /blob: %d %s", resp.StatusCode, body)
	}
	var s struct {
		Hash, Sig, Record string
		Size              int64
	}
	if err := json.Unmarshal(body, &s); err != nil {
		t.Fatal(err)
	}
	sum := sha256.Sum256(img)
	if s.Hash != hex.EncodeToString(sum[:]) || s.Size != int64(len(img)) {
		t.Fatalf("blob answer %+v", s)
	}
	if _, err := os.Stat(filepath.Join(blobDir(db), s.Hash)); err != nil {
		t.Fatal("blob not stored where the block plane serves from")
	}
	_, pubPath := keyPaths(db)
	pubHex, _ := os.ReadFile(pubPath)
	pub, _ := hex.DecodeString(string(pubHex))
	sig, _ := hex.DecodeString(s.Sig)
	if !ed25519.Verify(pub, sum[:], sig) {
		t.Fatal("the signature does not verify against porta.pub")
	}

	rec, _ := hex.DecodeString(s.Record)
	resp, body = do(t, srv, "PUT", "/node/n1/goal/apps/main", rec)
	if resp.StatusCode != 200 || string(bytes.TrimSpace(body)) != `{"Seq":1}` {
		t.Fatalf("PUT goal: %d %s", resp.StatusCode, body)
	}
	resp, body = do(t, srv, "GET", "/node/n1/goal/apps/main", nil)
	if resp.StatusCode != 200 || !bytes.Equal(body, rec) || resp.Header.Get("X-Porta-Seq") != "1" {
		t.Fatalf("GET goal: %d seq %q", resp.StatusCode, resp.Header.Get("X-Porta-Seq"))
	}
}

func TestOpsRefusals(t *testing.T) {
	srv, _ := opsServer(t)
	one, _ := cval.Encode(int64(1))
	for _, c := range []struct {
		name, method, path string
		body               []byte
		want               int
	}{
		{"an obs/ write is not intent", "PUT", "/node/n1/obs/apps/main", one, 400},
		{"a gw/ write is not intent", "PUT", "/node/n1/gw/liveness", one, 400},
		{"not a closed cval encoding", "PUT", "/node/n1/goal/x", []byte{0xff, 0x00}, 400},
		{"over the value cap", "PUT", "/node/n1/goal/x", make([]byte, space.MaxValueBytes+1), 413},
		{"a cell never written", "GET", "/node/n1/goal/x", nil, 404},
		{"a prefix is not a cell", "GET", "/node/n1/goal", nil, 404},
	} {
		if c.name == "a prefix is not a cell" {
			do(t, srv, "PUT", "/node/n1/goal/y", one)
		}
		if resp, body := do(t, srv, c.method, c.path, c.body); resp.StatusCode != c.want {
			t.Errorf("%s: %d %s, want %d", c.name, resp.StatusCode, body, c.want)
		}
	}
}

func TestOpsBlobWithoutAKeyRefuses(t *testing.T) {
	db := filepath.Join(t.TempDir(), "porta.db")
	st, err := space.Open(db, nowMs)
	if err != nil {
		t.Fatal(err)
	}
	defer st.Close()
	mux := http.NewServeMux()
	mountOps(mux, st, db)
	srv := httptest.NewServer(mux)
	defer srv.Close()
	if resp, body := do(t, srv, "POST", "/blob", []byte("x")); resp.StatusCode != 400 {
		t.Fatalf("keyless POST /blob: %d %s", resp.StatusCode, body)
	}
}
