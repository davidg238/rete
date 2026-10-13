// Copyright (c) 2026 Ekorau LLC

package profile

import (
	"fmt"
	"io"
	"regexp"
	"strings"
	"sync"
	"time"
)

// Sender is the request half of a console line (debug.Line satisfies it).
type Sender interface {
	Send(line string) error
}

// endLine is vm.c mprof_report's last line for a window closed by the kernel's
// Profiler; no row pattern matches it.
var endLine = regexp.MustCompile(`mprof\[window\]: end\s*$`)

// after is a timeout's channel; 0 is none (rete#415: a request through porta
// waits for a node that may be asleep, and a newer request supersedes it).
func after(d time.Duration) <-chan time.Time {
	if d <= 0 {
		return nil
	}
	return time.After(d)
}

// Capture opens the line, sends `profile <ms>` and answers every line from the
// request through `mprof[window]: end`. Lines that arrived before the request
// (an earlier window's rows) are not part of it. open hands its reader the
// callback for each whole line, and the callback for the line's end (a host
// VM's exit, a cable's loss); an end before the end line fails the capture at
// once (rete#364). The board's port is opened ONCE (rete#319: an open reboots
// the C6 -- with pyserial at least, #355) and closed on return when it is an
// io.Closer.
func Capture(open func(onLine func(string), onEnd func()) (Sender, error), ms int, timeout time.Duration) (string, error) {
	var (
		mu      sync.Mutex
		started bool
		got     []string
		done    = make(chan struct{})
		ended   = make(chan struct{})
		endOnce sync.Once
	)
	onEnd := func() { endOnce.Do(func() { close(ended) }) }
	onLine := func(l string) {
		mu.Lock()
		defer mu.Unlock()
		if !started {
			return
		}
		select {
		case <-done:
			return
		default:
		}
		got = append(got, l)
		if endLine.MatchString(l) {
			close(done)
		}
	}
	line, err := open(onLine, onEnd)
	if err != nil {
		return "", err
	}
	if c, ok := line.(io.Closer); ok {
		defer c.Close()
	}
	mu.Lock()
	started = true
	mu.Unlock()
	if err := line.Send(fmt.Sprintf("profile %d", ms)); err != nil {
		return "", err
	}
	select {
	case <-done:
	case <-ended:
		select {
		case <-done: // the end line arrived just before the end
		default:
			mu.Lock()
			n := len(got)
			mu.Unlock()
			return "", fmt.Errorf("the program ended before the window closed (%d lines after the request) — does it run as long as the window, and dispatch sys/timer?", n)
		}
	case <-after(timeout):
		mu.Lock()
		n := len(got)
		mu.Unlock()
		return "", fmt.Errorf("no 'mprof[window]: end' within %v (%d lines after the request) — is the profiler in this image, and does its app dispatch sys/timer?", timeout, n)
	}
	mu.Lock()
	defer mu.Unlock()
	return strings.Join(got, "\n") + "\n", nil
}
