// Copyright (c) 2026 Ekorau LLC

package gate

import (
	"bytes"
	"os"
	"path/filepath"
	"runtime"
	"sort"
	"strings"
	"testing"

	"github.com/davidg238/porta/internal/cval"
	"github.com/davidg238/porta/internal/manifest"
)

// repoFile answers a path under the repo root, from this file's own location.
func repoFile(t *testing.T, rel ...string) string {
	t.Helper()
	_, this, _, _ := runtime.Caller(0)
	parts := append([]string{filepath.Dir(this), "..", "..", ".."}, rel...)
	return filepath.Join(parts...)
}

func loadBoth(t *testing.T) (*manifest.Table, *Descriptors) {
	t.Helper()
	tbl, err := manifest.Load(repoFile(t, "protocol", "MANIFEST.cddl"))
	if err != nil {
		t.Fatalf("manifest: %v", err)
	}
	d, err := LoadDescriptors(repoFile(t, "protocol", "descriptors", "records.yaml"))
	if err != nil {
		t.Fatalf("descriptors: %v", err)
	}
	return tbl, d
}

// TestDescriptorsCoverManifest is the completeness gate itself: the shipped
// descriptor table against the shipped manifest.
func TestDescriptorsCoverManifest(t *testing.T) {
	tbl, d := loadBoth(t)
	for _, err := range CheckCompleteness(tbl, d) {
		t.Error(err)
	}
}

// TestCompletenessGateGoesRed is the negative control, and it is the half that
// makes the test above worth anything. Each case drifts the real table in one
// way a real edit drifts it, and the gate must see it. A gate whose red has
// never been witnessed is a gate that has never been shown to check anything.
func TestCompletenessGateGoesRed(t *testing.T) {
	cases := []struct {
		name   string
		mutate func(*Descriptors)
		want   string
	}{
		{
			name: "a wire field loses its descriptor",
			mutate: func(d *Descriptors) {
				r := d.Records["metrics"]
				r.Fields = r.Fields[:len(r.Fields)-1] // drop pst's neighbour at the tail
			},
			want: "field list disagrees",
		},
		{
			name: "a descriptor invents a field the wire does not carry",
			mutate: func(d *Descriptors) {
				r := d.Records["health"]
				r.Fields = append(r.Fields, &Field{Key: "zzz", Name: "zzz"})
			},
			want: "field list disagrees",
		},
		{
			name: "two fields are transposed (the builder-order mistake)",
			mutate: func(d *Descriptors) {
				r := d.Records["metrics"]
				r.Fields[0], r.Fields[1] = r.Fields[1], r.Fields[0]
			},
			want: "field list disagrees",
		},
		{
			name: "a descriptor names the wrong record tag",
			mutate: func(d *Descriptors) {
				d.Records["cheap"].Name = "Metrics"
			},
			want: "manifest head is",
		},
		{
			name: "a path in the manifest is claimed by nobody",
			mutate: func(d *Descriptors) {
				r := d.Records["latency-span"]
				r.Paths = []string{"sys/latency"} // drops diag/spans/<span>
			},
			want: "no descriptor claims this path",
		},
		{
			name: "a descriptor claims a path the manifest does not have",
			mutate: func(d *Descriptors) {
				r := d.Records["health"]
				r.Paths = append(r.Paths, "obs/healthz")
			},
			want: "not in MANIFEST.cddl",
		},
		{
			name: "a whole record shape is dropped",
			mutate: func(d *Descriptors) {
				delete(d.Records, "build")
			},
			want: "has no descriptor",
		},
		{
			name: "an orphan descriptor for a shape the wire retired",
			mutate: func(d *Descriptors) {
				d.Records["ghost"] = &Record{Rule: "ghost", Name: "Ghost", Paths: []string{"obs/ghost"}}
			},
			want: "matches no manifest row",
		},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			tbl, d := loadBoth(t)
			tc.mutate(d)
			errs := CheckCompleteness(tbl, d)
			if len(errs) == 0 {
				t.Fatalf("gate stayed GREEN under: %s", tc.name)
			}
			joined := joinErrs(errs)
			if !strings.Contains(joined, tc.want) {
				t.Errorf("gate went red for the wrong reason\n  want substring: %s\n  got: %s", tc.want, joined)
			}
		})
	}
}

// TestDescriptorOrderIsWireOrder checks the property the manifest's header
// ASSERTS rather than proves: that its key order is what cval.Encode's bytewise
// sort over encoded key bytes actually produces. Without this, a mis-authored
// CDDL would hand the completeness gate a wrong order and be agreed with — the
// two documents would be consistent and both wrong.
func TestDescriptorOrderIsWireOrder(t *testing.T) {
	_, d := loadBoth(t)
	for _, rule := range d.RecordRules() {
		keys := d.Columns(rule)
		want := append([]string(nil), keys...)
		enc := map[string][]byte{}
		for _, k := range keys {
			b, err := cval.Encode(cval.Symbol(k))
			if err != nil {
				t.Fatalf("%s: encoding key %q: %v", rule, k, err)
			}
			enc[k] = b
		}
		sort.SliceStable(want, func(i, j int) bool {
			return bytes.Compare(enc[want[i]], enc[want[j]]) < 0
		})
		if !equalStrings(keys, want) {
			t.Errorf("record %q: fields are not in wire order\n  authored: %v\n  wire:     %v", rule, keys, want)
		}
	}
}

