// Copyright (c) 2026 Ekorau LLC

// Package tui is `custos tui` (rete#341): a terminal front over a serving
// custos's /debug and /profile. It is an HTTP client like the page and an
// agent (design 2026-10-06-custos-tui-design.md D1); custos owns the cable.
package tui

import (
	"errors"
	"fmt"
	"path/filepath"
	"slices"
	"strconv"
	"time"

	tea "github.com/charmbracelet/bubbletea"

	"github.com/davidg238/rete/custos/internal/custos"
	"github.com/davidg238/rete/custos/internal/debug"
)

type tab int

const (
	debugTab tab = iota
	profileTab
)

// Model is the whole TUI. Exactly one long-poll is in flight at a time; it is
// re-issued only from the stateMsg it answers.
type Model struct {
	c         *Client
	tab       tab
	w, h      int
	src       custos.Source
	st        debug.State
	cursor    int    // the source line under the cursor, 1-based
	frame     int    // the frame whose locals are asked for
	prompt    string // non-empty while the one-line input is open: ":" or "window ms: "
	input     string
	status    string // the last refusal or error; "" when the last thing worked
	inflight  string // the command posted and not yet answered
	btFor     string // the stop a bt was already sent for
	noDebug   bool   // /debug answered 404: custos serves no debug target
	prof      *custos.Capture
	ms        int // the last profile window
	capturing bool
}

type sourceMsg struct {
	src custos.Source
	err error
}
type stateMsg struct {
	st  debug.State
	err error
}
type doneMsg struct {
	cmd string
	d   custos.Done
	err error
}
type profMsg struct {
	c    custos.Capture
	err  error
	last bool // from /profile/last at start-up: a 404 there is not an error
}

// New answers the model for c; Init starts the source fetch and the long-poll.
func New(c *Client) Model { return Model{c: c, cursor: 1, ms: 3000} }

func (m Model) Init() tea.Cmd {
	c := m.c
	return tea.Batch(
		func() tea.Msg { s, err := c.Source(); return sourceMsg{s, err} },
		m.poll(-1, 0),
		func() tea.Msg { p, err := c.Last(); return profMsg{p, err, true} },
	)
}

// poll long-polls past seq, after waiting back (the back-off after an error).
func (m Model) poll(seq int, back time.Duration) tea.Cmd {
	c := m.c
	return func() tea.Msg {
		time.Sleep(back)
		st, err := c.State(seq)
		return stateMsg{st, err}
	}
}

// send posts one command; a second while one is in flight is refused here,
// as the server would refuse it with 409.
func (m Model) send(cmd string) (Model, tea.Cmd) {
	if m.inflight != "" {
		m.status = fmt.Sprintf("%q is still running", m.inflight)
		return m, nil
	}
	m.inflight = cmd
	c := m.c
	return m, func() tea.Msg { d, err := c.Do(cmd); return doneMsg{cmd, d, err} }
}

// apply takes st if it is newer, moves the cursor to a new stop, and asks for
// the stack once per stop.
func (m Model) apply(st debug.State) (Model, tea.Cmd) {
	if st.Seq <= m.st.Seq && m.st.Run != "" {
		return m, nil
	}
	m.st = st
	if st.Run != "stopped" || st.Stop == nil {
		return m, nil
	}
	key := fmt.Sprintf("%s %d %d", st.Stop.Reason, st.Stop.Off, st.Stop.PC)
	if key == m.btFor {
		return m, nil
	}
	m.btFor, m.frame = key, 0
	if st.Stop.Line > 0 {
		m.cursor = st.Stop.Line
	}
	return m.send("bt")
}

func is404(err error) bool {
	var se *StatusError
	return errors.As(err, &se) && se.Code == 404
}

