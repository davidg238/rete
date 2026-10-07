// Copyright (c) 2026 Ekorau LLC

package custos

import (
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/davidg238/rete/custos/internal/debug"
)

// stubVM answers the debugger's verbs over the fixture image; a send waits for
// hold (when set) before it answers, so a command can be kept in flight.
type stubVM struct {
	s    *debug.Session
	hold chan struct{}
}

func (v *stubVM) Send(line string) error {
	var reply string
	switch {
	case line == "dbg:pause":
		reply = "dbg:ok pause"
	case strings.HasPrefix(line, "dbg:break "):
		reply = "dbg:ok break"
	case strings.HasPrefix(line, "dbg:clear "):
		reply = "dbg:ok clear"
	case line == "dbg:backtrace":
		reply = "dbg:frames 26616@1 3992@10"
	case strings.HasPrefix(line, "dbg:inspect"):
		reply = "dbg:stack off=1 r0=a Fixture r1=7"
	}
	go func() {
		if v.hold != nil {
			<-v.hold
		}
		if reply != "" {
			v.s.OnLine(reply)
		}
	}()
	return nil
}

func debugServer(t *testing.T, hold chan struct{}) (*httptest.Server, *debug.Session) {
	t.Helper()
	sm, err := debug.LoadSourceMap(fixtureMap(t), "")
	if err != nil {
		t.Fatal(err)
	}
	s := debug.NewSession(sm, io.Discard)
	s.AutoFetch = true
	s.Attach(&stubVM{s, hold})
	mux := http.NewServeMux()
	mountDebug(mux, s, &sync.Mutex{})
	srv := httptest.NewServer(mux)
	t.Cleanup(srv.Close)
	return srv, s
}

func getState(t *testing.T, url string) debug.State {
	t.Helper()
	resp, err := http.Get(url)
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	var st debug.State
	if err := json.NewDecoder(resp.Body).Decode(&st); err != nil {
		t.Fatal(err)
	}
	return st
}

func do(t *testing.T, srv *httptest.Server, cmd string) *http.Response {
	t.Helper()
	resp, err := http.Post(srv.URL+"/debug/do", "text/plain", strings.NewReader(cmd))
	if err != nil {
		t.Fatal(err)
	}
	return resp
}

func TestTheDebugSourceMarksTheLinesWithCode(t *testing.T) {
	srv, _ := debugServer(t, nil)
	resp, err := http.Get(srv.URL + "/debug/source")
	if err != nil {
		t.Fatal(err)
	}
	var src Source
	json.NewDecoder(resp.Body).Decode(&src)
	resp.Body.Close()
	if src.File != "Fixture.nsl" || len(src.Lines) < 24 {
		t.Fatalf("file %q, %d lines", src.File, len(src.Lines))
	}
	// main:args: starts statements on 19-24; the block's line is 21.
	for _, n := range []int{19, 21, 24} {
		if !src.Lines[n-1].Code {
			t.Errorf("line %d (%q) is not marked as code", n, src.Lines[n-1].Text)
		}
	}
	if src.Lines[0].Code {
		t.Errorf("line 1 (%q) is marked as code", src.Lines[0].Text)
	}
}

func doAnswer(t *testing.T, srv *httptest.Server, cmd string) (int, Done) {
	t.Helper()
	resp := do(t, srv, cmd)
	defer resp.Body.Close()
	var d Done
	if err := json.NewDecoder(resp.Body).Decode(&d); err != nil {
		t.Fatal(err)
	}
	return resp.StatusCode, d
}

func TestABreakByCommandIsInTheState(t *testing.T) {
	srv, _ := debugServer(t, nil)
	code, d := doAnswer(t, srv, "break 20")
	if code != http.StatusOK || !d.OK || d.Error != "" || strings.Join(d.State.Breaks, ",") != "Fixture.nsl:20" {
		t.Fatalf("status %d, answer %+v", code, d)
	}
}

// rete#362: a failed command is a 200 whose OK is false, with the reason.
func TestAFailedCommandSaysSo(t *testing.T) {
	srv, _ := debugServer(t, nil)
	code, d := doAnswer(t, srv, "frobnicate")
	if code != http.StatusOK || d.OK || d.Error != "unknown: frobnicate (type help)" {
		t.Fatalf("status %d, answer %+v", code, d)
	}
}

func TestTheLongPollWakesOnAStop(t *testing.T) {
	srv, s := debugServer(t, nil)
	seq := s.State(-1, 0).Seq
	got := make(chan debug.State, 1)
	go func() { got <- getState(t, fmt.Sprintf("%s/debug/state?after=%d", srv.URL, seq)) }()
	time.Sleep(50 * time.Millisecond)
	s.OnLine("dbg:paused break 26616 1")
	select {
	case st := <-got:
		if st.Run != "stopped" || st.Stop == nil || st.Stop.Line != 21 {
			t.Fatalf("woke with %+v", st)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("the long-poll did not wake on a stop")
	}
}

func TestOneCommandAtATime(t *testing.T) {
	hold := make(chan struct{})
	srv, _ := debugServer(t, hold)
	first := make(chan int, 1)
	go func() {
		resp := do(t, srv, "break 20")
		resp.Body.Close()
		first <- resp.StatusCode
	}()
	time.Sleep(100 * time.Millisecond)
	resp := do(t, srv, "break 22")
	resp.Body.Close()
	close(hold)
	if resp.StatusCode != http.StatusConflict {
		t.Errorf("a second command in flight answered %d", resp.StatusCode)
	}
	if code := <-first; code != http.StatusOK {
		t.Errorf("the first command answered %d", code)
	}
}

func TestDebugPortConfigIsChecked(t *testing.T) {
	if _, err := Handler(Config{DebugPort: "/dev/null"}); err == nil || !strings.Contains(err.Error(), "--debug-map") {
		t.Errorf("a debug port without a map: %v", err)
	}
	_, err := Handler(Config{DebugPort: "/dev/ttyX", DebugMap: fixtureMap(t), ProfilePort: "/dev/ttyX", ProfileMap: fixtureMap(t)})
	if err == nil || !strings.Contains(err.Error(), "one cable") {
		t.Errorf("one port for both pages: %v", err)
	}
}

// rete#362: a host VM is the second cable, and exactly one is held.
func TestDebugHostConfigIsChecked(t *testing.T) {
	m := fixtureMap(t)
	cases := map[string]Config{
		"one of":       {DebugPort: "/dev/ttyX", DebugHost: "/bin/true", DebugMap: m},
		"--debug-map":  {DebugHost: "/bin/true"},
		"--debug-host": {DebugHost: "/nonexistent/oevm_img", DebugMap: m},
	}
	for want, cfg := range cases {
		if _, err := Handler(cfg); err == nil || !strings.Contains(err.Error(), want) {
			t.Errorf("%+v: %v, want an error naming %q", cfg, err, want)
		}
	}
	wrong := filepath.Join(t.TempDir(), "wrong.oimg")
	os.WriteFile(wrong, []byte("not the image the map describes"), 0o644)
	if _, err := Handler(Config{DebugHost: "/bin/true " + wrong, DebugMap: m}); err == nil {
		t.Errorf("an image the map does not describe was accepted")
	}
}
