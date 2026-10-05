// Copyright (c) 2026 Ekorau LLC

package oimg

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/davidg238/rete/custos/internal/imagemap"
)

const fixture = "../../../protocol/fixtures/image/Fixture"

func TestTheFixtureReadsAndHashesAsItsMapSays(t *testing.T) {
	img, err := Read(fixture + ".oimg")
	if err != nil {
		t.Fatal(err)
	}
	m, err := imagemap.Load(fixture + ".oimg.map.json")
	if err != nil {
		t.Fatal(err)
	}
	if img.SHA256 != m.SHA256 || img.Size != m.Size || int64(img.BlobSize) != img.Size {
		t.Fatalf("image sha %s size %d blob %d; map sha %s size %d", img.SHA256, img.Size, img.BlobSize, m.SHA256, m.Size)
	}
}

func writeVariant(t *testing.T, edit func([]byte) []byte) string {
	t.Helper()
	b, err := os.ReadFile(fixture + ".oimg")
	if err != nil {
		t.Fatal(err)
	}
	p := filepath.Join(t.TempDir(), "x.oimg")
	if err := os.WriteFile(p, edit(append([]byte(nil), b...)), 0o644); err != nil {
		t.Fatal(err)
	}
	return p
}

func TestReadRefuses(t *testing.T) {
	cases := map[string]struct {
		edit func([]byte) []byte
		want string
	}{
		"bad magic":    {func(b []byte) []byte { b[0] = 'X'; return b }, "magic"},
		"truncated":    {func(b []byte) []byte { return b[:19] }, "20-byte prefix"},
		"size differs": {func(b []byte) []byte { return append(b, 0) }, "blob_size"},
	}
	for name, c := range cases {
		t.Run(name, func(t *testing.T) {
			_, err := Read(writeVariant(t, c.edit))
			if err == nil || !strings.Contains(err.Error(), c.want) {
				t.Fatalf("err %v, want it to mention %q", err, c.want)
			}
		})
	}
}
