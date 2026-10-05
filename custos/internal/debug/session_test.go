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
	steps [][2]int // (id, off) per bytecode step
	wg    sync.WaitGroup
}

func (f *fakeVM) Send(line string) error {
	var reply []string
	switch {
	case line == "dbg:methods":
		reply = registry[1:] // the registry lines and dbg:ok methods
	case strings.HasPrefix(line, "dbg:break "):
		reply = []string{"dbg:ok break"}
	case line == "dbg:continue":
		reply = []string{"dbg:paused break 2 2"}
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
	vm := &fakeVM{s: s, steps: [][2]int{{2, 5}, {2, 9}, {2, 16}}}
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
	if got != "-- stopped (step) at t.nsl:38  countTo:\n  38 | line 38\n" {
		t.Fatalf("a line step printed:\n%s", got)
	}
	if len(vm.steps) != 0 {
		t.Errorf("%d scripted steps left: the step stopped early", len(vm.steps))
	}
}
