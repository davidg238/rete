// Copyright (c) 2026 Ekorau LLC

package debug

import (
	"fmt"
	"io"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"time"
)

var pausedRE = regexp.MustCompile(`^dbg:paused (\w+) (-?\d+) (\d+)$`)

// LineStepLimit bounds a line step. A line of source is many bytecodes, and the
// VM steps one at a time; `step` and `next` repeat until the line changes. A
// loop on one line (`1 to: n do: [ ... ]`) is many round trips, and a `step`
// into the kernel can be thousands, so the repeat stops here and says where it
// is rather than running on unseen.
const LineStepLimit = 5000

// Help is what `help` prints.
const Help = `break/clear <line> or <file>:<line>, continue, step, next, finish, pause,
inspect, where, list, methods, quit; any ` + "`dbg:...`" + ` line is passed through unchanged.
Any line typed while the program runs stops it first (the pause rule), so
` + "`break 26`" + ` while running stops the program, sets the breakpoint, and stays stopped.`

var words = map[string]string{
	"c": "dbg:continue", "continue": "dbg:continue",
	"s": "dbg:step", "step": "dbg:step",
	"n": "dbg:over", "next": "dbg:over",
	"f": "dbg:out", "finish": "dbg:out",
	"p": "dbg:pause", "pause": "dbg:pause",
	"i": "dbg:inspect", "inspect": "dbg:inspect",
}

// stop is where the program is held: at boot, or at (id, off). A nil *stop is
// a running program.
type stop struct {
	boot    bool
	id, off int
}

// seen is one line from the VM, or its end.
type seen struct {
	text string
	end  bool
}

// Session is one debug session: the person's commands in, the VM's lines out,
// both in source terms.
type Session struct {
	sm   *SourceMap
	line Line

	outMu sync.Mutex
	out   io.Writer

	mu       sync.Mutex
	changed  chan struct{} // closed and replaced on every line seen
	seen     []seen        // every line from the VM, in order
	stopped  *stop
	gone     bool
	quiet    bool // fetching the registry: its lines are not the person's
	stepping bool // a line step: only its LAST bytecode stop is shown
}

// NewSession is a session over sm writing to out; Attach gives it its line.
func NewSession(sm *SourceMap, out io.Writer) *Session {
	return &Session{sm: sm, out: out, changed: make(chan struct{})}
}

// Attach gives the session the line its commands go down.
func (s *Session) Attach(l Line) { s.line = l }

func (s *Session) say(format string, args ...any) {
	s.outMu.Lock()
	defer s.outMu.Unlock()
	fmt.Fprintf(s.out, format+"\n", args...)
}

// Gone reports whether the program has ended.
func (s *Session) Gone() bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.gone
}

// Running reports whether the program is running (not held at a stop).
func (s *Session) Running() bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.stopped == nil
}

func (s *Session) setStopped(st *stop) {
	s.mu.Lock()
	s.stopped = st
	s.mu.Unlock()
}

func (s *Session) current() *stop {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.stopped
}

func (s *Session) record(l seen) {
	s.mu.Lock()
	s.seen = append(s.seen, l)
	close(s.changed)
	s.changed = make(chan struct{})
	s.mu.Unlock()
}

// OnLine is the reader's: record, then show, translating a stop into source terms.
func (s *Session) OnLine(l string) {
	s.mu.Lock()
	quiet, stepping := s.quiet, s.stepping
	s.mu.Unlock()
	if l == "dbg:ready" {
		s.setStopped(&stop{boot: true})
		s.say("-- stopped at boot, before the program's first line")
	} else if m := pausedRE.FindStringSubmatch(l); m != nil {
		id, _ := strconv.Atoi(m[2])
		off, _ := strconv.Atoi(m[3])
		s.setStopped(&stop{id: id, off: off})
		if !(stepping && m[1] == "step") {
			where, text, _ := s.sm.Where(id, off)
			s.say("-- stopped (%s) %s", m[1], where)
			if text != "" {
				s.say("%s", text)
			}
		}
	} else if !quiet {
		s.say("%s", l)
	}
	s.record(seen{text: l})
}

