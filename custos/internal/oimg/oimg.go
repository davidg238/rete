// Copyright (c) 2026 Ekorau LLC

// Package oimg reads the part of an .oimg custos reads: the 20-byte prefix and
// the hash of the whole file (protocol/IMAGE-ARTIFACTS.md §2). Everything after
// the prefix belongs to the VM.
package oimg

import (
	"bytes"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"fmt"
	"os"
)

// Image is an .oimg as custos sees it.
type Image struct {
	FormatVersion   uint16
	Flags           uint16
	RegistryVersion uint32
	BlobSize        uint32
	Size            int64
	SHA256          string // lowercase hex of the whole file: the program's identity (decision 2)
}

// Read answers the image at path, or refuses it: a wrong magic, a file shorter
// than the prefix, or a blob_size other than the file's length.
func Read(path string) (*Image, error) {
	b, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	if len(b) < 20 {
		return nil, fmt.Errorf("%s is %d bytes, shorter than the 20-byte prefix (IMAGE-ARTIFACTS.md §2)", path, len(b))
	}
	if !bytes.Equal(b[:4], []byte("OIMG")) {
		return nil, fmt.Errorf("%s is not an .oimg (magic %q)", path, b[:4])
	}
	le := binary.LittleEndian
	img := &Image{
		FormatVersion:   le.Uint16(b[4:]),
		Flags:           le.Uint16(b[6:]),
		RegistryVersion: le.Uint32(b[8:]),
		BlobSize:        le.Uint32(b[12:]),
		Size:            int64(len(b)),
	}
	if int64(img.BlobSize) != img.Size {
		return nil, fmt.Errorf("%s: blob_size %d is not the file's length %d", path, img.BlobSize, img.Size)
	}
	sum := sha256.Sum256(b)
	img.SHA256 = hex.EncodeToString(sum[:])
	return img, nil
}