func (m Model) Update(msg tea.Msg) (tea.Model, tea.Cmd) {
	switch msg := msg.(type) {
	case tea.WindowSizeMsg:
		m.w, m.h = msg.Width, msg.Height
	case sourceMsg:
		if is404(msg.err) {
			m.noDebug = true
		} else if msg.err != nil {
			m.status = "source: " + msg.err.Error()
		} else {
			m.src = msg.src
		}
	case stateMsg:
		if is404(msg.err) {
			m.noDebug = true
			return m, nil // no debug target: stop polling
		}
		if msg.err != nil {
			m.status = "custos: " + msg.err.Error()
			return m, m.poll(m.st.Seq, time.Second)
		}
		if m.status != "" && m.inflight == "" && m.st.Run == "" {
			m.status = "" // reconnected
		}
		var cmd tea.Cmd
		m, cmd = m.apply(msg.st)
		return m, tea.Batch(cmd, m.poll(m.st.Seq, 0))
	case doneMsg:
		m.inflight = ""
		switch {
		case msg.err != nil:
			m.status = msg.cmd + ": " + msg.err.Error()
			return m, nil
		case !msg.d.OK:
			m.status = msg.cmd + ": " + msg.d.Error
		default:
			m.status = ""
		}
		return m.apply(msg.d.State)
	case profMsg:
		m.capturing = false
		if msg.err != nil {
			if !(msg.last && is404(msg.err)) {
				m.status = "profile: " + msg.err.Error()
			}
			return m, nil
		}
		m.prof, m.status = &msg.c, ""
	case tea.KeyMsg:
		return m.key(msg)
	}
	return m, nil
}

// breakKey is how State.Breaks names line n of the app's file.
func (m Model) breakKey(n int) string {
	return fmt.Sprintf("%s:%d", filepath.Base(m.src.File), n)
}

func (m Model) key(k tea.KeyMsg) (tea.Model, tea.Cmd) {
	s := k.String()
	if m.prompt != "" {
		switch k.Type {
		case tea.KeyEsc:
			m.prompt, m.input = "", ""
		case tea.KeyEnter:
			line, prompt := m.input, m.prompt
			m.prompt, m.input = "", ""
			if prompt == ":" {
				return m.send(line)
			}
			ms, err := strconv.Atoi(line)
			if err != nil || ms <= 0 {
				m.status = "a window is a positive number of ms"
				return m, nil
			}
			m.ms = ms
			return m.capture()
		case tea.KeyBackspace:
			if m.input != "" {
				m.input = m.input[:len(m.input)-1]
			}
		case tea.KeyRunes:
			m.input += string(k.Runes)
		case tea.KeySpace:
			m.input += " "
		}
		return m, nil
	}
	switch s {
	case "q", "ctrl+c":
		return m, tea.Quit
	case "tab":
		m.tab = 1 - m.tab
		return m, nil
	}
	if m.tab == profileTab {
		if s == "r" {
			m.prompt, m.input = "window ms: ", strconv.Itoa(m.ms)
		}
		return m, nil
	}
	switch s {
	case "up", "k":
		m.cursor = max(1, m.cursor-1)
	case "down", "j":
		m.cursor = min(max(1, len(m.src.Lines)), m.cursor+1)
	case "b":
		verb := "break"
		if slices.Contains(m.st.Breaks, m.breakKey(m.cursor)) {
			verb = "clear"
		}
		return m.send(fmt.Sprintf("%s %d", verb, m.cursor))
	case "c":
		return m.send("continue")
	case "s":
		return m.send("step")
	case "n":
		return m.send("next")
	case "f":
		return m.send("finish")
	case "p":
		return m.send("pause")
	case "[", "]":
		if s == "[" {
			m.frame = max(0, m.frame-1)
		} else {
			m.frame = min(max(0, len(m.st.Frames)-1), m.frame+1)
		}
		return m.send(fmt.Sprintf("locals %d", m.frame))
	case ":":
		m.prompt = ":"
	}
	return m, nil
}

func (m Model) capture() (Model, tea.Cmd) {
	if m.capturing {
		m.status = "a capture is still running"
		return m, nil
	}
	m.capturing = true
	c, ms := m.c, m.ms
	return m, func() tea.Msg { p, err := c.Capture(ms); return profMsg{p, err, false} }
}

func (m Model) View() string { return "" } // replaced by view.go (Task 4)
