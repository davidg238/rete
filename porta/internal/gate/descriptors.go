// Copyright (c) 2026 Ekorau LLC

// Package gate is the custos query core: schema-aware where porta is
// schema-blind (design D3(b)). Everything that decodes a record lives here.
//
// descriptors.go loads protocol/descriptors/records.yaml — the reading rules for
// every wire field, and the ONLY authored input to this package's schema. The
// column list of every decoded record table comes from it, and so does the
// answer to "what does this number mean", which is the half a catalogue cannot
// supply. Completeness is gated against MANIFEST.cddl, not against this file.
package gate

import (
	"bytes"
	"fmt"
	"os"
	"sort"
	"strings"

	"gopkg.in/yaml.v3"

	"github.com/davidg238/porta/internal/cval"
	"github.com/davidg238/porta/internal/manifest"
)

// Vocabularies. Both are closed: an unknown value is an authoring error caught
// at load, not a string that reaches a materializer.
var (
	validKinds = map[string]bool{
		"identity":      true, // names a thing — a build, a span, a chip
		"config":        true, // an authored or declared setting, not a measurement
		"instantaneous": true, // sampled at publish; means nothing averaged blindly
		"cumulative":    true, // monotone since boot (or since the capture window)
		"high-water":    true, // max since boot; flat means "no new record", not "flat"
		"event":         true, // a fact about one occurrence
	}
	validUnits = map[string]bool{
		"bytes": true, "ms": true, "us": true, "seconds": true, "count": true,
		"symbol": true, "text": true, "opaque": true, "bool": true,
		"blobref": true, "any": true,
	}
	validStatus = map[string]bool{
		"live":      true, // something in this tree builds the record today
		"specified": true, // catalogued, no producer — silence at that path is expected
	}
)

// Field is one wire field's reading rule.
type Field struct {
	Key      string   `yaml:"key"`      // the symbol on the wire
	Name     string   `yaml:"name"`     // the long name a human reads
	Unit     string   `yaml:"unit"`     //
	Kind     string   `yaml:"kind"`     //
	Meaning  string   `yaml:"meaning"`  // required — what the value IS
	Validity string   `yaml:"validity"` // optional — how it lies if read naively
	Segment  []string `yaml:"segment"`  // what an aggregate must group by first
	Refs     []string `yaml:"refs"`     //
}

// Record is one CDDL rule — a record SHAPE, which may ride several paths.
type Record struct {
	Rule    string   `yaml:"-"`      // the map key; the shape's identity
	Name    string   `yaml:"record"` // the tag symbol on the wire (e.g. "Metrics")
	Paths   []string `yaml:"paths"`
	Status  string   `yaml:"status"`
	Summary string   `yaml:"summary"`
	Fields  []*Field `yaml:"fields"` // IN WIRE ORDER — gated against MANIFEST.cddl
}

// Scalar is a manifest row whose value is a bare scalar rather than a record.
// It has no columns, so it carries one meaning rather than a field list.
type Scalar struct {
	Rule     string   `yaml:"-"`
	Paths    []string `yaml:"paths"`
	Unit     string   `yaml:"unit"`
	Meaning  string   `yaml:"meaning"`
	Validity string   `yaml:"validity"`
	Refs     []string `yaml:"refs"`
}

// Descriptors is the loaded table.
type Descriptors struct {
	Version int                `yaml:"version"`
	Records map[string]*Record `yaml:"records"`
	Scalars map[string]*Scalar `yaml:"scalars"`
}

// LoadDescriptors reads and validates the descriptor table. Validation is
// deliberately strict — a descriptor that exists but says nothing is worse than
// one that is missing, because the gate would pass and the reader would still be
// stuck.
func LoadDescriptors(path string) (*Descriptors, error) {
	raw, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	var d Descriptors
	dec := yaml.NewDecoder(strings.NewReader(string(raw)))
	dec.KnownFields(true) // a misspelt attribute is an error, not a silent drop
	if err := dec.Decode(&d); err != nil {
		return nil, fmt.Errorf("%s: %w", path, err)
	}
	for rule, r := range d.Records {
		r.Rule = rule
	}
	for rule, s := range d.Scalars {
		s.Rule = rule
	}
	if err := d.validate(); err != nil {
		return nil, fmt.Errorf("%s: %w", path, err)
	}
	return &d, nil
}

