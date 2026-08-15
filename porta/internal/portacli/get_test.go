// Copyright (c) 2026 Ekorau LLC

// internal/portacli/get_test.go
package portacli

import (
	"bytes"
	"strings"
	"testing"

	"github.com/davidg238/porta/internal/cval"
	"github.com/davidg238/porta/internal/space"
)

// runSplit runs the root command with stdout and stderr kept APART. The shared
// runRootCapture helper merges them, which would pass this file's tests whether
// or not the mixed-read banner honours the decision that puts it on stdout — a
// warning a pipe eats is not a guard, so the stream is the assertion.
func runSplit(t *testing.T, args ...string) (stdout, stderr string) {
	t.Helper()
	var out, errb bytes.Buffer
	cmd := NewRootCmd()
	cmd.SetOut(&out)
	cmd.SetErr(&errb)
	cmd.SetArgs(args)
	cmd.SilenceUsage = true
	if err := cmd.Execute(); err != nil {
		t.Fatalf("%v: %v", args, err)
	}
	return out.String(), errb.String()
}

func getTestDB(t *testing.T) string {
	t.Helper()
	dbPath = t.TempDir() + "/porta.db"
	return dbPath
}

func mustEnc(t *testing.T, v any) []byte {
	t.Helper()
	b, err := cval.Encode(v)
	if err != nil {
		t.Fatal(err)
	}
	return b
}

// The 2026-08-14 reading, at the surface an operator actually touches: a node
// rebooted, republished part of its tree, and `porta get` answered with a
// mixture that read exactly like a coherent one (rete#210).
func TestGetAnnouncesAMixedReadOnStdout(t *testing.T) {
	path := getTestDB(t)
	st, err := space.Open(path, func() int64 { return 1000 })
	if err != nil {
		t.Fatal(err)
	}
	st.Ingest("c6spare", 40, []space.Entry{
		{Seq: 1, Path: "diag/spans/pushTrim", Kind: cval.KindCell, Value: mustEnc(t, int64(51923))},
		{Seq: 2, Path: "diag/spans/checkIn", Kind: cval.KindCell, Value: mustEnc(t, int64(243307))},
	})
	st.Ingest("c6spare", 41, []space.Entry{
		{Seq: 1, Path: "diag/spans/checkIn", Kind: cval.KindCell, Value: mustEnc(t, int64(235037))},
	})
	st.Close()

	stdout, stderr := runSplit(t, "--db", path, "get", "c6spare", "diag/")

	if !strings.Contains(stdout, "! MIXED READ") {
		t.Fatalf("no mixed-read banner on stdout:\n%s", stdout)
	}
	if strings.Contains(stderr, "MIXED READ") {
		t.Fatalf("banner went to stderr, where a pipe would eat it:\n%s", stderr)
	}
	if !strings.Contains(stdout, "predate boot 41") {
		t.Fatalf("banner does not name the current boot:\n%s", stdout)
	}
	// The stale row must be visible AND marked — hiding it would lose a path a
	// node may legitimately write only rarely.
	line := lineContaining(stdout, "pushTrim")
	if !strings.HasPrefix(line, "~") || !strings.Contains(line, "boot=40") || !strings.Contains(line, "STALE") {
		t.Fatalf("stale row not marked: %q", line)
	}
	if fresh := lineContaining(stdout, "checkIn"); !strings.Contains(fresh, "boot=41") || strings.Contains(fresh, "STALE") {
		t.Fatalf("current-boot row wrongly marked: %q", fresh)
	}
}

