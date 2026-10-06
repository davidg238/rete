// Copyright (c) 2026 Ekorau LLC

package tui

import (
	"testing"

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
