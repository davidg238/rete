// Copyright (c) 2026 Ekorau LLC
package blob

import (
	"bytes"
	"crypto/sha256"
	"errors"
	"os"
	"testing"
)

func TestPutIsContentAddressedAndIdempotent(t *testing.T) {
	st, err := Open(t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	body := []byte("the image bytes")
	h1, n, err := st.Put(bytes.NewReader(body))
	if err != nil || n != int64(len(body)) {
		t.Fatalf("Put: %v n=%d", err, n)
	}
	if want := sha256.Sum256(body); h1 != want {
		t.Fatalf("hash mismatch")
	}
	h2, _, err := st.Put(bytes.NewReader(body)) // second Put: same hash, no error
	if err != nil || h2 != h1 {
		t.Fatalf("not idempotent: %v", err)
	}
	got, err := st.Get(h1[:])
	if err != nil || !bytes.Equal(got, body) {
		t.Fatalf("Get: %v", err)
	}
}

func TestGetUnknownHashIsErrNotExist(t *testing.T) {
	st, _ := Open(t.TempDir())
	_, err := st.Get(make([]byte, 32))
	if !errors.Is(err, os.ErrNotExist) {
		t.Fatalf("want ErrNotExist, got %v", err)
	}
}

func TestNoTempDroppingsAfterPut(t *testing.T) {
	dir := t.TempDir()
	st, _ := Open(dir)
	st.Put(bytes.NewReader([]byte("x")))
	ents, _ := os.ReadDir(dir)
	if len(ents) != 1 {
		t.Fatalf("want exactly the blob, got %d entries", len(ents))
	}
}
