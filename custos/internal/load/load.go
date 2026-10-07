// Copyright (c) 2026 Ekorau LLC

// Package load is `custos load` (rete#310): it writes an .oimg into a board's
// program region over the cable, then proves from the boot that the firmware is
// the same build and the program is the one written.
package load

import (
	"fmt"
	"io"
	"regexp"

	"github.com/davidg238/rete/custos/internal/oimg"
)

// Region is a board's program region: protocol/IMAGE-ARTIFACTS.md §2a.
type Region struct {
	Chip   string // esptool's --chip
	Offset int64
	Size   int64
}

// ESP32C6 copies IMAGE-ARTIFACTS.md §2a's row; the writer is
// nodus/oevm/zephyr/boards/esp32c6_devkitc_esp32c6_hpcore.overlay.
var ESP32C6 = Region{Chip: "esp32c6", Offset: 0x320000, Size: 512 * 1024}

// Report is what one boot said about itself.
type Report struct {
	BuildID    string
	ProgramSHA string // empty when no program loaded
	Refused    string // the loader's own reason, when it refused the region
}

var (
	buildRe   = regexp.MustCompile(`oevm: build-id (\S+)`)
	programRe = regexp.MustCompile(`oevm: program sha256 ([0-9a-f]{64})`)
	refusedRe = regexp.MustCompile(`oimg_load_blob: (.+)`)
)

// Note folds one console line into r and answers whether the boot is complete:
// a build-id, then the program's hash or the loader's refusal. A second
// build-id is a second boot, and starts the report over.
func (r *Report) Note(line string) bool {
	if m := buildRe.FindStringSubmatch(line); m != nil {
		*r = Report{BuildID: m[1]}
		return false
	}
	if r.BuildID == "" {
		return false
	}
	if m := programRe.FindStringSubmatch(line); m != nil {
		r.ProgramSHA = m[1]
		return true
	}
	if m := refusedRe.FindStringSubmatch(line); m != nil && m[1] != "" {
		r.Refused = m[1]
		return true
	}
	return false
}

// Board resets a board and answers its boot.
type Board interface {
	Boot() (Report, error)
}

// Result is what a load proved: the program written, the firmware that loaded
// it, and where it went (Offset 0 on a host VM). It is /load's JSON answer.
type Result struct {
	Image   string
	Size    int64
	SHA256  string
	BuildID string
	Offset  int64
}

// Target is where a program is loaded: a board over its cable (Loader) or a host
// VM (Host). Load answers an error unless the target then loads exactly that
// program.
type Target interface {
	Load(path string) (Result, error)
}

// Loader is a board's Target: `custos load`, and /load over --load-port.
type Loader struct {
	Region  Region
	Esptool string
	Port    string
	Board   Board
	Flash   func(argv []string) error
}

// Load writes the image at path into the region, and answers an error unless
// the board then boots the same firmware and loads exactly that program. The
// firmware prints the hash after the loader accepts the image and before
// bootstrap, so a load proves the program loaded, not that it runs.
func (l Loader) Load(path string) (Result, error) {
	img, err := oimg.Read(path)
	if err != nil {
		return Result{}, err
	}
	if img.Size > l.Region.Size {
		return Result{}, fmt.Errorf("%s is %d B; the %s program region holds %d B", path, img.Size, l.Region.Chip, l.Region.Size)
	}
	before, err := l.Board.Boot()
	if err != nil {
		return Result{}, err
	}
	argv := []string{l.Esptool, "--chip", l.Region.Chip, "--port", l.Port, "--baud", "921600",
		"write_flash", fmt.Sprintf("0x%x", l.Region.Offset), path}
	if err := l.Flash(argv); err != nil {
		return Result{}, fmt.Errorf("esptool: %w", err)
	}
	after, err := l.Board.Boot()
	if err != nil {
		return Result{}, err
	}
	if after.BuildID != before.BuildID {
		return Result{}, fmt.Errorf("the firmware changed across the load: build-id %s, then %s", before.BuildID, after.BuildID)
	}
	return prove(path, img, after, l.Region.Offset)
}

// prove answers the Result a boot after a load shows, or why it shows another.
func prove(path string, img *oimg.Image, after Report, offset int64) (Result, error) {
	if after.Refused != "" {
		return Result{}, fmt.Errorf("the target refused the program: oimg_load_blob: %s", after.Refused)
	}
	if after.ProgramSHA != img.SHA256 {
		return Result{}, fmt.Errorf("the target runs sha256 %.12s, not %s's %.12s", after.ProgramSHA, path, img.SHA256)
	}
	return Result{path, img.Size, img.SHA256, after.BuildID, offset}, nil
}

// Say is a Result as `custos load` prints it.
func (r Result) Say(w io.Writer) {
	fmt.Fprintf(w, "wrote %s (%d B) at 0x%x\nfirmware build-id %s unchanged\nprogram sha256 %.12s loaded\n",
		r.Image, r.Size, r.Offset, r.BuildID, r.SHA256)
}
