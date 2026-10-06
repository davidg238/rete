// Copyright (c) 2026 Ekorau LLC

package tui

import (
	"errors"
	"strings"
	"testing"
	"time"

	tea "github.com/charmbracelet/bubbletea"

	"github.com/davidg238/rete/custos/internal/debug"
)

// custos fetches bt and locals 0 itself on every stop (AutoFetch), so a stop
// moves the cursor and sends nothing: a TUI bt would hold the person's next key.
func TestStopSendsNothingAndOnlyNewerApplies(t *testing.T) {
	m := New(NewClient("http://127.0.0.1:1"))
	stop := &debug.StopView{Reason: "break", File: "a.nsl", Line: 16, Off: 7920, PC: 22}
	nm, _ := m.Update(stateMsg{st: debug.State{Seq: 9, Run: "stopped", Stop: stop}})
	m = nm.(Model)
	if m.st.Seq != 9 || m.cursor != 16 || m.inflight != "" {
		t.Fatalf("stop: seq=%d cursor=%d inflight=%q", m.st.Seq, m.cursor, m.inflight)
	}
	nm, _ = m.Update(stateMsg{st: debug.State{Seq: 7, Run: "running"}})
	if nm.(Model).st.Seq != 9 {
		t.Fatalf("an older state was applied: %d", nm.(Model).st.Seq)
	}
}

// After an outage the custos that answers may be a new one, whose Seq restarts:
// its state is taken, the error is cleared, and the source is fetched again.
func TestReconnectTakesANewServer(t *testing.T) {
	m := New(NewClient("http://127.0.0.1:1"))
	nm, _ := m.Update(stateMsg{st: debug.State{Seq: 50, Run: "stopped", Stop: &debug.StopView{Line: 3}}})
	nm, _ = nm.(Model).Update(stateMsg{err: errors.New("connection refused")})
	nm, cmd := nm.(Model).Update(stateMsg{st: debug.State{Seq: 3, Run: "boot"}})
	m = nm.(Model)
	if m.st.Seq != 3 || m.st.Run != "boot" || m.status != "" {
		t.Fatalf("after reconnect: seq=%d run=%q status=%q", m.st.Seq, m.st.Run, m.status)
	}
	batch, _ := cmd().(tea.BatchMsg)
	if len(batch) < 2 {
		t.Fatalf("reconnect did not re-fetch the source: %d commands", len(batch))
	}
}

// Before any state has been seen, the first poll asks for after=-1, so it
// answers at once whatever the server's Seq.
func TestFirstPollAfterErrorAsksMinusOne(t *testing.T) {
	m := New(NewClient("http://x"))
	if m.after() != -1 {
		t.Fatalf("after() = %d before any state", m.after())
	}
}

func TestBreakTogglesOnTheCursorLine(t *testing.T) {
	f, srv := newFake(t)
	m := New(NewClient(srv.URL))
	m = drive(t, m, m.Init(), 5*time.Second, func(m Model) bool { return len(m.src.Lines) == 3 && m.st.Run == "boot" })
	m, cmd := press(t, m, "down", "b")
	m = drive(t, m, cmd, 5*time.Second, func(m Model) bool { return m.inflight == "" })
	f.set(debug.State{Seq: 50, Run: "boot", Breaks: []string{"a.nsl:2"}})
	m = drive(t, m, m.poll(m.st.Seq, 0), 5*time.Second, func(m Model) bool { return m.st.Seq >= 50 })
	m, cmd = press(t, m, "b")
	drive(t, m, cmd, 5*time.Second, func(m Model) bool { return m.inflight == "" })
	if got := f.cmds; len(got) != 2 || got[0] != "break 2" || got[1] != "clear 2" {
		t.Fatalf("cmds = %q", got)
	}
}

func TestBusy(t *testing.T) {
	f, srv := newFake(t)
	f.hold = make(chan struct{})
	m := New(NewClient(srv.URL))
	m, _ = press(t, m, "s")
	m, cmd := press(t, m, "n")
	if cmd != nil || m.status == "" || m.inflight != "step" {
		t.Fatalf("a second key in flight: status=%q inflight=%q", m.status, m.inflight)
	}
	close(f.hold)
}

func TestUnreachable(t *testing.T) {
	m := New(NewClient("http://127.0.0.1:1"))
	nm, cmd := m.Update(stateMsg{err: errors.New("connection refused")})
	m = nm.(Model)
	if cmd == nil || !strings.Contains(m.status, "connection refused") {
		t.Fatalf("no retry or no status: %q", m.status)
	}
	if _, isQuit := cmd().(tea.QuitMsg); isQuit {
		t.Fatal("quit on an unreachable custos")
	}
}

func TestNoDebugTarget(t *testing.T) {
	m := New(NewClient("http://x"))
	nm, cmd := m.Update(stateMsg{err: &StatusError{Code: 404}})
	m = nm.(Model)
	if !m.noDebug || cmd != nil {
		t.Fatalf("noDebug=%v cmd=%v", m.noDebug, cmd)
	}
	nm, _ = m.Update(profMsg{last: true, err: &StatusError{Code: 404}})
	if nm.(Model).status != "" {
		t.Fatal("no capture yet is not an error")
	}
}

func TestEndedNilStop(t *testing.T) {
	m := New(NewClient("http://x"))
	for _, run := range []string{"ended", "lost", "stopped"} {
		nm, _ := m.Update(stateMsg{st: debug.State{Seq: m.st.Seq + 1, Run: run}})
		m = nm.(Model)
		m.w, m.h = 100, 30
		if v := m.View(); run != "stopped" && !strings.Contains(v, run) {
			t.Fatalf("%s not shown:\n%s", run, v)
		}
	}
}
