// Copyright (c) 2026 Ekorau LLC

package debug

// frames.go — the stack and the names at a stop (rete#323): `bt` from
// `dbg:frames`, `locals` from `dbg:stack` joined to the image map's `vars`.

import (
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

// showLocals prints `self` and then each live var as `name = value`. A captured
// var whose Context is not built yet (a stop before the prologue) has no value.
func (s *Session) showLocals(regs, ctx map[int]string, vars []imagemap.Var) {
	s.say("  self = %s", regs[0])
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
		s.say("  %s = %s", v.Name, val)
	}
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
