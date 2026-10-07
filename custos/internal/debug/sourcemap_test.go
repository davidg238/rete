// Copyright (c) 2026 Ekorau LLC

package debug

// The two translations without a VM: file:line -> (blob offset, pc) through the
// image map, and a `dbg:paused` (blob offset, pc) -> file:line (rete#323).
// The synthetic map is 47_debug_pause_target's shape, as the compiler writes
// it; the checked-in fixture is the reader's test protocol/IMAGE-ARTIFACTS.md §4
// asks of custos. The end-to-end run against a real VM is the nsl suite's
// debugger-integration and tutorial-debugger gates.

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

var image = []byte("OIMG not really an image")

func method(name string, file int, lines ...[2]int) map[string]any {
	if lines == nil {
		lines = [][2]int{}
	}
	return map[string]any{"name": name, "file": file, "lines": lines, "vars": []any{}}
}

func synthetic() map[string]any {
	sum := sha256.Sum256(image)
	return map[string]any{
		"image": map[string]any{"sha256": hex.EncodeToString(sum[:]), "size": len(image), "layout": map[string]any{"oop_size": 4}},
		"entry": "T",
		"files": []string{"t.nsl", "kernel.nsl", "supervisor.nsl"},
		"methods": map[string]any{
			"100": method("T>>main:args:", 0, [2]int{1, 19}, [2]int{5, 20}, [2]int{21, 24}, [2]int{26, 28}, [2]int{51, 32}),
			"200": method("T>>countTo:", 0, [2]int{0, 36}, [2]int{2, 37}, [2]int{16, 38}),
			"260": method("T>>init_platform:", 0),
			"300": method("T_block2>>value:", 0, [2]int{1, 25}, [2]int{5, 26}),
			// a kernel line inside the app's line range: it must not be read as t.nsl's
			"400": method("Kernel>>pumpEvents", 1, [2]int{0, 27}),
		},
	}
}

func writeMap(t *testing.T, dir string, doc any) string {
	t.Helper()
	b, err := json.Marshal(doc)
	if err != nil {
		t.Fatal(err)
	}
	p := filepath.Join(dir, "t.oimg.map.json")
	if err := os.WriteFile(p, b, 0o644); err != nil {
		t.Fatal(err)
	}
	return p
}

func loaded(t *testing.T) (*SourceMap, string) {
	t.Helper()
	dir := t.TempDir()
	p := writeMap(t, dir, synthetic())
	var src []string
	for n := 1; n <= 40; n++ {
		src = append(src, fmt.Sprintf("line %d", n))
	}
	if err := os.WriteFile(filepath.Join(dir, "t.nsl"), []byte(strings.Join(src, "\n")), 0o644); err != nil {
		t.Fatal(err)
	}
	sm, err := LoadSourceMap(p, "")
	if err != nil {
		t.Fatal(err)
	}
	return sm, dir
}

func mustResolve(t *testing.T, sm *SourceMap, spec string) Breakpoint {
	t.Helper()
	bp, err := sm.Resolve(spec)
	if err != nil {
		t.Fatalf("Resolve(%q): %v", spec, err)
	}
	return bp
}

func refused(t *testing.T, sm *SourceMap, spec, want string) {
	t.Helper()
	_, err := sm.Resolve(spec)
	if err == nil || !strings.Contains(err.Error(), want) {
		t.Fatalf("Resolve(%q) = %v, want an error containing %q", spec, err, want)
	}
}

func TestALineResolvesToItsMethodAndFirstOffset(t *testing.T) {
	sm, _ := loaded(t)
	if got := mustResolve(t, sm, "37"); got != (Breakpoint{200, 2, 37, "T>>countTo:"}) {
		t.Errorf("37 -> %+v", got)
	}
	if got := mustResolve(t, sm, "t.nsl:20"); got != (Breakpoint{100, 5, 20, "T>>main:args:"}) {
		t.Errorf("t.nsl:20 -> %+v", got)
	}
}

func TestALineWithoutCodeInsideAMethodMovesForward(t *testing.T) {
	sm, _ := loaded(t)
	if got := mustResolve(t, sm, "30"); got != (Breakpoint{100, 51, 32, "T>>main:args:"}) {
		t.Errorf("30 -> %+v", got)
	}
}

