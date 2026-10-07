// Copyright (c) 2026 Ekorau LLC

package load

import (
	"flag"
	"fmt"
	"io"
	"os/exec"
	"time"
)

// Run is `custos load`. args excludes the verb; it answers the exit status.
//
//	custos load -port /dev/serial/by-id/usb-Silicon_Labs_CP2102N_... PortaBridgeNodeC6.oimg
//
// It needs the port to itself: stop the board's collector first
// (systemctl --user stop rete-console@<board>).
func Run(args []string, stdout, stderr io.Writer) int {
	fs := flag.NewFlagSet("custos load", flag.ContinueOnError)
	fs.SetOutput(stderr)
	port := fs.String("port", "", "the board's console cable (required)")
	esptool := fs.String("esptool", "esptool.py", "the esptool to run")
	baud := fs.Int("baud", 115200, "the console's baud rate")
	fs.Usage = func() {
		fmt.Fprintln(stderr, "usage: custos load -port <tty> [-esptool esptool.py] [-baud N] <name>.oimg")
		fs.PrintDefaults()
	}
	if err := fs.Parse(args); err != nil {
		return 2
	}
	if *port == "" || fs.NArg() != 1 {
		fs.Usage()
		return 2
	}
	l := Loader{
		Region:  ESP32C6,
		Esptool: *esptool,
		Port:    *port,
		Board:   SerialBoard{Port: *port, Baud: *baud, Timeout: 15 * time.Second},
		Flash: func(argv []string) error {
			cmd := exec.Command(argv[0], argv[1:]...)
			cmd.Stdout, cmd.Stderr = stderr, stderr
			return cmd.Run()
		},
	}
	res, err := l.Load(fs.Arg(0))
	if err != nil {
		fmt.Fprintf(stderr, "custos load: %v\n", err)
		return 1
	}
	res.Say(stdout)
	return 0
}
