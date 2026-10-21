// Copyright (c) 2026 Ekorau LLC

package portacli

import (
	"bytes"
	"crypto/ed25519"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
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
	evs, err := st.Events("n1", space.EventFilter{Limit: 10})
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

// rete#432 P6: a peer reads back the blob it pushed, by the hash porta answered.
func TestOpsBlobRead(t *testing.T) {
	srv, _ := opsServer(t)
	text := []byte("oevm-patch 1 vtable 280\n")
	_, body := do(t, srv, "POST", "/blob", text)
	var put struct{ Hash string }
	if err := json.Unmarshal(body, &put); err != nil {
		t.Fatalf("POST /blob: %s", body)
	}
	resp, got := do(t, srv, "GET", "/blob/"+put.Hash, nil)
	if resp.StatusCode != 200 || !bytes.Equal(got, text) {
		t.Fatalf("GET /blob: %d %q, want %q", resp.StatusCode, got, text)
	}
	absent := strings.Repeat("ab", 32)
	if resp, _ := do(t, srv, "GET", "/blob/"+absent, nil); resp.StatusCode != 404 {
		t.Errorf("an absent blob: %d, want 404", resp.StatusCode)
	}
	if resp, _ := do(t, srv, "GET", "/blob/xyz", nil); resp.StatusCode != 400 {
		t.Errorf("a malformed hash: %d, want 400", resp.StatusCode)
	}
}

// rete#432 P5: the log is read by porta's row id, oldest first, filtered to one
// path, so a poller that keeps the last ID it saw reads each row exactly once.
func TestOpsLogCursor(t *testing.T) {
	srv, _ := opsServer(t)
	for _, l := range []string{"dbg:break 1 0", "dbg:continue", "dbg:step"} {
		v, _ := cval.Encode(l)
		do(t, srv, "PUT", "/node/n1/dbg/req", v)
	}
	g, _ := cval.Encode("bell")
	do(t, srv, "PUT", "/node/n1/goal/name", g)
	type row struct {
		ID    int64
		Path  string
		Value string
	}
	read := func(q string) []row {
		t.Helper()
		resp, body := do(t, srv, "GET", "/node/n1/log"+q, nil)
		var rows []row
		if resp.StatusCode != 200 || json.Unmarshal(body, &rows) != nil {
			t.Fatalf("GET log%s: %d %s", q, resp.StatusCode, body)
		}
		return rows
	}
	all := read("?path=dbg/req")
	if len(all) != 3 {
		t.Fatalf("%d dbg/req rows, want 3 (the goal/name row filtered out): %+v", len(all), all)
	}
	for i, r := range all {
		want, _ := cval.Encode([]string{"dbg:break 1 0", "dbg:continue", "dbg:step"}[i])
		if r.Path != "dbg/req" || r.Value != hex.EncodeToString(want) || (i > 0 && r.ID <= all[i-1].ID) {
			t.Fatalf("row %d = %+v: want dbg/req rows oldest first", i, r)
		}
	}
	rest := read(fmt.Sprintf("?path=dbg/req&after=%d", all[1].ID))
	if len(rest) != 1 || rest[0].ID != all[2].ID {
		t.Fatalf("after %d: %+v, want only the last row", all[1].ID, rest)
	}
	if n := len(read("")); n != 4 {
		t.Errorf("no path: %d rows, want 4", n)
	}
	if empty := read(fmt.Sprintf("?path=dbg/req&after=%d", all[2].ID)); len(empty) != 0 {
		t.Errorf("after the last row: %+v, want []", empty)
	}
	if resp, _ := do(t, srv, "GET", "/node/n1/log?after=x", nil); resp.StatusCode != 400 {
		t.Errorf("a malformed cursor: %d, want 400", resp.StatusCode)
	}
}
