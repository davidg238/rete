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
// suite's custos-debug gate started on a --host board (rete#341).
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
// the custos-profile gate started on a --host board.
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
	ranked := false
	for _, r := range m.prof.Report.Rows {
		ranked = ranked || strings.HasSuffix(r.Name, ">>main:args:")
	}
	if !ranked || len(m.prof.Report.Rows) == 0 {
		t.Fatalf("capture lacks main:args:: %+v", m.prof.Report.Rows)
	}
	if v := m.View(); !strings.Contains(v, m.prof.Report.Rows[0].Name) {
		t.Fatalf("profile view lacks the top row %q:\n%s", m.prof.Report.Rows[0].Name, v)
	}
	t.Logf("live: a 200 ms window through the profile tab, %d rows", len(m.prof.Report.Rows))
}

// TestLiveLoad loads an .oimg through the load tab against a custos the
// custos-load gate started on --host boards (rete#367).
func TestLiveLoad(t *testing.T) {
	url, image, sha := os.Getenv("CUSTOS_TUI_LOAD_URL"), os.Getenv("CUSTOS_TUI_LOAD_IMAGE"), os.Getenv("CUSTOS_TUI_LOAD_SHA")
	if url == "" {
		t.Skip("CUSTOS_TUI_LOAD_URL unset: run by test_custos_load.py")
	}
	m := New(NewClient(url))
	m.w, m.h = 140, 40
	m, _ = press(t, m, "tab", "tab", "r", image) // the path arrives as one paste of runes
	m, cmd := press(t, m, "enter")
	m = drive(t, m, cmd, 60*time.Second, func(m Model) bool { return m.load != nil || m.status != "" })
	if m.load == nil {
		t.Fatalf("load failed: %s", m.status)
	}
	if m.load.Result.SHA256 != sha {
		t.Fatalf("loaded sha256 %s, want %s", m.load.Result.SHA256, sha)
	}
	if v := m.View(); !strings.Contains(v, sha) {
		t.Fatalf("load view lacks the sha:\n%s", v)
	}
	t.Logf("live: %s through the load tab, sha256 %.12s", m.load.Result.Image, sha)
}
