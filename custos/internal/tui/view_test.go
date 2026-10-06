// Copyright (c) 2026 Ekorau LLC

package tui

import (
	"strings"
	"testing"

	"github.com/davidg238/rete/custos/internal/custos"
	"github.com/davidg238/rete/custos/internal/debug"
	"github.com/davidg238/rete/custos/internal/profile"
)

func stopped() Model {
	m := New(NewClient("http://x"))
	m.w, m.h = 120, 30
	m.src = custos.Source{File: "/x/a.nsl", Lines: []custos.SourceLine{{Text: "sum: xs = ("}, {Text: "total:: 0.", Code: true}, {Text: "^ total )", Code: true}}}
	m.st = debug.State{Seq: 3, Run: "stopped",
		Stop:   &debug.StopView{Reason: "break", File: "a.nsl", Line: 2, Method: "A>>sum:"},
		Frames: []debug.FrameView{{Where: "at a.nsl:2  A>>sum:"}, {Where: "at a.nsl:9  A>>main:args:"}},
		Locals: &debug.LocalsView{Frame: 0, Vars: []debug.Var{{Name: "total", Value: "0"}}},
		Breaks: []string{"a.nsl:2", "a.nsl:3"}, Transcript: []string{"mean 4"}}
	m.cursor = 2
	return m
}

func TestDebugView(t *testing.T) {
	v := stopped().View()
	for _, want := range []string{"total:: 0.", "▶", "●", "A>>main:args:", "total = 0", "mean 4", "stopped (break) a.nsl:2"} {
		if !strings.Contains(v, want) {
			t.Errorf("missing %q in\n%s", want, v)
		}
	}
}

func TestProfileView(t *testing.T) {
	m := stopped()
	m.tab = profileTab
	m.prof = &custos.Capture{Ms: 200, Report: profile.Report{
		Header: &profile.Header{Window: "window", Methods: 2, Activations: 10},
		Rows:   []profile.Ranked{{Name: "A>>main:args:", Count: 7, Pct: 70}}}}
	v := m.View()
	for _, want := range []string{"A>>main:args:", "70.0%", "activations=10"} {
		if !strings.Contains(v, want) {
			t.Errorf("missing %q in\n%s", want, v)
		}
	}
}

func TestTinyTerminal(t *testing.T) {
	for _, wh := range [][2]int{{0, 0}, {10, 3}, {40, 8}} {
		m := stopped()
		m.w, m.h = wh[0], wh[1]
		_ = m.View() // must not panic
	}
}
