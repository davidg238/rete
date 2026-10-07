// Copyright (c) 2026 Ekorau LLC

package console

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// The fixtures are real captures, kept with the reader that parses them.
func fixture(t *testing.T, name string) []Entry {
	t.Helper()
	data, err := os.ReadFile(filepath.Join("testdata", name))
	if err != nil {
		t.Fatal(err)
	}
	var out []Entry
	for _, l := range strings.Split(strings.TrimSpace(string(data)), "\n") {
		e, ok := ParseLine(l)
		if !ok {
			t.Fatalf("unstamped fixture line %q", l)
		}
		out = append(out, e)
	}
	return out
}

func TestParseLineSplitsTheStampFromTheText(t *testing.T) {
	e, ok := ParseLine("2026-08-03T09:14:03.412-07:00 hello world")
	if !ok || e.Text != "hello world" || e.When.Year() != 2026 || e.When.Nanosecond() != 412e6 {
		t.Fatalf("%v %+v", ok, e)
	}
	if _, ok := ParseLine("no stamp here"); ok {
		t.Fatal("an unstamped line parsed")
	}
}

func TestReadAnswersEveryDayFromSince(t *testing.T) {
	s := &Store{Root: t.TempDir(), Board: "c6"}
	d1 := time.Date(2026, 10, 5, 23, 59, 0, 0, time.Local)
	s.Append("yesterday", d1)
	s.Append("today", d1.Add(2*time.Minute))
	s.Append("later", d1.Add(4*time.Minute))
	all, err := s.Read(time.Time{})
	if err != nil || len(all) != 3 || all[0].Text != "yesterday" || all[2].Text != "later" {
		t.Fatalf("%v %+v", err, all)
	}
	some, _ := s.Read(d1.Add(time.Minute))
	if len(some) != 2 || some[0].Text != "today" {
		t.Fatalf("%+v", some)
	}
}

func TestResolveSince(t *testing.T) {
	now := time.Date(2026, 10, 6, 18, 0, 0, 0, time.Local)
	for spec, want := range map[string]time.Time{
		"-30s":                          now.Add(-30 * time.Second),
		"-5m":                           now.Add(-5 * time.Minute),
		"2026-10-06T17:00:00":           time.Date(2026, 10, 6, 17, 0, 0, 0, time.Local),
		"2026-10-06T17:00:00.250-07:00": time.Date(2026, 10, 7, 0, 0, 0, 250e6, time.UTC),
	} {
		got, err := ResolveSince(spec, now)
		if err != nil || !got.Equal(want) {
			t.Errorf("%s: %v %v, want %v", spec, got, err, want)
		}
	}
	for _, bad := range []string{"", "5m", "-5h", "yesterday"} {
		if _, err := ResolveSince(bad, now); err == nil {
			t.Errorf("%q resolved", bad)
		}
	}
}

// A run starts at the kernel banner, on every board (0.1 is portable by
// processor): the C6's ROM lines before it, and the reset reason among them,
// end the run they describe.
func TestOneBootIsOneRunWithItsBuildID(t *testing.T) {
	runs := Runs(fixture(t, "c6-boot.log"))
	if len(runs) != 1 || runs[0].BuildID != "porta-v1-final-779-gc76a6d5-dirty-20260803T201928" {
		t.Fatalf("%+v", runs)
	}
	if !strings.Contains(runs[0].Lines[0].Text, "*** Booting Zephyr OS") {
		t.Fatalf("the run starts at %q", runs[0].Lines[0].Text)
	}
}

func TestEachBannerIsARunAndTheBuildIDIsItsOwn(t *testing.T) {
	at := time.Date(2026, 10, 6, 0, 0, 0, 0, time.Local)
	runs := Runs([]Entry{
		{at, "before any banner"},
		{at, "*** Booting Zephyr OS build v4.2.0 ***"},
		{at, "oevm: build-id A"},
		{at, "*** Booting Zephyr OS build v4.2.0 ***"},
		{at, "E: >>> ZEPHYR FATAL ERROR 0: CPU exception on CPU 0"},
		{at, "tuvm: build-id B"}, // before the 2026-08-24 rename (#254)
	})
	if len(runs) != 2 || runs[0].Index != 1 || runs[0].BuildID != "A" || runs[1].BuildID != "B" || len(runs[1].Lines) != 3 {
		t.Fatalf("%+v", runs)
	}
}

func TestAFaultIsAPanicWithItsCoredump(t *testing.T) {
	runs := Runs(fixture(t, "c6-fault.log"))
	if len(runs) != 1 {
		t.Fatalf("%d runs", len(runs))
	}
	ps := Panics(runs[0])
	if len(ps) != 1 || len(ps[0].Coredump) == 0 {
		t.Fatalf("%+v", ps)
	}
	for _, l := range ps[0].Coredump {
		if strings.Trim(l, "0123456789abcdefABCDEF") != "" {
			t.Fatalf("a coredump line that is not hex: %q", l)
		}
	}
}

// A fault that dies before the coredump backend runs still leaves its banner,
// and that is a panic to list though it cannot be symbolized.
func TestABannerWithoutACoredumpIsAPanic(t *testing.T) {
	at := time.Now()
	ps := Panics(Run{Lines: []Entry{{at, "E: >>> ZEPHYR FATAL ERROR 3: Kernel oops"}}})
	if len(ps) != 1 || len(ps[0].Coredump) != 0 {
		t.Fatalf("%+v", ps)
	}
}
