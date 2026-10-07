// Copyright (c) 2026 Ekorau LLC

package console

// backtrace.go — a panic resolved against the ELF that produced it, or refused.
//
// Symbolizing a panic against the current build dir after a reflash answers
// confident, precise and wrong locations, which is worse than no answer because
// it looks like one. So there is no fallback: the run names a build id, the
// archive (archive.sh, at flash) has its ELF or the answer is a refusal. The
// one exception is a build id the asker names (assume), and it is written on
// every line of the answer, so the assumption travels with it.
//
// The core is rebuilt by Zephyr's own scripts/coredump/ (RISC-V on both sides)
// and read by the SDK's gdb. Three knobs, as before: ZEPHYR_BASE,
// RETE_ZEPHYR_PYTHON (the venv with pyelftools), RETE_GDB; and RETE_ELF_ROOT,
// the archive, which archive.sh also reads.

import (
	"context"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"
	"time"
)

// Refused is a backtrace that would be a guess.
type Refused struct{ Why string }

func (r Refused) Error() string { return r.Why }

func home(p string) string { return filepath.Join(os.Getenv("HOME"), p) }

func envOr(name, def string) string {
	if v := os.Getenv(name); v != "" {
		return v
	}
	return def
}

// ELFRoot is the flash archive: <root>/<build-id>/zephyr.elf.
func ELFRoot() string { return envOr("RETE_ELF_ROOT", home(".local/state/rete/elf")) }

func zephyrPython() string {
	if v := os.Getenv("RETE_ZEPHYR_PYTHON"); v != "" {
		return v
	}
	for _, c := range []string{home("zephyr-esp32/.venv/bin/python"), home("zephyrproject/.venv/bin/python")} {
		if _, err := os.Stat(c); err == nil {
			return c
		}
	}
	return "python3" // without pyelftools the parser says so
}

// gdb is the SDK's RISC-V gdb. Never the host's: it answers plausible, wrong
// output for a RISC-V core.
func gdb() (string, error) {
	if v := os.Getenv("RETE_GDB"); v != "" {
		return v, nil
	}
	if p, err := exec.LookPath("riscv64-zephyr-elf-gdb"); err == nil {
		return p, nil
	}
	found, _ := filepath.Glob(home("zephyr-sdk-*/riscv64-zephyr-elf/bin/riscv64-zephyr-elf-gdb"))
	sort.Sort(sort.Reverse(sort.StringSlice(found)))
	if len(found) > 0 {
		return found[0], nil
	}
	return "", errors.New("riscv64-zephyr-elf-gdb is not on PATH or under ~/zephyr-sdk-*: set RETE_GDB (the host gdb is never used)")
}

// Backtrace resolves p, a panic of a run that printed buildID, or of the build
// the asker names in assume. workdir keeps the rebuilt core.
func Backtrace(ctx context.Context, p Panic, buildID, assume, root, workdir string) (string, error) {
	key := buildID
	if assume != "" {
		key = assume
	}
	if key == "" {
		return "", Refused{"this run never printed a build id (it faulted before the line), so nothing says which binary it ran; " +
			"name it with assume=<build-id> if you know it. Nothing infers it for you."}
	}
	if len(p.Coredump) == 0 {
		return "", Refused{"this panic has no #CD: coredump (the fault died before the backend ran): nothing to resolve"}
	}
	elf := filepath.Join(root, key, "zephyr.elf")
	if _, err := os.Stat(elf); err != nil {
		return "", Refused{fmt.Sprintf("no archived ELF for build id %s under %s; the current build dir would answer precise, wrong "+
			"locations, so this refuses. If you know the build, name it with assume=<build-id> (%s says what was flashed last).",
			key, root, filepath.Join(root, "last-flashed-<board>"))}
	}
	g, err := gdb()
	if err != nil {
		return "", err
	}
	// The core is rebuilt from this panic's lines alone, so a log holding two
	// panics never makes one unusable core.
	log, core := filepath.Join(workdir, "panic.log"), filepath.Join(workdir, "core.bin")
	text := "#CD:BEGIN#\n#CD:" + strings.Join(p.Coredump, "\n#CD:") + "\n#CD:END#\n"
	if err := os.WriteFile(log, []byte(text), 0o644); err != nil {
		return "", err
	}
	base := envOr("ZEPHYR_BASE", home("zephyr-esp32/zephyr"))
	py := zephyrPython()
	ctx, cancel := context.WithTimeout(ctx, 2*time.Minute)
	defer cancel()
	if out, err := exec.CommandContext(ctx, py, filepath.Join(base, "scripts/coredump/coredump_serial_log_parser.py"), log, core).CombinedOutput(); err != nil {
		return "", fmt.Errorf("coredump_serial_log_parser: %w: %s", err, out)
	}
	// --pipe, not a TCP port: the stub accepts exactly once, so a readiness
	// probe would take gdb's connection. The stub's teardown raises on --pipe
	// (upstream) after the backtrace is out, so its stderr is not the answer.
	remote := fmt.Sprintf("target remote | %s %s --pipe %s %s", py, filepath.Join(base, "scripts/coredump/coredump_gdbserver.py"), elf, core)
	out, _ := exec.CommandContext(ctx, g, elf, "-batch", "-ex", remote, "-ex", "bt").Output()
	if strings.TrimSpace(string(out)) == "" {
		return "", fmt.Errorf("gdb answered no backtrace for %s", key)
	}
	bt := strings.TrimRight(string(out), "\n")
	if assume != "" {
		bt = "[ASSUMED build-id " + key + "] " + strings.ReplaceAll(bt, "\n", "\n[ASSUMED build-id "+key+"] ")
	}
	return bt, nil
}
