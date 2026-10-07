// Copyright (c) 2026 Ekorau LLC

package console

import (
	"context"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func faultPanic(t *testing.T) (Panic, string) {
	t.Helper()
	r := Runs(fixture(t, "c6-fault.log"))[0]
	return Panics(r)[0], r.BuildID
}

func TestABacktraceWithoutItsELFIsRefused(t *testing.T) {
	p, id := faultPanic(t)
	root := t.TempDir()
	_, err := Backtrace(context.Background(), p, id, "", root, t.TempDir())
	var r Refused
	if !errors.As(err, &r) || !strings.Contains(r.Why, id) {
		t.Fatalf("%v", err)
	}
	if _, err := Backtrace(context.Background(), p, "", "", root, t.TempDir()); !errors.As(err, &r) || !strings.Contains(r.Why, "never printed a build id") {
		t.Fatalf("no build id: %v", err)
	}
	if _, err := Backtrace(context.Background(), Panic{}, id, "", root, t.TempDir()); !errors.As(err, &r) || !strings.Contains(r.Why, "no #CD:") {
		t.Fatalf("no coredump: %v", err)
	}
}

// The real tools against the fault fixture's archived ELF, which only the bench
// machine has: elsewhere this skips, and says why.
func TestTheFaultFixtureResolvesToItsSource(t *testing.T) {
	p, id := faultPanic(t)
	if _, err := os.Stat(filepath.Join(ELFRoot(), id, "zephyr.elf")); err != nil {
		t.Skipf("no archived ELF for the fault fixture (%v): not the bench machine", err)
	}
	bt, err := Backtrace(context.Background(), p, id, "", ELFRoot(), t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(bt, "main.c") {
		t.Fatalf("no main.c frame:\n%s", bt)
	}
	t.Log(bt)
	bt, _ = Backtrace(context.Background(), p, "", id, ELFRoot(), t.TempDir())
	for _, l := range strings.Split(bt, "\n") {
		if !strings.HasPrefix(l, "[ASSUMED build-id "+id+"] ") {
			t.Fatalf("an assumed line without its mark: %q", l)
		}
	}
}
