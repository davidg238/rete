// Copyright (c) 2026 Ekorau LLC

package console

// read.go — the reader over a board's day files (D16): the lines since a time,
// the runs (one per boot), and the panics in a run. All of the parsing is here,
// and none of it is in the collector, so it re-runs over history.

import (
	"bufio"
	"fmt"
	"os"
	"regexp"
	"strconv"
	"time"
)

// Entry is one stamped line.
type Entry struct {
	When time.Time
	Text string
}

var stamp = regexp.MustCompile(`^(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}[+-]\d{2}:\d{2}) (.*)$`)

// ParseLine splits a stored line into its stamp and text; a line the store
// did not write does not parse.
func ParseLine(line string) (Entry, bool) {
	m := stamp.FindStringSubmatch(line)
	if m == nil {
		return Entry{}, false
	}
	when, err := time.Parse("2006-01-02T15:04:05.000-07:00", m[1])
	if err != nil {
		return Entry{}, false
	}
	return Entry{when, m[2]}, true
}

// Read answers every line stamped at or after since (zero = all), oldest first.
func (s *Store) Read(since time.Time) ([]Entry, error) {
	days, err := s.Days()
	if err != nil {
		return nil, err
	}
	var out []Entry
	for _, d := range days {
		if !since.IsZero() && d.AddDate(0, 0, 1).Before(since) {
			continue
		}
		f, err := os.Open(s.Path(d))
		if err != nil {
			return nil, err
		}
		sc := bufio.NewScanner(f)
		sc.Buffer(make([]byte, 64<<10), 1<<20)
		for sc.Scan() {
			if e, ok := ParseLine(sc.Text()); ok && !e.When.Before(since) {
				out = append(out, e)
			}
		}
		err = sc.Err()
		f.Close()
		if err != nil {
			return nil, err
		}
	}
	return out, nil
}

var relative = regexp.MustCompile(`^-(\d+)([sm])$`)

// ResolveSince reads a since spec: -<N>s or -<N>m before now, or a time
// (RFC 3339, or without an offset in local time).
func ResolveSince(spec string, now time.Time) (time.Time, error) {
	if m := relative.FindStringSubmatch(spec); m != nil {
		n, _ := strconv.Atoi(m[1])
		unit := time.Second
		if m[2] == "m" {
			unit = time.Minute
		}
		return now.Add(-time.Duration(n) * unit), nil
	}
	if t, err := time.Parse(time.RFC3339Nano, spec); err == nil {
		return t, nil
	}
	for _, layout := range []string{"2006-01-02T15:04:05.999999999", "2006-01-02T15:04"} {
		if t, err := time.ParseInLocation(layout, spec, time.Local); err == nil {
			return t, nil
		}
	}
	return time.Time{}, fmt.Errorf("since %q: want -<N>s, -<N>m, or a time like 2026-10-06T17:00:00", spec)
}

var (
	ansi    = regexp.MustCompile(`\x1b\[[0-9;]*m`)
	banner  = regexp.MustCompile(`\*\*\* Booting Zephyr OS`)
	buildID = regexp.MustCompile(`(?:tuvm|oevm): build-id (\S+)`) // tuvm: before the 2026-08-24 rename (#254)
	fatal   = regexp.MustCompile(`ZEPHYR FATAL ERROR`)
	cdBegin = regexp.MustCompile(`#CD:BEGIN#`)
	cdEnd   = regexp.MustCompile(`#CD:END#`)
	cdLine  = regexp.MustCompile(`#CD:([0-9a-fA-F]+)\s*$`)
)

// Run is one boot: from the kernel banner to the next. BuildID is "" when the
// run never printed one, and a symbolizer refuses it rather than guess.
type Run struct {
	Index   int // from 1, in the lines read
	Start   time.Time
	BuildID string
	Lines   []Entry
}

// Runs splits lines at the kernel banner, which every board prints (0.1 is
// portable by processor). On the C6 the ROM's lines come first, the reset
// reason among them, and they end the run they describe. Lines before the first
// banner are no run's.
func Runs(lines []Entry) []Run {
	var runs []Run
	for _, e := range lines {
		e.Text = ansi.ReplaceAllString(e.Text, "")
		if banner.MatchString(e.Text) {
			runs = append(runs, Run{Index: len(runs) + 1, Start: e.When})
		}
		if len(runs) == 0 {
			continue
		}
		r := &runs[len(runs)-1]
		r.Lines = append(r.Lines, e)
		if r.BuildID == "" {
			if m := buildID.FindStringSubmatch(e.Text); m != nil {
				r.BuildID = m[1]
			}
		}
	}
	return runs
}

// Panic is a fatal-error banner, a #CD: coredump block, or both. Coredump holds
// the block's hex lines, empty when the fault died before the backend ran.
type Panic struct {
	Start    time.Time
	Coredump []string
}

// Panics answers a run's panics, in order.
func Panics(r Run) []Panic {
	var out []Panic
	var cur *Panic
	in := false
	for _, e := range r.Lines {
		if fatal.MatchString(e.Text) {
			out = append(out, Panic{Start: e.When})
			cur = &out[len(out)-1]
		}
		switch {
		case cdBegin.MatchString(e.Text):
			in = true
			if cur == nil {
				out = append(out, Panic{Start: e.When})
				cur = &out[len(out)-1]
			}
		case cdEnd.MatchString(e.Text):
			in = false
		case in && cur != nil:
			if m := cdLine.FindStringSubmatch(e.Text); m != nil {
				cur.Coredump = append(cur.Coredump, m[1])
			}
		}
	}
	return out
}
