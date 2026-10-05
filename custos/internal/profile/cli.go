// Copyright (c) 2026 Ekorau LLC

package profile

import (
	"flag"
	"fmt"
	"io"
	"os"
	"strings"
	"time"

	"github.com/davidg238/rete/custos/internal/debug"
	"github.com/davidg238/rete/custos/internal/imagemap"
)

// Run is `custos profile`. args excludes the verb; it answers the exit status.
//
//	custos profile -map PortaBridgeNodeC6.oimg.map.json ~/.local/state/rete/console/console-c6spare-2026-10-05.log
//	custos profile -map … -top 20 -alloc < captured.log
//	custos profile -map PortaBridgeNodeC6.oimg.map.json -port /dev/serial/by-id/usb-Silicon_Labs_CP2102N_... -ms 3000
//
// With -port it asks the node for a window over its console cable (the kernel's
// Profiler, rete#312) and ranks what comes back. The port is opened once, with
// no collector holding it: an open reboots the C6 (rete#319).
func Run(args []string, stdin io.Reader, stdout, stderr io.Writer) int {
	fs := flag.NewFlagSet("custos profile", flag.ContinueOnError)
	fs.SetOutput(stderr)
	mapPath := fs.String("map", "", "the .oimg.map.json written beside the image (required)")
	top := fs.Int("top", 0, "show only the first N rows (percentages stay of the whole window)")
	alloc := fs.Bool("alloc", false, "also rank the allocation columns")
	port := fs.String("port", "", "a board's console cable: capture a window live instead of reading logs")
	ms := fs.Int("ms", 0, "the window's length in ms (required with -port)")
	baud := fs.Int("baud", 115200, "the cable's baud rate")
	fs.Usage = func() {
		fmt.Fprintln(stderr, "usage: custos profile -map <name>.oimg.map.json [-top N] [-alloc] (-port <tty> -ms N | [log ...])   (flags before the logs; stdin when no log)")
		fs.PrintDefaults()
	}
	if err := fs.Parse(args); err != nil {
		return 2
	}
	fail := func(code int, format string, a ...any) int {
		fmt.Fprintf(stderr, "custos profile: "+format+"\n", a...)
		return code
	}
	if *top < 0 {
		return fail(2, "-top must be 0 (every row) or more")
	}
	if *mapPath == "" {
		fs.Usage()
		return fail(2, "-map is required")
	}
	if *port != "" && fs.NArg() > 0 {
		return fail(2, "give -port or logs, not both")
	}
	if (*port != "") != (*ms > 0) {
		fs.Usage()
		return fail(2, "-port and -ms (a positive window length) go together")
	}
	m, err := imagemap.Load(*mapPath)
	if err != nil {
		return fail(2, "%v", err)
	}
	var text strings.Builder
	if *port != "" {
		open := func(onLine func(string)) (Sender, error) { return debug.OpenPort(*port, *baud, onLine) }
		got, err := Capture(open, *ms, time.Duration(*ms)*time.Millisecond+10*time.Second)
		if err != nil {
			return fail(1, "%v", err)
		}
		text.WriteString(got)
	} else if fs.NArg() == 0 {
		b, err := io.ReadAll(stdin)
		if err != nil {
			return fail(2, "%v", err)
		}
		text.Write(b)
	}
	for _, p := range fs.Args() {
		b, err := os.ReadFile(p)
		if err != nil {
			return fail(2, "%v", err)
		}
		text.Write(b)
	}
	d := Parse(text.String())
	if len(d.Rows) == 0 {
		return fail(1, "no 'mprof:' lines found — was the image built with OEVM_METHOD_PROFILE=1, and did the window close?")
	}
	Write(stdout, d, m, *top, *alloc)
	return 0
}
