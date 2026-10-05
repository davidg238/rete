// Copyright (c) 2026 Ekorau LLC

package imagemap

import (
	"encoding/json"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"testing"
)

const fixture = "../../../protocol/fixtures/image/Fixture"

func TestTheFixtureMapLoadsEveryMethodByOffset(t *testing.T) {
	m, err := Load(fixture + ".oimg.map.json")
	if err != nil {
		t.Fatal(err)
	}
	if m.Entry != "Fixture" || m.Files[0] != "Fixture.nsl" || m.OopSize != 4 {
		t.Fatalf("entry %q files %v oop %d", m.Entry, m.Files, m.OopSize)
	}
	offs := m.Offsets()
	if len(offs) != len(m.Methods) || !sort.IntsAreSorted(offs) {
		t.Fatalf("offsets %d of %d, sorted %v", len(offs), len(m.Methods), sort.IntsAreSorted(offs))
	}
	// Every method, not only the app's: a profile ranks the kernel's too.
	kernel := 0
	for _, meth := range m.Methods {
		if m.Files[meth.File] == "kernel.nsl" {
			kernel++
		}
	}
	if kernel == 0 {
		t.Fatal("no kernel methods in the map")
	}
	if got := m.Methods[3872].Name; got != "Fixture>>main:args:" {
		t.Fatalf("offset 3872 is %q", got)
	}
}

func TestAMethodKeyThatIsNotAnOffsetIsRefused(t *testing.T) {
	raw, err := os.ReadFile(fixture + ".oimg.map.json")
	if err != nil {
		t.Fatal(err)
	}
	var d map[string]any
	json.Unmarshal(raw, &d)
	d["methods"].(map[string]any)["x12"] = map[string]any{"name": "A>>b", "file": 0, "lines": []any{}}
	p := filepath.Join(t.TempDir(), "m.json")
	b, _ := json.Marshal(d)
	os.WriteFile(p, b, 0o644)
	if _, err := Load(p); err == nil || !strings.Contains(err.Error(), `"x12"`) {
		t.Fatalf("loaded: %v", err)
	}
}

func TestAMapMissingAFieldOrCarryingAnExtraOneIsRefused(t *testing.T) {
	raw, err := os.ReadFile(fixture + ".oimg.map.json")
	if err != nil {
		t.Fatal(err)
	}
	for name, edit := range map[string]func(map[string]any){
		"no entry":       func(d map[string]any) { delete(d, "entry") },
		"no oop_size":    func(d map[string]any) { delete(d["image"].(map[string]any), "oop_size") },
		"an extra field": func(d map[string]any) { d["stmap"] = true },
	} {
		t.Run(name, func(t *testing.T) {
			var d map[string]any
			json.Unmarshal(raw, &d)
			edit(d)
			p := filepath.Join(t.TempDir(), "m.json")
			b, _ := json.Marshal(d)
			os.WriteFile(p, b, 0o644)
			if _, err := Load(p); err == nil || !strings.Contains(err.Error(), "not an image map") {
				t.Fatalf("loaded: %v", err)
			}
		})
	}
}

func TestTheFixtureImageIsAcceptedAndOneChangedByteRefused(t *testing.T) {
	m, err := Load(fixture + ".oimg.map.json")
	if err != nil {
		t.Fatal(err)
	}
	if err := m.CheckImage(fixture + ".oimg"); err != nil {
		t.Fatal(err)
	}
	b, _ := os.ReadFile(fixture + ".oimg")
	b[len(b)/2] ^= 1
	p := filepath.Join(t.TempDir(), "x.oimg")
	os.WriteFile(p, b, 0o644)
	if err := m.CheckImage(p); err == nil {
		t.Fatal("a changed image was accepted")
	}
}
