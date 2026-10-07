// Copyright (c) 2026 Ekorau LLC

package debug

// state.go — the session as data (rete#340): what a page reads where a person
// at the CLI reads text. Every verb updates it and still says what it said, so
// the CLI does not change; State is a snapshot, and its long-poll is the same
// `changed` channel the session's own waits use.

import (
	"sort"
	"strconv"
	"time"
)

// TranscriptCap is how many of the last transcript lines State keeps.
const TranscriptCap = 500

// StopView is a stop shown to the person, in source terms.
type StopView struct {
	Reason string // break, step, pause, ...
	File   string
	Line   int // 0 when the method has no lines
	Method string
	Off    int
	PC     int
}

// FrameView is one frame of the last bt.
type FrameView struct {
	Where  string // the sentence the CLI prints
	File   string
	Line   int
	Method string
	Off    int
	PC     int
}

// Var is one named value at a stop.
type Var struct{ Name, Value string }

// LocalsView is one frame's live names, self first.
type LocalsView struct {
	Frame int
	Where string
	Vars  []Var
}

// State is the session at one moment. Seq rises on every change.
type State struct {
	Seq        int
	Image      string // the sha256 the session's map describes: a change (a load) means fetch the source again
	Run        string // unknown, boot, running, stopped, ended, lost, stale
	Stop       *StopView
	Frames     []FrameView // the last bt at this stop, innermost first
	Locals     *LocalsView
	Breaks     []string // file:line, in line order
	Transcript []string // what the CLI would have shown, the last TranscriptCap lines
}

// State answers at once when Seq > after; otherwise it waits up to wait for
// the next change and answers what it has then.
func (s *Session) State(after int, wait time.Duration) State {
	timer := time.NewTimer(wait)
	defer timer.Stop()
	for {
		s.mu.Lock()
		if s.seq > after {
			defer s.mu.Unlock()
			return s.snapshot()
		}
		changed := s.changed
		s.mu.Unlock()
		select {
		case <-changed:
		case <-timer.C:
			s.mu.Lock()
			defer s.mu.Unlock()
			return s.snapshot()
		}
	}
}

// snapshot copies the state; s.mu is held.
func (s *Session) snapshot() State {
	st := State{
		Seq:        s.seq,
		Image:      s.sm.SHA256(),
		Run:        s.run,
		Frames:     append([]FrameView(nil), s.framesV...),
		Transcript: append([]string(nil), s.transcript...),
	}
	if s.stopV != nil {
		v := *s.stopV
		st.Stop = &v
	}
	if s.localsV != nil {
		v := *s.localsV
		v.Vars = append([]Var(nil), v.Vars...)
		st.Locals = &v
	}
	lines := make([]int, 0, len(s.breaks))
	for n := range s.breaks {
		lines = append(lines, n)
	}
	sort.Ints(lines)
	for _, n := range lines {
		st.Breaks = append(st.Breaks, s.sm.Source+":"+strconv.Itoa(n))
	}
	return st
}

// notify marks a change: State's waiters and the session's own wake. s.mu is
// held.
func (s *Session) notify() {
	s.seq++
	close(s.changed)
	s.changed = make(chan struct{})
}

// show makes a stop the one the person sees and, for a page, fetches its stack
// and frame 0's locals off the reader's goroutine.
func (s *Session) show(reason string, off, pc int) {
	file, line, name, _ := s.sm.Place(off, pc)
	v := &StopView{Reason: reason, File: file, Line: line, Method: name, Off: off, PC: pc}
	s.mu.Lock()
	s.run, s.stopV, s.framesV, s.localsV = "stopped", v, nil, nil
	s.notify()
	s.mu.Unlock()
	if s.AutoFetch {
		go s.fetch(v)
	}
}

// fetch is a page's bt and `locals 0` at stop v, run after any command in
// flight and skipped once v is no longer the stop.
func (s *Session) fetch(v *StopView) {
	s.cmdMu.Lock()
	defer s.cmdMu.Unlock()
	still := func() bool {
		s.mu.Lock()
		defer s.mu.Unlock()
		return s.stopV == v
	}
	if !still() {
		return
	}
	if _, err := s.backtrace(); err != nil || !still() {
		return
	}
	s.locals(0)
}

// say's record of itself, line by line; s.mu is held.
func (s *Session) transcribe(lines []string) {
	s.transcript = append(s.transcript, lines...)
	if over := len(s.transcript) - TranscriptCap; over > 0 {
		s.transcript = append([]string(nil), s.transcript[over:]...)
	}
	s.notify()
}
