package main

import (
	"bytes"
	"encoding/hex"
	"os"
	"strings"
	"testing"
)

// The golden vector both ends test against (protocol/RELAY-BLE.md).
func golden(t *testing.T) []byte {
	raw, err := os.ReadFile("../../protocol/fixtures/relay/frame.hex")
	if err != nil {
		t.Fatal(err)
	}
	b, err := hex.DecodeString(strings.TrimSpace(string(raw)))
	if err != nil {
		t.Fatal(err)
	}
	return b
}

var want = Frame{LocalPort: 16970, RemotePort: 6969, Host: "fd7a:115c:a1e0::1", Payload: []byte("hi")}

func TestEncodeGolden(t *testing.T) {
	got, err := want.Encode()
	if err != nil || !bytes.Equal(got, golden(t)) {
		t.Fatalf("encode = %x, %v; want %x", got, err, golden(t))
	}
}

func TestDecodeGolden(t *testing.T) {
	f, err := Decode(golden(t))
	if err != nil || f.LocalPort != want.LocalPort || f.RemotePort != want.RemotePort ||
		f.Host != want.Host || !bytes.Equal(f.Payload, want.Payload) {
		t.Fatalf("decode = %+v, %v", f, err)
	}
}

func TestDecodeMalformed(t *testing.T) {
	g := golden(t)
	for _, b := range [][]byte{g[:4], g[:header+3]} { // short header; host runs past the end
		if _, err := Decode(b); err != errMalformed {
			t.Fatalf("Decode(%x) = %v, want errMalformed", b, err)
		}
	}
}
