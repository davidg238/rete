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

func TestStateNewerOnlyAndBtOncePerStop(t *testing.T) {
	m := New(NewClient("http://127.0.0.1:1"))
	stop := &debug.StopView{Reason: "break", File: "a.nsl", Line: 16, Off: 7920, PC: 22}
	nm, cmd := m.Update(stateMsg{st: debug.State{Seq: 9, Run: "stopped", Stop: stop}})
	m = nm.(Model)
	if m.st.Seq != 9 || m.cursor != 16 || cmd == nil || m.inflight != "bt" {
		t.Fatalf("first stop: seq=%d cursor=%d inflight=%q", m.st.Seq, m.cursor, m.inflight)
	}
	m.inflight = "" // as if bt answered
	nm, _ = m.Update(stateMsg{st: debug.State{Seq: 7, Run: "running"}})
	m = nm.(Model)
	if m.st.Seq != 9 {
		t.Fatalf("an older state was applied: %d", m.st.Seq)
	}
	nm, _ = m.Update(stateMsg{st: debug.State{Seq: 10, Run: "stopped", Stop: stop}})
	m = nm.(Model)
	if m.inflight == "bt" {
		t.Fatal("bt sent twice for one stop")
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