func (d *Descriptors) validate() error {
	if d.Version != 1 {
		return fmt.Errorf("version: want 1, got %d", d.Version)
	}
	if len(d.Records) == 0 {
		return fmt.Errorf("no records")
	}
	seenPath := map[string]string{} // path -> rule, so one path cannot have two shapes
	claim := func(rule, path string) error {
		if prev, ok := seenPath[path]; ok {
			return fmt.Errorf("path %q claimed by both %q and %q", path, prev, rule)
		}
		seenPath[path] = rule
		return nil
	}
	for _, rule := range d.RecordRules() {
		r := d.Records[rule]
		if r.Name == "" {
			return fmt.Errorf("record %q: no record tag name", rule)
		}
		if !validStatus[r.Status] {
			return fmt.Errorf("record %q: status %q not in {live, specified}", rule, r.Status)
		}
		if strings.TrimSpace(r.Summary) == "" {
			return fmt.Errorf("record %q: empty summary", rule)
		}
		if len(r.Paths) == 0 {
			return fmt.Errorf("record %q: no paths", rule)
		}
		for _, p := range r.Paths {
			if err := claim(rule, p); err != nil {
				return err
			}
		}
		seenKey := map[string]bool{}
		for _, f := range r.Fields {
			switch {
			case f.Key == "":
				return fmt.Errorf("record %q: a field has no key", rule)
			case seenKey[f.Key]:
				return fmt.Errorf("record %q: duplicate field %q", rule, f.Key)
			case f.Name == "":
				return fmt.Errorf("record %q field %q: no name", rule, f.Key)
			case strings.TrimSpace(f.Meaning) == "":
				return fmt.Errorf("record %q field %q: empty meaning", rule, f.Key)
			case !validUnits[f.Unit]:
				return fmt.Errorf("record %q field %q: unit %q not in the vocabulary", rule, f.Key, f.Unit)
			case !validKinds[f.Kind]:
				return fmt.Errorf("record %q field %q: kind %q not in the vocabulary", rule, f.Key, f.Kind)
			}
			seenKey[f.Key] = true
		}
	}
	for _, rule := range d.ScalarRules() {
		s := d.Scalars[rule]
		if len(s.Paths) == 0 {
			return fmt.Errorf("scalar %q: no paths", rule)
		}
		if !validUnits[s.Unit] {
			return fmt.Errorf("scalar %q: unit %q not in the vocabulary", rule, s.Unit)
		}
		if strings.TrimSpace(s.Meaning) == "" {
			return fmt.Errorf("scalar %q: empty meaning", rule)
		}
		for _, p := range s.Paths {
			if err := claim(rule, p); err != nil {
				return err
			}
		}
	}
	return nil
}

// RecordRules answers every record rule in a deterministic order.
func (d *Descriptors) RecordRules() []string { return sortedKeysRecords(d.Records) }

// ScalarRules answers every scalar rule in a deterministic order.
func (d *Descriptors) ScalarRules() []string { return sortedKeysScalars(d.Scalars) }

func sortedKeysRecords(m map[string]*Record) []string {
	out := make([]string, 0, len(m))
	for k := range m {
		out = append(out, k)
	}
	sort.Strings(out)
	return out
}

func sortedKeysScalars(m map[string]*Scalar) []string {
	out := make([]string, 0, len(m))
	for k := range m {
		out = append(out, k)
	}
	sort.Strings(out)
	return out
}

// Columns answers a record shape's field keys in wire order — the column list of
// every table built from it. Nil for an unknown rule.
func (d *Descriptors) Columns(rule string) []string {
	r, ok := d.Records[rule]
	if !ok {
		return nil
	}
	out := make([]string, len(r.Fields))
	for i, f := range r.Fields {
		out[i] = f.Key
	}
	return out
}

// Note answers one field's reading rule — the lookup a decoded row's annotation
// goes through. Nil when the rule or the key is unknown.
func (d *Descriptors) Note(rule, key string) *Field {
	r, ok := d.Records[rule]
	if !ok {
		return nil
	}
	for _, f := range r.Fields {
		if f.Key == key {
			return f
		}
	}
	return nil
}

