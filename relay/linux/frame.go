package main

import "errors"

// Frame is one datagram on the Bluetooth L2CAP channel. The contract is
// protocol/RELAY-BLE.md (rete#439); held to protocol/fixtures/relay/frame.hex by
// frame_test.go here, by FrameTest.kt in relay/android, and by
// nodus/oevm/tests/test_relay_frame.c on the node. Big-endian:
//
//	u16 localPort | u16 remotePort | u8 hostLen | host (text) | payload
type Frame struct {
	LocalPort  uint16
	RemotePort uint16
	Host       string
	Payload    []byte
}

const header = 5

var errMalformed = errors.New("malformed frame")

func (f Frame) Encode() ([]byte, error) {
	if len(f.Host) > 255 {
		return nil, errors.New("host longer than 255 bytes")
	}
	out := make([]byte, 0, header+len(f.Host)+len(f.Payload))
	out = append(out, byte(f.LocalPort>>8), byte(f.LocalPort), byte(f.RemotePort>>8), byte(f.RemotePort), byte(len(f.Host)))
	out = append(out, f.Host...)
	return append(out, f.Payload...), nil
}

// Decode answers the frame in b, or errMalformed (dropped, as UDP would drop it).
func Decode(b []byte) (Frame, error) {
	if len(b) < header {
		return Frame{}, errMalformed
	}
	hl := int(b[4])
	if len(b) < header+hl {
		return Frame{}, errMalformed
	}
	return Frame{
		LocalPort:  uint16(b[0])<<8 | uint16(b[1]),
		RemotePort: uint16(b[2])<<8 | uint16(b[3]),
		Host:       string(b[header : header+hl]),
		Payload:    append([]byte(nil), b[header+hl:]...),
	}, nil
}
