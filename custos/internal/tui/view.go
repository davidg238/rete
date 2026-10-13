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
	if len(lines) > h-1 { // the title takes a row
		lines = lines[len(lines)-max(h-1, 0):]
	}
	for i, l := range lines {
		lines[i] = lipgloss.NewStyle().MaxWidth(w).Render(l)
	}
	return box.Width(w).Height(h).Render(accent.Render(title) + "\n" + strings.Join(lines, "\n"))
}

func (m Model) View() string {
	w, h := max(m.w, 20), max(m.h, 6)
	var bar string
	for t, name := range []string{" debug ", " profile ", " load ", " build "} {
		if tab(t) == m.tab {
			bar += cursorS.Render(name)
		} else {
			bar += dim.Render(name)
		}
	}
	var body string
	switch m.tab {
	case debugTab:
		body = m.debugView(w, h-3)
	case profileTab:
		body = m.profileView(w, h-3)
	case loadTab:
		body = m.loadView(w, h-3)
	default:
		body = m.buildView(w, h-3)
	}
	clip := lipgloss.NewStyle().MaxWidth(w)
	return lipgloss.JoinVertical(lipgloss.Left, bar, body, clip.Render(m.statusLine()), clip.Render(m.help()))
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
		s = "no debug target (give the board a --map, or pick one with --board)"
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
	switch m.tab {
	case profileTab:
		return dim.Render("r capture · tab load · q quit")
	case loadTab:
		return dim.Render("r load an .oimg · tab build · q quit")
	case buildTab:
		return dim.Render("b build an .nsl · x run it (host) · l load the last build · tab debug · q quit")
	}
	return dim.Render("↑↓ line · b break · c continue · s step · n next · f finish · p pause · R restart · P patch · [ ] frame · : command · tab profile · q quit")
}

func (m Model) debugView(w, h int) string {
	tail := 6
	top := max(h-tail, 3)
	left := w * 3 / 5
	right := w - left
	src := pane("source", m.sourceBody(top-3), left, top)
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
		line := fmt.Sprintf("#%d %s  %s:%d", i, f.Method, f.File, f.Line) // the method first: a narrow pane clips the tail
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

func (m Model) loadView(w, h int) string {
	if m.load == nil {
		msg := "no load yet — press r and give an .oimg path"
		if m.loading {
			msg = "loading…"
		}
		return pane("load", dim.Render(msg), w, h)
	}
	if p := m.load.Push; p != nil {
		runs, conv := p.Running, "not yet: the node has not asked"
		if runs == "" {
			runs = "nothing published yet"
		}
		if p.Converged {
			conv = "yes"
		}
		body := fmt.Sprintf("image           %s (%d B)\nsha256 (signed) %s\ngoal seq        %d\nthe node runs   %s\nconverged       %s",
			p.Image, p.Size, p.SHA256, p.Seq, runs, conv)
		return pane("load", body, w, h)
	}
	r := m.load.Result
	where := "host VM"
	if r.Offset != 0 {
		where = fmt.Sprintf("0x%x", r.Offset)
	}
	body := fmt.Sprintf("image           %s (%d B)\nprogram sha256  %s\nfirmware        %s\nwritten at      %s",
		r.Image, r.Size, r.SHA256, r.BuildID, where)
	if m.load.Debug != "" {
		body += "\ndebug page      " + m.load.Debug
	}
	if m.loading {
		body += "\n" + dim.Render("loading…")
	}
	return pane("load", body, w, h)
}

func (m Model) buildView(w, h int) string {
	var body string
	switch {
	case m.ran != nil:
		end := fmt.Sprintf("exit %d", m.ran.Exit)
		if m.ran.TimedOut {
			end = "stopped: it outlived the run time"
		}
		body = fmt.Sprintf("ran %s on %s · sha256 %.12s · %s\n\n%s",
			m.ran.Image, m.ran.Board, m.ran.SHA256, end, strings.Join(m.ran.Output, "\n"))
	case m.built != nil:
		body = fmt.Sprintf("image    %s (%d B)\nboard    %s\nsha256   %s\n\n%s",
			m.built.Image, m.built.Size, m.built.Board, m.built.SHA256,
			dim.Render("l loads it onto the board; the load tab shows the board's proof"))
	default:
		body = dim.Render("nothing built yet — press b and give an .nsl path (x runs it on a host board)")
	}
	if m.building {
		body += "\n" + dim.Render("working…")
	}
	return pane("build", body, w, h)
}
