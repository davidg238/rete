// Copyright (c) 2026 Ekorau LLC

package debug

import (
	"fmt"
	"strings"
	"sync"
	"testing"
	"time"
)

// fakeVM answers each command the way the Supervisor does, from a script of
// stops: every dbg:step or dbg:over moves to the next one. It answers on its own
// goroutine, as the reader does, so the session's waits are exercised for real.
type fakeVM struct {
	s      *Session
	steps  [][2]int // (blob offset, pc) per bytecode step
	refuse bool     // break and clear answer dbg:error
	wg     sync.WaitGroup

	mu   sync.Mutex
	sent []string // every line sent, in order
}

func (f *fakeVM) Send(line string) error {
	f.mu.Lock()
	f.sent = append(f.sent, line)
	f.mu.Unlock()
	var reply []string
	switch {
	case f.refuse && (strings.HasPrefix(line, "dbg:break ") || strings.HasPrefix(line, "dbg:clear ")):
		reply = []string{"dbg:error no code at that pc"}
	case strings.HasPrefix(line, "dbg:break "):
		reply = []string{"dbg:ok break"}
	case strings.HasPrefix(line, "dbg:clear "):
		reply = []string{"dbg:ok clear"}
	case line == "dbg:pause":
		reply = []string{"dbg:ok pause"}
	case line == "dbg:continue":
		reply = []string{"dbg:paused break 200 2"}
	case line == "dbg:backtrace":
		reply = []string{"dbg:frames 300@6 100@21 400@0"}
	case strings.HasPrefix(line, "dbg:inspect"):
		reply = []string{"dbg:stack off=6 r0=an Object r1=5 r2=nil"}
	case line == "dbg:step" || line == "dbg:over":
		next := f.steps[0]
		f.steps = f.steps[1:]
		reply = []string{fmt.Sprintf("dbg:paused step %d %d", next[0], next[1])}
	}
	f.wg.Add(1)
	go func() {
		defer f.wg.Done()
		for _, l := range reply {
			f.s.OnLine(l)
		}
	}()
	return nil
}

func TestALineStepShowsOnlyItsLastStop(t *testing.T) {
	sm, _ := loaded(t)
	var out strings.Builder
	s := NewSession(sm, &out)
	// From countTo:'s offset 2 (line 37): two more bytecodes on 37, then 16 is
	// line 38.
	vm := &fakeVM{s: s, steps: [][2]int{{200, 5}, {200, 9}, {200, 16}}}
	s.Attach(vm)
	s.OnLine("dbg:ready")
	for _, cmd := range []string{"break 37", "continue"} {
		s.Do(cmd)
		vm.wg.Wait()
	}
	out.Reset()
	s.Do("next")
	vm.wg.Wait()
	got := out.String()
	if got != "-- stopped (step) at t.nsl:38  T>>countTo:\n  38 | line 38\n" {
		t.Fatalf("a line step printed:\n%s", got)
	}
	if len(vm.steps) != 0 {
		t.Errorf("%d scripted steps left: the step stopped early", len(vm.steps))
	}
}

// bt names every frame through the map, the kernel's included; locals names the
// live registers from the map's vars and hides the raw dbg:stack line.
func TestBtAndLocals(t *testing.T) {
	d := synthetic()
	d["methods"].(map[string]any)["300"].(map[string]any)["vars"] = []any{
		map[string]any{"name": "x", "reg": 1}, map[string]any{"name": "gone", "reg": 2, "from": 0, "to": 3}}
	dir := t.TempDir()
	sm, err := LoadSourceMap(writeMap(t, dir, d), "")
	if err != nil {
		t.Fatal(err)
	}
	var out strings.Builder
	s := NewSession(sm, &out)
	vm := &fakeVM{s: s}
	s.Attach(vm)
	s.OnLine("dbg:paused break 300 6")
	out.Reset()
	s.Do("bt")
	vm.wg.Wait()
	want := "  #0 at t.nsl:26  T_block2>>value:\n  #1 at t.nsl:24  T>>main:args:\n  #2 at kernel.nsl:27  Kernel>>pumpEvents\n"
	if got := out.String(); got != want {
		t.Fatalf("bt printed:\n%s", got)
	}
	out.Reset()
	s.Do("locals")
	vm.wg.Wait()
	want = "-- frame 0 at t.nsl:26  T_block2>>value:\n  self = an Object\n  x = 5\n"
	if got := out.String(); got != want {
		t.Fatalf("locals printed:\n%s", got)
	}
	out.Reset()
	s.Do("locals 5")
	if got := out.String(); !strings.Contains(got, "no frame 5") {
		t.Fatalf("locals 5 printed:\n%s", got)
	}
}

