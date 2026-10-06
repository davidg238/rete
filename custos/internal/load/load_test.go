// Copyright (c) 2026 Ekorau LLC

package load

import (
	"bytes"
	"errors"
	"strings"
	"testing"

	"github.com/davidg238/rete/custos/internal/oimg"
)

const fixture = "../../../protocol/fixtures/image/Fixture.oimg"

func TestNoteKeepsTheLatestBoot(t *testing.T) {
	var r Report
	if r.Note("oevm: build-id OLD") {
		t.Fatal("a build-id alone is not a complete boot")
	}
	r.Note("oevm: program sha256 " + strings.Repeat("a", 64) + " (40 ms)") // the first boot, complete
	if r.Note("oevm: build-id NEW") {                                      // a second boot: start over
		t.Fatal("a second build-id must start the report over")
	}
	if r.ProgramSHA != "" {
		t.Fatalf("the second boot kept the first boot's program: %+v", r)
	}
	if !r.Note("2026-10-05T14:00:52.394-07:00 oevm: program sha256 " + strings.Repeat("b", 64) + " (41 ms)") {
		t.Fatal("build-id + program should complete the boot")
	}
	if r.BuildID != "NEW" || r.ProgramSHA != strings.Repeat("b", 64) {
		t.Fatalf("%+v", r)
	}
}

func TestNoteCompletesOnTheLoadersRefusal(t *testing.T) {
	var r Report
	r.Note("oevm: build-id B1")
	if !r.Note("oimg_load_blob: image too small (0 bytes)") || r.Refused != "image too small (0 bytes)" {
		t.Fatalf("%+v", r)
	}
}

type fakeBoard struct {
	boots []Report
	errs  []error
	n     int
}

func (b *fakeBoard) Boot() (Report, error) {
	i := b.n
	b.n++
	return b.boots[i], b.errs[i]
}

func fixtureSHA(t *testing.T) string {
	img, err := oimg.Read(fixture)
	if err != nil {
		t.Fatal(err)
	}
	return img.SHA256
}

func run(t *testing.T, board *fakeBoard, region Region) (argv []string, out string, err error) {
	var buf bytes.Buffer
	l := Loader{
		Region:  region,
		Esptool: "esptool.py",
		Port:    "/dev/ttyX",
		Board:   board,
		Flash:   func(a []string) error { argv = a; return nil },
	}
	res, err := l.Load(fixture)
	if err == nil {
		res.Say(&buf)
	}
	return argv, buf.String(), err
}

var big = Region{Chip: "esp32c6", Offset: 0x320000, Size: 1 << 20}

func TestLoadWritesAtTheRegionAndProvesTheFirmwareUnchanged(t *testing.T) {
	sha := fixtureSHA(t)
	b := &fakeBoard{boots: []Report{{BuildID: "B1", ProgramSHA: strings.Repeat("0", 64)}, {BuildID: "B1", ProgramSHA: sha}}, errs: []error{nil, nil}}
	argv, out, err := run(t, b, big)
	if err != nil {
		t.Fatal(err)
	}
	want := "esptool.py --chip esp32c6 --port /dev/ttyX --baud 921600 write_flash 0x320000 " + fixture
	if strings.Join(argv, " ") != want {
		t.Fatalf("argv %q\nwant %q", strings.Join(argv, " "), want)
	}
	// The hash line is printed before bootstrap, so it proves the program LOADED,
	// not that it runs (final review, #310): the report must not claim more.
	if !strings.Contains(out, "build-id B1 unchanged") || !strings.Contains(out, "program sha256 "+sha[:12]+" loaded") ||
		strings.Contains(out, "running") {
		t.Fatalf("out %q", out)
	}
}

func TestLoadIntoAnEmptyPartition(t *testing.T) {
	b := &fakeBoard{boots: []Report{{BuildID: "B1", Refused: "image too small (0 bytes)"}, {BuildID: "B1", ProgramSHA: fixtureSHA(t)}}, errs: []error{nil, nil}}
	if _, _, err := run(t, b, big); err != nil {
		t.Fatal(err)
	}
}

func TestLoadFailsWhenTheFirmwareChanged(t *testing.T) {
	b := &fakeBoard{boots: []Report{{BuildID: "B1"}, {BuildID: "B2", ProgramSHA: fixtureSHA(t)}}, errs: []error{nil, nil}}
	if _, _, err := run(t, b, big); err == nil || !strings.Contains(err.Error(), "build-id") {
		t.Fatalf("err %v", err)
	}
}

func TestLoadReportsTheLoadersRefusal(t *testing.T) {
	b := &fakeBoard{boots: []Report{{BuildID: "B1"}, {BuildID: "B1", Refused: "registry_version 7 != firmware 8"}}, errs: []error{nil, nil}}
	if _, _, err := run(t, b, big); err == nil || !strings.Contains(err.Error(), "registry_version 7 != firmware 8") {
		t.Fatalf("err %v", err)
	}
}

func TestLoadFailsWhenTheBoardRunsAnotherProgram(t *testing.T) {
	b := &fakeBoard{boots: []Report{{BuildID: "B1"}, {BuildID: "B1", ProgramSHA: strings.Repeat("f", 64)}}, errs: []error{nil, nil}}
	if _, _, err := run(t, b, big); err == nil || !strings.Contains(err.Error(), "sha256") {
		t.Fatalf("err %v", err)
	}
}

func TestLoadRefusesAnImageLargerThanTheRegion(t *testing.T) {
	b := &fakeBoard{boots: []Report{{BuildID: "B1"}}, errs: []error{nil}}
	argv, _, err := run(t, b, Region{Chip: "esp32c6", Offset: 0x320000, Size: 16})
	if err == nil || argv != nil || b.n != 0 {
		t.Fatalf("err %v argv %v boots %d: must refuse before touching the board", err, argv, b.n)
	}
}

func TestLoadStopsWhenTheBoardDoesNotBoot(t *testing.T) {
	b := &fakeBoard{boots: []Report{{}}, errs: []error{errors.New("no complete boot on /dev/ttyX within 15s — is a collector holding the port?")}}
	argv, _, err := run(t, b, big)
	if err == nil || argv != nil || !strings.Contains(err.Error(), "collector") {
		t.Fatalf("err %v argv %v", err, argv)
	}
}
