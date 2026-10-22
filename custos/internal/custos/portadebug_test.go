// Copyright (c) 2026 Ekorau LLC

package custos

import (
	"context"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"reflect"
	"strings"
	"testing"
	"time"

	"github.com/davidg238/rete/custos/internal/debug"
	"github.com/davidg238/rete/protocol/cval"
)

// portaDebugBoard is a custos serving node n1 with the fixture's map against f,
// with dbg/resp read every 10 ms and the compiler stubbed: a patch of method
// `V>>sel` compiles to `oevm-patch 1 vtable V selector sel <body>`, the body
// the edited source.
func portaDebugBoard(t *testing.T, f *fakePorta) (*httptest.Server, *debug.SourceMap) {
	t.Helper()
	every, compile := respEvery, compilePatch
	respEvery = 10 * time.Millisecond
	compilePatch = func(_, _ string, edited []byte, method string, blob bool) (string, error) {
		if !blob {
			t.Errorf("a porta patch compiled for the cable")
		}
		v, sel, _ := strings.Cut(method, ">>")
		body := "same"
		if !strings.Contains(string(edited), "Fixture") { // the unedited source holds its class name
			body = string(edited)
		}
		return fmt.Sprintf("oevm-patch 1 vtable %s selector %s %s\n", v, sel, body), nil
	}
	t.Cleanup(func() { respEvery, compilePatch = every, compile })
	ps := f.server(t)
	mux := http.NewServeMux()
	ctx, cancel := context.WithCancel(context.Background())
	t.Cleanup(cancel)
	if err := mountBoards(ctx, mux, []Board{{Name: "bell", Node: "n1", Map: fixtureMap(t)}}, "", t.TempDir(), ps.URL, ""); err != nil {
		t.Fatal(err)
	}
	srv := httptest.NewServer(mux)
	t.Cleanup(srv.Close)
	sm, err := debug.LoadSourceMap(fixtureMap(t), "")
	if err != nil {
		t.Fatal(err)
	}
	return srv, sm
}

// theCell decodes the Debug cell custos wrote for n1.
func (f *fakePorta) theCell(t *testing.T) debugCell {
	t.Helper()
	f.mu.Lock()
	b := f.cells["n1/"+debugCellPath]
	f.mu.Unlock()
	v, err := cval.Decode(b)
	if err != nil {
		t.Fatalf("%s: %v", debugCellPath, err)
	}
	c, err := parseDebugCell(v)
	if err != nil {
		t.Fatal(err)
	}
	return c
}

func (f *fakePorta) put(t *testing.T, path string, v any) {
	t.Helper()
	b, err := cval.Encode(v)
	if err != nil {
		t.Fatal(err)
	}
	f.mu.Lock()
	f.cells["n1/"+path] = b
	f.mu.Unlock()
}

func send(t *testing.T, srv *httptest.Server, method, path, body string) Done {
	t.Helper()
	req, _ := http.NewRequest(method, srv.URL+"/board/bell"+path, strings.NewReader(body))
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	var d Done
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("%s %s: status %d", method, path, resp.StatusCode)
	}
	if err := json.NewDecoder(resp.Body).Decode(&d); err != nil {
		t.Fatal(err)
	}
	return d
}

func TestTheDebugCellRoundTrips(t *testing.T) {
	for _, c := range []debugCell{
		{sha: []byte{1, 2}, breaks: [][2]int64{{4096, 0}, {21640, 3}}},
		{sha: []byte{1, 2}, patches: []byte{9, 9}, size: 300},
	} {
		enc, err := cval.Encode(c.record())
		if err != nil {
			t.Fatal(err)
		}
		v, _ := cval.Decode(enc)
		got, err := parseDebugCell(v)
		if err != nil || !reflect.DeepEqual(got, c) {
			t.Fatalf("%+v came back %+v (%v)", c, got, err)
		}
	}
}

// break and clear rewrite the cell's breaks for the map's program, answer
// Pending at the write, and keep the patch set the cell names.
func TestAPortaBreakRewritesTheCell(t *testing.T) {
	f := &fakePorta{}
	srv, sm := portaDebugBoard(t, f)
	sha, _ := hex.DecodeString(sm.SHA256())
	f.put(t, debugCellPath, debugCell{sha: sha, patches: []byte{7}, size: 10}.record())
	bp, err := sm.Resolve("20")
	if err != nil {
		t.Fatal(err)
	}
	d := send(t, srv, "POST", "/debug/do", "break 20")
	if !d.OK || !d.Pending || d.Seq == 0 || strings.Join(d.State.Breaks, ",") != "Fixture.nsl:20" {
		t.Fatalf("break: %+v", d)
	}
	c := f.theCell(t)
	if !reflect.DeepEqual(c.breaks, [][2]int64{{int64(bp.Off), int64(bp.PC)}}) || !reflect.DeepEqual(c.patches, []byte{7}) {
		t.Fatalf("cell after break: %+v", c)
	}
	d = send(t, srv, "POST", "/debug/do", "clear 20")
	if c := f.theCell(t); len(c.breaks) != 0 || !d.Pending || len(d.State.Breaks) != 0 || c.patches == nil {
		t.Fatalf("cell after clear: %+v, answer %+v", c, d)
	}
}

// A cell naming another image is the empty set: its breaks and patches are not
// carried into this program's.
func TestACellForAnotherImageStartsEmpty(t *testing.T) {
	f := &fakePorta{}
	srv, sm := portaDebugBoard(t, f)
	f.put(t, debugCellPath, debugCell{sha: []byte{0xee}, breaks: [][2]int64{{1, 1}}, patches: []byte{7}, size: 1}.record())
	send(t, srv, "POST", "/debug/do", "break 20")
	c := f.theCell(t)
	if hex.EncodeToString(c.sha) != sm.SHA256() || len(c.breaks) != 1 || c.patches != nil {
		t.Fatalf("cell %+v", c)
	}
}

