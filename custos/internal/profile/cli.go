// Copyright (c) 2026 Ekorau LLC

package profile

import (
	"flag"
	"fmt"
	"io"
	"os"
	"strings"

	"github.com/davidg238/rete/custos/internal/imagemap"
)

// Run is `custos profile`. args excludes the verb; it answers the exit status.
//
//	custos profile -map PortaBridgeNodeC6.oimg.map.json ~/.local/state/rete/console/console-c6spare-2026-10-05.log
//	custos profile -map … -top 20 -alloc < captured.log
func Run(args []string, stdin io.Reader, stdout, stderr io.Writer) int {
	fs := flag.NewFlagSet("custos profile", flag.ContinueOnError)
	fs.SetOutput(stderr)
	mapPath := fs.String("map", "", "the .oimg.map.json written beside the image (required)")
	top := fs.Int("top", 0, "show only the first N rows (percentages stay of the whole window)")
	alloc := fs.Bool("alloc", false, "also rank the allocation columns")
	fs.Usage = func() {
		fmt.Fprintln(stderr, "usage: custos profile -map <name>.oimg.map.json [-top N] [-alloc] [log ...]   (flags before the logs; stdin when no log)")
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
	m, err := imagemap.Load(*mapPath)
	if err != nil {
		return fail(2, "%v", err)
	}
	var text strings.Builder
	if fs.NArg() == 0 {
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
