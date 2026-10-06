// Copyright (c) 2026 Ekorau LLC

package load

import (
	"bufio"
	"fmt"
	"os"
	"os/exec"
	"time"

	"github.com/davidg238/rete/custos/internal/oimg"
)

// Host is a host VM's Target (peer design D14, rete#367): the agent's board-free
// path. A host VM has no program region, so loading is running the image once
// and reading the boot lines a board prints (oevm_image_main.c); the VM is
// stopped as soon as the boot is complete.
type Host struct {
	Argv    []string // the VM's command line; the image's path is appended
	Timeout time.Duration
}

func (h Host) Load(path string) (Result, error) {
	img, err := oimg.Read(path)
	if err != nil {
		return Result{}, err
	}
	cmd := exec.Command(h.Argv[0], append(h.Argv[1:len(h.Argv):len(h.Argv)], path)...)
	r, w, err := os.Pipe()
	if err != nil {
		return Result{}, err
	}
	cmd.Stdout, cmd.Stderr = w, w
	if err := cmd.Start(); err != nil {
		w.Close()
		r.Close()
		return Result{}, err
	}
	w.Close() // the child holds its own copy; EOF arrives when it exits
	defer func() {
		cmd.Process.Kill()
		cmd.Wait()
		r.Close()
	}()
	done := make(chan Report, 1)
	go func() {
		var rep Report
		for sc := bufio.NewScanner(r); sc.Scan(); {
			if rep.Note(sc.Text()) {
				break
			}
		}
		done <- rep
	}()
	select {
	case rep := <-done:
		if rep.ProgramSHA == "" && rep.Refused == "" {
			return Result{}, fmt.Errorf("the host VM ended before a complete boot (build-id %q)", rep.BuildID)
		}
		return prove(path, img, rep, 0)
	case <-time.After(h.Timeout):
		return Result{}, fmt.Errorf("no complete boot from the host VM within %s", h.Timeout)
	}
}
