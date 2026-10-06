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
	sent   []string
	reply  []string
	closed bool
}

func (f *fakeNode) Close() error { f.closed = true; return nil }

func (f *fakeNode) Send(line string) error {
	f.sent = append(f.sent, line)
	go func() {
		for _, l := range f.reply {
			f.onLine(l)
		}
	}()
	return nil
}

func (f *fakeNode) open(before ...string) func(func(string)) (Sender, error) {
	return func(onLine func(string)) (Sender, error) {
		f.onLine = onLine
		for _, l := range before { // what the console printed before the request
			onLine(l)
		}
		return f, nil
	}
}

func TestCaptureSendsTheRequestAndStopsAtTheEndLine(t *testing.T) {
	n := &fakeNode{reply: []string{"tick 9",
		"mprof[window]: methods=1 activations=5 overflow=0 cap=256", "mprof: 3992 5",
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
	if len(d.Rows) != 1 || d.Rows[0] != (Row{"3992", 5}) {
		t.Fatalf("rows %v from\n%s", d.Rows, text)
	}
}

func TestCaptureTimesOutNamingWhatItSaw(t *testing.T) {
	n := &fakeNode{reply: []string{"tick 9"}}
	_, err := Capture(n.open(), 50, 50*time.Millisecond)
	if err == nil || !strings.Contains(err.Error(), "mprof[window]: end") {
		t.Fatalf("err %v", err)
	}
}

func TestCaptureReportsAFailedOpen(t *testing.T) {
	_, err := Capture(func(func(string)) (Sender, error) { return nil, errors.New("no such port") }, 50, time.Second)
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
