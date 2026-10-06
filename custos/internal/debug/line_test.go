// Copyright (c) 2026 Ekorau LLC

package debug

import (
	"errors"
	"io"
	"testing"
	"time"
)

// A cable's read fails when it is unplugged (go.bug.st/serial answers
// PortClosed); the reader hands over what it had, then ends once.
func TestAFailedReadEndsTheReaderOnce(t *testing.T) {
	reads := []string{"dbg-probe tick 1\r\n", ""}
	var lines []string
	ends := 0
	pump(func(b []byte) (int, error) {
		if len(reads) == 0 {
			t.Fatal("read after the end")
		}
		r := reads[0]
		reads = reads[1:]
		if r == "" {
			return 0, errors.New("port closed")
		}
		return copy(b, r), nil
	}, func(l string) { lines = append(lines, l) }, func() { ends++ })
	if len(lines) != 1 || lines[0] != "dbg-probe tick 1" || ends != 1 {
		t.Fatalf("lines %q, ends %d", lines, ends)
	}
}

// rete#364: a host VM for the profiler runs without its debugger, so the
// console feeds the kernel's Profiler, and Close stops it (a capture's VM
// must not outlive the capture).
func TestAHostVMWithoutTheDebuggerIsStoppedByClose(t *testing.T) {
	got := make(chan string, 4)
	ended := make(chan struct{})
	line, err := StartHost([]string{"sh", "-c", `echo "debug=[$OEVM_DEBUG]"; exec cat`}, false,
		func(l string) { got <- l }, func() { close(ended) })
	if err != nil {
		t.Fatal(err)
	}
	if l := <-got; l != "debug=[]" {
		t.Fatalf("the VM saw %q: its debugger was on", l)
	}
	c, ok := line.(io.Closer)
	if !ok {
		t.Fatal("a host line is not an io.Closer")
	}
	c.Close()
	select {
	case <-ended:
	case <-time.After(2 * time.Second):
		t.Fatal("Close did not end the VM")
	}
}
