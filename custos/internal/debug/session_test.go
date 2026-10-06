// Copyright (c) 2026 Ekorau LLC

package debug

import (
	"fmt"
	"strings"
	"sync"
	"testing"
)

// fakeVM answers each command the way the Supervisor does, from a script of
// stops: every dbg:step or dbg:over moves to the next one. It answers on its own
// goroutine, as the reader does, so the session's waits are exercised for real.
type fakeVM struct {
	s     *Session
	steps [][2]int // (blob offset, pc) per bytecode step
	wg    sync.WaitGroup
}

func (f *fakeVM) Send(line string) error {
	var reply []string
	switch {
	case strings.HasPrefix(line, "dbg:break "):
		reply = []string{"dbg:ok break"}
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
