// Copyright (c) 2026 Ekorau LLC

package profile

import (
	"bytes"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

const fixtureMap = "../../../protocol/fixtures/image/Fixture.oimg.map.json"

func run(t *testing.T, stdin string, args ...string) (int, string, string) {
	t.Helper()
	var out, errb bytes.Buffer
	code := Run(args, strings.NewReader(stdin), &out, &errb)
	return code, out.String(), errb.String()
}

func TestALogInARankedTableOut(t *testing.T) {
	// 4088 is Fixture>>main:args: in the fixture map; 7 is no method's offset.
	log := filepath.Join(t.TempDir(), "console.log")
	os.WriteFile(log, []byte("tick 1\nmprof[window]: methods=2 activations=30 overflow=0 cap=256\n"+
		"mprof: 7 10\nmprof: 4088 20\ntick 2\n"), 0o644)
	code, out, errs := run(t, "", "-map", fixtureMap, log)
	if code != 0 || errs != "" {
		t.Fatalf("exit %d: %s", code, errs)
	}
	if !strings.Contains(out, "          20   66.7%  Fixture>>main:args:") || !strings.Contains(out, "UNKNOWN (offset 7") {
		t.Fatal(out)
	}
}

func TestStdinWhenNoLogIsNamed(t *testing.T) {
	code, out, _ := run(t, "mprof[window]: methods=1 activations=5 overflow=0 cap=256\nmprof: 4088 5\n", "-map", fixtureMap)
	if code != 0 || !strings.Contains(out, "100.0%  Fixture>>main:args:") {
		t.Fatalf("exit %d\n%s", code, out)
	}
}

func TestNoProfileLinesNamesTheBuildFlag(t *testing.T) {
	code, _, errs := run(t, "tick 1\n", "-map", fixtureMap)
	if code != 1 || !strings.Contains(errs, "OEVM_METHOD_PROFILE") {
		t.Fatalf("exit %d: %s", code, errs)
	}
}

func TestAMissingMapIsAUsageError(t *testing.T) {
	code, _, errs := run(t, "")
	if code != 2 || !strings.Contains(errs, "-map is required") {
		t.Fatalf("exit %d: %s", code, errs)
	}
}

func TestANegativeTopIsRefused(t *testing.T) {
	if code, _, errs := run(t, "", "-map", fixtureMap, "-top", "-1"); code != 2 || !strings.Contains(errs, "-top") {
		t.Fatalf("exit %d: %s", code, errs)
	}
}

func TestTopAndAllocTogether(t *testing.T) {
	in := "mprof[window]: methods=2 activations=30 overflow=0 cap=256\nmprof: 4088 20\nmprof: 7 10\n" +
		"mprofa[window]: noframe_arena=0/0 noframe_cheap=0/0 allocOverflow=0\nmprofa: 4088 64 2 0 0\nmprofa: 7 32 1 0 0\n"
	code, out, _ := run(t, in, "-map", fixtureMap, "-top", "1", "-alloc")
	if code != 0 || !strings.Contains(out, "showing top 1 of 2 rows") ||
		!strings.Contains(out, "showing top 1 of 2 allocating methods") || !strings.Contains(out, "distinct allocating methods: 2") {
		t.Fatalf("exit %d\n%s", code, out)
	}
}