// Any other command is one dbg/req line, Pending; the node's answer comes back
// from dbg/resp and the state shows it in source terms. What dbg/resp held
// before custos started is not fed.
func TestAPortaCommandIsALineAndItsAnswerArrives(t *testing.T) {
	f := &fakePorta{}
	f.says("dbg:paused break 1 1") // an old answer, before custos
	srv, _ := portaDebugBoard(t, f)
	d := send(t, srv, "POST", "/debug/do", "bt")
	f.mu.Lock()
	line, _ := cval.Decode(f.cells["n1/dbg/req"])
	f.mu.Unlock()
	if !d.Pending || line != "dbg:backtrace" {
		t.Fatalf("answer %+v, dbg/req %v", d, line)
	}
	time.Sleep(50 * time.Millisecond)
	if st := getState(t, srv.URL+"/board/bell/debug/state"); st.Stop != nil {
		t.Fatalf("an answer from before custos started was fed: %+v", st.Stop)
	}
	f.says("dbg:paused break 4096 10")
	f.says("dbg:frames 21640@1 4096@10")
	deadline := time.Now().Add(2 * time.Second)
	for {
		st := getState(t, srv.URL+"/board/bell/debug/state")
		if st.Stop != nil && st.Stop.Off == 4096 && len(st.Frames) == 2 {
			break
		}
		if time.Now().After(deadline) {
			t.Fatalf("state %+v", st)
		}
		time.Sleep(10 * time.Millisecond)
	}
	if d := send(t, srv, "POST", "/debug/do", "frobnicate"); d.OK || d.Pending {
		t.Fatalf("an unknown word: %+v", d)
	}
}

// The patch set lives in porta: a patch adds the method's line, a second patch
// of the same method replaces it, a DELETE takes it out, and a 17th method is
// refused with nothing written.
func TestThePatchSetMerges(t *testing.T) {
	f := &fakePorta{}
	srv, _ := portaDebugBoard(t, f)
	set := func() []string {
		c := f.theCell(t)
		if c.patches == nil {
			return nil
		}
		f.mu.Lock()
		b := f.blobs[hex.EncodeToString(c.patches)]
		f.mu.Unlock()
		if int64(len(b)) != c.size {
			t.Fatalf("BlobRef size %d, blob %d", c.size, len(b))
		}
		return strings.Split(strings.TrimSpace(string(b)), "\n")
	}
	if d := send(t, srv, "POST", "/debug/patch?method=A>>a", "one"); !d.OK || !d.Pending {
		t.Fatalf("patch: %+v", d)
	}
	send(t, srv, "POST", "/debug/patch?method=B>>b", "two")
	send(t, srv, "POST", "/debug/patch?method=A>>a", "three")
	if got := strings.Join(set(), " | "); got != "oevm-patch 1 vtable B selector b two | oevm-patch 1 vtable A selector a three" {
		t.Fatalf("set %q", got)
	}
	if d := send(t, srv, "DELETE", "/debug/patch?method=B>>b", ""); !d.OK || !d.Pending {
		t.Fatalf("delete: %+v", d)
	}
	if got := strings.Join(set(), " | "); got != "oevm-patch 1 vtable A selector a three" {
		t.Fatalf("set after delete %q", got)
	}
	if d := send(t, srv, "DELETE", "/debug/patch?method=B>>b", ""); d.OK {
		t.Fatalf("a delete of no patch: %+v", d)
	}
	send(t, srv, "DELETE", "/debug/patch?method=A>>a", "")
	if c := f.theCell(t); c.patches != nil {
		t.Fatalf("an empty set still names a blob: %+v", c)
	}
	for i := 0; i < patchSetMax; i++ {
		send(t, srv, "POST", fmt.Sprintf("/debug/patch?method=M>>m%d", i), "x")
	}
	before := f.theCell(t)
	if d := send(t, srv, "POST", "/debug/patch?method=M>>m99", "x"); d.OK || !strings.Contains(d.Error, "16") {
		t.Fatalf("a 17th method: %+v", d)
	}
	if !reflect.DeepEqual(f.theCell(t), before) {
		t.Fatal("a refused patch rewrote the cell")
	}
}

// A porta load is a new image: the cell is cleared to its sha, and the session
// follows the map when it describes the pushed image.
func TestAPortaLoadClearsTheCell(t *testing.T) {
	f := &fakePorta{}
	srv, sm := portaDebugBoard(t, f)
	send(t, srv, "POST", "/debug/do", "break 20")
	send(t, srv, "POST", "/debug/patch?method=A>>a", "one")
	code, body := post(t, srv.URL+"/board/bell/load?name=Fixture.oimg", fixtureImage(t))
	var l Load
	if code != 200 || json.Unmarshal([]byte(body), &l) != nil || l.Debug != "restarted" {
		t.Fatalf("POST /load: %d %s", code, body)
	}
	c := f.theCell(t)
	if hex.EncodeToString(c.sha) != sm.SHA256() || len(c.breaks) != 0 || c.patches != nil {
		t.Fatalf("cell after load %+v", c)
	}
	if st := getState(t, srv.URL+"/board/bell/debug/state"); len(st.Breaks) != 0 {
		t.Fatalf("state after load shows breaks %v", st.Breaks)
	}
}
