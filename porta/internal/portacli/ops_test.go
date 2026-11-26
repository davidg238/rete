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
		{"gw/liveness is porta's own verdict", "PUT", "/node/n1/gw/liveness", one, 400},
		{"dbg/resp is the node's", "PUT", "/node/n1/dbg/resp", one, 400},
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

// rete#415: a dbg/req line is the one stream porta writes; it lands as a
// stream row (a second write is a second row, never an overwrite).
func TestOpsDbgReqIsAStream(t *testing.T) {
	srv, db := opsServer(t)
	line, _ := cval.Encode("profile 3000")
	for i, want := range []string{`{"Seq":1}`, `{"Seq":2}`} {
		resp, body := do(t, srv, "PUT", "/node/n1/dbg/req", line)
		if resp.StatusCode != 200 || string(bytes.TrimSpace(body)) != want {
			t.Fatalf("PUT dbg/req #%d: %d %s", i+1, resp.StatusCode, body)
		}
	}
	st, err := space.Open(db, nowMs)
	if err != nil {
		t.Fatal(err)
	}
	defer st.Close()
	evs, err := st.Events("n1", 10, 0)
	if err != nil {
		t.Fatal(err)
	}
	n := 0
	for _, e := range evs {
		if e.Path == "dbg/req" {
			n++
			if e.Kind != cval.KindStream {
				t.Errorf("dbg/req row %d has kind %v, want a stream", e.Seq, e.Kind)
			}
		}
	}
	if n != 2 {
		t.Fatalf("%d dbg/req rows, want 2", n)
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

// rete#515: gw/apps/<name> is an operator's record porta keeps about a node.
// It lands as a gateway row under the caller's actor, reads back through GET,
// and never rides to the node.
func TestOpsGatewayAppRecord(t *testing.T) {
	srv, db := opsServer(t)
	rec, _ := cval.Encode(cval.Record{Tag: "AppDebug", Fields: cval.Map{
		{K: cval.Symbol("program"), V: cval.Bytes(make([]byte, 16))},
		{K: cval.Symbol("snapshot"), V: cval.Record{Tag: "BlobRef", Fields: cval.Map{
			{K: cval.Symbol("hash"), V: cval.Bytes(make([]byte, 32))}}}},
	}})
	req, _ := http.NewRequest("PUT", srv.URL+"/node/n1/gw/apps/main", bytes.NewReader(rec))
	req.Header.Set("X-Porta-Actor", "faber")
	resp, err := srv.Client().Do(req)
	if err != nil || resp.StatusCode != 200 {
		t.Fatalf("PUT gw/apps: %v %v", err, resp)
	}
	resp.Body.Close()
	resp, body := do(t, srv, "GET", "/node/n1/gw/apps/main", nil)
	if resp.StatusCode != 200 || !bytes.Equal(body, rec) {
		t.Fatalf("GET gw/apps: %d", resp.StatusCode)
	}
	st, err := space.Open(db, nowMs)
	if err != nil {
		t.Fatal(err)
	}
	defer st.Close()
	out, _, err := st.OutboundSince("n1", 0, 4096)
	if err != nil || len(out) != 0 {
		t.Fatalf("a gw row must never ride to the node: %v %+v", err, out)
	}
	evs, err := st.Events("n1", 10, 0)
	if err != nil || len(evs) != 1 || evs[0].Path != "gw/apps/main" || evs[0].Actor != "faber" || evs[0].Direction != "gw" {
		t.Fatalf("want one gw event by faber: %v %+v", err, evs)
	}
}

// A path ending in / lists that subtree (rete#517): every goal/apps/<name>
// of a node, in path order, and nothing from a sibling such as goal/appsX.
// An empty subtree is an answer, not a 404.
func TestOpsListSubtree(t *testing.T) {
	srv, _ := opsServer(t)
	resp, body := do(t, srv, "GET", "/node/n1/goal/apps/", nil)
	if resp.StatusCode != 200 || string(bytes.TrimSpace(body)) != `{"Boot":0,"Cells":[]}` {
		t.Fatalf("empty list: %d %s", resp.StatusCode, body)
	}
	a, _ := cval.Encode(cval.Symbol("a"))
	b, _ := cval.Encode(cval.Symbol("b"))
	for _, w := range []struct {
		path string
		v    []byte
	}{{"goal/apps/zeta", b}, {"goal/apps/main", a}, {"goal/appsX", a}} {
		if resp, body := do(t, srv, "PUT", "/node/n1/"+w.path, w.v); resp.StatusCode != 200 {
			t.Fatalf("PUT %s: %d %s", w.path, resp.StatusCode, body)
		}
	}
	resp, body = do(t, srv, "GET", "/node/n1/goal/apps/", nil)
	if resp.StatusCode != 200 || resp.Header.Get("Content-Type") != "application/json" {
		t.Fatalf("list: %d %s", resp.StatusCode, body)
	}
	var got struct {
		Boot  int64
		Cells []struct {
			Path  string
			Seq   int64
			Boot  int64
			Value string
		}
	}
	if err := json.Unmarshal(body, &got); err != nil {
		t.Fatal(err)
	}
	if len(got.Cells) != 2 ||
		got.Cells[0].Path != "goal/apps/main" || got.Cells[0].Value != hex.EncodeToString(a) || got.Cells[0].Seq != 2 ||
		got.Cells[1].Path != "goal/apps/zeta" || got.Cells[1].Value != hex.EncodeToString(b) || got.Cells[1].Seq != 1 {
		t.Fatalf("list: %s", body)
	}
	// Without the trailing slash the route is unchanged: one cell, raw CBOR.
	resp, body = do(t, srv, "GET", "/node/n1/goal/apps/main", nil)
	if resp.StatusCode != 200 || !bytes.Equal(body, a) {
		t.Fatalf("single cell: %d %s", resp.StatusCode, body)
	}
}
