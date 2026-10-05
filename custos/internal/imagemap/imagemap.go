// Copyright (c) 2026 Ekorau LLC

// Package imagemap reads the image map the compiler writes beside an image
// (protocol/IMAGE-ARTIFACTS.md §3): every method's blob offset, its name, its
// file and its lines. custos debug and custos profile both read it through here.
package imagemap

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"
	"sort"
	"strconv"
	"strings"
)

// doc is §3, field for field. A missing field or an extra one refuses the file:
// the format changes by editing that document and rebuilding both sides, never
// by a reader tolerating a shape.
type doc struct {
	Image *struct {
		SHA256  *string `json:"sha256"`
		Size    *int64  `json:"size"`
		OopSize *int    `json:"oop_size"`
	} `json:"image"`
	Entry   *string              `json:"entry"`
	Files   []string             `json:"files"`
	Methods map[string]docMethod `json:"methods"`
}

type docMethod struct {
	Name  *string  `json:"name"`
	File  *int     `json:"file"`
	Lines [][2]int `json:"lines"`
}

// Method is one method of the image: "<Vtable>>selector", an index into Files,
// and its [bytecode offset, line] pairs (empty for a method with no source).
type Method struct {
	Name  string
	File  int
	Lines [][2]int
}

// Map is an image map. Methods is keyed by the blob byte offset, the same key
// the VM prints in a profile dump (`mprof: <offset> <count>`).
type Map struct {
	SHA256  string
	Size    int64
	OopSize int
	Entry   string
	Files   []string
	Methods map[int]Method
}

// Load reads and checks an image map.
func Load(path string) (*Map, error) {
	raw, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	dec := json.NewDecoder(bytes.NewReader(raw))
	dec.DisallowUnknownFields()
	var d doc
	if err := dec.Decode(&d); err != nil {
		return nil, fmt.Errorf("%s is not an image map (%v); rebuild it (rete#337)", path, err)
	}
	var missing []string
	if d.Image == nil || d.Image.SHA256 == nil || d.Image.Size == nil || d.Image.OopSize == nil {
		missing = append(missing, "image")
	}
	if d.Entry == nil {
		missing = append(missing, "entry")
	}
	if len(d.Files) == 0 {
		missing = append(missing, "files")
	}
	if d.Methods == nil {
		missing = append(missing, "methods")
	}
	for k, m := range d.Methods {
		if m.Name == nil || m.File == nil || m.Lines == nil {
			missing = append(missing, "methods["+k+"]")
		}
	}
	if len(missing) > 0 {
		sort.Strings(missing)
		return nil, fmt.Errorf("%s is not an image map (no %s); rebuild it (rete#337)",
			path, strings.Join(missing, ", "))
	}
	m := &Map{
		SHA256:  *d.Image.SHA256,
		Size:    *d.Image.Size,
		OopSize: *d.Image.OopSize,
		Entry:   *d.Entry,
		Files:   d.Files,
		Methods: make(map[int]Method, len(d.Methods)),
	}
	for k, dm := range d.Methods {
		off, err := strconv.Atoi(k)
		if err != nil || off < 0 || strconv.Itoa(off) != k {
			return nil, fmt.Errorf("%s is not an image map (method key %q is not a blob offset); rebuild it (rete#337)", path, k)
		}
		m.Methods[off] = Method{Name: *dm.Name, File: *dm.File, Lines: dm.Lines}
	}
	return m, nil
}

// Offsets answers the method offsets in ascending order.
func (m *Map) Offsets() []int {
	offs := make([]int, 0, len(m.Methods))
	for off := range m.Methods {
		offs = append(offs, off)
	}
	sort.Ints(offs)
	return offs
}

// CheckImage refuses an image other than the one the map describes: its hash
// names one build.
func (m *Map) CheckImage(oimgPath string) error {
	b, err := os.ReadFile(oimgPath)
	if err != nil {
		return err
	}
	sum := sha256.Sum256(b)
	got := hex.EncodeToString(sum[:])
	if got != m.SHA256 || int64(len(b)) != m.Size {
		return fmt.Errorf("%s is not the image this map describes (sha256 %s, map says %s); rebuild both together",
			oimgPath, got[:12], m.SHA256[:min(12, len(m.SHA256))])
	}
	return nil
}
