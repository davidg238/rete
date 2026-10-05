// Copyright (c) 2026 Ekorau LLC

package load

import (
	"bytes"
	"fmt"
	"strings"
	"time"

	"go.bug.st/serial"
)

// SerialBoard boots a CP2102N devkit by its reset line and reads the boot. It
// pulses the reset itself rather than relying on whether an open reboots the
// board (rete#319 says yes, #355 says not from Go).
type SerialBoard struct {
	Port    string
	Baud    int
	Timeout time.Duration
}

func (b SerialBoard) Boot() (Report, error) {
	var r Report
	p, err := serial.Open(b.Port, &serial.Mode{
		BaudRate:          b.Baud,
		InitialStatusBits: &serial.ModemOutputBits{DTR: false, RTS: false},
	})
	if err != nil {
		return r, err
	}
	defer p.Close()
	if err := p.SetReadTimeout(200 * time.Millisecond); err != nil {
		return r, err
	}
	// RTS is EN and DTR is BOOT (rete#311): EN low with BOOT high, then release,
	// is a run-mode reset — target::reset_board's sequence.
	if err := p.SetRTS(true); err != nil {
		return r, err
	}
	time.Sleep(150 * time.Millisecond)
	if err := p.SetRTS(false); err != nil {
		return r, err
	}
	var buf []byte
	chunk := make([]byte, 256)
	for deadline := time.Now().Add(b.Timeout); time.Now().Before(deadline); {
		n, err := p.Read(chunk)
		if err != nil {
			return r, err
		}
		buf = append(buf, chunk[:n]...)
		for {
			i := bytes.IndexByte(buf, '\n')
			if i < 0 {
				break
			}
			line := strings.TrimSuffix(string(buf[:i]), "\r")
			buf = buf[i+1:]
			if r.Note(line) {
				return r, nil
			}
		}
	}
	return r, fmt.Errorf("no complete boot on %s within %s (build-id %q) — is a collector holding the port?", b.Port, b.Timeout, r.BuildID)
}