// OnEnd is the reader's when a host VM exits.
func (s *Session) OnEnd() {
	s.mu.Lock()
	s.gone = true
	s.mu.Unlock()
	s.say("-- the program ended")
	s.record(seen{end: true})
}

// mark is how many lines have been seen: a wait from here counts only what
// arrives after.
func (s *Session) mark() int {
	s.mu.Lock()
	defer s.mu.Unlock()
	return len(s.seen)
}

// ask sends line, then waits for a VM line matching pattern (or the VM's end),
// and answers the lines that arrived since the send. Marking BEFORE the send is
// what lets an answer that beats us to the wait still count.
func (s *Session) ask(line, pattern string, timeout time.Duration) []seen {
	m := s.mark()
	s.line.Send(line)
	return s.waitFrom(pattern, timeout, m)
}

// Wait waits up to timeout for a line matching pattern, counting from mark, or
// from now when mark is negative.
func (s *Session) Wait(pattern string, timeout time.Duration, mark int) {
	if mark < 0 {
		mark = s.mark()
	}
	s.waitFrom(pattern, timeout, mark)
}

func (s *Session) waitFrom(pattern string, timeout time.Duration, mark int) []seen {
	rx := regexp.MustCompile(pattern)
	deadline := time.NewTimer(timeout)
	defer deadline.Stop()
	i := mark
	for {
		s.mu.Lock()
		for i < len(s.seen) {
			l := s.seen[i]
			i++
			if l.end || rx.MatchString(l.text) {
				got := append([]seen(nil), s.seen[mark:i]...)
				s.mu.Unlock()
				return got
			}
		}
		changed := s.changed
		s.mu.Unlock()
		select {
		case <-changed:
		case <-deadline.C:
			s.mu.Lock()
			defer s.mu.Unlock()
			return append([]seen(nil), s.seen[mark:]...)
		}
	}
}

func texts(ls []seen) []string {
	out := make([]string, 0, len(ls))
	for _, l := range ls {
		if !l.end {
			out = append(out, l.text)
		}
	}
	return out
}

func (s *Session) registry() {
	if s.sm.HasRegistry() {
		return
	}
	s.mu.Lock()
	s.quiet = true
	s.mu.Unlock()
	defer func() {
		s.mu.Lock()
		s.quiet = false
		s.mu.Unlock()
	}()
	s.sm.LoadRegistry(texts(s.ask("dbg:methods", `^dbg:ok methods$|^dbg:error`, 10*time.Second)))
}

// stopFirst: a line typed while running stops the program; wait for that stop
// so the registry and the answer that follow are read in order.
func (s *Session) stopFirst() {
	if s.Running() {
		s.ask("dbg:pause", `^dbg:ok pause$`, 10*time.Second)
	}
}

// here is the current stop as (id, line); line is 0 outside the app's
// methods, and ok is false when not stopped in a method.
func (s *Session) here() (id, line int, ok bool) {
	st := s.current()
	if st == nil || st.boot {
		return 0, 0, false
	}
	s.registry()
	_, _, line = s.sm.Where(st.id, st.off)
	return st.id, line, true
}

