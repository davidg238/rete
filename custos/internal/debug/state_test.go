// Copyright (c) 2026 Ekorau LLC

package debug

// The session as data (rete#340): what the page reads. The CLI's text is
// session_test.go's; these read State.

import (
	"strings"
	"testing"
	"time"
)

// until reads State through its long-poll until ok holds, or fails after 5 s.
func until(t *testing.T, s *Session, what string, ok func(State) bool) State {
	t.Helper()
	deadline := time.Now().Add(5 * time.Second)
	st := s.State(-1, 0)
	for !ok(st) {
		if time.Now().After(deadline) {
			t.Fatalf("never %s; last state %+v", what, st)
		}
		st = s.State(st.Seq, time.Second)
	}
	return st
}

func (f *fakeVM) count(line string) int {
	f.mu.Lock()
	defer f.mu.Unlock()
	n := 0
	for _, l := range f.sent {
		if l == line {
			n++
		}
	}
	return n
}

func TestAShownStopArrivesWithItsStackAndLocals(t *testing.T) {
	sm, _ := loaded(t)
	var out strings.Builder
	s := NewSession(sm, &out)
	s.AutoFetch = true
	vm := &fakeVM{s: s}
	s.Attach(vm)
	if got := s.State(-1, 0).Run; got != "unknown" {
		t.Fatalf("before any dbg: line the run is %q", got)
	}
	s.OnLine("dbg:ready")
	until(t, s, "at boot", func(st State) bool { return st.Run == "boot" })
	s.Do("break 37")
	s.Do("continue")
	st := until(t, s, "stopped with locals", func(st State) bool { return st.Locals != nil })
	if st.Run != "stopped" || st.Stop == nil {
		t.Fatalf("run %q stop %+v", st.Run, st.Stop)
	}
	if want := (StopView{Reason: "break", File: "t.nsl", Line: 37, Method: "T>>countTo:", Off: 200, PC: 2}); *st.Stop != want {
		t.Errorf("stop %+v, want %+v", *st.Stop, want)
	}
	if len(st.Frames) != 3 || st.Frames[2].File != "kernel.nsl" || st.Frames[2].Line != 27 || st.Frames[1].Method != "T>>main:args:" {
		t.Errorf("frames %+v", st.Frames)
	}
	if st.Locals.Frame != 0 || len(st.Locals.Vars) != 1 || st.Locals.Vars[0] != (Var{"self", "an Object"}) {
		t.Errorf("locals %+v", st.Locals)
	}
	if strings.Contains(out.String(), "#0") || strings.Contains(out.String(), "self =") {
		t.Errorf("the fetch printed:\n%s", out.String())
	}
}

func TestTheCLIFetchesNothingItWasNotAskedFor(t *testing.T) {
	sm, _ := loaded(t)
	var out strings.Builder
	s := NewSession(sm, &out)
	vm := &fakeVM{s: s}
	s.Attach(vm)
	s.OnLine("dbg:ready")
	s.Do("continue")
	vm.wg.Wait()
	until(t, s, "stopped", func(st State) bool { return st.Run == "stopped" })
	if n := vm.count("dbg:backtrace"); n != 0 {
		t.Errorf("without AutoFetch the session sent dbg:backtrace %d times", n)
	}
}

func TestALineStepFetchesOnlyItsShownStop(t *testing.T) {
	sm, _ := loaded(t)
	s := NewSession(sm, &strings.Builder{})
	s.AutoFetch = true
	vm := &fakeVM{s: s, steps: [][2]int{{200, 5}, {200, 9}, {200, 16}}}
	s.Attach(vm)
	s.OnLine("dbg:paused break 200 2")
	until(t, s, "the first stop's locals", func(st State) bool { return st.Locals != nil })
	s.Do("next")
	st := until(t, s, "line 38 with locals", func(st State) bool {
		return st.Stop != nil && st.Stop.Line == 38 && st.Locals != nil
	})
	if st.Run != "stopped" {
		t.Errorf("run %q", st.Run)
	}
	if n := vm.count("dbg:backtrace"); n != 2 {
		t.Errorf("dbg:backtrace sent %d times; want 2 (the break, the step's last stop)", n)
	}
}

func TestBreaksAreTheVMsAnswerNotTheRequest(t *testing.T) {
	sm, _ := loaded(t)
	s := NewSession(sm, &strings.Builder{})
	vm := &fakeVM{s: s}
	s.Attach(vm)
	s.OnLine("dbg:ready")
	s.Do("break 37")
	s.Do("break 26")
	if got := s.State(-1, 0).Breaks; strings.Join(got, ",") != "t.nsl:26,t.nsl:37" {
		t.Fatalf("breaks %v", got)
	}
	s.Do("clear 37")
	if got := s.State(-1, 0).Breaks; strings.Join(got, ",") != "t.nsl:26" {
		t.Fatalf("after clear, breaks %v", got)
	}
	vm.refuse = true
	s.Do("break 38")
	s.Do("clear 26")
	if got := s.State(-1, 0).Breaks; strings.Join(got, ",") != "t.nsl:26" {
		t.Fatalf("a refused break or clear changed the breaks: %v", got)
	}
}

func TestStateWaitsForTheNextChange(t *testing.T) {
	sm, _ := loaded(t)
	s := NewSession(sm, &strings.Builder{})
	s.Attach(&fakeVM{s: s})
	seq := s.State(-1, 0).Seq
	start := time.Now()
	if got := s.State(seq, 50*time.Millisecond); got.Seq != seq || time.Since(start) < 50*time.Millisecond {
		t.Fatalf("a wait with no change answered seq %d after %v", got.Seq, time.Since(start))
	}
	go func() {
		time.Sleep(20 * time.Millisecond)
		s.OnLine("hello from the program")
	}()
	got := s.State(seq, 5*time.Second)
	if got.Seq <= seq || got.Transcript[len(got.Transcript)-1] != "hello from the program" {
		t.Fatalf("seq %d transcript %q", got.Seq, got.Transcript)
	}
}

func TestTheTranscriptKeepsTheLastLines(t *testing.T) {
	sm, _ := loaded(t)
	s := NewSession(sm, &strings.Builder{})
	for i := 0; i < TranscriptCap+10; i++ {
		s.OnLine("x")
	}
	s.OnLine("last")
	got := s.State(-1, 0).Transcript
	if len(got) != TranscriptCap || got[len(got)-1] != "last" {
		t.Fatalf("%d lines, last %q", len(got), got[len(got)-1])
	}
}

func TestALostCableIsSaidAndEndsTheSession(t *testing.T) {
	sm, _ := loaded(t)
	var out strings.Builder
	s := NewSession(sm, &out)
	s.OnLine("dbg:paused break 200 2")
	s.OnLost()
	st := s.State(-1, 0)
	if st.Run != "lost" || st.Stop != nil || !s.Gone() {
		t.Fatalf("run %q stop %+v gone %v", st.Run, st.Stop, s.Gone())
	}
	if want := "-- " + LostCable; !strings.HasSuffix(out.String(), want+"\n") || st.Transcript[len(st.Transcript)-1] != want {
		t.Fatalf("printed:\n%s", out.String())
	}
}
