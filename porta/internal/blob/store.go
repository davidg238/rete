// Copyright (c) 2026 Ekorau LLC

// Package blob is the content-addressed file store for the bulk/data plane
// (#25 design D8). Deliberately NOT a sqlite table: the space store's four
// tables stay four, and bulk bytes stay off the space (PROTOCOL-NSL §4.3).
package blob

import (
	"crypto/sha256"
	"encoding/hex"
	"io"
	"os"
	"path/filepath"
)

type Store struct{ dir string }

func Open(dir string) (*Store, error) {
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return nil, err
	}
	return &Store{dir: dir}, nil
}

func (s *Store) Path(hash []byte) string {
	return filepath.Join(s.dir, hex.EncodeToString(hash))
}

// Put streams r to a temp file, hashes as it copies, then renames to the
// hash name. A blob that already exists is a success by construction.
func (s *Store) Put(r io.Reader) (hash [32]byte, size int64, err error) {
	tmp, err := os.CreateTemp(s.dir, ".tmp-*")
	if err != nil {
		return hash, 0, err
	}
	defer os.Remove(tmp.Name())
	h := sha256.New()
	size, err = io.Copy(io.MultiWriter(tmp, h), r)
	if cerr := tmp.Close(); err == nil {
		err = cerr
	}
	if err != nil {
		return hash, 0, err
	}
	copy(hash[:], h.Sum(nil))
	return hash, size, os.Rename(tmp.Name(), s.Path(hash[:]))
}

func (s *Store) Get(hash []byte) ([]byte, error) { return os.ReadFile(s.Path(hash)) }