// CheckCompleteness compares the descriptor table against MANIFEST.cddl and
// answers every disagreement — D5 made honest. The authority is the manifest,
// which is machine-normative for shape and whose key order IS wire order; it is
// deliberately NOT the nsl record builders, whose source order is not wire order
// and three of whose records are not built in kernel.nsl at all (design A1).
//
// It catches: a wire field with no descriptor, a descriptor for a field that is
// not on the wire, a field-order disagreement, a manifest path no descriptor
// claims, a descriptor claiming a path the manifest does not have, an orphan
// descriptor rule, and a descriptor naming the wrong record tag.
//
// A function rather than a test body so the negative controls can drive it with
// deliberately-drifted tables — a gate whose red has never been witnessed is not
// a gate.
func CheckCompleteness(tbl *manifest.Table, d *Descriptors) []error {
	var errs []error
	fail := func(format string, a ...any) { errs = append(errs, fmt.Errorf(format, a...)) }

	claimed := map[string]bool{}
	for _, rule := range d.RecordRules() {
		for _, p := range d.Records[rule].Paths {
			claimed[p] = true
		}
	}
	for _, rule := range d.ScalarRules() {
		for _, p := range d.Scalars[rule].Paths {
			claimed[p] = true
		}
	}

	seenRule := map[string]bool{}
	for _, e := range tbl.Entries() {
		path := e.Path()
		seenRule[e.Rule] = true
		if !claimed[path] {
			fail("%s (rule %q): no descriptor claims this path", path, e.Rule)
		}
		delete(claimed, path)

		if e.Head == nil { // a scalar row
			s, ok := d.Scalars[e.Rule]
			if !ok {
				fail("%s: scalar rule %q has no descriptor", path, e.Rule)
				continue
			}
			if !contains(s.Paths, path) {
				fail("%s: scalar descriptor %q does not list this path", path, e.Rule)
			}
			continue
		}

		r, ok := d.Records[e.Rule]
		if !ok {
			fail("%s: record rule %q has no descriptor", path, e.Rule)
			continue
		}
		if !contains(r.Paths, path) {
			fail("%s: record descriptor %q does not list this path", path, e.Rule)
		}
		// The descriptor names a record tag; the manifest pinned that tag's
		// encoded head. A descriptor pointing at the wrong shape is caught here
		// rather than by a materializer decoding into the wrong columns.
		head, err := cval.Encode(cval.Record{Tag: cval.Symbol(r.Name)})
		if err != nil {
			fail("%s: encoding tag %q: %v", path, r.Name, err)
			continue
		}
		head = head[:len(head)-1] // drop the empty-map byte
		if !bytes.Equal(head, e.Head) {
			fail("%s: descriptor says record %q (head %x), manifest head is %x",
				path, r.Name, head, e.Head)
		}
		if got := d.Columns(e.Rule); !equalStrings(got, e.Fields) {
			fail("%s: field list disagrees with MANIFEST.cddl (wire order): manifest %v, descriptor %v",
				path, e.Fields, got)
		}
	}

	for _, p := range sortedSet(claimed) {
		fail("descriptor claims path %q, which is not in MANIFEST.cddl", p)
	}
	for _, rule := range d.RecordRules() {
		if !seenRule[rule] {
			fail("descriptor record %q matches no manifest row", rule)
		}
	}
	for _, rule := range d.ScalarRules() {
		if !seenRule[rule] {
			fail("descriptor scalar %q matches no manifest row", rule)
		}
	}
	return errs
}

func contains(ss []string, s string) bool {
	for _, x := range ss {
		if x == s {
			return true
		}
	}
	return false
}

func equalStrings(a, b []string) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

func sortedSet(m map[string]bool) []string {
	out := make([]string, 0, len(m))
	for k := range m {
		out = append(out, k)
	}
	sort.Strings(out)
	return out
}

// TableName derives a decoded record table's name from a manifest path:
// "sys/metrics" -> "sys_metrics", "diag/spans/<span>" -> "diag_spans". Derived
// and never authored, so a table name cannot drift from the path it holds.
func TableName(path string) string {
	segs := strings.Split(path, "/")
	out := make([]string, 0, len(segs))
	for _, s := range segs {
		if strings.HasPrefix(s, "<") {
			continue // a wildcard is a row's key, not part of the table's identity
		}
		out = append(out, s)
	}
	return strings.Join(out, "_")
}
