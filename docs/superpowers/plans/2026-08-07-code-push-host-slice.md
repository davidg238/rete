# Code-Push Host Slice (#25) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The #25 host-only vertical slice: signed `.oimg` push over UDP blocks, A/B slots with a deploy watchdog and revert, proven by a 3-node fleet-sim gate.

**Architecture:** porta gains a content-addressed blob store, an ed25519 signing seat at push, and a stateless binary block server on exchange-port+1. The node gains a C `ota` seam (fetch → stage → verify hash) driven by new prims from the kernel bridge's `checkIn` tail. A Python runner owns two slots per node, verifies signatures outside the image, trial-boots, and reverts on no-confirm. Spec: `docs/design/2026-08-07-code-push-host-slice-design.md` (D1–D8).

**Tech Stack:** Go 1.26 (crypto/ed25519, stdlib only), C99 (static SHA-256, udp_backend seam), nsl (kernel bridge), Python 3 (runner + vendored ed25519 verify), bash TAP-lite gate.

## Global Constraints

- porta and the node NEVER import each other's code; they meet only on the wire (CLAUDE.md).
- Copyright header `// Copyright (c) 2026 Ekorau LLC` on every new Go file; `gofmt -l .` prints nothing before every commit.
- Prim ids are **counted, not computed** — the 1-based position in dispatch.c's `PRIM_NAMES`; append-only; mirror in `prim_ids.h`; keep the parity `typedef` check compiling (dispatch.c:915 idiom).
- A new engine `.c` file must be added to **all nine enumerated source lists** (see Task 5 Step 8 for the list; #63). After adding, run `tests/http_roundtrip_host.sh` and `tests/hanoi_ab_host.sh` by hand — they are NOT in the suite.
- Never run two nsl suites concurrently (`/tmp` collisions, #167).
- The nsl suite verdict is `Passed: N / Failed: 0` **plus a green aux-gate line** — read the aux line.
- All porta values are raw deterministic CBOR via `internal/cval` (D4.1). The block port is the ONE binary-framed surface (data plane, CoAP stand-in — design D1 note).
- nsl gotchas: no `#( )` literals; `[ ... ] whileTrue:`, never `repeat`; cross-class calls inside `whenever:do:` blocks must be hoisted to self-send helpers (#133); a wrong-arity class-side send silently answers an uninitialised instance.
- Standing gates that must stay green after every task: `cd nodus/nsl && python3 tools/run_tests_oevm.py` (239 passed), `cd porta && go test ./...`, `cd nodus/tuvm && bash tests/porta_sync_host.sh` (PORTA SYNC OK).

---

### Task 1: porta blob store

**Files:**
- Create: `porta/internal/blob/store.go`
- Test: `porta/internal/blob/store_test.go`

**Interfaces:**
- Produces: `blob.Open(dir string) (*Store, error)` (mkdir-all's dir); `(*Store) Put(r io.Reader) (hash [32]byte, size int64, err error)`; `(*Store) Get(hash []byte) ([]byte, error)` (`os.ErrNotExist` when absent); `(*Store) Path(hash []byte) string`. Files live at `<dir>/<hex-sha256>`; Put stages to `<dir>/.tmp-*` then renames.

- [ ] **Step 1: Write the failing tests**

```go
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
```

- [ ] **Step 2: Run to verify failure**

Run: `cd porta && go test ./internal/blob/`
Expected: FAIL (package does not exist / undefined: Open)

- [ ] **Step 3: Implement**

```go
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
```

- [ ] **Step 4: Run to verify pass**

Run: `cd porta && go test ./internal/blob/ && gofmt -l internal/blob/`
Expected: PASS, no gofmt output

- [ ] **Step 5: Commit**

```bash
git add porta/internal/blob/
git commit -m "feat(#25): content-addressed blob store — the bulk plane's file half (D8)"
```

---

### Task 2: signing keys — `porta key gen` and the sign/load helpers

**Files:**
- Create: `porta/internal/blob/keys.go`
- Create: `porta/internal/portacli/key.go`
- Modify: `porta/internal/portacli/root.go` (register the new command; see how `newServeCmd()` etc. are added in `NewRootCmd`)
- Test: `porta/internal/blob/keys_test.go`

**Interfaces:**
- Consumes: nothing from Task 1 (same package, independent file).
- Produces: `blob.GenerateKeys(privPath, pubPath string) error` (refuses to overwrite);
  `blob.SignHash(privPath string, hash []byte) ([]byte, error)` (64-byte ed25519 sig over the 32 hash bytes);
  key files are lower-case hex: priv = 64-byte ed25519 private key (seed‖pub), pub = 32-byte public key.
  Key paths derive from the db flag: `keyPaths(dbPath)` → `<dir(db)>/porta.key`, `<dir(db)>/porta.pub` (unexported helper in portacli, used again by Task 3).
- CLI: `porta key gen` — writes both files next to the db, prints the pub hex; errors if either exists.

- [ ] **Step 1: Write the failing tests**

```go
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
```

- [ ] **Step 2: Run to verify failure**

Run: `cd porta && go test ./internal/blob/ -run 'Generate|Sign'`
Expected: FAIL (undefined: GenerateKeys)

- [ ] **Step 3: Implement keys.go**

```go
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
```

- [ ] **Step 4: Write key.go (CLI) — follow put.go's shape**

```go
// Copyright (c) 2026 Ekorau LLC
package portacli

import (
	"fmt"
	"os"
	"path/filepath"

	"github.com/spf13/cobra"

	"porta/internal/blob" // match the module path used by existing imports (see serve.go's import block)
)

func keyPaths(db string) (priv, pub string) {
	d := filepath.Dir(db)
	return filepath.Join(d, "porta.key"), filepath.Join(d, "porta.pub")
}

func blobDir(db string) string { return filepath.Join(filepath.Dir(db), "blobs") }

func newKeyCmd() *cobra.Command {
	cmd := &cobra.Command{Use: "key", Short: "Signing keypair for image push"}
	gen := &cobra.Command{
		Use:   "gen",
		Short: "Generate the porta signing keypair beside the db",
		Args:  cobra.NoArgs,
		RunE: func(c *cobra.Command, args []string) error {
			priv, pub := keyPaths(dbPath)
			if err := blob.GenerateKeys(priv, pub); err != nil {
				return err
			}
			raw, _ := os.ReadFile(pub)
			fmt.Fprintf(c.OutOrStdout(), "pub %s (%s)\n", string(raw), pub)
			return nil
		},
	}
	cmd.AddCommand(gen)
	return cmd
}
```

Register `newKeyCmd()` in `NewRootCmd` (root.go) alongside the existing `AddCommand` calls. Check the real module import path with `head -1 porta/go.mod` and match it.

- [ ] **Step 5: Run tests + a smoke of the CLI**

Run: `cd porta && go test ./... && go run ./cmd/porta --db /tmp/kgen.db key gen && rm -f /tmp/porta.key /tmp/porta.pub /tmp/kgen.db`
Expected: tests PASS; `pub <hex>` printed. (Note the key files land in /tmp — the db's dir — clean them up.)

- [ ] **Step 6: Commit**

```bash
cd porta && gofmt -l . # must print nothing
git add porta/internal/blob/keys.go porta/internal/blob/keys_test.go porta/internal/portacli/key.go porta/internal/portacli/root.go
git commit -m "feat(#25): the signing seat — ed25519 keypair gen + sign-over-hash (D3)"
```

---

### Task 3: `porta blob put` and `porta put --raw`

**Files:**
- Create: `porta/internal/portacli/blobcmd.go`
- Modify: `porta/internal/portacli/put.go` (add `--raw` flag)
- Modify: `porta/internal/portacli/root.go` (register `newBlobCmd()`)
- Test: `porta/internal/portacli/blobcmd_test.go`

**Interfaces:**
- Consumes: `blob.Open/Put`, `blob.SignHash`, `keyPaths`, `blobDir` (Tasks 1–2); `cval.Encode`, `cval.Record{Tag, Fields}`, `cval.Map`/`cval.Pair`, `cval.Bytes`, `cval.Symbol` (existing).
- Produces: `porta blob put <file.oimg>` — stores the blob under `blobDir(dbPath)`, signs the hash, prints three lines: `hash <hex>`, `sig <hex>`, and a ready-to-paste `porta put <node> goal/apps/main --raw <hex-of-App-record>` template (with literal `<node>` for the operator to fill).
  `porta put ... --raw <hex>` — writes the given raw deterministic CBOR value verbatim instead of parsing arg 3 as a scalar (honest under D4.1: the store holds raw CBOR anyway). With `--raw`, the value argument IS the hex.
- The App record encoded by `blob put`:
  `Record{Tag:"App", Fields:{image: Record{Tag:"BlobRef", Fields:{hash: Bytes, size: int64}}, sig: Bytes}}` — field NAMES as Symbols, matching RECORDS §4 (`App`, `image`, `BlobRef`, `hash`, `size`, `sig`).

- [ ] **Step 1: Write the failing test**

```go
// Copyright (c) 2026 Ekorau LLC
package portacli

import (
	"crypto/sha256"
	"encoding/hex"
	"os"
	"path/filepath"
	"testing"

	"porta/internal/cval" // match go.mod module path
)

// appRecord is the pure builder blob put uses; test it directly.
func TestAppRecordEncodesTheCatalogueShape(t *testing.T) {
	h := sha256.Sum256([]byte("img"))
	sig := make([]byte, 64)
	raw, err := appRecord(h[:], 3, sig)
	if err != nil {
		t.Fatal(err)
	}
	v, err := cval.Decode(raw)
	if err != nil {
		t.Fatal(err)
	}
	r, ok := v.(cval.Record)
	if !ok || r.Tag != "App" {
		t.Fatalf("want Record App, got %#v", v)
	}
	var img cval.Record
	var gotSig cval.Bytes
	for _, p := range r.Fields {
		switch p.K {
		case cval.Symbol("image"):
			img = p.V.(cval.Record)
		case cval.Symbol("sig"):
			gotSig = p.V.(cval.Bytes)
		}
	}
	if img.Tag != "BlobRef" || len(gotSig) != 64 {
		t.Fatalf("shape wrong: %#v", r)
	}
}

func TestBlobPutStoresSignsAndPrints(t *testing.T) {
	dir := t.TempDir()
	dbPath = filepath.Join(dir, "porta.db") // package-level flag var, same trick loopback_test uses if any; otherwise pass via cobra flags
	priv, pub := keyPaths(dbPath)
	_ = pub
	if err := os.WriteFile(filepath.Join(dir, "app.oimg"), []byte("fake image"), 0o644); err != nil {
		t.Fatal(err)
	}
	// generate keys first
	if err := runRoot(t, "--db", dbPath, "key", "gen"); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(priv); err != nil {
		t.Fatal("key gen wrote nothing")
	}
	out, err := runRootCapture(t, "--db", dbPath, "blob", "put", filepath.Join(dir, "app.oimg"))
	if err != nil {
		t.Fatal(err)
	}
	sum := sha256.Sum256([]byte("fake image"))
	if !containsLine(out, "hash "+hex.EncodeToString(sum[:])) {
		t.Fatalf("hash line missing in:\n%s", out)
	}
	if _, err := os.Stat(filepath.Join(dir, "blobs", hex.EncodeToString(sum[:]))); err != nil {
		t.Fatal("blob not stored under blobs/")
	}
}
```

Write the small `runRoot`/`runRootCapture`/`containsLine` helpers in the test file: build `NewRootCmd()`, `cmd.SetArgs(args)`, capture with `cmd.SetOut(&buf)`, `cmd.Execute()`. Look at `loopback_test.go` first — if it already has an execute-helper idiom, reuse it instead.

- [ ] **Step 2: Run to verify failure**

Run: `cd porta && go test ./internal/portacli/ -run 'AppRecord|BlobPut'`
Expected: FAIL (undefined: appRecord)

- [ ] **Step 3: Implement blobcmd.go**

```go
// Copyright (c) 2026 Ekorau LLC
package portacli

import (
	"encoding/hex"
	"fmt"
	"os"

	"github.com/spf13/cobra"

	"porta/internal/blob"
	"porta/internal/cval"
)

// appRecord builds the goal/apps/<name> App record (RECORDS §4 + design §3):
// the image as a BlobRef, the push-time signature over its hash (D3).
func appRecord(hash []byte, size int64, sig []byte) ([]byte, error) {
	return cval.Encode(cval.Record{
		Tag: "App",
		Fields: cval.Map{
			{K: cval.Symbol("image"), V: cval.Record{
				Tag: "BlobRef",
				Fields: cval.Map{
					{K: cval.Symbol("hash"), V: cval.Bytes(hash)},
					{K: cval.Symbol("size"), V: size},
				},
			}},
			{K: cval.Symbol("sig"), V: cval.Bytes(sig)},
		},
	})
}

func newBlobCmd() *cobra.Command {
	cmd := &cobra.Command{Use: "blob", Short: "Bulk-plane image blobs"}
	put := &cobra.Command{
		Use:   "put <file.oimg>",
		Short: "Store an image blob, sign its hash, print the goal/apps write",
		Args:  cobra.ExactArgs(1),
		RunE: func(c *cobra.Command, args []string) error {
			f, err := os.Open(args[0])
			if err != nil {
				return err
			}
			defer f.Close()
			st, err := blob.Open(blobDir(dbPath))
			if err != nil {
				return err
			}
			hash, size, err := st.Put(f)
			if err != nil {
				return err
			}
			priv, _ := keyPaths(dbPath)
			sig, err := blob.SignHash(priv, hash[:])
			if err != nil {
				return err
			}
			raw, err := appRecord(hash[:], size, sig)
			if err != nil {
				return err
			}
			fmt.Fprintf(c.OutOrStdout(), "hash %s\nsig %s\nporta put <node> goal/apps/main --raw %s\n",
				hex.EncodeToString(hash[:]), hex.EncodeToString(sig), hex.EncodeToString(raw))
			return nil
		},
	}
	cmd.AddCommand(put)
	return cmd
}
```

- [ ] **Step 4: Add `--raw` to put.go**

In `newPutCmd`: add `var raw bool`, flag `cmd.Flags().BoolVar(&raw, "raw", false, "value is hex-encoded raw deterministic CBOR")`. In `RunE`, replace the fixed encode with:

```go
var enc []byte
if raw {
	enc, err = hex.DecodeString(args[2])
	if err != nil {
		return fmt.Errorf("--raw value is not hex: %w", err)
	}
	if err := cval.Validate(enc); err != nil {
		return fmt.Errorf("--raw value is not a closed cval encoding: %w", err)
	}
} else {
	enc, err = cval.Encode(parseScalar(args[2]))
	if err != nil {
		return err
	}
}
```

(`cval.Validate` exists — decode.go:27. It is what keeps `--raw` from smuggling a non-algebra value into the store; decoding is closed by design.)

- [ ] **Step 5: Run tests**

Run: `cd porta && go test ./... && gofmt -l .`
Expected: all PASS, no gofmt output

- [ ] **Step 6: Commit**

```bash
git add porta/internal/portacli/
git commit -m "feat(#25): porta blob put (store+sign+print) and porta put --raw"
```

---

### Task 4: the block server, wired into `porta serve` on exchange-port+1

**Files:**
- Create: `porta/internal/blockserv/blockserv.go`
- Modify: `porta/internal/portacli/serve.go`
- Test: `porta/internal/blockserv/blockserv_test.go`

**Interfaces:**
- Consumes: `blob.Store.Get` (Task 1).
- Produces: `blockserv.New(st *blob.Store) *Server`; `(*Server) Handle(req []byte) []byte` (pure);
  `(*Server) ListenAndServe(ctx context.Context, addr string) error` — copy the UDP loop shape from `wire/server.go:207` verbatim (read → Handle → write back to sender).
- **The binary block frame** (design D1 note — the data plane's CoAP stand-in; the ONE non-CBOR surface):
  - request: `0x01 ‖ hash[32] ‖ blockNo u32 BE` (37 bytes exactly)
  - response: `0x02 ‖ blockNo u32 BE ‖ more u8 (0|1) ‖ data[≤256]`
  - error: `0x7F ‖ ascii text` (unknown hash, malformed request, block past end)
  - block size 256; `more=1` iff `(blockNo+1)*256 < len(blob)`. A zero-length blob answers block 0 with 0 data bytes, `more=0`.

- [ ] **Step 1: Write the failing golden-frame tests**

```go
// Copyright (c) 2026 Ekorau LLC
package blockserv

import (
	"bytes"
	"crypto/sha256"
	"encoding/binary"
	"testing"

	"porta/internal/blob"
)

func srv(t *testing.T, body []byte) (*Server, [32]byte) {
	st, err := blob.Open(t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	h, _, err := st.Put(bytes.NewReader(body))
	if err != nil {
		t.Fatal(err)
	}
	return New(st), h
}

func req(hash [32]byte, blk uint32) []byte {
	b := make([]byte, 37)
	b[0] = 0x01
	copy(b[1:33], hash[:])
	binary.BigEndian.PutUint32(b[33:], blk)
	return b
}

func TestMiddleAndLastBlock(t *testing.T) {
	body := make([]byte, 600) // 3 blocks: 256, 256, 88
	for i := range body {
		body[i] = byte(i)
	}
	s, h := srv(t, body)
	r1 := s.Handle(req(h, 1))
	if r1[0] != 0x02 || binary.BigEndian.Uint32(r1[1:5]) != 1 || r1[5] != 1 {
		t.Fatalf("block 1 header wrong: % x", r1[:6])
	}
	if !bytes.Equal(r1[6:], body[256:512]) {
		t.Fatal("block 1 payload wrong")
	}
	r2 := s.Handle(req(h, 2))
	if r2[5] != 0 || len(r2[6:]) != 88 {
		t.Fatalf("last block: more=%d len=%d", r2[5], len(r2[6:]))
	}
}

func TestUnknownHashAndMalformed(t *testing.T) {
	s, _ := srv(t, []byte("x"))
	if r := s.Handle(req(sha256.Sum256([]byte("other")), 0)); r[0] != 0x7F {
		t.Fatal("unknown hash must answer 0x7F")
	}
	if r := s.Handle([]byte{0x01, 0x02}); r[0] != 0x7F {
		t.Fatal("short frame must answer 0x7F")
	}
	s2, h := srv(t, []byte("x"))
	if r := s2.Handle(req(h, 9)); r[0] != 0x7F {
		t.Fatal("block past end must answer 0x7F")
	}
}
```

- [ ] **Step 2: Run to verify failure**

Run: `cd porta && go test ./internal/blockserv/`
Expected: FAIL (undefined: New)

- [ ] **Step 3: Implement**

```go
// Copyright (c) 2026 Ekorau LLC

// Package blockserv answers the node's stage-time block pulls (#25 design §2):
// a stateless stop-and-wait fetch by content hash — the CoAP blockwise GET
// stand-in, in a fixed binary frame (D1 note). Resume is the client asking
// from any block; the server holds nothing per client.
package blockserv

import (
	"context"
	"encoding/binary"

	"porta/internal/blob"
)

const BlockSize = 256

type Server struct{ st *blob.Store }

func New(st *blob.Store) *Server { return &Server{st: st} }

func fail(msg string) []byte { return append([]byte{0x7F}, msg...) }

func (s *Server) Handle(req []byte) []byte {
	if len(req) != 37 || req[0] != 0x01 {
		return fail("bad request")
	}
	body, err := s.st.Get(req[1:33])
	if err != nil {
		return fail("unknown hash")
	}
	blk := binary.BigEndian.Uint32(req[33:37])
	off := int64(blk) * BlockSize
	if off > int64(len(body)) || (off == int64(len(body)) && blk != 0) {
		return fail("block past end")
	}
	end := off + BlockSize
	more := byte(0)
	if end < int64(len(body)) {
		more = 1
	} else {
		end = int64(len(body))
	}
	resp := make([]byte, 6, 6+end-off)
	resp[0] = 0x02
	binary.BigEndian.PutUint32(resp[1:5], blk)
	resp[5] = more
	return append(resp, body[off:end]...)
}
```

Then `ListenAndServe`: open `wire/server.go:207`, copy its UDP read loop shape (net.ListenPacket / ReadFrom / WriteTo, ctx cancellation) with `Handle` in the middle. Keep it under ~40 lines; no per-peer state.

- [ ] **Step 4: Wire into serve.go**

In `newServeCmd`'s RunE, after the wire server is set up: parse the `--udp` addr with `net.SplitHostPort`, `strconv.Atoi` the port, and start `blockserv` on `host:(port+1)` in its own goroutine (same error-channel pattern the file already uses for udp/http — mirror `awaitServeExit`'s inputs, adding the third channel or joining errors; keep the existing shape, smallest edit that fits). Blob store: `blob.Open(blobDir(dbPath))`. Log one line: `porta: serving blobs on udp <addr>` (the gate greps it).

- [ ] **Step 5: Run everything**

Run: `cd porta && go test ./... && gofmt -l .`
Then smoke: `go run ./cmd/porta serve --db /tmp/bs.db --udp 127.0.0.1:16969 --http-port 0` — expect both "serving the space" and "serving blobs on udp 127.0.0.1:16970" lines; Ctrl-C; `rm -f /tmp/bs.db`.
Expected: PASS, both log lines.

- [ ] **Step 6: Commit**

```bash
git add porta/internal/blockserv/ porta/internal/portacli/serve.go
git commit -m "feat(#25): stateless block server on exchange-port+1 — the bulk plane's wire half (D1)"
```

---

### Task 5: the node's C `ota` seam

**Files:**
- Create: `nodus/tuvm/src/ota.h`, `nodus/tuvm/src/ota.c`, `nodus/tuvm/src/ota_backend.h`, `nodus/tuvm/src/backend/posix/ota_posix.c`
- Modify: the nine source lists (Step 8)
- Test: `nodus/tuvm/tests/ota_seam_test.c` + a line in `nodus/tuvm/tools/run_c_tests.sh` (open it first and follow how existing C tests are compiled/invoked)

**Interfaces:**
- Consumes: `udp_backend.h` (`udp_open/udp_socket_bind/udp_send/udp_receive/udp_close`) — the fetch loop is written against the SEAM, so it is portable to the zephyr backend unchanged.
- Produces (`ota.h`, VM-facing, all called from platform.c in Task 6):

```c
/* All answers: 1 = yes/ok, 0 = no/failure. Failure detail goes to the console
   (tel/log is the nsl caller's job). */
int  ota_running_hash(uint8_t out[32]);            /* 0 if OEVM_IMAGE_HASH unset/bad */
int  ota_is_running(const uint8_t hash[32]);       /* memcmp against env hash */
int  ota_trial(void);                              /* OEVM_OTA_TRIAL=1 */
int  ota_stage(const uint8_t hash[32], const uint8_t *sig, int sig_len,
               const char *host, int port);        /* blocking fetch (design D7) */
void ota_confirm(void);                            /* touch confirm marker */
void ota_activate(const uint8_t hash[32], const uint8_t *sig, int sig_len);
                                                   /* write ready.json, exit(42) */
```

- Produces (`ota_backend.h`, the file/env/exit half — posix now, zephyr flash later):

```c
const char *ota_env(const char *name);             /* getenv now; NULL if unset */
long ota_stage_size(void);                         /* current staged length; -1 = none */
int  ota_stage_read(uint8_t *buf, long off, long len);
int  ota_stage_append(const uint8_t *data, int len);
void ota_stage_truncate(void);
int  ota_write_ready(const char *json);            /* OEVM_OTA_READY path */
void ota_touch_confirm(void);                      /* OEVM_OTA_CONFIRM path */
void ota_exit_stage_ready(void);                   /* exit(42) */
```

- `ota.c` holds: a **static, file-local SHA-256** (public-domain single-implementation, ~150 lines — static so no tenth file joins the nine lists), the block-frame client (Task 4's exact frames: 37-byte request, `0x02`-header response, 5 retries × 1 s timeout per block — `udp_receive` is non-blocking, so the wait is a poll loop with short sleeps against a deadline), and the stage logic:
  1. If `ota_stage_size() > 0` and the staged bytes hash to `hash` → success immediately (idempotent re-entry).
  2. Otherwise resume from block `ota_stage_size()/256` (truncate first to a 256 multiple if it isn't one).
  3. On `more=0`: hash the full staged file; mismatch → `ota_stage_truncate()`, return 0.
- `ready.json` written by `ota_activate`: exactly `{"hash":"<hex>","sig":"<hex>"}`.

- [ ] **Step 1: Write the failing C test.** Test the pure parts through the seam: link `ota.c` + a **stub backend** defined in the test file (in-memory stage buffer, fixed env map, recorded exit), plus a stub `udp_*` set that serves a 600-byte blob (3 blocks) from memory, with a scripted drop of the first request for block 1 (retry must recover). Asserts:
  - resume: pre-load the stub stage with the first 256 bytes → fetch asks for blocks 1,2 only (record requested block numbers), final content equals the blob, returns 1.
  - idempotent: pre-load the complete blob → returns 1 with zero block requests.
  - corrupt: serve a blob whose bytes differ from the requested hash → returns 0 and the stub stage is truncated.
  - `ota_is_running`: stub env `OEVM_IMAGE_HASH` = known hex → matches those bytes, not others.

- [ ] **Step 2: Add the test to run_c_tests.sh and run it**

Run: `cd nodus/tuvm && bash tools/run_c_tests.sh`
Expected: FAIL to compile (ota.c does not exist)

- [ ] **Step 3: Implement `ota_backend.h` + `ota_posix.c`** (plain stdio/stdlib: fopen-append, ftell, getenv, exit). The stage path comes from `ota_env("OEVM_OTA_STAGE")`; every function is a no-op/failure when the env is unset — a node launched without a runner must never crash in the seam.

- [ ] **Step 4: Implement `ota.c`** per the Interfaces block. Console prints use the same mechanism as neighboring engine code — grep `platform_console_write` (dispatch.c:1556 area) and use that, not printf.

- [ ] **Step 5: Run the C test until green**

Run: `cd nodus/tuvm && bash tools/run_c_tests.sh`
Expected: PASS including the new ota test

- [ ] **Step 6: (moved into Step 8 — lists first, then the full gates)**

- [ ] **Step 7: Commit the seam**

```bash
git add nodus/tuvm/src/ota.h nodus/tuvm/src/ota.c nodus/tuvm/src/ota_backend.h nodus/tuvm/src/backend/posix/ota_posix.c nodus/tuvm/tests/ota_seam_test.c nodus/tuvm/tools/run_c_tests.sh
git commit -m "feat(#25): C ota seam — block fetch, staged-slot hash verify, confirm/activate markers"
```

- [ ] **Step 8: The nine lists.** Add `src/ota.c` + `src/backend/$TARGET/ota_$TARGET.c` to every enumerated source list. Count them with commands, never from memory (#63). Known sites (verify each, look for more):
  1. `nodus/tuvm/tools/run_c_tests.sh` — `ENG_COMMON`
  2. `nodus/nsl/tools/run_tests_oevm.py` — `OEVM_BACKEND` plus its four inline `"src/vm.c", …` lists
  3. `nodus/nsl/tools/test_supervisor_grant.py` — `OEVM_BACKEND` and two inline lists
  4. `nodus/nsl/tools/test_debugger_integration.py` — inline list
  5. `nodus/tuvm/tests/test_portability_32.sh` — the `gcc -m32` line
  6. `nodus/tuvm/zephyr/CMakeLists.txt` — `target_sources(app PRIVATE …)` (**zephyr has no ota backend yet**: add `src/ota.c` only if it compiles standalone there; otherwise leave zephyr out entirely this rung and note it in the commit — the seam is posix-first by design, and a zephyr build break is worse than absence)
  7. `nodus/tuvm/tests/http_roundtrip_host.sh`
  8. `nodus/tuvm/tests/hanoi_ab_host.sh`
  9. `nodus/tuvm/tests/porta_sync_host.sh` — the `gcc` line at [4/8]

  Verify by search: `rg -l "timer_posix|src/timer.c|timer_\$TARGET|timer_\$\{" nodus/ --type-add 'build:*.{sh,py,txt}' -t build` and diff that file set against the list above — `timer` is a fully-enrolled precedent, so any file it appears in that ota does not is a missed list.

- [ ] **Step 9: Run the full gates** (sequentially, never two nsl suites at once)

Run: `cd nodus/nsl && python3 tools/run_tests_oevm.py` → `Passed: 239 / Failed: 0` AND a green aux line.
Run: `cd nodus/tuvm && bash tests/porta_sync_host.sh` → `PORTA SYNC OK`
Run: `cd nodus/tuvm && bash tests/http_roundtrip_host.sh && bash tests/hanoi_ab_host.sh` (the two the suite never invokes)
Expected: all green.

- [ ] **Step 10: Commit the lists**

```bash
git add -A
git commit -m "build(#25): enroll ota.c/ota_posix.c in the nine source lists (#63)"
```

---

### Task 6: the six ota prims + `Platform` accessors

**Files:**
- Modify: `nodus/tuvm/src/dispatch.c` (PRIM_NAMES + enum + parity check — follow the `profileStart`/`buildId` append at dispatch.c:824–915 exactly)
- Modify: `nodus/tuvm/src/prim_ids.h` (counted ids, appended after `PRIM_pf_buildId` 144)
- Modify: `nodus/tuvm/src/platform.c` (six cases — model each on `PRIM_pf_buildId` at platform.c:1236 for GC-safety idiom)
- Modify: `nodus/nsl/lib/kernel.nsl` (`Platform` accessors beside `buildId` at kernel.nsl:3200)
- Test: new nsl test under the suite (put it where the getenv-gated tests live — find them with `rg -l "env:" nodus/nsl/tools/run_tests_oevm.py` and follow the per-test `env:` mechanism; a global export leaks into every test)

**Interfaces:**
- Consumes: the six `ota_*` functions from Task 5's `ota.h`.
- Produces — prim ids 145–150, in PRIM_NAMES order, arrived at by COUNTING the entries (144 before this append), not arithmetic:

| id | PRIM_NAMES entry | nsl accessor on Platform | answers |
|---|---|---|---|
| 145 | `"otaRunningHash"` | `public otaRunningHash = ( ^ platform otaRunningHash )` | 32-byte Bytes, or nil if env unset |
| 146 | `"otaIsRunning:"` | `public otaIsRunning: h = ( ^ platform otaIsRunning: h )` | bool (C memcmp — Bytes `=` is never relied on) |
| 147 | `"otaTrial"` | `public otaTrial = ( ^ platform otaTrial )` | bool |
| 148 | `"otaStage:sig:host:port:"` | `public otaStage: h sig: s host: hst port: p = ( ^ platform otaStage: h sig: s host: hst port: p )` | bool — blocking (D7) |
| 149 | `"otaConfirm"` | `public otaConfirm = ( platform otaConfirm. ^ self )` | self |
| 150 | `"otaActivate:sig:"` | `public otaActivate: h sig: s = ( platform otaActivate: h sig: s. ^ self )` | does not return (exit 42) |

- Bytes arguments in C: find how an existing prim reads a Bytes/String argument's bytes+length (grep platform.c for a prim taking Bytes — the cbor leaf prims around `cborEncodeInt:` are the neighbors) and use that idiom; build the Bytes answer for 145 the way `space_make_string_pub` builds Strings (there is a bytes analog nearby — grep `RawBytes` construction in platform.c/space.c).
- Without a runner (env unset): 145 answers nil, 146/147/148 answer false, 149/150 are no-ops that print one console line. **A node launched bare must behave exactly as before this task.**

- [ ] **Step 1: Write the failing nsl test** (env-seam only — no network; staging is gate-tested):

```
(* expect:
nohash
trial-no
hash-yes
match-yes
match-no
trial-yes
*)
class OtaPrimsProbe usingPlatform: platform args: args = ()(
  public main: p args: a = (
    (* Phase 1 runs with NO ota env: accessors must answer their honest nils. *)
    p otaRunningHash == nil ifTrue: [ p println: 'nohash' ].
    p otaTrial ifFalse: [ p println: 'trial-no' ].
    ...
  )
)
```

Split into TWO suite entries with different per-test `env:` maps (one bare, one with `OEVM_IMAGE_HASH` = a known 64-hex constant and `OEVM_OTA_TRIAL=1`); the second phase builds the matching Bytes in nsl and asserts `otaIsRunning:` both ways. Follow the suite's existing test-file convention exactly (inline `(* expect: *)` at top, class with `main:platform args:`, `println` — see any test in `nodus/tests/`). Adjust the sketch above to the real Platform-accessor spelling once written.

- [ ] **Step 2: Run the suite, verify the new test fails** (`doesNotUnderstand` on the accessor)

- [ ] **Step 3: Implement** — dispatch.c names+enum+parity, prim_ids.h, platform.c cases (each case is a thin translation to the `ota_*` call), kernel.nsl accessors.

- [ ] **Step 4: Run the suite to green** — `Passed: 241 / Failed: 0` (239 + the two new), aux line green.

- [ ] **Step 5: Run the other seven list-consumers still green** (they now compile the new prims): `bash tools/run_c_tests.sh`, `bash tests/porta_sync_host.sh`, `bash tests/http_roundtrip_host.sh`, `bash tests/hanoi_ab_host.sh` from `nodus/tuvm`.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "feat(#25): prims 145-150 — the ota seam reaches nsl via Platform"
```

---

### Task 7: `PortaBridge` — the update check, confirm-once, and `obs/apps/main`

**Files:**
- Modify: `nodus/nsl/lib/kernel.nsl` (`class PortaBridge` at kernel.nsl:1557 — `checkIn` at :1952, instance slots, `publishMetrics` as the publish idiom)
- Test: the host gate exercises this end-to-end (Task 9); this task's own verification is that ALL standing gates stay green and one new suite test covers the pure decision helper.

**Interfaces:**
- Consumes: Platform accessors from Task 6; existing bridge state: `peerHost`, `peerPort`, `acked` (the counter PortaBridgeNode reads as `bridge acked`), `space put:value:` idiom from `publishMetrics`, channel read idiom `(space channel: '<path>') peek` (as SpanCapture's `ctl peek`, kernel.nsl:1286).
- Produces, all inside PortaBridge:
  - new instance slots: `otaStaged ::= false. otaConfirmed ::= false.`
  - `checkIn` gains a tail: `... self pushOnce. ^ self otaCheck` (otaCheck runs AFTER the exchange, so a stage on tick T is pushed as `tel/log` on tick T+1 *before* that tick's otaCheck activates — the design §4 one-tick delay falls out of the ordering, no timer needed).
  - `otaCheck` (a self-send, not a block — #133):
    1. If `otaTrial` and `acked > 0` and not `otaConfirmed` → `otaConfirm`, set `otaConfirmed`, `tel/log` `'ota confirmed'`.
    2. If `otaStaged` → `platform otaActivate: stagedHash sig: stagedSig` (does not return; keep the two in slots when staging).
    3. Else read `goal/apps/main`: nil → done. Otherwise extract `hash` (Bytes, from the `image` BlobRef record) and `sig`; malformed record → `tel/log` `'ota bad record'` once per checkIn, done (a porta that writes garbage must not wedge the bridge).
    4. If `(platform otaIsRunning: hash)` → done.
    5. `platform otaStage: hash sig: sig host: peerHost port: peerPort + 1` → true: set `otaStaged`, keep hash/sig in slots, `tel/log` `'ota staged'`; false: `tel/log` `'ota stage failed'` (the next tick retries — D6, free).
  - `publishApps` called from `checkIn` beside `publishMetrics`: only when `otaRunningHash` is non-nil (a bare node publishes nothing — mirrors the `obs/build` absence-is-honest precedent), write `obs/apps/main` = `Record tag: #AppObs with: #image -> (Record tag: #BlobRef with: #hash -> runningHash)` plus `#lifecycle -> #running` (check `Record tag:with:`'s arity family at kernel.nsl:515 and the `#SpanCtl` construction for the exact multi-field spelling; `Record tag:fields:` with a Map is the fallback).
  - Record field extraction helper `otaFieldsOf:` — Record fields access per `class Record` (kernel.nsl:515); guard every step (`isRecord`, map lookup nil-checks): this record arrives off the wire.
- **Do not touch** `pullFrame`/`pushFrame`/spans — the six-span partition of checkIn must not change (`otaCheck` sits outside all six, in the un-bracketed remainder, which is correct: it is not exchange time).

- [ ] **Step 1: Write the failing suite test for the decision helper.** Make the *parse/decide* step a pure method on PortaBridge (e.g. `otaWantedFrom: record` answering nil or a `hash sig` pair-ish List) and test THAT from a suite test with hand-built Records (well-formed, missing image, wrong tag, sig absent) — no network, no env. TDD the helper; the effectful `otaCheck` choreography is gate territory.

- [ ] **Step 2: Suite runs, new test fails.**

- [ ] **Step 3: Implement** the helper + `otaCheck` + `publishApps` + slots + the `checkIn` tail.

- [ ] **Step 4: Suite green (242), aux line green. Then `bash tests/porta_sync_host.sh` — MUST stay `PORTA SYNC OK`** (the bare gate node has no ota env: publishApps publishes nothing, otaCheck reads no goal/apps and does nothing).

- [ ] **Step 5: Commit**

```bash
git add nodus/nsl/lib/kernel.nsl nodus/tests/
git commit -m "feat(#25): PortaBridge stages, confirms and reports the image — checkIn tail, one mechanism"
```

---

### Task 8: the runner

**Files:**
- Create: `nodus/tuvm/tools/oevm_runner.py`
- Create: `nodus/tuvm/tools/ed25519_verify.py` (vendored pure-Python verify — single file, verify-only, no dependencies; target <100 ms/verify, so use the pow-based field inversion form, not the naive reference)
- Test: `nodus/tuvm/tools/test_oevm_runner.py` (+ add its invocation beside the other python tool tests — see how `test_mprof_resolve.py` is run; if nothing runs it, add it to `run_c_tests.sh`'s tail as a python line)

**Interfaces:**
- Consumes: `ready.json` `{"hash":"<hex>","sig":"<hex>"}` (Task 5), exit code 42, confirm marker file; `porta.pub` hex format (Task 2).
- Produces: `oevm_runner.py --node-dir D --vm PATH_TO_OEVM --image-args "..." [--watchdog-secs 30] [--env K=V ...]`
  - `D/manifest.json`: `{"active":"a","prev":null,"trial":false,"slots":{"a":{"hash":"<hex>","sig":"<hex>"},"b":null}}`
  - `D/slots/a.oimg`, `D/slots/b.oimg`; `D/stage/ready.json`; `D/confirm`; `D/pub.key`
  - Provisioning contract (the gate does this): write slot a, its manifest entry, and pub.key by hand before first start.
  - Runner env exported to the VM: `OEVM_IMAGE_HASH` (active slot hex), `OEVM_OTA_STAGE` (the INACTIVE slot path), `OEVM_OTA_READY`, `OEVM_OTA_CONFIRM`, and `OEVM_OTA_TRIAL=1` iff trial — plus any `--env` passthroughs (the gate needs `OEVM_TIMER_WALLCLOCK=1`).
  - `ed25519_verify.verify(pub32: bytes, sig64: bytes, msg: bytes) -> bool`
- State machine exactly as design §5: VERIFY every exec (refuse+revert on bad active sig); on exit 42 verify staged (sha256 file == ready.hash, ed25519(sig, hash)) → swap+trial (and record the new slot's `{hash,sig}` in `manifest.slots`); reject → truncate staged slot, delete ready.json, re-exec old; exit-while-trial-unconfirmed → revert; watchdog kill after `--watchdog-secs` without the confirm marker → revert; confirm marker seen → `trial=false`. Delete the confirm marker on every trial entry. One line to stdout per transition: `runner: <event>` (`exec a`, `staged-ok`, `staged-reject <why>`, `trial b`, `confirmed b`, `revert a`, ...) — the gate greps these.

- [ ] **Step 1: Write the failing tests.** Use a **fake VM** — the test writes a small shell script the runner execs instead of oevm; each scenario's script does what a VM would (touch confirm; or write ready.json+exit 42; or exit 1; or sleep past the watchdog). Test:
  - `test_confirm_path`: trial entered after a valid staged image; fake VM touches confirm; manifest ends `trial=false`, active flipped.
  - `test_reject_bad_sig`: staged file valid hash, sig flipped one byte → `staged-reject`, active unchanged, staged slot truncated.
  - `test_revert_on_crash`: trial VM exits 1 immediately → revert, active back to prev.
  - `test_revert_on_watchdog`: trial VM sleeps; watchdog 1 s → kill, revert.
  - `test_verify_cross_impl`: build porta once (`go build -o <tmp>/porta ./cmd/porta` from `porta/`), `porta key gen` + `porta blob put` a temp file, then `ed25519_verify.verify(pub, sig, hash)` is True and False on a flipped byte. **This is the Go-sign/Python-verify golden cross-check — do not replace it with a Python-signed vector.** Skip (with a printed SKIP) only if `go` is absent, same posture as porta_sync_host.sh.

- [ ] **Step 2: Run: `cd nodus/tuvm && python3 tools/test_oevm_runner.py`** — FAIL (no runner).

- [ ] **Step 3: Implement `ed25519_verify.py`, make `test_verify_cross_impl` pass first** (it pins the file format contract with Task 2 before any state-machine code exists).

- [ ] **Step 4: Implement `oevm_runner.py`; run tests to green.**

- [ ] **Step 5: Commit**

```bash
git add nodus/tuvm/tools/oevm_runner.py nodus/tuvm/tools/ed25519_verify.py nodus/tuvm/tools/test_oevm_runner.py tools/run_c_tests.sh
git commit -m "feat(#25): the runner — A/B slots, sig verify outside the image, trial watchdog, revert (D2/D4)"
```

(Adjust the run_c_tests.sh path in the add if the invocation landed elsewhere.)

---

### Task 9: `OtaBridgeNode` + the 3-node fleet-sim gate

**Files:**
- Create: `nodus/tuvm/zephyr/OtaBridgeNode.nsl.tmpl` (template; the gate seds `@NODE_ID@`, `@LOCAL_PORT@`, `@VERSION@`)
- Create: `nodus/tuvm/tests/host_ota/run.sh`
- Create: `nodus/tuvm/tests/host_ota/never_bridges.nsl` (prints `SILENT NODE READY`, then `[ true ] whileTrue: [ ... ]` idle loop — a valid, signed image that never contacts porta; scenario 4's payload)

**Interfaces:**
- Consumes: everything. `porta serve` + blocks port (T4), `porta key gen`/`blob put`/`put --raw` (T2/T3), the bridge's ota tail (T7), the runner (T8), compile/launch mechanics copied from `porta_sync_host.sh` (its [2/8]–[6/8] stanzas: `dump_layout`, `ns_compile_oevm.py --format=oimg`, the gcc line — which now includes ota.c via Task 5 Step 8, plus `-DOEVM_METHOD_PROFILE` is NOT needed here).
- Produces: `bash tests/host_ota/run.sh` printing a TAP-lite **declared plan** (`plan 4` up front; a run that dies mid-way is a FAIL, not a short pass — the target-harness rule) and `HOST OTA OK` on full green.
- `OtaBridgeNode.nsl.tmpl` is `PortaBridgeNode.nsl` minus the span-capture machinery (no `-DOEVM_METHOD_PROFILE`, no profile leg), with `nodeId`/`localPort` from template substitution, `gwPort = ( ^ 26969 )` (NOT 16969 — this gate must be runnable while porta_sync artifacts linger), plus one line `version = ( ^ @VERSION@ )` so v1/v2 compile to different bytes, and startup print `OTA NODE READY`.

- [ ] **Step 1: Write the gate skeleton with the plan declared and all four scenarios failing-by-absence.** Shape (follow porta_sync_host.sh's preflight discipline — every preflight FAILS rather than skips, except the `go`-absent SKIP):

```bash
#!/usr/bin/env bash
# #25 host fleet-sim gate (G10 seed): 3 runner-wrapped VMs + porta, one signed
# push, kill-mid-transfer, bad-sig refusal, no-check-in revert. TAP-lite with a
# DECLARED plan: a probe that dies half-way is a FAIL, never a short pass.
set -euo pipefail
cd "$(dirname "$0")/../.."          # nodus/tuvm
WORK=/tmp/host_ota_gate; DB=$WORK/porta.db
echo "plan 4"
# [preflight] build porta, layout, VM (the porta_sync gcc line + src/ota.c already enrolled)
# [provision] porta key gen; for n in 1 2 3: sed the template (node-$n, port 2697$n, version 1),
#   compile to $WORK/n$n-v1.oimg, blob put it (capture hash+sig), seed $WORK/node$n/
#   {slots/a.oimg, manifest.json, pub.key (copy of porta.pub hex)}
# [start] porta serve --udp 127.0.0.1:26969 (blocks on 26970); 3 runners with
#   --env OEVM_TIMER_WALLCLOCK=1, --image-args unused (constants are baked)
# scenario 1: for each n: compile v2 (version 2) per node, blob put, porta put node-$n
#   goal/apps/main --raw <hex from blob put>; poll until porta get shows obs/apps/main
#   carrying the v2 hash for all 3   -> "ok 1 - clean deploy converged"
# scenario 2: push v3 to node-1; SIGKILL node-1's VM pid when the staged slot file is
#   >0 and < full size (poll the file); runner re-execs; assert the fetch RESUMED
#   (staged file never shrinks to 0 between kill and converge) and obs/apps/main
#   reaches v3                        -> "ok 2 - kill mid-transfer resumed"
# scenario 3: push v4 to node-2 with a CORRUPTED record: take blob put's printed raw
#   App hex, flip one byte INSIDE the sig field bytes, porta put --raw it; assert
#   runner log prints staged-reject AND obs/apps/main still v2 AND obs/health still
#   advancing                        -> "ok 3 - bad signature refused, node alive"
# scenario 4: compile never_bridges.nsl, blob put (VALID sig), push to node-3; assert
#   runner log: trial -> revert; obs/apps/main back on v2 hash within 2*watchdog
#                                     -> "ok 4 - no-check-in reverted"
echo "HOST OTA OK"
```

Every `poll until` is a bounded `for _ in $(seq 1 100); do ...; sleep 0.2; done` + hard assert after, porta_sync style. Kill everything in a `trap ... EXIT`. Assert hashes by grepping `porta get` output — first check how `format.go` renders Bytes (likely hex); if it truncates, assert via `porta log` or add nothing and grep the runner's `OEVM_IMAGE_HASH` in its `exec` line instead (runner prints it — cheapest anchor, no porta change).

- [ ] **Step 2: Run it; watch it fail at scenario 1.** Fix what surfaces — this is the integration step; expect the first real protocol/env mismatches here (that is the gate's job).

- [ ] **Step 3: Iterate to `HOST OTA OK` with `plan 4` and four `ok` lines.**

- [ ] **Step 4: Run ALL standing gates sequentially one final time** (nsl suite; `go test ./...`; porta_sync_host.sh; run_c_tests.sh; the two orphan .sh gates).

- [ ] **Step 5: Commit**

```bash
git add nodus/tuvm/zephyr/OtaBridgeNode.nsl.tmpl nodus/tuvm/tests/host_ota/
git commit -m "feat(#25): the G10 fleet-sim gate — 3 nodes, signed push, resume, refusal, revert. HOST OTA OK"
```

---

### Task 10: documents of record + close-out

**Files:**
- Modify: `protocol/PROTOCOL-NSL-RECORDS.md` (§4 table: `sig: bytes` joins the `App` record with one sentence on the seat (D3/D4); a scope note that this slice reads/writes exactly `apps/main` — D5)
- Modify: `protocol/PROTOCOL-NSL.md` (§5.3: one paragraph — the day-one bulk plane is a fixed binary block frame on exchange-port+1, the CoAP blockwise stand-in; TFTP→CoAP direction unchanged, still gated on §7 q7)
- Modify: `porta/docs/ARCHITECTURE.md` (blob store + blockserv, one short section each, in the file's existing voice)
- Modify: `docs/next-steps-overview.md` (Part 2: #25 status)
- Modify: `docs/design/2026-08-07-code-push-host-slice-design.md` (only if implementation diverged further — record what actually happened, dated)

**Interfaces:** none — prose.

- [ ] **Step 1: Make the four document edits.** Every durable finding from Tasks 1–9 must land in a mainline doc, not only in the plan (promotion rule): at minimum the block-frame spec (RECORDS or PROTOCOL §5.3 — pick PROTOCOL, it is transport), the App/`sig` shape, and the runner's manifest/marker contract (a short §
 in `porta/docs/ARCHITECTURE.md`'s node-lifecycle area or a pointer to the design doc).
- [ ] **Step 2: Re-run `tools/docmap.sh`** if DOCMAP tracks the new files (check `review/DOCMAP.md`'s generation note first; `review/` itself is never edited by hand).
- [ ] **Step 3: Close #25 on GitHub** with a comment naming: the gate command, the four scenario results, which mainline docs absorbed what, and the follow-ons deliberately NOT done (async fetch — earns in on a measured target stall, D7; rejected-hash memo — earns in on measured noise; multi-app — D5; zephyr ota backend — next rung with the real flash map).
- [ ] **Step 4: Commit**

```bash
git add protocol/ porta/docs/ docs/
git commit -m "docs(#25): promote the block frame, App.sig and runner contract to the documents of record"
```

---

## Self-review notes (already applied)

- Spec coverage: D1→T4/T5, D2→T7/T8, D3→T2/T3/T8, D4→T8 (runner-only key), D5→T7 (`apps/main` only), D6→T7 (checkIn tail), D7→T5 (blocking + retry consts), D8→T1; design §6 failure table→T5 (retry/truncate/idempotent), T7 (bad record, stage-fail log), T8 (reject/revert/marker hygiene); design §7 testing→T1–T4 (Go), T5 (C), T8 (runner+cross-impl), T9 (the four scenarios).
- Deviation from spec, deliberate: block frames are fixed binary, not cval CBOR — the C seam cannot reach the nsl Cbor class and a C CBOR encoder for one fixed shape is more mechanism than a 37-byte frame; CoAP (the ratified successor) is itself binary-framed. Recorded as a D1 note in the design doc (amended alongside this plan).
- Type consistency: `ready.json` keys (`hash`, `sig`, hex) uniform across T5/T8/T9; exit 42 uniform; env names uniform (`OEVM_IMAGE_HASH`, `OEVM_OTA_STAGE`, `OEVM_OTA_READY`, `OEVM_OTA_CONFIRM`, `OEVM_OTA_TRIAL`); ports: gate uses 26969/26970 to avoid porta_sync residue; prim table ids 145–150 counted.
- Known soft spots the executor must resolve against the code, flagged inline: go.mod module path (T2/T3), cval Bytes rendering in `porta get` (T9), the Bytes-argument prim idiom (T6), `Record tag:with:` multi-field arity (T7), whether zephyr/CMakeLists gets ota.c now (T5 Step 8 item 6).
