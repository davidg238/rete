// Copyright (c) 2026 Ekorau LLC

package custos

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"strconv"
	"strings"
	"sync"
	"testing"

	"github.com/davidg238/rete/protocol/cval"
)

// fakePorta answers porta's three ops routes (rete#398) from memory: custos may
// not import porta (#338), so the real pair meets only in host_ota's gate.
type fakePorta struct {
	mu     sync.Mutex
	cells  map[string][]byte
	seq    int64
	actor  string
	refuse string // a PUT refusal, porta's words
	blobs  map[string][]byte
	resp   []string // the node's dbg/resp rows; row id = index + 1
}

// says is the node's half of a debug answer: one dbg/resp row.
func (f *fakePorta) says(line string) {
	f.mu.Lock()
	f.resp = append(f.resp, line)
	f.mu.Unlock()
}

func (f *fakePorta) server(t *testing.T) *httptest.Server {
	f.cells, f.blobs = map[string][]byte{}, map[string][]byte{}
	mux := http.NewServeMux()
	mux.HandleFunc("POST /blob", func(w http.ResponseWriter, r *http.Request) {
		b, _ := io.ReadAll(r.Body)
		sum := sha256.Sum256(b)
		f.mu.Lock()
		f.blobs[hex.EncodeToString(sum[:])] = b
		f.mu.Unlock()
		rec, _ := cval.Encode(cval.Record{Tag: "App", Fields: cval.Map{{K: cval.Symbol("sig"), V: cval.Bytes{1}}}})
		json.NewEncoder(w).Encode(map[string]any{"Hash": hex.EncodeToString(sum[:]), "Size": len(b), "Sig": "01", "Record": hex.EncodeToString(rec)})
	})
	mux.HandleFunc("PUT /node/{id}/{path...}", func(w http.ResponseWriter, r *http.Request) {
		if f.refuse != "" {
			http.Error(w, f.refuse, http.StatusBadRequest)
			return
		}
		b, _ := io.ReadAll(r.Body)
		f.mu.Lock()
		defer f.mu.Unlock()
		f.seq++
		f.actor = r.Header.Get("X-Porta-Actor")
		f.cells[r.PathValue("id")+"/"+r.PathValue("path")] = b
		json.NewEncoder(w).Encode(map[string]any{"Seq": f.seq})
	})
	mux.HandleFunc("GET /blob/{hash}", func(w http.ResponseWriter, r *http.Request) {
		f.mu.Lock()
		b, ok := f.blobs[r.PathValue("hash")]
		f.mu.Unlock()
		if !ok {
			http.NotFound(w, r)
			return
		}
		w.Write(b)
	})
	mux.HandleFunc("GET /node/{id}/log", func(w http.ResponseWriter, r *http.Request) {
		after, _ := strconv.Atoi(r.URL.Query().Get("after"))
		rows := []map[string]any{}
		f.mu.Lock()
		for i, l := range f.resp {
			if i+1 > after && r.URL.Query().Get("path") == "dbg/resp" {
				v, _ := cval.Encode(l)
				rows = append(rows, map[string]any{"ID": i + 1, "Value": hex.EncodeToString(v)})
			}
		}
		f.mu.Unlock()
		json.NewEncoder(w).Encode(rows)
	})
	mux.HandleFunc("GET /node/{id}/{path...}", func(w http.ResponseWriter, r *http.Request) {
		f.mu.Lock()
		v, ok := f.cells[r.PathValue("id")+"/"+r.PathValue("path")]
		f.mu.Unlock()
		if !ok {
			http.NotFound(w, r)
			return
		}
		w.Write(v)
	})
	srv := httptest.NewServer(mux)
	t.Cleanup(srv.Close)
	return srv
}

// runs is the node's half: it publishes obs/apps/main naming hash.
func (f *fakePorta) runs(t *testing.T, node, hash string) {
	h, _ := hex.DecodeString(hash)
	v, err := cval.Encode(cval.Record{Tag: "AppObs", Fields: cval.Map{
		{K: cval.Symbol("image"), V: cval.Record{Tag: "BlobRef", Fields: cval.Map{{K: cval.Symbol("hash"), V: cval.Bytes(h)}}}},
		{K: cval.Symbol("lifecycle"), V: cval.Symbol("running")},
	}})
	if err != nil {
		t.Fatal(err)
	}
	f.mu.Lock()
	f.cells[node+"/obs/apps/main"] = v
	f.mu.Unlock()
}

