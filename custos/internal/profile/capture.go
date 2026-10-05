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

// Capture opens the line, sends `profile <ms>` and answers every line from the
// request through `mprof[window]: end`. Lines that arrived before the request
// (an earlier window's rows) are not part of it. open hands its reader the
// callback for each whole line; the board's port is opened ONCE (rete#319: an
// open reboots the C6 -- with pyserial at least, #355) and closed on return when
// it is an io.Closer.
func Capture(open func(onLine func(string)) (Sender, error), ms int, timeout time.Duration) (string, error) {
	var (
		mu      sync.Mutex
		started bool
		got     []string
		done    = make(chan struct{})
	)
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
	line, err := open(onLine)
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
	case <-time.After(timeout):
		mu.Lock()
		n := len(got)
		mu.Unlock()
		return "", fmt.Errorf("no 'mprof[window]: end' within %v (%d lines after the request) — is the profiler in this image, and does its app dispatch sys/timer?", timeout, n)
	}
	mu.Lock()
	defer mu.Unlock()
	return strings.Join(got, "\n") + "\n", nil
}
