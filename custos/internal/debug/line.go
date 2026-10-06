// Copyright (c) 2026 Ekorau LLC

package debug

import (
	"bytes"
	"io"
	"os"
	"os/exec"
	"strings"
	"time"

	"go.bug.st/serial"
)

// Line is one text line each way to a VM: a board's console cable or a host VM's
// stdio. A reader goroutine hands every whole line to onLine as it arrives, and
// calls onEnd once when a host VM exits, so output reaches the person while they
// sit at the prompt and nothing waits on the next read.
type Line interface {
	Send(line string) error
}

type writerLine struct{ w io.Writer }

func (l writerLine) Send(line string) error {
	_, err := io.WriteString(l.w, line+"\n")
	return err
}

// StartHost runs a host VM with its debugger on (OEVM_DEBUG=1) and reads its
// stdout and stderr as one stream.
func StartHost(argv []string, onLine func(string), onEnd func()) (Line, error) {
	cmd := exec.Command(argv[0], argv[1:]...)
	cmd.Env = append(os.Environ(), "OEVM_DEBUG=1")
	stdin, err := cmd.StdinPipe()
	if err != nil {
		return nil, err
	}
	r, w, err := os.Pipe()
	if err != nil {
		return nil, err
	}
	cmd.Stdout, cmd.Stderr = w, w
	if err := cmd.Start(); err != nil {
		return nil, err
	}
	w.Close() // the child holds its own copy; EOF arrives when it exits
	go pump(func(b []byte) (int, error) { return r.Read(b) }, onLine, onEnd)
	return writerLine{stdin}, nil
}

// portLine is a cable: Close releases the port (opened exclusively) and ends
// its reader.
type portLine struct {
	writerLine
	p serial.Port
}

func (l portLine) Close() error { return l.p.Close() }

// OpenPort opens a board's console cable without a reset pulse: DTR and RTS are
// held low from the open on (rete#311; the CP2102N devkit wires them to EN and
// BOOT). The Line it answers is also an io.Closer. onEnd is called once when the
// cable's read fails: unplugged, or closed by Close.
func OpenPort(port string, baud int, onLine func(string), onEnd func()) (Line, error) {
	p, err := serial.Open(port, &serial.Mode{
		BaudRate:          baud,
		InitialStatusBits: &serial.ModemOutputBits{DTR: false, RTS: false},
	})
	if err != nil {
		return nil, err
	}
	if err := p.SetReadTimeout(200 * time.Millisecond); err != nil {
		return nil, err
	}
	// A read timeout is an empty read, not the end; an unplugged cable is a
	// failed read (go.bug.st/serial answers PortClosed).
	go pump(func(b []byte) (int, error) { return p.Read(b) }, onLine, onEnd)
	return portLine{writerLine{p}, p}, nil
}

// pump splits what read returns into lines, strips a trailing \r, and calls
// onEnd when read fails (a host VM's exit, a cable's loss). A partial last line
// is dropped.
func pump(read func([]byte) (int, error), onLine func(string), onEnd func()) {
	var buf []byte
	chunk := make([]byte, 256)
	for {
		n, err := read(chunk)
		buf = append(buf, chunk[:n]...)
		for {
			i := bytes.IndexByte(buf, '\n')
			if i < 0 {
				break
			}
			onLine(strings.TrimSuffix(strings.ToValidUTF8(string(buf[:i]), "�"), "\r"))
			buf = buf[i+1:]
		}
		if err != nil {
			onEnd()
			return
		}
	}
}
