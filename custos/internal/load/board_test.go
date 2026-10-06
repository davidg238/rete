// Copyright (c) 2026 Ekorau LLC

package load

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// A collector holding the port must stop the load before anything touches the
// board: two readers split the ROM handshake, and a failed flash parks the chip
// in the ROM loader (final review, #310).
func TestBootRefusesAPortAnotherProcessHolds(t *testing.T) {
	dir := t.TempDir()
	port := filepath.Join(dir, "tty")
	if err := os.WriteFile(port, nil, 0o644); err != nil {
		t.Fatal(err)
	}
	link := filepath.Join(dir, "by-id") // the by-id path is a symlink to the tty
	if err := os.Symlink(port, link); err != nil {
		t.Fatal(err)
	}
	if err := portFree(link); err != nil {
		t.Fatalf("nobody holds it yet: %v", err)
	}
	holder := exec.Command("sh", "-c", "exec 3<\"$0\"; echo ready; exec sleep 30", port)
	out, err := holder.StdoutPipe()
	if err != nil {
		t.Fatal(err)
	}
	if err := holder.Start(); err != nil {
		t.Fatal(err)
	}
	defer holder.Process.Kill()
	if _, err := out.Read(make([]byte, 6)); err != nil {
		t.Fatal(err)
	}
	_, err = SerialBoard{Port: link, Baud: 115200, Timeout: time.Second}.Boot()
	if err == nil || !strings.Contains(err.Error(), "held by pid") || !strings.Contains(err.Error(), "collector") {
		t.Fatalf("err %v, want a refusal naming the holder", err)
	}
}
