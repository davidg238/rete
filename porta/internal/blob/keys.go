// Copyright (c) 2026 Ekorau LLC
package blob

// The signing seat (#25 design D3): one keypair, two hex files, no ceremony.
// The private key never leaves porta; the runner gets only the pub file.

import (
	"crypto/ed25519"
	"crypto/rand"
	"encoding/hex"
	"fmt"
	"os"
)

func GenerateKeys(privPath, pubPath string) error {
	for _, p := range []string{privPath, pubPath} {
		if _, err := os.Stat(p); err == nil {
			return fmt.Errorf("%s already exists — refusing to rotate the key implicitly", p)
		}
	}
	pub, priv, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		return err
	}
	if err := os.WriteFile(privPath, []byte(hex.EncodeToString(priv)), 0o600); err != nil {
		return err
	}
	return os.WriteFile(pubPath, []byte(hex.EncodeToString(pub)), 0o644)
}

func SignHash(privPath string, hash []byte) ([]byte, error) {
	raw, err := os.ReadFile(privPath)
	if err != nil {
		return nil, fmt.Errorf("signing key: %w (run `porta key gen`)", err)
	}
	priv, err := hex.DecodeString(string(raw))
	if err != nil || len(priv) != ed25519.PrivateKeySize {
		return nil, fmt.Errorf("signing key %s is not a %d-byte hex ed25519 key", privPath, ed25519.PrivateKeySize)
	}
	return ed25519.Sign(ed25519.PrivateKey(priv), hash), nil
}
