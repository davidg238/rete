// Copyright (c) 2026 Ekorau LLC

// Package manifest parses protocol/MANIFEST.cddl's restricted subset — the
// ~100-line parser rete#243's design input 5 budgeted — into a per-path check
// table enforced at the store's three write doors (space.Store: Write,
// WriteGateway, Ingest).
//
// It reads the ;# annotation lines (path → kind, class, dir) and just enough
// of each rule head to derive a cheap byte-level check: record rows carry the
// deterministic encoding's constant prefix (tag 27, array 2, tag 39, name —
// computable because the wire is deterministic), scalar rows carry a
// first-byte class set. Deep shape validation stays at the zcbor gates; a
// value here is checked without ever being decoded, so the store stays opaque
// (rewrite design D4.1).
package manifest

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"os"
	"regexp"
	"sort"
	"strings"

	"github.com/davidg238/porta/internal/cval"
)

// Dir is who writes a path, from the node's point of view: the node (up, the
// wire's push leg), porta (down, Write / `porta put`), or porta's own verdicts
// about a node (gw, WriteGateway — never on the wire).
type Dir int

const (
	DirUp Dir = iota
	DirDown
	DirGw
)

func (d Dir) String() string {
	switch d {
	case DirUp:
		return "up"
	case DirDown:
		return "down"
	default:
		return "gw"
	}
}

// first-byte classes for scalar rows
const (
	classInt = 1 << iota
	classTstr
	classBool
	classSym
	classBstr
	classFloat
	classNil
)

// Entry is one annotated manifest row, compiled for checking.
type Entry struct {
	Pattern []string // path segments; "<...>" is a one-segment wildcard
	Kind    cval.Kind
	Class   string
	Dir     Dir
	Rule    string
	Head    []byte   // record rows: required value prefix; nil for scalar rows
	Fields  []string // record rows: field key literals IN WIRE ORDER (excludes the tag); nil for scalar rows
	scalars int      // scalar rows: allowed first-byte class mask
}

// Table is the compiled manifest. A nil *Table gates nothing — every check
// answers nil — so a store without a manifest behaves exactly as before.
type Table struct {
	Raw     []byte
	SHA256  string
	literal map[string]*Entry
	wild    []*Entry
}

// Path renders an entry's pattern back to a path string (wildcard segments kept
// as written, e.g. "obs/apps/<name>"). The custos descriptor table keys on
// this.
func (e *Entry) Path() string { return strings.Join(e.Pattern, "/") }

