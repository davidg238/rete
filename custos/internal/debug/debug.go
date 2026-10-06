// Copyright (c) 2026 Ekorau LLC

// Package debug is `custos debug`, the in-image debugger for a person: source
// lines in, source lines out (rete#311, rewritten from dbg.py in rete#339).
//
// The VM's debugger speaks `dbg:*` lines (nodus/nsl/lib/supervisor.nsl) and
// knows a method only by its blob offset -- the image map's key -- and a pc:
// `dbg:break 3952 2`, `dbg:paused break 3952 2` (rete#323). This package sits
// between a person and that line and does the two translations, using the image
// map the compiler writes beside every image (`<name>.oimg.map.json`,
// protocol/IMAGE-ARTIFACTS.md):
//
//	break 26                 ->  dbg:break 3952 2   (line 26 is in countTo:, pc 2)
//	dbg:paused break 3952 2  ->  stopped (break) at 46_debug_target.nsl:26  DebugTargetTest>>countTo:
//	                               26 |     1 to: n do: [ :i | i println. sum:: sum + i ].
//
// The same session drives a board over its console cable (-port, one handle, a
// collector stopped first) or a host VM it starts itself (everything after --):
//
//	custos debug -map /tmp/46_debug_target.oimg.map.json -- /tmp/oevm_img /tmp/46_debug_target.oimg
//	custos debug -map DebugProbeC6.oimg.map.json -port /dev/serial/by-id/usb-Silicon_Labs_CP2102N_...
//
// What a line can be: any line with code in the app's file -- a method of any
// of its classes, or a block the compiler kept as a block, which is its own
// method with its own offset. A line with no code of its own (a comment, a
// blank) inside a method moves forward to the next line that has some, and says
// so; outside every method (a header, `| locals |`) it is refused with the
// nearest lines that have code. docs/tutorials/04-debugger.md is the page for a
// person.
package debug

import (
	"bufio"
	"flag"
	"fmt"
	"io"
	"os"
	"strings"
	"time"
)

// Run is `custos debug`. args excludes the verb; it answers the exit status.
func Run(args []string, stdin *os.File, stdout, stderr io.Writer) int {
	fs := flag.NewFlagSet("custos debug", flag.ContinueOnError)
	fs.SetOutput(stderr)
	mapPath := fs.String("map", "", "the .oimg.map.json written beside the image (required)")
	source := fs.String("source", "", "the .nsl it came from (default: beside the map)")
	port := fs.String("port", "", "a board's console cable (else run the host VM command after --)")
	baud := fs.Int("baud", 115200, "the cable's baud rate")
	fs.Usage = func() {
		fmt.Fprintln(stderr, "usage: custos debug -map <name>.oimg.map.json [-source <file>.nsl] (-port <tty> | -- <host VM command>)")
		fs.PrintDefaults()
		fmt.Fprintln(stderr, "\nCommands at the prompt (type `help`):\n"+Help)
	}
	if err := fs.Parse(args); err != nil {
		return 2
	}
	argv := fs.Args()
	fail := func(format string, a ...any) int {
		fmt.Fprintf(stderr, "custos debug: "+format+"\n", a...)
		return 2
	}
	if *mapPath == "" {
		return fail("-map is required")
	}
	if (*port != "") == (len(argv) > 0) {
		return fail("give exactly one of -port or a host VM command after --")
	}
	sm, err := LoadSourceMap(*mapPath, *source)
	if err != nil {
		return fail("%v", err)
	}
	for _, a := range argv {
		if strings.HasSuffix(a, ".oimg") {
			if err := sm.CheckImage(a); err != nil {
				return fail("%v", err)
			}
		}
	}

	s := NewSession(sm, stdout)
	var line Line
	if *port != "" {
		line, err = OpenPort(*port, *baud, s.OnLine, s.OnLost)
	} else {
		line, err = StartHost(argv, s.OnLine, s.OnEnd)
	}
	if err != nil {
		return fail("%v", err)
	}
	s.Attach(line)

	// A board may already be waiting (its dbg:ready printed before we opened the
	// port) or running; either way the first command finds out. A host VM prints
	// dbg:ready.
	ready := 10 * time.Second
	if *port != "" {
		ready = 5 * time.Second
	}
	s.Wait(`^dbg:ready$`, ready, 0)

	interactive := isTerminal(stdin)
	in := bufio.NewScanner(stdin)
	for !s.Gone() {
		if interactive {
			fmt.Fprint(stdout, "(dbg) ")
		}
		if !in.Scan() {
			// Out of commands: watch the program to its next stop or its end, so
			// a piped script sees what its last `continue` led to.
			if s.Running() {
				s.Wait(`^dbg:paused |^dbg:ready$`, 10*time.Second, -1)
			}
			break
		}
		cmd := in.Text()
		if !interactive {
			s.say("(dbg) %s", cmd)
		}
		if !s.Do(cmd) {
			break
		}
		if s.Running() {
			s.Wait(`^dbg:paused |^dbg:ready$`, 100*time.Millisecond, -1) // let a prompt stop print first
		}
	}
	return 0
}

func isTerminal(f *os.File) bool {
	st, err := f.Stat()
	return err == nil && st.Mode()&os.ModeCharDevice != 0
}
