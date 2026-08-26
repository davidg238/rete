// Copyright (c) 2026 Ekorau LLC

// internal/portacli/nocreate_test.go
package portacli

import (
	"bytes"
	"os"
	"strings"
	"testing"

	"github.com/davidg238/porta/internal/space"
)

// runExpectingError runs the root command and returns the error and stderr,
// where runSplit would t.Fatal. The command under test is SUPPOSED to fail.
func runExpectingError(t *testing.T, args ...string) (error, string) {
	t.Helper()
	var out, errb bytes.Buffer
	cmd := NewRootCmd()
	cmd.SetOut(&out)
	cmd.SetErr(&errb)
	cmd.SetArgs(args)
	cmd.SilenceUsage = true
	return cmd.Execute(), errb.String()
}

// The 2026-08-18 gateway reading (rete#221): `porta get` with a wrong --db
// printed nothing, exited 0, and deposited a fresh 49 KB porta.db beside the
// live store. "No rows" and "wrong database" must be distinguishable, so the
// operator verbs refuse to manufacture a store — only serve creates.
func TestOperatorVerbsRefuseToManufactureAStore(t *testing.T) {
	cases := [][]string{
		{"get", "c6spare"},
		{"log", "c6spare"},
		{"put", "c6spare", "goal/name", "bench"},
	}
	for _, args := range cases {
		t.Run(args[0], func(t *testing.T) {
			missing := t.TempDir() + "/nonexistent.db"
			err, _ := runExpectingError(t, append([]string{"--db", missing}, args...)...)
			if err == nil {
				t.Fatalf("porta %s with a missing --db succeeded; it must refuse", args[0])
			}
			if !strings.Contains(err.Error(), missing) {
				t.Errorf("error does not name the path it looked for: %v", err)
			}
			if _, statErr := os.Stat(missing); !os.IsNotExist(statErr) {
				t.Errorf("a decoy store was deposited at %s", missing)
			}
		})
	}
}

// A correct-looking empty answer must still say what it read: the resolved
// store path goes to stderr, where it cannot pollute output a gate parses.
func TestGetAndLogNameTheStoreTheyRead(t *testing.T) {
	path := getTestDB(t)
	st, err := space.Open(path, func() int64 { return 1000 })
	if err != nil {
		t.Fatal(err)
	}
	st.Close()

	for _, verb := range []string{"get", "log"} {
		stdout, stderr := runSplit(t, "--db", path, verb, "c6spare")
		if !strings.Contains(stderr, path) {
			t.Errorf("%s: stderr does not name the store it read:\n%s", verb, stderr)
		}
		if strings.Contains(stdout, path) {
			t.Errorf("%s: store path leaked onto stdout, where a gate would parse it:\n%s", verb, stdout)
		}
	}
}
