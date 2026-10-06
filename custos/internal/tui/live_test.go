// Copyright (c) 2026 Ekorau LLC

package tui

import (
	"os"
	"slices"
	"strconv"
	"strings"
	"testing"
	"time"

	tea "github.com/charmbracelet/bubbletea"
)

// TestLiveDebug drives the TUI's model, no terminal, against a custos the
// suite's custos-debug gate started over --debug-host (rete#341).
func TestLiveDebug(t *testing.T) {
	url, line := os.Getenv("CUSTOS_TUI_URL"), os.Getenv("CUSTOS_TUI_BREAK")
	if url == "" {
		t.Skip("CUSTOS_TUI_URL unset: run by test_custos_debug.py")
	}
	n, _ := strconv.Atoi(line)
	m := New(NewClient(url))
	m.w, m.h = 140, 40
	m = drive(t, m, m.Init(), 30*time.Second, func(m Model) bool { return len(m.src.Lines) > 0 && m.st.Run == "boot" })
	keys := slices.Repeat([]string{"down"}, n-1)
	m, cmd := press(t, m, append(keys, "b")...)
	m = drive(t, m, cmd, 30*time.Second, func(m Model) bool { return slices.Contains(m.st.Breaks, m.breakKey(n)) && m.inflight == "" })
	m, cmd = press(t, m, "c")
	m = drive(t, m, tea.Batch(cmd, m.poll(m.st.Seq, 0)), 60*time.Second, func(m Model) bool {
		return m.st.Run == "stopped" && m.st.Stop != nil && m.st.Stop.Line == n && len(m.st.Frames) > 0 && m.st.Locals != nil && m.inflight == ""
	})
	frames := len(m.st.Frames)
	v := m.View()
	for _, want := range []string{"▶", "stopped (break)", "main:args:"} {
		if !strings.Contains(v, want) {
			t.Fatalf("view at the stop lacks %q:\n%s", want, v)
		}
	}
	m, cmd = press(t, m, "]")
	m = drive(t, m, cmd, 30*time.Second, func(m Model) bool { return m.st.Locals != nil && m.st.Locals.Frame == 1 && m.inflight == "" })
	m, cmd = press(t, m, "b") // the cursor is on the stop line: clear it
	m = drive(t, m, cmd, 30*time.Second, func(m Model) bool { return len(m.st.Breaks) == 0 && m.inflight == "" })
	m, cmd = press(t, m, "c")
	m = drive(t, m, tea.Batch(cmd, m.poll(m.st.Seq, 0)), 60*time.Second, func(m Model) bool { return m.st.Run == "ended" })
	t.Logf("live: break %d, stopped with %d frames, locals 1, cleared, ended", n, frames)
}

// TestLiveProfile captures a window through the profile tab against a custos
// the custos-profile gate started over --profile-host.
func TestLiveProfile(t *testing.T) {
	url := os.Getenv("CUSTOS_TUI_PROFILE_URL")
	if url == "" {
		t.Skip("CUSTOS_TUI_PROFILE_URL unset: run by test_custos_profile.py")
	}
	m := New(NewClient(url))
	m.w, m.h = 140, 40
	m, _ = press(t, m, "tab", "r", "backspace", "backspace", "backspace", "backspace", "2", "0", "0")
	m, cmd := press(t, m, "enter")
	m = drive(t, m, cmd, 60*time.Second, func(m Model) bool { return m.prof != nil || m.status != "" })
	if m.prof == nil {
		t.Fatalf("capture failed: %s", m.status)
	}
	if v := m.View(); !strings.Contains(v, "main:args:") {
		t.Fatalf("profile view lacks main:args::\n%s", v)
	}
	t.Logf("live: a 200 ms window through the profile tab, %d rows", len(m.prof.Report.Rows))
}
