// Copyright (c) 2026 Ekorau LLC

package tui

import (
	"fmt"
	"strings"
	"testing"

	"github.com/charmbracelet/lipgloss"

	"github.com/davidg238/rete/custos/internal/custos"
	"github.com/davidg238/rete/custos/internal/debug"
	"github.com/davidg238/rete/custos/internal/load"
	"github.com/davidg238/rete/custos/internal/profile"
)

func stopped() Model {
	m := New(NewClient("http://x"))
	m.w, m.h = 120, 30
	m.src = custos.Source{File: "/x/a.nsl", Lines: []custos.SourceLine{{Text: "sum: xs = ("}, {Text: "total:: 0.", Code: true}, {Text: "^ total )", Code: true}}}
	m.st = debug.State{Seq: 3, Run: "stopped",
		Stop:   &debug.StopView{Reason: "break", File: "a.nsl", Line: 2, Method: "A>>sum:"},
		Frames: []debug.FrameView{{Where: "at a.nsl:2  A>>sum:", File: "a.nsl", Line: 2, Method: "A>>sum:"}, {Where: "at a.nsl:9  A>>main:args:", File: "a.nsl", Line: 9, Method: "A>>main:args:"}},
		Locals: &debug.LocalsView{Frame: 0, Vars: []debug.Var{{Name: "total", Value: "0"}}},
		Breaks: []string{"a.nsl:2", "a.nsl:3"}, Transcript: []string{"mean 4"}}
	m.cursor = 2
	return m
}

func TestDebugView(t *testing.T) {
	v := stopped().View()
	for _, want := range []string{"total:: 0.", "▶", "●", "#1 A>>main:args:  a.nsl:9", "total = 0", "mean 4", "stopped (break) a.nsl:2"} {
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

func TestLoadView(t *testing.T) {
	m := stopped()
	m.tab = loadTab
	m.load = &custos.Load{Result: load.Result{Image: "A.oimg", Size: 1234, SHA256: "c4c31f03ab", BuildID: "host"}, Debug: "restarted"}
	v := m.View()
	for _, want := range []string{"A.oimg (1234 B)", "c4c31f03ab", "host VM", "restarted", " load "} {
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

// The view never renders taller than the terminal: Bubble Tea keeps the last
// h lines, so an overflow drops the tab bar and the panes' top borders.
func TestViewFitsTheTerminal(t *testing.T) {
	m := stopped()
	m.src.Lines = nil
	for i := 0; i < 80; i++ {
		m.src.Lines = append(m.src.Lines, custos.SourceLine{Text: fmt.Sprintf("line %d", i), Code: true})
		m.st.Transcript = append(m.st.Transcript, fmt.Sprintf("out %d", i))
		m.st.Frames = append(m.st.Frames, debug.FrameView{Where: fmt.Sprintf("at a.nsl:%d", i)})
	}
	for _, wh := range [][2]int{{100, 24}, {140, 40}} {
		m.w, m.h = wh[0], wh[1]
		for _, tb := range []tab{debugTab, profileTab, loadTab} {
			m.tab = tb
			if got := lipgloss.Height(m.View()); got > m.h {
				t.Errorf("%dx%d tab %d: %d rows", m.w, m.h, tb, got)
			}
		}
	}
}
