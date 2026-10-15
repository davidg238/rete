// Copyright (c) 2026 Ekorau LLC

package profile

import (
	"errors"
	"strings"
	"testing"
	"time"
)

// fakeNode answers `profile <ms>` the way the kernel's Profiler does: the dump,
// then the end line, on its own goroutine as the cable's reader would.
type fakeNode struct {
	onLine func(string)
	onEnd  func()
	sent   []string
	reply  []string
	ends   bool // the program exits after its reply, as a host VM does
	closed bool
}

func (f *fakeNode) Close() error { f.closed = true; return nil }

func (f *fakeNode) Send(line string) error {
	f.sent = append(f.sent, line)
	go func() {
		for _, l := range f.reply {
			f.onLine(l)
		}
		if f.ends {
			f.onEnd()
		}
	}()
	return nil
}

func (f *fakeNode) open(before ...string) func(func(string), func()) (Sender, error) {
	return func(onLine func(string), onEnd func()) (Sender, error) {
		f.onLine, f.onEnd = onLine, onEnd
		for _, l := range before { // what the console printed before the request
			onLine(l)
		}
		return f, nil
	}
}

func TestCaptureSendsTheRequestAndStopsAtTheEndLine(t *testing.T) {
	n := &fakeNode{reply: []string{"tick 9",
		"mprof[window]: methods=1 activations=5 overflow=0 cap=256", "mprof: 4096 5",
		"mprof[window]: end", "mprof: 7 99"}}
	// An earlier window's rows on the console must not join this one.
	text, err := Capture(n.open("mprof: 7 1000"), 50, time.Second)
	if err != nil {
		t.Fatal(err)
	}
	if !n.closed {
		t.Fatal("the line was left open: the next capture cannot open an exclusive port")
	}
	if len(n.sent) != 1 || n.sent[0] != "profile 50" {
		t.Fatalf("sent %q", n.sent)
	}
	d := Parse(text)
	if len(d.Rows) != 1 || d.Rows[0] != (Row{"4096", 5}) {
		t.Fatalf("rows %v from\n%s", d.Rows, text)
	}
}

// rete#415, seen on c6b over Thread: a dump that lost its end line on the way is
// followed by the next window's; the capture answers that one alone, and keeps the
// kernel's cut line that came just before it.
func TestCaptureStartsAgainAtANewHeader(t *testing.T) {
	n := &fakeNode{reply: []string{
		"mprof[window]: methods=2 activations=9 overflow=0 cap=256", "mprof: 4096 5",
		"profile: window cut by sleep",
		"mprof[window]: methods=1 activations=3 overflow=0 cap=256", "mprof: 8192 3",
		"mprof[window]: end"}}
	text, err := Capture(n.open(), 50, time.Second)
	if err != nil {
		t.Fatal(err)
	}
	d := Parse(text)
	if len(d.Rows) != 1 || d.Rows[0] != (Row{"8192", 3}) || !d.Cut {
		t.Fatalf("rows %v cut %v from\n%s", d.Rows, d.Cut, text)
	}
}

func TestCaptureTimesOutNamingWhatItSaw(t *testing.T) {
	n := &fakeNode{reply: []string{"tick 9"}}
	_, err := Capture(n.open(), 50, 50*time.Millisecond)
	if err == nil || !strings.Contains(err.Error(), "mprof[window]: end") {
		t.Fatalf("err %v", err)
	}
}

// rete#364: a host VM whose program ends before the window closes fails the
// capture at once, not after the timeout an agent would sit through.
func TestCaptureFailsAtOnceWhenTheProgramEnds(t *testing.T) {
	n := &fakeNode{reply: []string{"tick 1", "done"}, ends: true}
	start := time.Now()
	_, err := Capture(n.open(), 50, 10*time.Second)
	if err == nil || !strings.Contains(err.Error(), "program ended before the window closed") {
		t.Fatalf("err %v", err)
	}
	if time.Since(start) > 2*time.Second {
		t.Fatalf("took %v: it waited for the timeout", time.Since(start))
	}
}

func TestCaptureReportsAFailedOpen(t *testing.T) {
	_, err := Capture(func(func(string), func()) (Sender, error) { return nil, errors.New("no such port") }, 50, time.Second)
	if err == nil || !strings.Contains(err.Error(), "no such port") {
		t.Fatalf("err %v", err)
	}
}

func TestPortWithoutMsIsUsage(t *testing.T) {
	code, _, errs := run(t, "", "-map", fixtureMap, "-port", "/dev/null")
	if code != 2 || !strings.Contains(errs, "-ms") {
		t.Fatalf("exit %d: %s", code, errs)
	}
}

func TestPortAndLogsAreExclusive(t *testing.T) {
	code, _, errs := run(t, "", "-map", fixtureMap, "-port", "/dev/null", "-ms", "50", "x.log")
	if code != 2 || !strings.Contains(errs, "-port") {
		t.Fatalf("exit %d: %s", code, errs)
	}
}