func TestALineOutsideEveryMethodNamesItsNeighbours(t *testing.T) {
	sm, _ := loaded(t)
	refused(t, sm, "34", "line 34 has no code; the nearest lines that do: 32 (main:args:), 36 (countTo:)")
}

// 26 lies inside main:args:'s span (19-32); taking the span would break at line 28.
// The block is its own method with its own offset (rete#323).
func TestALineInABlockResolvesToTheBlockNotItsHome(t *testing.T) {
	sm, _ := loaded(t)
	if got := mustResolve(t, sm, "26"); got != (Breakpoint{300, 5, 26, "T_block2>>value:"}) {
		t.Errorf("26 -> %+v", got)
	}
}

func TestAnotherFileIsRefused(t *testing.T) {
	sm, _ := loaded(t)
	refused(t, sm, "other.nsl:37", "built from t.nsl")
}

func TestAStopReadsBackAsTheLineOfTheLastStatementStarted(t *testing.T) {
	sm, _ := loaded(t)
	if where, text, line := sm.Where(200, 2); where != "at t.nsl:37  T>>countTo:" || text != "  37 | line 37" || line != 37 {
		t.Errorf("Where(200, 2) = %q, %q, %d", where, text, line)
	}
	if where, _, _ := sm.Where(200, 9); where != "at t.nsl:37  T>>countTo:" {
		t.Errorf("Where(200, 9) = %q", where)
	}
	if where, _, _ := sm.Where(100, 0); where != "at t.nsl:19  T>>main:args:" { // before the first entry
		t.Errorf("Where(100, 0) = %q", where)
	}
	if where, _, _ := sm.Where(300, 6); where != "at t.nsl:26  T_block2>>value:" { // a block
		t.Errorf("Where(300, 6) = %q", where)
	}
}

// A kernel frame is named by its own file, and answers no app line: its 27 must
// not read as t.nsl:27.
func TestWhereKernel(t *testing.T) {
	sm, _ := loaded(t)
	if where, text, line := sm.Where(400, 0); where != "at kernel.nsl:27  Kernel>>pumpEvents" || text != "" || line != 0 {
		t.Errorf("Where(400, 0) = %q, %q, %d", where, text, line)
	}
	if where, _, line := sm.Where(260, 0); where != "in t.nsl's T>>init_platform:, pc 0" || line != 0 { // no lines
		t.Errorf("Where(260, 0) = %q, %d", where, line)
	}
}

func TestWhereUnknownOffset(t *testing.T) {
	sm, _ := loaded(t)
	if where, _, line := sm.Where(999, 0); !strings.Contains(where, "wrong build?") || line != 0 {
		t.Errorf("Where(999, 0) = %q, %d", where, line)
	}
}

func TestWhereNoBlob(t *testing.T) {
	sm, _ := loaded(t)
	if where, _, line := sm.Where(-1, 3); !strings.Contains(where, "no blob") || line != 0 {
		t.Errorf("Where(-1, 3) = %q, %d", where, line)
	}
}

func TestMethodsListsTheAppFileByOffset(t *testing.T) {
	sm, _ := loaded(t)
	got := strings.Join(sm.Methods(), "\n")
	for _, want := range []string{"   100  T>>main:args:", "   200  T>>countTo:", "   300  T_block2>>value:"} {
		if !strings.Contains(got, want) {
			t.Errorf("Methods() lacks %q:\n%s", want, got)
		}
	}
	if strings.Contains(got, "pumpEvents") {
		t.Errorf("Methods() lists a kernel method:\n%s", got)
	}
}

func TestAFileThatIsNotAnImageMapIsRefusedByName(t *testing.T) {
	dir := t.TempDir()
	p := filepath.Join(dir, "old.stmap")
	os.WriteFile(p, []byte(`{"source": "t.nsl", "entry": "T", "functions": {}}`), 0o644)
	if _, err := LoadSourceMap(p, ""); err == nil || !strings.Contains(err.Error(), "not an image map") {
		t.Fatalf("an old .stmap loaded: %v", err)
	}
}

