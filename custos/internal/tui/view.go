// Copyright (c) 2026 Ekorau LLC

package tui

// view.go — the screens. Colours are the terminal's 16 ANSI colours only, so
// an Omarchy theme (which sets the terminal palette) carries through (D3).

import (
	"fmt"
	"slices"
	"strings"

	"github.com/charmbracelet/lipgloss"
)

var (
	dim     = lipgloss.NewStyle().Foreground(lipgloss.Color("8"))
	accent  = lipgloss.NewStyle().Foreground(lipgloss.Color("4")).Bold(true)
	warn    = lipgloss.NewStyle().Foreground(lipgloss.Color("3"))
	bad     = lipgloss.NewStyle().Foreground(lipgloss.Color("1"))
	here    = lipgloss.NewStyle().Foreground(lipgloss.Color("2")).Bold(true)
	cursorS = lipgloss.NewStyle().Reverse(true)
	box     = lipgloss.NewStyle().Border(lipgloss.RoundedBorder()).BorderForeground(lipgloss.Color("8"))
)

// pane boxes body to w×h outer cells, titled; sizes clamp so a tiny terminal
// renders rather than panics.
func pane(title, body string, w, h int) string {
	w, h = max(w-2, 1), max(h-2, 1)
	lines := strings.Split(body, "\n")
	if len(lines) > h {
		lines = lines[len(lines)-h:]
	}
	for i, l := range lines {
		lines[i] = lipgloss.NewStyle().MaxWidth(w).Render(l)
	}
	return box.Width(w).Height(h).Render(accent.Render(title) + "\n" + strings.Join(lines, "\n"))
}

func (m Model) View() string {
	w, h := max(m.w, 20), max(m.h, 6)
	var tabs string
	if m.tab == debugTab {
		tabs = cursorS.Render(" debug ") + dim.Render(" profile ")
	} else {
		tabs = dim.Render(" debug ") + cursorS.Render(" profile ")
	}
	var body string
	if m.tab == debugTab {
		body = m.debugView(w, h-3)
	} else {
		body = m.profileView(w, h-3)
	}
	clip := lipgloss.NewStyle().MaxWidth(w)
	return lipgloss.JoinVertical(lipgloss.Left, tabs, body, clip.Render(m.statusLine()), clip.Render(m.help()))
}

func (m Model) statusLine() string {
	if m.prompt != "" {
		return m.prompt + m.input + "█"
	}
	s := m.st.Run
	if m.st.Run == "stopped" && m.st.Stop != nil {
		s = fmt.Sprintf("stopped (%s) %s:%d  %s", m.st.Stop.Reason, m.st.Stop.File, m.st.Stop.Line, m.st.Stop.Method)
	}
	if m.noDebug && m.tab == debugTab {
		s = "no debug target (start custos with --debug-host or --debug-port)"
	}
	if m.inflight != "" {
		s += dim.Render("  … " + m.inflight)
	}
	if m.status != "" {
		s += "  " + bad.Render(m.status)
	}
	return s
}

func (m Model) help() string {
	if m.tab == profileTab {
		return dim.Render("r capture · tab debug · q quit")
	}
	return dim.Render("↑↓ line · b break · c continue · s step · n next · f finish · p pause · [ ] frame · : command · tab profile · q quit")
}

func (m Model) debugView(w, h int) string {
	tail := 6
	top := max(h-tail, 3)
	left := w * 3 / 5
	right := w - left
	src := pane("source", m.sourceBody(top-2), left, top)
	stack := pane("stack", m.stackBody(), right, top/2)
	locals := pane("locals", m.localsBody(), right, top-top/2)
	upper := lipgloss.JoinHorizontal(lipgloss.Top, src, lipgloss.JoinVertical(lipgloss.Left, stack, locals))
	return lipgloss.JoinVertical(lipgloss.Left, upper, pane("transcript", strings.Join(m.st.Transcript, "\n"), w, tail))
}

// sourceBody is the window of lines around the cursor, rows tall.
func (m Model) sourceBody(rows int) string {
	n := len(m.src.Lines)
	if n == 0 {
		return dim.Render("(no source)")
	}
	rows = max(rows, 1)
	first := min(max(1, m.cursor-rows/2), max(1, n-rows+1))
	var b strings.Builder
	for i := first; i <= n && i < first+rows; i++ {
		mark := " "
		if slices.Contains(m.st.Breaks, m.breakKey(i)) {
			mark = bad.Render("●")
		}
		if m.st.Stop != nil && m.st.Run == "stopped" && m.st.Stop.Line == i {
			mark = here.Render("▶")
		}
		num := fmt.Sprintf("%4d ", i)
		if !m.src.Lines[i-1].Code {
			num = dim.Render(num)
		}
		text := m.src.Lines[i-1].Text
		if i == m.cursor {
			text = cursorS.Render(text)
		}
		fmt.Fprintf(&b, "%s%s %s\n", mark, num, text)
	}
	return strings.TrimRight(b.String(), "\n")
}

func (m Model) stackBody() string {
	var b strings.Builder
	for i, f := range m.st.Frames {
		line := fmt.Sprintf("#%d %s", i, f.Where)
		if i == m.frame {
			line = cursorS.Render(line)
		}
		b.WriteString(line + "\n")
	}
	return strings.TrimRight(b.String(), "\n")
}

func (m Model) localsBody() string {
	if m.st.Locals == nil {
		return ""
	}
	var b strings.Builder
	b.WriteString(dim.Render(fmt.Sprintf("frame %d", m.st.Locals.Frame)) + "\n")
	for _, v := range m.st.Locals.Vars {
		fmt.Fprintf(&b, "%s = %s\n", v.Name, v.Value)
	}
	return strings.TrimRight(b.String(), "\n")
}

func (m Model) profileView(w, h int) string {
	if m.prof == nil {
		msg := "no capture yet — press r"
		if m.capturing {
			msg = "capturing…"
		}
		return pane("profile", dim.Render(msg), w, h)
	}
	r := m.prof.Report
	var b strings.Builder
	if r.Header != nil {
		fmt.Fprintf(&b, "%d ms  window=%s methods=%d activations=%d\n", m.prof.Ms, r.Header.Window, r.Header.Methods, r.Header.Activations)
	}
	for _, wn := range r.Warnings {
		b.WriteString(warn.Render(wn) + "\n")
	}
	for _, row := range r.Rows {
		fmt.Fprintf(&b, "%8d %5.1f%%  %s\n", row.Count, row.Pct, row.Name)
	}
	body := strings.TrimRight(b.String(), "\n")
	if lines := strings.Split(body, "\n"); len(lines) > h-2 { // keep the top of a ranking, not its tail
		body = strings.Join(lines[:max(h-2, 1)], "\n")
	}
	return pane("profile", body, w, h)
}
