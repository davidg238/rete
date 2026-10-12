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
	if got := m.Methods[4096].Name; got != "Fixture>>main:args:" {
		t.Fatalf("offset 4096 is %q", got)
	}
}

func TestAMethodKeyThatIsNotAnOffsetIsRefused(t *testing.T) {
	raw, err := os.ReadFile(fixture + ".oimg.map.json")
	if err != nil {
		t.Fatal(err)
	}
	var d map[string]any
	json.Unmarshal(raw, &d)
	d["methods"].(map[string]any)["x12"] = map[string]any{"name": "A>>b", "file": 0, "lines": []any{}, "vars": []any{}}
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
		"no layout":      func(d map[string]any) { delete(d["image"].(map[string]any), "layout") },
		"no oop_size":    func(d map[string]any) { delete(d["image"].(map[string]any)["layout"].(map[string]any), "oop_size") },
		"an extra field": func(d map[string]any) { d["stmap"] = true },
		"a method with no vars": func(d map[string]any) {
			for _, m := range d["methods"].(map[string]any) {
				delete(m.(map[string]any), "vars")
				break
			}
		},
		"a var with two homes":    func(d map[string]any) { firstVar(d)["ctx"] = 0.0 },
		"a var with no home":      func(d map[string]any) { delete(firstVar(d), "reg") },
		"a var with half a range": func(d map[string]any) { firstVar(d)["from"] = 0.0 },
		"a ctx var with a range": func(d map[string]any) {
			v := firstVar(d)
			delete(v, "reg")
			v["ctx"], v["from"], v["to"] = 0.0, 0.0, 4.0
		},
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

func TestAMethodKeyThatIsNotCanonicalIsRefused(t *testing.T) {
	raw, _ := os.ReadFile(fixture + ".oimg.map.json")
	var d map[string]any
	json.Unmarshal(raw, &d)
	d["methods"].(map[string]any)["0012"] = map[string]any{"name": "A>>b", "file": 0, "lines": []any{}, "vars": []any{}}
	p := filepath.Join(t.TempDir(), "m.json")
	b, _ := json.Marshal(d)
	os.WriteFile(p, b, 0o644)
	if _, err := Load(p); err == nil || !strings.Contains(err.Error(), `"0012"`) {
		t.Fatalf("loaded: %v", err)
	}
}

// firstVar answers Fixture>>main:args:'s first var (its `platform2` register),
// for a test to break.
func firstVar(d map[string]any) map[string]any {
	for _, m := range d["methods"].(map[string]any) {
		mm := m.(map[string]any)
		if mm["name"] == "Fixture>>main:args:" {
			return mm["vars"].([]any)[0].(map[string]any)
		}
	}
	panic("no Fixture>>main:args:")
}

// Each §3 shape of `vars` (rete#323): a whole-method register, a captured
// local's Context slot, an inlined loop variable's register with its range.
func TestVarsFromFixture(t *testing.T) {
	m, err := Load(fixture + ".oimg.map.json")
	if err != nil {
		t.Fatal(err)
	}
	var main *Method
	for _, x := range m.Methods {
		if x.Name == "Fixture>>main:args:" {
			main = &x
		}
	}
	if main == nil {
		t.Fatal("no Fixture>>main:args:")
	}
	got := map[string]Var{}
	for _, v := range main.Vars {
		got[v.Name] = v
	}
	if v := got["args"]; v != (Var{Name: "args", Reg: 2, Ctx: -1, From: 0, To: -1}) {
		t.Errorf("args = %+v, want reg 2 for the whole method", v)
	}
	if v := got["k"]; v != (Var{Name: "k", Reg: -1, Ctx: 0, From: 0, To: -1}) {
		t.Errorf("k = %+v, want context slot 0", v)
	}
	if v := got["i"]; v.Reg < 0 || v.Ctx != -1 || v.From >= v.To {
		t.Errorf("i = %+v, want a register with a range", v)
	}
}
