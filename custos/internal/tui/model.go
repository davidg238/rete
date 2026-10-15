// Copyright (c) 2026 Ekorau LLC

// Package tui is `custos tui` (rete#341): a terminal front over a serving
// custos's /debug, /profile and /load (rete#367). It is an HTTP client like the page and an
// agent (design 2026-10-06-custos-tui-design.md D1); custos owns the cable.
package tui

import (
	"errors"
	"fmt"
	"path/filepath"
	"slices"
	"strconv"
	"strings"
	"time"

	tea "github.com/charmbracelet/bubbletea"

	"github.com/davidg238/rete/custos/internal/custos"
	"github.com/davidg238/rete/custos/internal/debug"
)

type tab int

const (
	debugTab tab = iota
	profileTab
	loadTab
	buildTab
	tabs // how many
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
	prompt    string // non-empty while the one-line input is open: ":", "window ms: ", "image: ", "patch: ", "build: " or "run: "
	input     string
	status    string // the last refusal or error; "" when the last thing worked
	inflight  string // the command posted and not yet answered
	lost      bool   // the last poll failed: the next answer may be a new custos
	noDebug   bool   // /debug answered 404: custos serves no debug target
	prof      *custos.Capture
	ms        int // the last profile window
	capturing bool
	load      *custos.Load
	image     string // the last .oimg path asked for
	loading   bool
	built     *custos.Built
	ran       *custos.Ran
	nsl       string // the last .nsl path asked for: build, run, patch
	building  bool
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
type loadMsg struct {
	l    custos.Load
	err  error
	last bool // from /load/last at start-up: a 404 there is not an error
}
type builtMsg struct {
	b   custos.Built
	err error
}
type ranMsg struct {
	r   custos.Ran
	err error
}
type profMsg struct {
	c    custos.Capture
	err  error
	last bool // from /profile/last at start-up: a 404 there is not an error
}

// New answers the model for c; Init starts the source fetch and the long-poll.
func New(c *Client) Model { return Model{c: c, cursor: 1, ms: 3000} }

func (m Model) Init() tea.Cmd { return tea.Batch(m.fetch(), m.poll(-1, 0)) }

// fetch asks for the source and the last profile: at start-up, and again
// after an outage, when the custos that answers may be a new one.
func (m Model) fetch() tea.Cmd {
	c := m.c
	return tea.Batch(
		func() tea.Msg { s, err := c.Source(); return sourceMsg{s, err} },
		func() tea.Msg { p, err := c.Last(); return profMsg{p, err, true} },
		func() tea.Msg { l, err := c.LastLoad(); return loadMsg{l, err, true} },
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

// apply takes st if it is newer and moves the cursor to a stop. custos itself
// fetches each stop's bt and locals 0 (AutoFetch), so a stop sends nothing.
func (m Model) apply(st debug.State) Model {
	if st.Seq <= m.st.Seq && m.st.Run != "" {
		return m
	}
	m.st = st
	m.frame = 0 // the highlighted frame is the one whose locals are shown
	if st.Locals != nil {
		m.frame = st.Locals.Frame
	}
	if st.Run == "stopped" && st.Stop != nil && st.Stop.Line > 0 {
		m.cursor = st.Stop.Line
	}
	return m
}

// after is the Seq the next poll waits past: -1 until a state has been seen,
// so the first answer comes at once whatever the server's Seq.
func (m Model) after() int {
	if m.st.Run == "" {
		return -1
	}
	return m.st.Seq
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
			m.status, m.lost = "custos: "+msg.err.Error(), true
			return m, m.poll(m.after(), time.Second)
		}
		var cmd tea.Cmd
		if m.lost { // a restarted custos starts its Seq again: take whatever it has
			m.lost, m.status, m.st = false, "", debug.State{}
			cmd = m.fetch()
		} else if m.st.Run != "" && msg.st.Image != m.st.Image { // a load restarted the session on a new map
			c := m.c
			cmd = func() tea.Msg { s, err := c.Source(); return sourceMsg{s, err} }
		}
		m = m.apply(msg.st)
		return m, tea.Batch(cmd, m.poll(m.after(), 0))
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
		return m.apply(msg.d.State), nil
	case profMsg:
		m.capturing = false
		if msg.err != nil {
			if !(msg.last && is404(msg.err)) {
				m.status = "profile: " + msg.err.Error()
			}
			return m, nil
		}
		// A profile through porta (rete#415) answers at the goal/profile write: keep
		// the last capture on screen, say what is pending, and re-read every 5 s
		// until the dump has come back by the console tee, as the page does.
		if msg.c.Pending {
			m.status = fmt.Sprintf("profile: %d ms requested (porta seq %d), waiting for the node's dump", msg.c.Ms, msg.c.Seq)
			c := m.c
			return m, tea.Tick(5*time.Second, func(time.Time) tea.Msg { p, err := c.Last(); return profMsg{p, err, true} })
		}
		if msg.c.Error != "" {
			m.status = "profile: " + msg.c.Error
			return m, nil
		}
		m.prof, m.status = &msg.c, ""
	case loadMsg:
		m.loading = false
		if msg.err != nil {
			if !(msg.last && is404(msg.err)) {
				m.status = "load: " + msg.err.Error()
			}
			return m, nil
		}
		m.load, m.status = &msg.l, ""
		// A push through porta (rete#398) answers before the node has asked:
		// re-read it every 5 s until the node runs the image, as the page does.
		if p := msg.l.Push; p != nil && !p.Converged {
			c := m.c
			return m, tea.Tick(5*time.Second, func(time.Time) tea.Msg { l, err := c.LastLoad(); return loadMsg{l, err, true} })
		}
	case builtMsg:
		m.building = false
		if msg.err != nil {
			m.status = "build: " + msg.err.Error()
			return m, nil
		}
		m.built, m.ran, m.status = &msg.b, nil, ""
	case ranMsg:
		m.building = false
		if msg.err != nil {
			m.status = "run: " + msg.err.Error()
			return m, nil
		}
		m.ran, m.status = &msg.r, ""
	case tea.KeyMsg:
		return m.key(msg)
	}
	return m, nil
}

// patch takes "<edited.nsl> <Class>>selector" and sends it (peer design D17).
func (m Model) patch(line string) (Model, tea.Cmd) {
	f := strings.Fields(line)
	if len(f) != 2 {
		m.status = "patch: give the edited .nsl and the method, Class>>selector"
		return m, nil
	}
	if m.inflight != "" {
		m.status = fmt.Sprintf("%q is still running", m.inflight)
		return m, nil
	}
	m.nsl, m.inflight = f[0], "patch"
	c := m.c
	return m, func() tea.Msg { d, err := c.Patch(f[0], f[1]); return doneMsg{"patch", d, err} }
}

// compile builds (or runs, on a host board) the .nsl at m.nsl (peer design D18).
func (m Model) compile(run bool) (Model, tea.Cmd) {
	if m.building {
		m.status = "a build is still running"
		return m, nil
	}
	m.building = true
	c, path := m.c, m.nsl
	if run {
		return m, func() tea.Msg { r, err := c.Run(path); return ranMsg{r, err} }
	}
	return m, func() tea.Msg { b, err := c.Build(path); return builtMsg{b, err} }
}

// loadBuild loads the last build onto the board; the answer is the load tab's.
func (m Model) loadBuild() (Model, tea.Cmd) {
	if m.loading {
		m.status = "a load is still running"
		return m, nil
	}
	if m.built == nil {
		m.status = "nothing built yet: press b"
		return m, nil
	}
	m.loading = true
	c, name := m.c, m.built.Image
	return m, func() tea.Msg { l, err := c.LoadLastBuild(name); return loadMsg{l, err, false} }
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
			if prompt == "image: " {
				m.image = line
				return m.upload()
			}
			if prompt == "patch: " {
				return m.patch(line)
			}
			if prompt == "build: " || prompt == "run: " {
				m.nsl = line
				return m.compile(prompt == "run: ")
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
		m.tab = (m.tab + 1) % tabs
		return m, nil
	}
	if m.tab == loadTab {
		if s == "r" {
			m.prompt, m.input = "image: ", m.image
		}
		return m, nil
	}
	if m.tab == buildTab {
		switch s {
		case "b":
			m.prompt, m.input = "build: ", m.nsl
		case "x":
			m.prompt, m.input = "run: ", m.nsl
		case "l":
			return m.loadBuild()
		}
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
	case "R":
		return m.send("restart")
	case "P":
		// the edited file, then the method (the stopped one by default)
		m.prompt, m.input = "patch: ", m.nsl
		if m.st.Stop != nil && m.st.Stop.Method != "" {
			m.input = strings.TrimSpace(m.nsl + " " + m.st.Stop.Method)
		}
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

func (m Model) upload() (Model, tea.Cmd) {
	if m.loading {
		m.status = "a load is still running"
		return m, nil
	}
	m.loading = true
	c, path := m.c, m.image
	return m, func() tea.Msg { l, err := c.Load(path); return loadMsg{l, err, false} }
}