// TestLoaderRefusesAnEmptyDescriptor: the vocabulary and the required attributes
// are closed, because a descriptor that exists and says nothing passes the
// completeness gate while leaving the reader exactly as stuck.
func TestLoaderRefusesAnEmptyDescriptor(t *testing.T) {
	cases := map[string]string{
		"unknown unit": `version: 1
records:
  r:
    record: R
    status: live
    summary: s
    paths: [a/b]
    fields:
      - {key: k, name: k, unit: furlongs, kind: cumulative, meaning: m}
`,
		"unknown kind": `version: 1
records:
  r:
    record: R
    status: live
    summary: s
    paths: [a/b]
    fields:
      - {key: k, name: k, unit: bytes, kind: vibes, meaning: m}
`,
		"empty meaning": `version: 1
records:
  r:
    record: R
    status: live
    summary: s
    paths: [a/b]
    fields:
      - {key: k, name: k, unit: bytes, kind: cumulative, meaning: "  "}
`,
		"unknown status": `version: 1
records:
  r:
    record: R
    status: probably
    summary: s
    paths: [a/b]
    fields: []
`,
		"two shapes claim one path": `version: 1
records:
  r:
    record: R
    status: live
    summary: s
    paths: [a/b]
    fields: []
  r2:
    record: R2
    status: live
    summary: s
    paths: [a/b]
    fields: []
`,
		"a misspelt attribute": `version: 1
records:
  r:
    record: R
    status: live
    summary: s
    paths: [a/b]
    fields:
      - {key: k, name: k, unit: bytes, kind: cumulative, meaning: m, validty: oops}
`,
	}
	for name, body := range cases {
		t.Run(name, func(t *testing.T) {
			p := filepath.Join(t.TempDir(), "records.yaml")
			if err := os.WriteFile(p, []byte(body), 0o644); err != nil {
				t.Fatal(err)
			}
			if _, err := LoadDescriptors(p); err == nil {
				t.Fatalf("loader accepted: %s", name)
			}
		})
	}
}

// TestTableName pins the derivation, since it is the one name a query surface
// exposes that nobody authored.
func TestTableName(t *testing.T) {
	cases := map[string]string{
		"sys/metrics":             "sys_metrics",
		"obs/health":              "obs_health",
		"tel/log":                 "tel_log",
		"diag/spans/<span>":       "diag_spans",
		"tel/m/<name>":            "tel_m",
		"goal/config/<app>/<key>": "goal_config",
		"goal/do/reboot":          "goal_do_reboot",
	}
	for in, want := range cases {
		if got := TableName(in); got != want {
			t.Errorf("TableName(%q) = %q, want %q", in, got, want)
		}
	}
}

// TestEveryLiveRecordHasReadingRules is the D4 half: a status:live record whose
// fields carry no validity rule anywhere is a table that answers with numbers and
// no way to read them. One rule per record is a floor, not a target.
func TestEveryLiveRecordHasReadingRules(t *testing.T) {
	_, d := loadBoth(t)
	for _, rule := range d.RecordRules() {
		r := d.Records[rule]
		if r.Status != "live" || len(r.Fields) == 0 {
			continue
		}
		found := false
		for _, f := range r.Fields {
			if f.Validity != "" {
				found = true
				break
			}
		}
		if !found {
			t.Errorf("record %q is live and carries no validity rule on any field", rule)
		}
	}
}

// TestNoteAndColumns exercises the two lookups the materializer and the
// annotator go through.
func TestNoteAndColumns(t *testing.T) {
	_, d := loadBoth(t)
	if got := d.Columns("health"); len(got) != 7 || got[0] != "up" {
		t.Errorf("obs/health columns = %v", got)
	}
	f := d.Note("metrics", "lfb")
	if f == nil {
		t.Fatal("no note for sys/metrics lfb")
	}
	if f.Name != "largestFreeBlock" || f.Unit != "bytes" {
		t.Errorf("lfb note = %+v", f)
	}
	if !strings.Contains(f.Validity, "INVALID as a point sample") {
		t.Errorf("lfb lost its reading rule: %q", f.Validity)
	}
	if d.Note("metrics", "nope") != nil || d.Note("nope", "lfb") != nil {
		t.Error("Note answered for an unknown rule or key")
	}
}

func joinErrs(errs []error) string {
	parts := make([]string, len(errs))
	for i, e := range errs {
		parts[i] = e.Error()
	}
	return strings.Join(parts, "; ")
}