// silentVM never answers: a hung board, as the session sees one.
type silentVM struct{}

func (silentVM) Send(string) error { return nil }

// rete#362: a command says whether it did what it says, as well as saying it.
func TestACommandSaysWhetherItDidWhatItSays(t *testing.T) {
	sm, _ := loaded(t)
	var out strings.Builder
	s := NewSession(sm, &out)
	if r := s.Do("list"); r.Err != "not stopped in a line" {
		t.Errorf("list before any stop: %+v", r)
	}
	vm := &fakeVM{s: s}
	s.Attach(vm)
	s.OnLine("dbg:ready")
	for cmd, want := range map[string]string{
		"break 37":   "",
		"frobnicate": "unknown: frobnicate (type help)",
		"locals x":   `locals takes a frame number from bt, got "x"`,
	} {
		if r := s.Do(cmd); r.Err != want || r.Leave {
			t.Errorf("%s: %+v, want Err %q", cmd, r, want)
		}
		vm.wg.Wait()
	}
	if r := s.Do("break 9999"); r.Err == "" {
		t.Errorf("a line with no code anywhere was accepted: %+v", r)
	}
	if r := s.Do("quit"); !r.Leave || r.Err != "" {
		t.Errorf("quit: %+v", r)
	}
	vm.refuse = true
	if r := s.Do("break 37"); r.Err != "dbg:error no code at that pc" {
		t.Errorf("a refused break: %+v", r)
	}
	vm.wg.Wait()
}

func TestNoAnswerIsAFailure(t *testing.T) {
	defer func(w time.Duration) { answerWait = w }(answerWait)
	answerWait = 50 * time.Millisecond
	sm, _ := loaded(t)
	var out strings.Builder
	s := NewSession(sm, &out)
	s.Attach(silentVM{})
	s.OnLine("dbg:ready")
	out.Reset()
	r := s.Do("break 37")
	if !strings.HasPrefix(r.Err, "no answer to dbg:break ") {
		t.Fatalf("a break nobody answered: %+v", r)
	}
	if !strings.Contains(out.String(), "-- no answer to dbg:break ") {
		t.Errorf("the CLI did not say it:\n%s", out.String())
	}
	if len(s.State(-1, 0).Breaks) != 0 {
		t.Errorf("an unanswered break is in the state")
	}
}

func TestTheEndIsAnAnswer(t *testing.T) {
	sm, _ := loaded(t)
	var out strings.Builder
	s := NewSession(sm, &out)
	s.Attach(&fakeVM{s: s}) // answers nothing to dbg:out
	s.OnLine("dbg:paused break 200 2")
	go func() { time.Sleep(20 * time.Millisecond); s.OnEnd() }()
	if r := s.Do("finish"); r.Err != "" {
		t.Fatalf("finish into the end: %+v", r)
	}
	if run := s.State(-1, 0).Run; run != "ended" {
		t.Errorf("run %q", run)
	}
}

// A program that has ended (or a lost cable) takes no command: continue must not
// answer OK and turn the state back to running (rete#362 review).
func TestNoCommandAfterTheEnd(t *testing.T) {
	sm, _ := loaded(t)
	var out strings.Builder
	s := NewSession(sm, &out)
	s.Attach(&fakeVM{s: s})
	s.OnLine("dbg:ready")
	s.OnEnd()
	for _, cmd := range []string{"continue", "dbg:continue", "step", "break 37"} {
		if r := s.Do(cmd); r.Err != "the program ended" {
			t.Errorf("%s after the end: %+v", cmd, r)
		}
	}
	if run := s.State(-1, 0).Run; run != "ended" {
		t.Errorf("run %q after the end", run)
	}
}
