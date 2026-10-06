// Copyright (c) 2026 Ekorau LLC

package debug

import (
	"fmt"
	"testing"

	"github.com/davidg238/rete/custos/internal/imagemap"
)

func TestParseFrames(t *testing.T) {
	got, err := ParseFrames("dbg:frames 3832@9 4100@2 -1@0")
	if err != nil || len(got) != 3 || got[0] != [2]int{3832, 9} || got[2] != [2]int{-1, 0} {
		t.Fatalf("got %v, %v", got, err)
	}
	if _, err := ParseFrames("dbg:frames 3832"); err == nil {
		t.Fatal("a frame with no pc parsed")
	}
}

func TestParseInspect(t *testing.T) {
	regs, ctx, err := ParseInspect("dbg:stack off=9 r0=an Object r1=5 r2=[a context] c0=7 c1=nil")
	if err != nil || regs[1] != "5" || regs[0] != "an Object" || regs[2] != "[a context]" || ctx[0] != "7" || ctx[1] != "nil" {
		t.Fatalf("regs %v ctx %v err %v", regs, ctx, err)
	}
}

// A to:do: variable outside its inlined range (the loop's increment) is not
// shown: its register may hold anything there.
func TestLocalsOutsideInlinedRange(t *testing.T) {
	vs := []imagemap.Var{{Name: "n", Reg: 1, Ctx: -1, To: -1}, {Name: "i", Reg: 4, Ctx: -1, From: 5, To: 20}}
	if got := LiveVars(vs, 22); len(got) != 1 || got[0].Name != "n" {
		t.Fatalf("pc 22: %v", got)
	}
	if got := LiveVars(vs, 20); len(got) != 1 { // to is exclusive
		t.Fatalf("pc 20: %v", got)
	}
	if got := LiveVars(vs, 5); len(got) != 2 {
		t.Fatalf("pc 5: %v", got)
	}
}

func TestLiveVarsShadowing(t *testing.T) {
	vs := []imagemap.Var{{Name: "x", Reg: 3, Ctx: -1, To: -1}, {Name: "x", Reg: 6, Ctx: -1, From: 2, To: 9}}
	if got := LiveVars(vs, 4); len(got) != 1 || got[0].Reg != 6 {
		t.Fatalf("got %v, want the inner x", got)
	}
	if got := LiveVars(vs, 9); len(got) != 1 || got[0].Reg != 3 {
		t.Fatalf("got %v, want the outer x", got)
	}
}

// A stop before the prologue: the Context is not built, so the line has no c<k>.
func TestLocalsBeforePrologue(t *testing.T) {
	got := localVars(map[int]string{0: "an Object", 1: "5"}, map[int]string{},
		[]imagemap.Var{{Name: "n", Reg: 1, Ctx: -1, To: -1}, {Name: "k", Reg: -1, Ctx: 0, To: -1}})
	want := []Var{{"self", "an Object"}, {"n", "5"}, {"k", "(not yet set)"}}
	if fmt.Sprint(got) != fmt.Sprint(want) {
		t.Fatalf("got %q", got)
	}
}