// IMAGE-ARTIFACTS.md §3: a missing field or an extra one is an error in the
// writer, never a shape to tolerate.
func TestAMapMissingAFieldOrCarryingAnExtraOneIsRefused(t *testing.T) {
	for name, edit := range map[string]func(map[string]any){
		"no entry":       func(d map[string]any) { delete(d, "entry") },
		"no image size":  func(d map[string]any) { delete(d["image"].(map[string]any), "size") },
		"no method file": func(d map[string]any) { delete(d["methods"].(map[string]any)["200"].(map[string]any), "file") },
		"an extra field": func(d map[string]any) { d["stmap"] = true },
	} {
		t.Run(name, func(t *testing.T) {
			d := synthetic()
			edit(d)
			if _, err := LoadSourceMap(writeMap(t, t.TempDir(), d), ""); err == nil ||
				!strings.Contains(err.Error(), "not an image map") {
				t.Fatalf("loaded: %v", err)
			}
		})
	}
}

func TestTheImageTheMapNamesIsAcceptedAndAnyOtherRefused(t *testing.T) {
	sm, dir := loaded(t)
	good, bad := filepath.Join(dir, "t.oimg"), filepath.Join(dir, "u.oimg")
	os.WriteFile(good, image, 0o644)
	os.WriteFile(bad, append(append([]byte(nil), image...), '!'), 0o644)
	if err := sm.CheckImage(good); err != nil {
		t.Fatal(err)
	}
	if err := sm.CheckImage(bad); err == nil || !strings.Contains(err.Error(), "not the image this map describes") {
		t.Fatalf("CheckImage(bad) = %v", err)
	}
}

// --- the checked-in fixture: protocol/fixtures/image/ ---

func fixture(t *testing.T, name string) string {
	t.Helper()
	_, here, _, _ := runtime.Caller(0)
	return filepath.Join(filepath.Dir(here), "..", "..", "..", "protocol", "fixtures", "image", name)
}

func fixtureMap(t *testing.T) *SourceMap {
	t.Helper()
	sm, err := LoadSourceMap(fixture(t, "Fixture.oimg.map.json"), "")
	if err != nil {
		t.Fatal(err)
	}
	return sm
}

// The facts below were read off Fixture.nsl by hand: a reader that gets one
// wrong is wrong about the contract, not about this file.
func TestTheFixtureMapReadsAsItsSourceSays(t *testing.T) {
	sm := fixtureMap(t)
	if sm.Entry != "Fixture" || sm.Source != "Fixture.nsl" {
		t.Fatalf("entry %q, source %q", sm.Entry, sm.Source)
	}
	// `(twice value: c bump) println.` is line 23 of main:args:, from offset 23.
	if got := mustResolve(t, sm, "Fixture.nsl:23"); got != (Breakpoint{3992, 23, 23, "Fixture>>main:args:"}) {
		t.Errorf("23 -> %+v", got)
	}
	// Line 21 holds the block `[ :x | x + x + k ]`, its own method at its own offset.
	if got := mustResolve(t, sm, "21"); got != (Breakpoint{26616, 1, 21, "Fixture_block1>>value:"}) {
		t.Errorf("21 -> %+v", got)
	}
	// Line 11 is the nested class's bump; line 14 the class side's from:, a Meta_ vtable.
	if got := mustResolve(t, sm, "11"); got != (Breakpoint{4088, 0, 11, "Fixture_Counter>>bump"}) {
		t.Errorf("11 -> %+v", got)
	}
	if got := mustResolve(t, sm, "14"); got != (Breakpoint{4152, 0, 14, "Meta_Fixture_Counter>>from:"}) {
		t.Errorf("14 -> %+v", got)
	}
	if where, text, _ := sm.Where(3992, 31); where != "at Fixture.nsl:24  Fixture>>main:args:" || !strings.Contains(text, "^ 0 )") {
		t.Errorf("Where(3992, 31) = %q, %q", where, text)
	}
}

func TestTheFixtureImageIsAcceptedAndOneChangedByteRefused(t *testing.T) {
	sm := fixtureMap(t)
	img := fixture(t, "Fixture.oimg")
	if err := sm.CheckImage(img); err != nil {
		t.Fatal(err)
	}
	b, err := os.ReadFile(img)
	if err != nil {
		t.Fatal(err)
	}
	b[len(b)/2] ^= 1
	changed := filepath.Join(t.TempDir(), "Fixture.oimg")
	os.WriteFile(changed, b, 0o644)
	if err := sm.CheckImage(changed); err == nil {
		t.Fatal("an image with one byte changed was accepted")
	}
}
