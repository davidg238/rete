// Package nsl runs the nsl compiler's tools for custos's verbs that compile:
// patch (peer design D17) and build and run (D18). custos is Go and the compiler
// is Python; it runs them as commands, as it runs esptool and gdb.
package nsl

import (
	"bytes"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
)

// Tools is the nsl compiler's directory: RETE_NSL_TOOLS, or the checkout's
// (the pattern console's ELFRoot uses).
func Tools() string {
	if v := os.Getenv("RETE_NSL_TOOLS"); v != "" {
		return v
	}
	return filepath.Join(os.Getenv("HOME"), "workspace", "rete", "nodus", "nsl", "tools")
}

// Refused is the tool's own refusal, said as it said it.
type Refused struct{ Why string }

func (r Refused) Error() string { return r.Why }

// run runs a tool and answers its stdout, or the refusal its exit status 1 and
// its last lines carry.
func run(tool string, args ...string) (string, error) {
	path := filepath.Join(Tools(), tool)
	if _, err := os.Stat(path); err != nil {
		return "", fmt.Errorf("no nsl compiler at %s (set RETE_NSL_TOOLS): %w", Tools(), err)
	}
	var out, errb bytes.Buffer
	cmd := exec.Command("python3", append([]string{path}, args...)...)
	cmd.Stdout, cmd.Stderr = &out, &errb
	if err := cmd.Run(); err != nil {
		why := strings.TrimSpace(out.String() + "\n" + errb.String())
		if ee, ok := err.(*exec.ExitError); ok && ee.ExitCode() == 1 {
			return "", Refused{lastLines(why, 4)}
		}
		return "", fmt.Errorf("%s: %v: %s", tool, err, lastLines(why, 4))
	}
	return out.String(), nil
}

func lastLines(s string, n int) string {
	lines := strings.Split(strings.TrimSpace(s), "\n")
	if len(lines) > n {
		lines = lines[len(lines)-n:]
	}
	return strings.Join(lines, "\n")
}

// Patch compiles one method of edited (the whole edited source of the program
// whose map is mapPath and whose source is source) against that image, and
// answers the VM's patch text: what `dbg:patch` sends (rete#313).
func Patch(mapPath, source string, edited []byte, method string) (string, error) {
	f, err := os.CreateTemp("", "custos-patch-*.nsl")
	if err != nil {
		return "", err
	}
	defer os.Remove(f.Name())
	if _, err := f.Write(edited); err != nil {
		f.Close()
		return "", err
	}
	f.Close()
	out, err := run("patch_method.py", mapPath, source, f.Name(), method)
	return strings.TrimSpace(out), err
}
