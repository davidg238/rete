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

// answerWait is how long a command waits for the VM's answer (a var so a test
// can shorten it). A run to the next stop (finish, a line step) waits 30 s.
var answerWait = 10 * time.Second

// Result is what one command came to.
type Result struct {
	Leave bool   // quit: the person is done
	Err   string // why the command did not do what it says, as said; "" when it did
}

// LineStepLimit bounds a line step. A line of source is many bytecodes, and the
// VM steps one at a time; `step` and `next` repeat until the line changes. A
// loop on one line (`1 to: n do: [ ... ]`) is many round trips, and a `step`
// into the kernel can be thousands, so the repeat stops here and says where it
// is rather than running on unseen.
const LineStepLimit = 5000

// Help is what `help` prints.
const Help = `break/clear <line> or <file>:<line>, continue, step, next, finish, pause,
inspect, bt, locals [N], where, list, methods, quit; any ` + "`dbg:...`" + ` line is passed through unchanged.
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

// stop is where the program is held: at boot, or at (blob offset, pc). A nil
// *stop is a running program.
type stop struct {
	boot    bool
	off, pc int
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

	// AutoFetch makes every shown stop fetch its bt and `locals 0` into State,
	// unprinted: a page's, so a stop arrives whole. The CLI leaves it off and
	// sends only what the person types.
	AutoFetch bool
	cmdMu     sync.Mutex // one command, or one fetch, on the line at a time

	mu       sync.Mutex
	changed  chan struct{} // closed and replaced on every change (notify)
	seen     []seen        // every line from the VM, in order
	stopped  *stop
	frames   [][2]int // the last bt at this stop: (blob offset, pc), innermost first
	gone     bool
	quiet    bool // bt, locals: the raw answer is not the person's; its translation is
	stepping bool // a line step: only its LAST bytecode stop is shown

	// The view State answers (state.go).
	seq        int
	run        string
	stopV      *StopView
	framesV    []FrameView
	localsV    *LocalsView
	breaks     map[int]bool // lines of the app's file the VM accepted a break on
	transcript []string
}

// NewSession is a session over sm writing to out; Attach gives it its line.
func NewSession(sm *SourceMap, out io.Writer) *Session {
	return &Session{sm: sm, out: out, changed: make(chan struct{}), run: "unknown", breaks: map[int]bool{}}
}

// Attach gives the session the line its commands go down.
func (s *Session) Attach(l Line) { s.line = l }

func (s *Session) say(format string, args ...any) {
	text := fmt.Sprintf(format, args...)
	s.outMu.Lock()
	fmt.Fprintln(s.out, text)
	s.outMu.Unlock()
	s.mu.Lock()
	s.transcribe(strings.Split(text, "\n"))
	s.mu.Unlock()
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
	s.frames = nil // a new stop invalidates the last bt
	switch {
	case st == nil:
		s.run, s.stopV, s.framesV, s.localsV = "running", nil, nil, nil
		s.notify()
	case st.boot:
		s.run, s.stopV, s.framesV, s.localsV = "boot", nil, nil, nil
		s.notify()
	} // a stop in a method is shown (or not) by show
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
	s.notify()
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
		off, _ := strconv.Atoi(m[2])
		pc, _ := strconv.Atoi(m[3])
		s.setStopped(&stop{off: off, pc: pc})
		if !(stepping && m[1] == "step") {
			s.show(m[1], off, pc)
			where, text, _ := s.sm.Where(off, pc)
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
func (s *Session) OnEnd() { s.end("ended", "the program ended") }

// LostCable is what a session says when its cable's read fails.
const LostCable = "the cable to the board was lost (unplugged?): reconnect it and restart"

// OnLost is the reader's when a cable's read fails: the session is over, and
// the person is told why rather than left watching a board that went quiet.
func (s *Session) OnLost() { s.end("lost", LostCable) }

func (s *Session) end(run, why string) {
	s.mu.Lock()
	s.gone = true
	s.run, s.stopV, s.framesV, s.localsV = run, nil, nil, nil
	s.notify()
	s.mu.Unlock()
	s.say("-- %s", why)
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
// and answers the lines that arrived since the send, and whether the wait was
// answered (false: it timed out). Marking BEFORE the send is
// what lets an answer that beats us to the wait still count.
func (s *Session) ask(line, pattern string, timeout time.Duration) ([]seen, bool) {
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

func (s *Session) waitFrom(pattern string, timeout time.Duration, mark int) ([]seen, bool) {
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
				return got, true
			}
		}
		changed := s.changed
		s.mu.Unlock()
		select {
		case <-changed:
		case <-deadline.C:
			s.mu.Lock()
			defer s.mu.Unlock()
			return append([]seen(nil), s.seen[mark:]...), false
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

// askQuiet is ask for a verb whose answer the session translates: the raw lines
// are recorded but not shown.
func (s *Session) askQuiet(line, pattern string) []string {
	s.mu.Lock()
	s.quiet = true
	s.mu.Unlock()
	defer func() {
		s.mu.Lock()
		s.quiet = false
		s.mu.Unlock()
	}()
	got, _ := s.ask(line, pattern, answerWait)
	return texts(got)
}

// command is ask for a command: a dbg:error answer is the VM's refusal (its line
// is already in the transcript), and no answer is said, since nothing else would
// say it (rete#362).
func (s *Session) command(line, pattern string, timeout time.Duration) ([]seen, Result) {
	got, ok := s.ask(line, pattern, timeout)
	if !ok {
		msg := fmt.Sprintf("no answer to %s in %s", line, timeout)
		s.say("-- %s", msg)
		return got, Result{Err: msg}
	}
	if l := got[len(got)-1]; !l.end && strings.HasPrefix(l.text, "dbg:error") {
		return got, Result{Err: l.text}
	}
	return got, Result{}
}

// stopFirst: a line typed while running stops the program; wait for that stop
// so the answer that follows is read in order.
func (s *Session) stopFirst() Result {
	if s.Running() {
		_, r := s.command("dbg:pause", `^dbg:ok pause$`, answerWait)
		return r
	}
	return Result{}
}

// here is the current stop as (blob offset, line); line is 0 outside the app's
// file, and ok is false when not stopped in a method.
func (s *Session) here() (off, line int, ok bool) {
	st := s.current()
	if st == nil || st.boot {
		return 0, 0, false
	}
	_, _, line = s.sm.Where(st.off, st.pc)
	return st.off, line, true
}

// Do runs one command and answers what it came to.
func (s *Session) Do(cmd string) Result {
	s.cmdMu.Lock()
	defer s.cmdMu.Unlock()
	fail := func(format string, args ...any) Result {
		msg := fmt.Sprintf(format, args...)
		s.say("-- %s", msg)
		return Result{Err: msg}
	}
	cmd = strings.TrimSpace(cmd)
	word, arg, _ := strings.Cut(cmd, " ")
	arg = strings.TrimSpace(arg)
	switch {
	case word == "":
		return Result{}
	case word == "q" || word == "quit":
		return Result{Leave: true}
	case word == "h" || word == "help":
		s.say("%s", Help)
		return Result{}
	case strings.HasPrefix(word, "dbg:"):
		if word == "dbg:continue" || word == "dbg:run" {
			s.setStopped(nil)
			s.line.Send(cmd)
		} else {
			_, r := s.command(cmd, `^dbg:(ok|error|stack|frames|paused)`, answerWait)
			return r
		}
		return Result{}
	case word == "b" || word == "break" || word == "clear":
		if r := s.stopFirst(); r.Err != "" {
			return r
		}
		bp, err := s.sm.Resolve(arg)
		if err != nil {
			return fail("%s", err)
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
		s.say("-- %s %s:%d  %s%s", verb, s.sm.Source, bp.Line, bp.Name, moved)
		got, r := s.command(fmt.Sprintf("dbg:%s %d %d", verb, bp.Off, bp.PC), `^dbg:(ok|error)`, answerWait)
		for _, l := range got {
			if strings.HasPrefix(l.text, "dbg:ok") {
				s.mu.Lock()
				if verb == "break" {
					s.breaks[bp.Line] = true
				} else {
					delete(s.breaks, bp.Line)
				}
				s.notify()
				s.mu.Unlock()
			}
		}
		return r
	case word == "w" || word == "where":
		st := s.current()
		switch {
		case st == nil:
			s.say("-- running")
		case st.boot:
			s.say("-- at boot")
		default:
			where, text, _ := s.sm.Where(st.off, st.pc)
			if text != "" {
				where += "\n" + text
			}
			s.say("-- %s", where)
		}
		return Result{}
	case word == "l" || word == "list":
		_, at, ok := s.here()
		if !ok {
			return fail("not stopped in a line")
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
		return Result{}
	case word == "bt" || word == "backtrace":
		if r := s.stopFirst(); r.Err != "" {
			return r
		}
		frames, err := s.backtrace()
		if err != nil {
			return fail("%s", err)
		}
		for i, f := range frames {
			s.say("  #%d %s", i, f.Where)
		}
		return Result{}
	case word == "locals":
		if r := s.stopFirst(); r.Err != "" {
			return r
		}
		n := 0
		if arg != "" {
			var err error
			if n, err = strconv.Atoi(arg); err != nil || n < 0 {
				return fail("locals takes a frame number from bt, got %q", arg)
			}
		}
		lv, err := s.locals(n)
		if err != nil {
			return fail("%s", err)
		}
		s.say("-- frame %d %s", n, lv.Where)
		for _, v := range lv.Vars {
			s.say("  %s = %s", v.Name, v.Value)
		}
		return Result{}
	case word == "methods":
		for _, row := range s.sm.Methods() {
			s.say("%s", row)
		}
		return Result{}
	}

	verb, ok := words[word]
	if !ok {
		return fail("unknown: %s (type help)", word)
	}
	if verb == "dbg:pause" {
		if !s.Running() {
			s.say("-- already stopped")
		} else {
			if r := s.stopFirst(); r.Err != "" {
				return r
			}
		}
		return Result{}
	}
	if r := s.stopFirst(); r.Err != "" {
		return r
	}
	switch verb {
	case "dbg:continue":
		s.setStopped(nil)
		s.line.Send(verb)
	case "dbg:inspect":
		_, r := s.command(verb, `^dbg:(stack|error)`, answerWait)
		return r
	case "dbg:out":
		s.setStopped(nil)
		_, r := s.command(verb, `^dbg:paused |^dbg:error`, 30*time.Second)
		return r
	default:
		return s.stepLine(verb)
	}
	return Result{}
}

// stepLine repeats a bytecode step until the stop is on a different line of
// the app (a line inside a block or the kernel does not count for `step`, and
// `next` never goes there), a breakpoint or the end intervenes, or
// LineStepLimit runs out. Only the final stop is shown.
func (s *Session) stepLine(verb string) Result {
	startOff, startLine, startOK := s.here()
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
		got, r := s.command(verb, `^dbg:paused |^dbg:error`, 30*time.Second)
		if r.Err != "" {
			stepping(false)
			return r
		}
		var m []string
		if len(got) > 0 && !got[len(got)-1].end {
			m = pausedRE.FindStringSubmatch(got[len(got)-1].text)
		}
		if m == nil || m[1] != "step" || s.Gone() {
			stepping(false)
			return Result{} // a breakpoint, the end
		}
		off, line, ok := s.here()
		if ok && line != 0 && (!startOK || off != startOff || line != startLine) {
			moved = true
			break
		}
	}
	stepping(false)
	st := s.current()
	where, text, _ := s.sm.Where(st.off, st.pc)
	s.show("step", st.off, st.pc)
	if !moved {
		s.say("-- still %s after %d steps; `step` again or `continue`", where, LineStepLimit)
		return Result{}
	}
	s.say("-- stopped (step) %s", where)
	if text != "" {
		s.say("%s", text)
	}
	return Result{}
}