// The guard must be invisible until the hazard exists, or it becomes noise that
// trains the reader to ignore it.
func TestGetIsSilentWhenTheTreeIsCoherent(t *testing.T) {
	path := getTestDB(t)
	st, _ := space.Open(path, func() int64 { return 1000 })
	st.Write("c6spare", "goal/name", cval.KindCell, mustEnc(t, "bench"), "david", "")
	st.Ingest("c6spare", 41, []space.Entry{
		{Seq: 1, Path: "obs/name", Kind: cval.KindCell, Value: mustEnc(t, "bench")},
		{Seq: 2, Path: "obs/health", Kind: cval.KindCell, Value: mustEnc(t, int64(1))},
	})
	st.Close()

	stdout, _ := runSplit(t, "--db", path, "get", "c6spare")

	if strings.Contains(stdout, "MIXED READ") || strings.Contains(stdout, "STALE") {
		t.Fatalf("coherent tree reported as mixed:\n%s", stdout)
	}
	// porta's own write is not a node observation and carries no boot.
	if g := lineContaining(stdout, "goal/name"); !strings.Contains(g, "boot=-") {
		t.Fatalf("outbound row should carry no boot: %q", g)
	}
}

// A node with no persistent identity is UNKNOWN, never stale. Marking it would
// fabricate a distinction the node never made.
func TestGetReportsABootlessNodeAsUnknown(t *testing.T) {
	path := getTestDB(t)
	st, _ := space.Open(path, func() int64 { return 1000 })
	st.Ingest("hostnode", 0, []space.Entry{
		{Seq: 1, Path: "obs/health", Kind: cval.KindCell, Value: mustEnc(t, int64(1))},
	})
	st.Close()

	stdout, _ := runSplit(t, "--db", path, "get", "hostnode")

	if strings.Contains(stdout, "MIXED READ") || strings.Contains(stdout, "STALE") {
		t.Fatalf("bootless node reported as stale:\n%s", stdout)
	}
	if h := lineContaining(stdout, "obs/health"); !strings.Contains(h, "boot=?") {
		t.Fatalf("bootless row should read unknown: %q", h)
	}
}

func TestLogFiltersByBoot(t *testing.T) {
	path := getTestDB(t)
	st, _ := space.Open(path, func() int64 { return 1000 })
	st.Ingest("c6spare", 40, []space.Entry{
		{Seq: 1, Path: "tel/log", Kind: cval.KindStream, Value: mustEnc(t, "old")},
	})
	st.Ingest("c6spare", 41, []space.Entry{
		{Seq: 1, Path: "tel/log", Kind: cval.KindStream, Value: mustEnc(t, "new")},
	})
	st.Close()

	stdout, _ := runSplit(t, "--db", path, "log", "c6spare", "--boot", "41")
	if strings.Contains(stdout, "old") {
		t.Fatalf("--boot 41 leaked a boot-40 row:\n%s", stdout)
	}
	if !strings.Contains(stdout, "new") || !strings.Contains(stdout, "boot=41") {
		t.Fatalf("--boot 41 lost its own row:\n%s", stdout)
	}
}

func lineContaining(out, want string) string {
	for _, l := range strings.Split(out, "\n") {
		if strings.Contains(l, want) {
			return l
		}
	}
	return ""
}

// isStale is tested directly because the CLI cannot reach every arm: Write
// always stamps an outbound row with boot 0, so the direction clause is
// unreachable from the surface and a mutation that deleted it stayed green
// against the tests above. The clause is kept rather than deleted because it
// states the invariant that makes it unreachable — porta's intent is not a node
// observation and no reboot invalidates it — and this table is what holds it.
func TestIsStaleMarksOnlyASupersededNodeObservation(t *testing.T) {
	cases := []struct {
		name string
		row  space.Cell
		boot int64
		want bool
	}{
		{"inbound row from a previous boot", space.Cell{BootID: 40, Direction: "in"}, 41, true},
		{"inbound row from the current boot", space.Cell{BootID: 41, Direction: "in"}, 41, false},
		{"outbound row, even if somehow stamped", space.Cell{BootID: 40, Direction: "out"}, 41, false},
		{"node with no persistent identity", space.Cell{BootID: 0, Direction: "in"}, 41, false},
		{"row written before boot_id existed", space.Cell{BootID: 0, Direction: "in"}, 41, false},
		{"node porta has never heard from", space.Cell{BootID: 40, Direction: "in"}, 0, false},
	}
	for _, tc := range cases {
		if got := isStale(tc.row, tc.boot); got != tc.want {
			t.Errorf("%s: isStale(%+v, %d) = %v, want %v", tc.name, tc.row, tc.boot, got, tc.want)
		}
	}
}