// Entries returns every compiled row (literal paths, then wildcard patterns) in a
// deterministic order. The custos query core walks these to build its schema
// and to gate descriptor completeness against the wire-order authority.
func (t *Table) Entries() []*Entry {
	if t == nil {
		return nil
	}
	keys := make([]string, 0, len(t.literal))
	for k := range t.literal {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	out := make([]*Entry, 0, len(t.literal)+len(t.wild))
	for _, k := range keys {
		out = append(out, t.literal[k])
	}
	out = append(out, t.wild...)
	return out
}

var (
	annotRe = regexp.MustCompile(`^;#\s+(.*)$`)
	symRe   = regexp.MustCompile(`^(sym-[A-Za-z0-9-]+)\s*=\s*#6\.39\("([^"]+)"\)\s*$`)
	ruleRe  = regexp.MustCompile(`^([a-z][a-z0-9-]*)\s*=\s*(.*)$`)
	recRe   = regexp.MustCompile(`^#6\.27\(\[\s*(sym-[A-Za-z0-9-]+)\s*,`)
)

var kinds = map[string]cval.Kind{"cell": cval.KindCell, "stream": cval.KindStream}
var classes = map[string]bool{"routine": true, "must-deliver": true, "drop-first": true}
var dirs = map[string]Dir{"up": DirUp, "down": DirDown, "gw": DirGw}

func bracketDelta(s string) int {
	return strings.Count(s, "[") + strings.Count(s, "{") + strings.Count(s, "(") -
		strings.Count(s, "]") - strings.Count(s, "}") - strings.Count(s, ")")
}

type annot struct {
	path, class string
	kind        cval.Kind
	dir         Dir
}

func parseAnnot(s string, line int) (annot, error) {
	var a annot
	got := map[string]string{}
	for _, tok := range strings.Fields(s) {
		k, v, ok := strings.Cut(tok, "=")
		if !ok {
			return a, fmt.Errorf("line %d: malformed annotation token %q", line, tok)
		}
		got[k] = v
	}
	for _, req := range []string{"path", "kind", "class", "dir"} {
		if got[req] == "" {
			return a, fmt.Errorf("line %d: annotation missing %s=", line, req)
		}
	}
	kind, ok := kinds[got["kind"]]
	if !ok {
		return a, fmt.Errorf("line %d: unknown kind %q", line, got["kind"])
	}
	if !classes[got["class"]] {
		return a, fmt.Errorf("line %d: unknown class %q", line, got["class"])
	}
	dir, ok := dirs[got["dir"]]
	if !ok {
		return a, fmt.Errorf("line %d: unknown dir %q", line, got["dir"])
	}
	a.path, a.kind, a.class, a.dir = got["path"], kind, got["class"], dir
	return a, nil
}

// recordHead answers the constant prefix every encoding of a record named
// `name` starts with: the record's own encoding minus the trailing empty map
// byte (0xa0). Deterministic encoding is what makes this a constant.
func recordHead(name string) ([]byte, error) {
	enc, err := cval.Encode(cval.Record{Tag: cval.Symbol(name)})
	if err != nil {
		return nil, err
	}
	if len(enc) == 0 || enc[len(enc)-1] != 0xa0 {
		return nil, fmt.Errorf("record head derivation broke: %x", enc)
	}
	return enc[:len(enc)-1], nil
}

// fieldKeyRe finds the map-key symbol references in a record body: `sym-X =>`.
// It matches only keys (left of `=>`), so a `=> anysym` value type is never
// mistaken for a field. The tag sym (before the `{`) has no `=>` and is excluded
// by construction.
var fieldKeyRe = regexp.MustCompile(`(sym-[A-Za-z0-9-]+)\s*=>`)

// recordFields returns a record body's field key literals IN WIRE ORDER — which,
// because the wire sorts map keys bytewise (cval.Encode), is exactly the order
// MANIFEST.cddl lists them (its header declares key order == wire order). The
// tag is excluded. This makes the manifest the single wire-order authority the
// custos descriptor table checks against — NOT the nsl record builders,
// whose source order is not wire order.
func recordFields(body string, syms map[string]string) ([]string, error) {
	var out []string
	for _, m := range fieldKeyRe.FindAllStringSubmatch(body, -1) {
		lit, ok := syms[m[1]]
		if !ok {
			return nil, fmt.Errorf("references undefined %s", m[1])
		}
		out = append(out, lit)
	}
	return out, nil
}

// scalarMask resolves a scalar-choice rule body ("int / tstr / anysym") to a
// first-byte class mask, following named-rule references one level at a time.
func scalarMask(body string, rules map[string]string, depth int) (int, error) {
	if depth > 4 {
		return 0, fmt.Errorf("scalar rule nesting too deep in %q", body)
	}
	mask := 0
	for _, alt := range strings.Split(body, "/") {
		tok := strings.TrimSpace(alt)
		switch {
		case tok == "int" || tok == "uint":
			mask |= classInt
		case tok == "tstr":
			mask |= classTstr
		case tok == "bool":
			mask |= classBool
		case tok == "bstr":
			mask |= classBstr
		case tok == "float":
			mask |= classFloat
		case tok == "nil":
			mask |= classNil
		case strings.HasPrefix(tok, "#6.39("):
			mask |= classSym // open or literal symbol — same first bytes
		default:
			ref, ok := rules[tok]
			if !ok {
				return 0, fmt.Errorf("scalar rule %q: unresolvable token %q", body, tok)
			}
			m, err := scalarMask(ref, rules, depth+1)
			if err != nil {
				return 0, err
			}
			mask |= m
		}
	}
	return mask, nil
}

func firstByteClass(v []byte) int {
	if len(v) == 0 {
		return 0
	}
	b := v[0]
	switch {
	case b <= 0x1b || (b >= 0x20 && b <= 0x3b):
		return classInt
	case b >= 0x40 && b <= 0x5b:
		return classBstr
	case b >= 0x60 && b <= 0x7b:
		return classTstr
	case b == 0xf4 || b == 0xf5:
		return classBool
	case b == 0xf6:
		return classNil
	case b >= 0xf9 && b <= 0xfb:
		return classFloat
	case b == 0xd8 && len(v) > 1 && v[1] == 0x27:
		return classSym
	}
	return 0
}

// Load reads and compiles MANIFEST.cddl. Every parse defect is an error —
// the manifest is a contract, and a half-read contract must not gate.
func Load(path string) (*Table, error) {
	raw, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	t, err := Parse(raw)
	if err != nil {
		return nil, fmt.Errorf("%s: %w", path, err)
	}
	return t, nil
}

// Parse compiles manifest bytes. Split from Load for tests and for serving.
func Parse(raw []byte) (*Table, error) {
	sum := sha256.Sum256(raw)
	t := &Table{Raw: raw, SHA256: hex.EncodeToString(sum[:]),
		literal: map[string]*Entry{}}
	syms := map[string]string{}
	rules := map[string]string{}
	type pendingRule struct {
		annots []annot
		name   string
	}
	var ruleAnnots []pendingRule
	var pending []annot

	lines := strings.Split(string(raw), "\n")
	for i := 0; i < len(lines); i++ {
		line := strings.TrimRight(lines[i], " \t")
		lineno := i + 1
		if m := annotRe.FindStringSubmatch(line); m != nil {
			a, err := parseAnnot(m[1], lineno)
			if err != nil {
				return nil, err
			}
			pending = append(pending, a)
			continue
		}
		if line == "" || strings.HasPrefix(strings.TrimSpace(line), ";") {
			continue
		}
		if m := symRe.FindStringSubmatch(line); m != nil {
			if len(pending) > 0 {
				return nil, fmt.Errorf("line %d: annotation may not bind a sym-* rule", lineno)
			}
			syms[m[1]] = m[2]
			continue
		}
		m := ruleRe.FindStringSubmatch(line)
		if m == nil {
			return nil, fmt.Errorf("line %d: not a rule, annotation, or comment: %q", lineno, line)
		}
		name, body := m[1], m[2]
		depth := bracketDelta(body)
		for depth > 0 && i+1 < len(lines) {
			i++
			body += " " + strings.TrimSpace(lines[i])
			depth += bracketDelta(lines[i])
		}
		rules[name] = body
		if len(pending) > 0 {
			ruleAnnots = append(ruleAnnots, pendingRule{annots: pending, name: name})
			pending = nil
		}
	}
	if len(pending) > 0 {
		return nil, fmt.Errorf("annotation for %q has no rule beneath it", pending[0].path)
	}

	for _, pr := range ruleAnnots {
		body := rules[pr.name]
		var head []byte
		var fields []string
		mask := 0
		if m := recRe.FindStringSubmatch(body); m != nil {
			lit, ok := syms[m[1]]
			if !ok {
				return nil, fmt.Errorf("rule %s references undefined %s", pr.name, m[1])
			}
			h, err := recordHead(lit)
			if err != nil {
				return nil, err
			}
			head = h
			f, err := recordFields(body, syms)
			if err != nil {
				return nil, fmt.Errorf("rule %s: %w", pr.name, err)
			}
			fields = f
		} else {
			m, err := scalarMask(body, rules, 0)
			if err != nil {
				return nil, fmt.Errorf("rule %s: %w", pr.name, err)
			}
			mask = m
		}
		for _, a := range pr.annots {
			e := &Entry{Pattern: strings.Split(a.path, "/"), Kind: a.kind,
				Class: a.class, Dir: a.dir, Rule: pr.name, Head: head, Fields: fields, scalars: mask}
			if strings.Contains(a.path, "<") {
				t.wild = append(t.wild, e)
			} else {
				t.literal[a.path] = e
			}
		}
	}
	return t, nil
}

func (t *Table) match(path string) *Entry {
	if e, ok := t.literal[path]; ok {
		return e
	}
	segs := strings.Split(path, "/")
	for _, e := range t.wild {
		if len(e.Pattern) != len(segs) {
			continue
		}
		ok := true
		for j, p := range e.Pattern {
			if !strings.HasPrefix(p, "<") && p != segs[j] {
				ok = false
				break
			}
		}
		if ok {
			return e
		}
	}
	return nil
}

// Check is the gate: known path, agreeing kind, agreeing head bytes, agreeing
// direction — in that order, each failing with a named reason. A nil receiver
// checks nothing (no manifest loaded = gate off, loudly noted at serve).
func (t *Table) Check(path string, kind cval.Kind, dir Dir, value []byte) error {
	if t == nil {
		return nil
	}
	e := t.match(path)
	if e == nil {
		return fmt.Errorf("manifest:path %q matches no manifest entry", path)
	}
	if kind != e.Kind {
		return fmt.Errorf("manifest:kind path=%s got=%d want=%d (rule %s)", path, kind, e.Kind, e.Rule)
	}
	if e.Head != nil {
		if !bytes.HasPrefix(value, e.Head) {
			return fmt.Errorf("manifest:head path=%s value does not start with %s's record head (rule %s)",
				path, e.Rule, e.Rule)
		}
	} else if e.scalars != 0 {
		if firstByteClass(value)&e.scalars == 0 {
			return fmt.Errorf("manifest:head path=%s first byte %#02x outside rule %s's scalar classes",
				path, first(value), e.Rule)
		}
	}
	if dir != e.Dir {
		return fmt.Errorf("manifest:dir path=%s written from %s, manifest says %s", path, dir, e.Dir)
	}
	return nil
}

func first(v []byte) byte {
	if len(v) == 0 {
		return 0
	}
	return v[0]
}
