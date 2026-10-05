// Copyright (c) 2026 Ekorau LLC

package debug

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
)

// imageMap is protocol/IMAGE-ARTIFACTS.md §3, field for field. A missing field
// or an extra one refuses the file: the format changes by editing that document
// and rebuilding both sides, never by a reader tolerating a shape.
type imageMap struct {
	Image *struct {
		SHA256  *string `json:"sha256"`
		Size    *int64  `json:"size"`
		OopSize *int    `json:"oop_size"`
	} `json:"image"`
	Entry   *string              `json:"entry"`
	Files   []string             `json:"files"`
	Methods map[string]mapMethod `json:"methods"`
}

type mapMethod struct {
	Name  *string  `json:"name"`
	File  *int     `json:"file"`
	Lines [][2]int `json:"lines"`
}

// pair is one [bytecode offset, line] entry of a method's lines.
type pair struct{ off, line int }

// function is one method of the app's file, as the map names it.
type function struct {
	name  string // "<Vtable>>selector"
	pairs []pair
}

// SourceMap is the image map (offset -> name, file, lines per method) joined to
// the VM's registry (id -> selector). It does no I/O beyond reading the files it
// is given.
type SourceMap struct {
	sha256 string
	size   int64
	Source string // files[0], the app's file
	Entry  string // the class the image runs
	// functions is every method with lines in files[0] — the entry's, its
	// blocks', its other classes' — in the map's offset order. The kernel and
	// the Supervisor are other files.
	functions []function
	// methods is selector -> pairs for the entry class's own methods.
	methods map[string][]pair
	text    []string // the app's source, nil when it cannot be read

	ids  map[string]int   // selector -> id, the entry class's group only
	sels map[int]selector // id -> selector
}

type selector struct {
	name string
	own  bool // in the entry class's group, not a delegate's (the kernel's Object)
}

