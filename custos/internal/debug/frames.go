// Copyright (c) 2026 Ekorau LLC

package debug

// frames.go — the stack and the names at a stop (rete#323): `bt` from
// `dbg:frames`, `locals` from `dbg:stack` joined to the image map's `vars`.

import (
	"errors"
	"fmt"
	"regexp"
	"strconv"
	"strings"

	"github.com/davidg238/rete/custos/internal/imagemap"
)

// ParseFrames reads `dbg:frames <off>@<pc> ...`, innermost first.
func ParseFrames(line string) ([][2]int, error) {
	rest, ok := strings.CutPrefix(line, "dbg:frames")
	if !ok {
		return nil, fmt.Errorf("not a dbg:frames line: %q", line)
	}
	var out [][2]int
	for _, f := range strings.Fields(rest) {
		a, b, ok := strings.Cut(f, "@")
		off, err1 := strconv.Atoi(a)
		pc, err2 := strconv.Atoi(b)
		if !ok || err1 != nil || err2 != nil {
			return nil, fmt.Errorf("bad frame %q in %q", f, line)
		}
		out = append(out, [2]int{off, pc})
	}
	return out, nil
}

// fieldRE is one ` r<n>=` or ` c<n>=` of a dbg:stack line. A value runs to the
// next field: printStrings contain spaces (`an Object`).
var fieldRE = regexp.MustCompile(` ([rc])(\d+)=`)

// ParseInspect reads `dbg:stack off=<pc> r0=... c0=...` into registers and
// Context slots, by index.
func ParseInspect(line string) (regs, ctx map[int]string, err error) {
	if !strings.HasPrefix(line, "dbg:stack ") {
		return nil, nil, fmt.Errorf("not a dbg:stack line: %q", line)
	}
	regs, ctx = map[int]string{}, map[int]string{}
	locs := fieldRE.FindAllStringSubmatchIndex(line, -1)
	for i, l := range locs {
		end := len(line)
		if i+1 < len(locs) {
			end = locs[i+1][0]
		}
		n, _ := strconv.Atoi(line[l[4]:l[5]])
		if v := line[l[1]:end]; line[l[2]:l[3]] == "r" {
			regs[n] = v
		} else {
			ctx[n] = v
		}
	}
	return regs, ctx, nil
}

// LiveVars is the vars live at pc: whole-method ones, and inlined ones whose
// range holds pc; of two live with one name, the later-declared wins (an inner
// scope).
func LiveVars(vars []imagemap.Var, pc int) []imagemap.Var {
	var out []imagemap.Var
	at := map[string]int{}
	for _, v := range vars {
		if v.To >= 0 && (pc < v.From || pc >= v.To) {
			continue
		}
		if i, ok := at[v.Name]; ok {
			out[i] = v
			continue
		}
		at[v.Name] = len(out)
		out = append(out, v)
	}
	return out
}

// localVars is `self` and then each live var with its value. A captured var
// whose Context is not built yet (a stop before the prologue) has no value.
func localVars(regs, ctx map[int]string, vars []imagemap.Var) []Var {
	out := []Var{{"self", regs[0]}}
	for _, v := range vars {
		var val string
		var ok bool
		if v.Ctx >= 0 {
			val, ok = ctx[v.Ctx]
		} else {
			val, ok = regs[v.Reg]
		}
		if !ok {
			val = "(not yet set)"
		}
		out = append(out, Var{v.Name, val})
	}
	return out
}

// backtrace asks the VM for the stack at this stop; OnLine keeps the answer,
// for `locals N` and for State.
func (s *Session) backtrace() ([]FrameView, error) {
	got := s.askQuiet("dbg:backtrace", `^dbg:(frames|error)`)
	if len(got) == 0 {
		return nil, errors.New("no answer to dbg:backtrace")
	}
	if _, err := ParseFrames(got[len(got)-1]); err != nil {
		return nil, err
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	return append([]FrameView(nil), s.framesV...), nil
}

// takeFrames keeps a dbg:frames line as the stack at this stop, whoever asked
// for it: the session's own bt, or a line posted through porta (rete#432).
func (s *Session) takeFrames(line string) {
	frames, err := ParseFrames(line)
	if err != nil {
		return
	}
	views := make([]FrameView, len(frames))
	for i, f := range frames {
		where, _, _ := s.sm.Where(f[0], f[1])
		file, line, name, _ := s.sm.Place(f[0], f[1])
		views[i] = FrameView{Where: where, File: file, Line: line, Method: name, Off: f[0], PC: f[1]}
	}
	s.mu.Lock()
	s.frames, s.framesV = frames, views
	s.notify()
	s.mu.Unlock()
}

// locals asks the VM for frame n's registers and Context and names the ones
// live at its pc, through the map's vars.
func (s *Session) locals(n int) (*LocalsView, error) {
	// The frame's method and pc: frame 0 is the stop; deeper ones need a bt.
	off, pc, ok := s.frameAt(n)
	if !ok {
		return nil, fmt.Errorf("no frame %d (run bt first)", n)
	}
	got := s.askQuiet(fmt.Sprintf("dbg:inspect %d", n), `^dbg:(stack|error)`)
	if len(got) == 0 {
		return nil, errors.New("no answer to dbg:inspect")
	}
	regs, ctx, err := ParseInspect(got[len(got)-1])
	if err != nil {
		return nil, errors.New(got[len(got)-1])
	}
	m, _ := s.sm.Method(off)
	where, _, _ := s.sm.Where(off, pc)
	lv := &LocalsView{Frame: n, Where: where, Vars: localVars(regs, ctx, LiveVars(m.Vars, pc))}
	s.mu.Lock()
	s.localsV = lv
	s.notify()
	s.mu.Unlock()
	return lv, nil
}

// frameAt is frame n's (blob offset, pc): frame 0 is the stop itself; a deeper
// one is from the last bt at this stop.
func (s *Session) frameAt(n int) (off, pc int, ok bool) {
	st := s.current()
	if st == nil || st.boot {
		return 0, 0, false
	}
	if n == 0 {
		return st.off, st.pc, true
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if n >= len(s.frames) {
		return 0, 0, false
	}
	return s.frames[n][0], s.frames[n][1], true
}
