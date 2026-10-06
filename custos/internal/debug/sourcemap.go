// Copyright (c) 2026 Ekorau LLC

package debug

import (
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"

	"github.com/davidg238/rete/custos/internal/imagemap"
)

// pair is one [bytecode offset, line] entry of a method's lines.
type pair struct{ off, line int }

// function is one method of the app's file, as the map names it.
type function struct {
	off   int    // blob offset: the VM's name for it
	name  string // "<Vtable>>selector"
	pairs []pair
}

// SourceMap is the image map read for a debugging session: a method is its blob
// offset (rete#323), so the map is the only lookup. It does no I/O beyond reading
// the files it is given.
type SourceMap struct {
	img    *imagemap.Map
	Source string // files[0], the app's file
	Entry  string // the class the image runs
	// functions is every method with lines in files[0] — the entry's, its
	// blocks', its other classes' — in the map's offset order. The kernel and
	// the Supervisor are other files.
	functions []function
	text      []string // the app's source, nil when it cannot be read
}

// LoadSourceMap reads an image map, and the source beside it (or at source).
func LoadSourceMap(mapPath, source string) (*SourceMap, error) {
	img, err := imagemap.Load(mapPath)
	if err != nil {
		return nil, err
	}
	sm := &SourceMap{img: img, Source: img.Files[0], Entry: img.Entry}
	for _, off := range img.Offsets() {
		m := img.Methods[off]
		if m.File != 0 || len(m.Lines) == 0 {
			continue
		}
		f := function{off: off, name: m.Name}
		for _, p := range m.Lines {
			f.pairs = append(f.pairs, pair{p[0], p[1]})
		}
		sm.functions = append(sm.functions, f)
	}

	if source == "" {
		abs, _ := filepath.Abs(mapPath)
		source = filepath.Join(filepath.Dir(abs), sm.Source)
	}
	if text, err := os.ReadFile(source); err == nil {
		lines := strings.Split(strings.ReplaceAll(string(text), "\r\n", "\n"), "\n")
		if lines[len(lines)-1] == "" {
			lines = lines[:len(lines)-1] // the newline that ends the last line
		}
		sm.text = lines
	}
	return sm, nil
}

// CheckImage refuses an image other than the one the map describes: its hash
// names one build.
func (sm *SourceMap) CheckImage(oimgPath string) error { return sm.img.CheckImage(oimgPath) }

// LineText is source line n as the tool shows it, or "" when there is none.
func (sm *SourceMap) LineText(n int) string {
	if n < 1 || n > len(sm.text) {
		return ""
	}
	return fmt.Sprintf("%4d | %s", n, sm.text[n-1])
}

func span(pairs []pair) (lo, hi int) {
	lo, hi = pairs[0].line, pairs[0].line
	for _, p := range pairs[1:] {
		lo, hi = min(lo, p.line), max(hi, p.line)
	}
	return lo, hi
}

// methodAtLine is the narrowest method of the app's file whose lines span line,
// or nil. Narrowest, because a block that is not inlined (a `whenever:do:`
// handler) is its own method, `<Entry>_blockN>>value:`, inside its home's span.
func (sm *SourceMap) methodAtLine(line int) *function {
	var best *function
	width := -1
	for i := range sm.functions {
		f := &sm.functions[i]
		lo, hi := span(f.pairs)
		if lo <= line && line <= hi && (width < 0 || hi-lo < width) {
			best, width = f, hi-lo
		}
	}
	return best
}

// Breakpoint is where a source line lands in the VM's terms.
type Breakpoint struct {
	Off, PC int    // the method's blob offset and the bytecode offset to send
	Line    int    // the line it lands on: the one asked for, or the next with code
	Name    string // the method, "<Vtable>>selector"
}