// LoadSourceMap reads an image map, and the source beside it (or at source).
func LoadSourceMap(mapPath, source string) (*SourceMap, error) {
	raw, err := os.ReadFile(mapPath)
	if err != nil {
		return nil, err
	}
	dec := json.NewDecoder(bytes.NewReader(raw))
	dec.DisallowUnknownFields()
	var doc imageMap
	if err := dec.Decode(&doc); err != nil {
		return nil, fmt.Errorf("%s is not an image map (%v); rebuild it (rete#337)", mapPath, err)
	}
	var missing []string
	if doc.Image == nil || doc.Image.SHA256 == nil || doc.Image.Size == nil || doc.Image.OopSize == nil {
		missing = append(missing, "image")
	}
	if doc.Entry == nil {
		missing = append(missing, "entry")
	}
	if len(doc.Files) == 0 {
		missing = append(missing, "files")
	}
	if doc.Methods == nil {
		missing = append(missing, "methods")
	}
	for k, m := range doc.Methods {
		if m.Name == nil || m.File == nil || m.Lines == nil {
			missing = append(missing, "methods["+k+"]")
		}
	}
	if len(missing) > 0 {
		sort.Strings(missing)
		return nil, fmt.Errorf("%s is not an image map (no %s); rebuild it (rete#337)",
			mapPath, strings.Join(missing, ", "))
	}

	sm := &SourceMap{
		sha256:  *doc.Image.SHA256,
		size:    *doc.Image.Size,
		Source:  doc.Files[0],
		Entry:   *doc.Entry,
		methods: map[string][]pair{},
		ids:     map[string]int{},
		sels:    map[int]selector{},
	}
	keys := make([]string, 0, len(doc.Methods))
	for k := range doc.Methods {
		keys = append(keys, k)
	}
	offset := func(k string) int { n, _ := strconv.Atoi(k); return n }
	sort.Slice(keys, func(i, j int) bool { return offset(keys[i]) < offset(keys[j]) })
	prefix := sm.Entry + ">>"
	for _, k := range keys {
		m := doc.Methods[k]
		if *m.File != 0 || len(m.Lines) == 0 {
			continue
		}
		f := function{name: *m.Name}
		for _, p := range m.Lines {
			f.pairs = append(f.pairs, pair{p[0], p[1]})
		}
		sm.functions = append(sm.functions, f)
		if sel, ok := strings.CutPrefix(f.name, prefix); ok {
			sm.methods[sel] = f.pairs
		}
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
func (sm *SourceMap) CheckImage(oimgPath string) error {
	b, err := os.ReadFile(oimgPath)
	if err != nil {
		return err
	}
	sum := sha256.Sum256(b)
	got := hex.EncodeToString(sum[:])
	if got != sm.sha256 || int64(len(b)) != sm.size {
		return fmt.Errorf("%s is not the image this map describes (sha256 %s, map says %s); rebuild both together",
			oimgPath, got[:12], sm.sha256[:min(12, len(sm.sha256))])
	}
	return nil
}

var regRE = regexp.MustCompile(`^(\d+) #?(\S+) (-?\d+)$`)

// LoadRegistry reads the `<id> #<selector> <tag>` lines of `dbg:methods`. The
// first tag is the entry class's vtable; later tags are its delegates (the
// kernel's Object).
func (sm *SourceMap) LoadRegistry(lines []string) {
	first := ""
	for _, l := range lines {
		m := regRE.FindStringSubmatch(l)
		if m == nil {
			continue
		}
		id, _ := strconv.Atoi(m[1])
		sel, tag := m[2], m[3]
		if first == "" {
			first = tag
		}
		sm.sels[id] = selector{sel, tag == first}
		if tag == first {
			sm.ids[sel] = id
		}
	}
}

// HasRegistry reports whether the VM's registry has been read.
func (sm *SourceMap) HasRegistry() bool { return len(sm.ids) > 0 }

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
// by its map name, or "". Narrowest, because a block that is not inlined (a
// `whenever:do:` handler) is its own method, `<Entry>_blockN>>value:`, inside its
// home's span.
func (sm *SourceMap) methodAtLine(line int) string {
	best, width := "", -1
	for _, f := range sm.functions {
		lo, hi := span(f.pairs)
		if lo <= line && line <= hi && (width < 0 || hi-lo < width) {
			best, width = f.name, hi-lo
		}
	}
	return best
}

// Breakpoint is where a source line lands in the VM's terms.
type Breakpoint struct {
	ID, Off int    // the registry id and bytecode offset to send
	Line    int    // the line it lands on: the one asked for, or the next with code
	Sel     string // the method's selector
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
	key := sm.methodAtLine(line)
	if key == "" {
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
	cls, sel, _ := strings.Cut(key, ">>")
	if cls != sm.Entry {
		return Breakpoint{}, fmt.Errorf("line %d is in %s, a block or class the registry does not list; "+
			"break on a line of a %s method instead", line, key, sm.Entry)
	}
	id, ok := sm.ids[sel]
	if !ok {
		return Breakpoint{}, fmt.Errorf("%s is not in the VM's registry (run `methods`)", sel)
	}
	pairs := append([]pair(nil), sm.methods[sel]...)
	sort.Slice(pairs, func(i, j int) bool {
		if pairs[i].line != pairs[j].line {
			return pairs[i].line < pairs[j].line
		}
		return pairs[i].off < pairs[j].off
	})
	for _, p := range pairs {
		if p.line >= line {
			return Breakpoint{ID: id, Off: p.off, Line: p.line, Sel: sel}, nil
		}
	}
	return Breakpoint{}, fmt.Errorf("line %d has no code in %s", line, sel)
}

// Where turns the (id, offset) of a `dbg:paused` line into a sentence and the
// source line ("" when the stop is not on a line of the app). line is 0 then.
func (sm *SourceMap) Where(id, off int) (where, text string, line int) {
	s, ok := sm.sels[id]
	if id < 0 || !ok {
		return fmt.Sprintf("outside the registry (in a block or the kernel), offset %d", off), "", 0
	}
	pairs, mine := sm.methods[s.name]
	if !s.own || !mine {
		return fmt.Sprintf("in the kernel's %s, offset %d", s.name, off), "", 0
	}
	// The line of the last statement started: the pair with the largest offset
	// not past off (ties to the larger line); before the first, the first.
	var at *pair
	for i := range pairs {
		p := &pairs[i]
		if p.off <= off && (at == nil || p.off > at.off || (p.off == at.off && p.line > at.line)) {
			at = p
		}
	}
	if at == nil {
		for i := range pairs {
			p := &pairs[i]
			if at == nil || p.off < at.off || (p.off == at.off && p.line < at.line) {
				at = p
			}
		}
	}
	return fmt.Sprintf("at %s:%d  %s", sm.Source, at.line, s.name), sm.LineText(at.line), at.line
}

// Methods lists the entry class's registered methods by id, with their line
// spans, as `methods` shows them.
func (sm *SourceMap) Methods() []string {
	type row struct {
		id  int
		sel string
	}
	var rows []row
	for sel, id := range sm.ids {
		rows = append(rows, row{id, sel})
	}
	sort.Slice(rows, func(i, j int) bool { return rows[i].id < rows[j].id })
	var out []string
	for _, r := range rows {
		where := "(no source lines)"
		if pairs, ok := sm.methods[r.sel]; ok {
			lo, hi := span(pairs)
			where = fmt.Sprintf("%s:%d-%d", sm.Source, lo, hi)
		}
		out = append(out, fmt.Sprintf("%4d  %-24s %s", r.id, r.sel, where))
	}
	return out
}
