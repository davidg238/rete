// Copyright (c) 2026 Ekorau LLC
package blob

import (
	"crypto/ed25519"
	"crypto/sha256"
	"encoding/hex"
	"os"
	"path/filepath"
	"testing"
)

func TestGenerateSignVerifyRoundTrip(t *testing.T) {
	dir := t.TempDir()
	priv, pub := filepath.Join(dir, "porta.key"), filepath.Join(dir, "porta.pub")
	if err := GenerateKeys(priv, pub); err != nil {
		t.Fatal(err)
	}
	h := sha256.Sum256([]byte("image"))
	sig, err := SignHash(priv, h[:])
	if err != nil || len(sig) != ed25519.SignatureSize {
		t.Fatalf("SignHash: %v len=%d", err, len(sig))
	}
	pubHex, _ := os.ReadFile(pub)
	pubKey, err := hex.DecodeString(string(pubHex))
	if err != nil || !ed25519.Verify(pubKey, h[:], sig) {
		t.Fatalf("signature does not verify: %v", err)
	}
}

func TestGenerateRefusesToOverwrite(t *testing.T) {
	dir := t.TempDir()
	priv, pub := filepath.Join(dir, "porta.key"), filepath.Join(dir, "porta.pub")
	if err := GenerateKeys(priv, pub); err != nil {
		t.Fatal(err)
	}
	if err := GenerateKeys(priv, pub); err == nil {
		t.Fatal("second gen must refuse — silently rotating the fleet key is the disaster case")
	}
}
