// Copyright (c) 2026 Ekorau LLC

package debug

import (
	"errors"
	"testing"
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