// Resolve turns `26` or `file.nsl:26` into a breakpoint. Its error is a sentence
// a person can act on.
func (sm *SourceMap) Resolve(spec string) (Breakpoint, error) {
	f, n := "", spec
	if i := strings.LastIndex(spec, ":"); i >= 0 {
		f, n = spec[:i], spec[i+1:]
	}
	if f != "" && filepath.Base(f) != sm.Source {
		return Breakpoint{}, fmt.Errorf("this image was built from %s, not %s", sm.Source, f)
	}
	line, err := strconv.Atoi(n)
	if err != nil {
		return Breakpoint{}, fmt.Errorf("expected <line> or %s:<line>, got %q", sm.Source, spec)
	}
	fn := sm.methodAtLine(line)
	if fn == nil {
		// A header, a `| locals |` line, a comment: the map starts a method at its
		// first statement, so name the code on either side instead of guessing.
		type near struct {
			line int
			sel  string
		}
		var all []near
		for _, f := range sm.functions {
			_, sel, _ := strings.Cut(f.name, ">>")
			for _, p := range f.pairs {
				all = append(all, near{p.line, sel})
			}
		}
		sort.Slice(all, func(i, j int) bool {
			if all[i].line != all[j].line {
				return all[i].line < all[j].line
			}
			return all[i].sel < all[j].sel
		})
		var named []string
		for i := len(all) - 1; i >= 0; i-- {
			if all[i].line < line {
				named = append(named, fmt.Sprintf("%d (%s)", all[i].line, all[i].sel))
				break
			}
		}
		for _, p := range all {
			if p.line > line {
				named = append(named, fmt.Sprintf("%d (%s)", p.line, p.sel))
				break
			}
		}
		if len(named) == 0 {
			named = []string{"none"}
		}
		return Breakpoint{}, fmt.Errorf("line %d has no code; the nearest lines that do: %s",
			line, strings.Join(named, ", "))
	}
	pairs := append([]pair(nil), fn.pairs...)
	sort.Slice(pairs, func(i, j int) bool {
		if pairs[i].line != pairs[j].line {
			return pairs[i].line < pairs[j].line
		}
		return pairs[i].off < pairs[j].off
	})
	for _, p := range pairs {
		if p.line >= line {
			return Breakpoint{Off: fn.off, PC: p.off, Line: p.line, Name: fn.name}, nil
		}
	}
	return Breakpoint{}, fmt.Errorf("line %d has no code in %s", line, fn.name)
}

// lineAt is the line of the last statement started at pc: the pair with the
// largest offset not past pc (ties to the larger line); before the first, the
// first. 0 for a method with no lines.
func lineAt(lines [][2]int, pc int) int {
	var at *[2]int
	for i := range lines {
		p := &lines[i]
		if p[0] <= pc && (at == nil || p[0] > at[0] || (p[0] == at[0] && p[1] > at[1])) {
			at = p
		}
	}
	if at == nil {
		for i := range lines {
			p := &lines[i]
			if at == nil || p[0] < at[0] || (p[0] == at[0] && p[1] < at[1]) {
				at = p
			}
		}
	}
	if at == nil {
		return 0
	}
	return at[1]
}

// Where turns the (blob offset, pc) of a stop into a sentence and the source
// line ("" when the stop is not in the app's file). line is answered only for
// the app's file, and is 0 otherwise: the session compares lines of the app, and
// a kernel line must not read as "moved to line N".
func (sm *SourceMap) Where(off, pc int) (where, text string, line int) {
	if off < 0 {
		return fmt.Sprintf("in an image with no blob (Path A), pc %d", pc), "", 0
	}
	m, ok := sm.img.Methods[off]
	if !ok {
		return fmt.Sprintf("at blob offset %d, which the map does not name (wrong build?)", off), "", 0
	}
	at := lineAt(m.Lines, pc)
	if at == 0 {
		return fmt.Sprintf("in %s's %s, pc %d", sm.img.Files[m.File], m.Name, pc), "", 0
	}
	if m.File != 0 {
		return fmt.Sprintf("at %s:%d  %s", sm.img.Files[m.File], at, m.Name), "", 0
	}
	return fmt.Sprintf("at %s:%d  %s", sm.Source, at, m.Name), sm.LineText(at), at
}

// Method is the image map's entry for a blob offset.
func (sm *SourceMap) Method(off int) (imagemap.Method, bool) {
	m, ok := sm.img.Methods[off]
	return m, ok
}

// Methods lists every method of the app's file by blob offset, with its line
// span, as `methods` shows them. It reads the map alone.
func (sm *SourceMap) Methods() []string {
	var out []string
	for _, f := range sm.functions {
		lo, hi := span(f.pairs)
		out = append(out, fmt.Sprintf("%6d  %-36s %s:%d-%d", f.off, f.name, sm.Source, lo, hi))
	}
	return out
}