// Do runs one command; false means leave.
func (s *Session) Do(cmd string) bool {
	cmd = strings.TrimSpace(cmd)
	word, arg, _ := strings.Cut(cmd, " ")
	arg = strings.TrimSpace(arg)
	switch {
	case word == "":
		return true
	case word == "q" || word == "quit":
		return false
	case word == "h" || word == "help":
		s.say("%s", Help)
		return true
	case strings.HasPrefix(word, "dbg:"):
		if word == "dbg:continue" || word == "dbg:run" {
			s.setStopped(nil)
			s.line.Send(cmd)
		} else {
			s.ask(cmd, `^dbg:(ok|error|stack|paused)`, 10*time.Second)
		}
		return true
	case word == "b" || word == "break" || word == "clear":
		s.stopFirst()
		s.registry()
		bp, err := s.sm.Resolve(arg)
		if err != nil {
			s.say("-- %s", err)
			return true
		}
		verb := "break"
		if word == "clear" {
			verb = "clear"
		}
		asked := arg
		if i := strings.LastIndex(arg, ":"); i >= 0 {
			asked = arg[i+1:]
		}
		moved := ""
		if strconv.Itoa(bp.Line) != asked {
			moved = fmt.Sprintf(" (line %s has no code; next is %d)", arg, bp.Line)
		}
		s.say("-- %s %s:%d  %s%s", verb, s.sm.Source, bp.Line, bp.Sel, moved)
		s.ask(fmt.Sprintf("dbg:%s %d %d", verb, bp.ID, bp.Off), `^dbg:(ok|error)`, 10*time.Second)
		return true
	case word == "w" || word == "where":
		st := s.current()
		switch {
		case st == nil:
			s.say("-- running")
		case st.boot:
			s.say("-- at boot")
		default:
			s.registry()
			where, text, _ := s.sm.Where(st.id, st.off)
			if text != "" {
				where += "\n" + text
			}
			s.say("-- %s", where)
		}
		return true
	case word == "l" || word == "list":
		_, at, ok := s.here()
		if !ok {
			s.say("-- not stopped in a line")
			return true
		}
		if at > 0 {
			for n := max(1, at-3); n < at+4; n++ {
				if t := s.sm.LineText(n); t != "" {
					mark := "   "
					if n == at {
						mark = "=> "
					}
					s.say("%s%s", mark, t)
				}
			}
		}
		return true
	case word == "methods":
		s.stopFirst()
		s.registry()
		for _, row := range s.sm.Methods() {
			s.say("%s", row)
		}
		return true
	}

	verb, ok := words[word]
	if !ok {
		s.say("-- unknown: %s (type help)", word)
		return true
	}
	if verb == "dbg:pause" {
		if !s.Running() {
			s.say("-- already stopped")
		} else {
			s.stopFirst()
		}
		return true
	}
	s.stopFirst()
	switch verb {
	case "dbg:continue":
		s.setStopped(nil)
		s.line.Send(verb)
	case "dbg:inspect":
		s.ask(verb, `^dbg:(stack|error)`, 10*time.Second)
	case "dbg:out":
		s.setStopped(nil)
		s.ask(verb, `^dbg:paused |^dbg:error`, 30*time.Second)
	default:
		s.stepLine(verb)
	}
	return true
}

// stepLine repeats a bytecode step until the stop is on a different line of
// the app (a line inside a block or the kernel does not count for `step`, and
// `next` never goes there), a breakpoint or the end intervenes, or
// LineStepLimit runs out. Only the final stop is shown.
func (s *Session) stepLine(verb string) {
	startID, startLine, startOK := s.here()
	s.mu.Lock()
	s.stepping = true
	s.mu.Unlock()
	stepping := func(on bool) {
		s.mu.Lock()
		s.stepping = on
		s.mu.Unlock()
	}
	moved := false
	for n := 0; n < LineStepLimit; n++ {
		s.setStopped(nil)
		got := s.ask(verb, `^dbg:paused |^dbg:error`, 30*time.Second)
		var m []string
		if len(got) > 0 && !got[len(got)-1].end {
			m = pausedRE.FindStringSubmatch(got[len(got)-1].text)
		}
		if m == nil || m[1] != "step" || s.Gone() {
			stepping(false)
			return // a breakpoint, an error, the end
		}
		id, line, ok := s.here()
		if ok && line != 0 && (!startOK || id != startID || line != startLine) {
			moved = true
			break
		}
	}
	stepping(false)
	st := s.current()
	where, text, _ := s.sm.Where(st.id, st.off)
	if !moved {
		s.say("-- still %s after %d steps; `step` again or `continue`", where, LineStepLimit)
		return
	}
	s.say("-- stopped (step) %s", where)
	if text != "" {
		s.say("%s", text)
	}
}