func portaBoard(t *testing.T, porta string) *httptest.Server {
	t.Helper()
	mux := http.NewServeMux()
	if err := mountBoards(context.Background(), mux, []Board{{Name: "bell", Node: "206ef1195438"}}, "", t.TempDir(), porta, ""); err != nil {
		t.Fatal(err)
	}
	srv := httptest.NewServer(mux)
	t.Cleanup(srv.Close)
	return srv
}

// A push answers at the goal write, and /load/last follows the node until it
// runs the image.
func TestPortaLoadPushesThenConverges(t *testing.T) {
	f := &fakePorta{}
	srv := portaBoard(t, f.server(t).URL)

	var infos []BoardInfo
	if code := getJSON(t, srv.URL+"/boards", &infos); code != 200 || len(infos) != 1 ||
		infos[0].Target != "porta" || infos[0].Node != "206ef1195438" || strings.Join(infos[0].Verbs, ",") != "load" {
		t.Fatalf("/boards: %d %+v", code, infos)
	}
	img := fixtureImage(t)
	sum := sha256.Sum256(img)
	want := hex.EncodeToString(sum[:])
	f.runs(t, "206ef1195438", strings.Repeat("ab", 32)) // the old program

	code, body := post(t, srv.URL+"/load?name=dir/Bell.oimg", img)
	var l Load
	if code != 200 || json.Unmarshal([]byte(body), &l) != nil {
		t.Fatalf("POST /load: %d %s", code, body)
	}
	p := l.Push
	if p == nil || l.Result.SHA256 != want || l.Result.Image != "Bell.oimg" {
		t.Fatalf("push answered %s", body)
	}
	if p.Image != "Bell.oimg" || p.SHA256 != want || p.Seq != 1 || p.Size != int64(len(img)) ||
		p.Running != strings.Repeat("ab", 32) || p.Converged {
		t.Fatalf("push answered %+v", p)
	}
	if f.actor != "custos" || f.cells["206ef1195438/goal/apps/main"] == nil {
		t.Fatalf("goal not written as custos: actor %q", f.actor)
	}

	f.runs(t, "206ef1195438", want) // the node asked, staged, confirmed
	var again Load
	if code := getJSON(t, srv.URL+"/load/last", &again); code != 200 || !again.Push.Converged ||
		again.Push.Running != want || again.LoadedMs != l.LoadedMs {
		t.Fatalf("/load/last: %d %+v", code, again)
	}
}

func TestPortaLoadAnswersPortasRefusal(t *testing.T) {
	f := &fakePorta{refuse: "porta: value is not a well-formed algebra value"}
	srv := portaBoard(t, f.server(t).URL)
	code, body := post(t, srv.URL+"/load", fixtureImage(t))
	if code != 502 || !strings.Contains(body, "not a well-formed algebra value") {
		t.Fatalf("refused push: %d %s", code, body)
	}
	if code, _ := get(t, srv.URL+"/load/last"); code != 404 {
		t.Fatalf("a refused push became the last: %d", code)
	}
}

func TestPortaLoadRefusesANonImageBeforePorta(t *testing.T) {
	f := &fakePorta{}
	srv := portaBoard(t, f.server(t).URL)
	if code, body := post(t, srv.URL+"/load", []byte("not an image")); code != 400 || f.seq != 0 {
		t.Fatalf("non-image: %d %s, porta writes %d", code, body, f.seq)
	}
}

func TestPortaNodeNeedsPortaHTTP(t *testing.T) {
	err := mountBoards(context.Background(), http.NewServeMux(), []Board{{Name: "bell", Node: "n"}}, "", t.TempDir(), "", "")
	if err == nil || !strings.Contains(err.Error(), "--porta-http") {
		t.Fatalf("no porta: %v", err)
	}
}
