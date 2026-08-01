# porta space-sync core — Implementation Plan (plan 1 of 3)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Delete porta v1 and rebuild its core as a CBOR/UDP space mirror — value codec, store, wire — proven by a loopback gate in which a synthetic node pulls goal writes and pushes obs/tel entries over a real UDP socket.

**Architecture:** Three new packages. `internal/cval` is a hand-rolled, byte-exact implementation of the node's shipped CBOR codec. `internal/space` is the sqlite store (`nodes`/`cells`/`events`/`exchanges`). `internal/wire` turns request bytes into response bytes with a pure `Handle` function, wrapped by a thin UDP loop. Everything v1 is deleted first so no decision is made with it in the room.

**Tech Stack:** Go 1.26, sqlite via `github.com/mattn/go-sqlite3`, cobra CLI. **No CBOR dependency** — see Global Constraints.

## Global Constraints

- **Spec of record:** `docs/design/2026-08-01-porta-space-sync-rewrite-design.md`. Wire shapes in its §5, schema in §4, decisions D1–D8.
- **Protocol conformance:** `docs/PROTOCOL-NSL.md` §§1–4 and `docs/PROTOCOL-NSL-RECORDS.md`.
- **The node's codec is the authority, not the RFC.** `~/workspace/nsl-tuvm/lib/kernel.nsl` `class Cbor` is the shipped implementation both ends must agree with. Two shapes are non-obvious and **not** standard CBOR tag-27 usage:
  - A **Record** is `tag 27` → `array(2)` → `[<Symbol>, <Map>]`. The record name is a **Symbol (tag 39)**, *not* a text string.
  - **Map keys are values**, commonly Symbols (tag 39). Keys are sorted **bytewise over their encoded bytes** (RFC 8949 §4.2), which is not the Map's own iteration order.
  - Verified golden, from `~/workspace/nsl-tests/14_cbor/02_golden_hex.nsl`: `Map with: #n -> 42 with: #s -> 'hi'` encodes to `a2d827616e182ad8276173626869`.
- **No CBOR library.** `fxamacker/cbor` cannot express tagged map keys or the closed-decode rule without fighting it. `cval` is hand-rolled (~400 lines) and golden-tested against fixtures produced by the node's own encoder.
- **Closed decoding.** Unknown tags and majors are an error, never pass-through. Depth bound 32. Trailing bytes are an error. Error vocabulary mirrors the node's: `not a value`, `message depth exceeded`, `invalid utf-8`, `integer out of range`.
- **Integer band is ±(2^64−1)** (L7 ratified), which exceeds `int64`. Represent out-of-range values exactly with `cval.Uint` / `cval.NegUint`; never silently clamp.
- **Kind constants match the VM:** `KindStream = 1`, `KindCell = 2`, mirroring `SPACE_KIND_STREAM` / `SPACE_KIND_CELL` in `~/workspace/tuvm/src/space.h`.
- **All timestamps are milliseconds** (D4.2). All paths are **node-relative** (D4.5). All stored values are **raw CBOR bytes** (D4.1) — decode only for display.
- **Caps:** 256 B per value, 512 B per batch (RECORDS §2.2).
- Copyright header on every new Go file: `// Copyright (c) 2026 Ekorau LLC`.
- Run `gofmt -l .` before every commit; it must print nothing.

## File Structure

| file | responsibility |
|---|---|
| `internal/cval/value.go` | the nine algebra types, `Kind`, error vocabulary |
| `internal/cval/encode.go` | deterministic encode |
| `internal/cval/decode.go` | closed decode + `Validate` |
| `internal/cval/format.go` | operator-facing rendering (display only) |
| `internal/space/schema.go` | DDL + `Open`/`Close` |
| `internal/space/store.go` | `Write`, `OutboundSince`, `Ingest`, `RecordExchange`, `Tree`, `Events` |
| `internal/wire/message.go` | pull/batch/push/ack ↔ `cval` |
| `internal/wire/server.go` | pure `Handle([]byte) []byte` (and `HandleFrom` with the peer address) + the UDP loop |
| `internal/portacli/*.go` | `serve`, `get`, `put`, `log` |

Plans 2 and 3 (node-side `seq`/`changedSince:`/bridge job; the C6 hardware gate) are separate documents and depend on this one only through the §5 wire contract.

---

### Task 1: Tag, audit, and demolish v1

**Files:**
- Delete: `internal/{command,config,control,handler,tftp,telemetry,apisrv,web,mcpsrv,store}/`, `devsdk/apiclient/`
- Modify: `internal/httpsrv/health.go`, `internal/httpsrv/server.go`, `internal/portacli/` (reduce to `serve`)
- Modify: `docs/design/2026-08-01-porta-space-sync-rewrite-design.md` (append the audit table)
- Modify: `CLAUDE.md` (it describes the deleted system — step 8)

**Where to run this from:** start the session in `~/workspace/tuvm`, not `~/workspace/porta`, and work here by absolute path. The auto-memory namespace is keyed to the working directory, and the two memories written for this plan (`nsl-cbor-wire-shapes` — required by Task 2 — and `rewrite-may-not-delete-a-capability` — required by step 2 below) live in the tuvm namespace. The porta namespace holds six v1-era memories that this rewrite invalidates and that have not been reviewed against it.

**Interfaces:**
- Consumes: nothing.
- Produces: a tree where `go build ./...` and `go test ./...` pass with only `cmd/porta`, `internal/httpsrv`, `internal/serverstat`, `internal/portacli` remaining. `httpsrv.New(cfg Config, nodes NodeCounter)` where `type NodeCounter interface { CountNodes() (int, error) }`.

- [ ] **Step 1: Tag the last v1 tree**

```bash
cd ~/workspace/porta
git tag -a porta-v1-final -m "the last v1 tree: TFTP + JSON + command queue, before the space-sync rewrite (porta#25)"
git tag -l porta-v1-final
```

- [ ] **Step 2: Run the capability audit before deleting anything**

D8's rule: *a rewrite may delete a mechanism, but it may not silently delete a capability.* Enumerate every v1 operator capability and name where it lands. Produce the list mechanically, do not recall it:

```bash
cd ~/workspace/porta
go run ./cmd/porta --help
for c in scan ping device container log monitor debug profile; do echo "=== $c ==="; go run ./cmd/porta $c --help 2>&1 | head -20; done
grep -rhoE '"/(api|v1)/[a-z0-9/{}._-]*"' internal/apisrv internal/web | sort -u
```

Append the result to the design record as a table `§4.2 Capability audit`, one row per verb/endpoint with fate ∈ {absorbed → `<path>`, deferred → `<plan>`, **regression**}. Any row landing on **regression** stops this task and gets raised before deletion — that is exactly how `obs/build` was caught.

- [ ] **Step 3: Commit the audit**

```bash
git add docs/design/2026-08-01-porta-space-sync-rewrite-design.md
git commit -m "docs(design): capability audit of v1's operator surface before demolition"
```

- [ ] **Step 4: Decouple httpsrv from the v1 store**

Replace the `*store.Store` parameter with a narrow interface so `httpsrv` survives the deletion. In `internal/httpsrv/server.go`:

```go
// NodeCounter is the only thing the health endpoint needs from a store.
// Narrow by design: httpsrv outlived v1's store and should outlive the next one.
type NodeCounter interface {
	CountNodes() (int, error)
}

func New(cfg Config, nodes NodeCounter) (*Server, error) { /* … unchanged body, s.nodes = nodes … */ }
```

In `internal/httpsrv/health.go`, replace the `st.ListNodes()` call:

```go
func healthHandler(nodes NodeCounter, statsFn func() *serverstat.Stats) http.HandlerFunc {
	return func(w http.ResponseWriter, _ *http.Request) {
		n := -1
		if nodes != nil {
			if c, err := nodes.CountNodes(); err == nil {
				n = c
			}
		}
		out := map[string]any{"status": "ok", "nodes": n}
		if statsFn != nil {
			if s := statsFn(); s != nil {
				snap := s.Snapshot()
				out["version"] = snap.Version
				out["uptime_s"] = snap.UptimeSeconds
				out["reports_rejected"] = snap.ReportsRejected
			}
		}
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusOK)
		_ = json.NewEncoder(w).Encode(out)
	}
}
```

- [ ] **Step 5: Delete the v1 packages**

```bash
cd ~/workspace/porta
git rm -r internal/command internal/config internal/control internal/handler \
          internal/tftp internal/telemetry internal/apisrv internal/web \
          internal/mcpsrv internal/store devsdk/apiclient
```

- [ ] **Step 6: Reduce the CLI to `serve`**

Delete every command file in `internal/portacli/` except the root and `serve`, and strip `serve.go` to a listener that opens no store yet:

```go
// serveCmd runs the gateway. During the rewrite it serves only /health;
// the UDP space listener arrives in Task 4 and the operator surface in plan 4.
func serveCmd() *cobra.Command {
	var httpAddr string
	cmd := &cobra.Command{
		Use:   "serve",
		Short: "Run the porta gateway",
		RunE: func(c *cobra.Command, _ []string) error {
			srv, err := httpsrv.New(httpsrv.Config{Addr: httpAddr}, nil)
			if err != nil {
				return err
			}
			return srv.ListenAndServe(c.Context())
		},
	}
	cmd.Flags().StringVar(&httpAddr, "http", ":6970", "operator HTTP listen address")
	return cmd
}
```

- [ ] **Step 7: Verify the tree builds and tests green**

Run: `cd ~/workspace/porta && gofmt -l . && go build ./... && go test ./...`
Expected: `gofmt` prints nothing; build succeeds; tests pass (only `httpsrv` and `serverstat` have any left).

- [ ] **Step 8: Rewrite `CLAUDE.md`, which now describes a deleted system**

`~/workspace/porta/CLAUDE.md` opens by defining porta as the thing that "defines the command vocabulary and report schema, queues commands, delivers container images over TFTP, and ingests telemetry" — every clause of which this task just deleted. It auto-loads as project context, so leaving it stale actively misleads the next session.

Rewrite the opening description and the `## Layout` section to describe the space model: porta mirrors each node's space (`cells`/`events`/`exchanges`), speaks the pull/push exchange over UDP+CBOR, and holds the operator vocabulary as CLI sugar over path writes. Point at `docs/design/2026-08-01-porta-space-sync-rewrite-design.md` as the current record and mark `docs/PROTOCOL.md` as v1, deleted, historical only.

Two lines that must survive verbatim, because they are still true and still load-bearing:

> porta and the nodes are coupled **only** over the wire; they never import each other's code.

> This is a *different project context* — this file + `docs/` are your context.

Also note in it that the porta auto-memory namespace (`~/.claude/projects/-home-david-workspace-porta/memory/`) is **v1-era and unreviewed** against this rewrite.

- [ ] **Step 9: Commit the demolition**

```bash
git add -A
git commit -m "refactor!: delete porta v1 — command queue, TFTP, reports, operator surface (porta#25)

D7: demolition precedes construction, so no decision in the rewrite is
made with v1 still in the room. The last v1 tree is tagged porta-v1-final.

Deleted: command, config, control, handler, tftp, telemetry, apisrv, web,
mcpsrv, store, devsdk/apiclient. httpsrv keeps its health endpoint behind
a narrow NodeCounter interface. The capability audit in the design record
names where every v1 verb lands."
```

---

### Task 2: `internal/cval` — the node's codec, in Go

**Files:**
- Create: `internal/cval/value.go`, `internal/cval/encode.go`, `internal/cval/decode.go`, `internal/cval/format.go`
- Test: `internal/cval/cval_test.go`

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `type Kind int`; `KindUnset Kind = 0`, `KindStream Kind = 1`, `KindCell Kind = 2`
  - `type Bytes []byte`, `type Symbol string`, `type List []any`, `type Pair struct{ K, V any }`, `type Map []Pair`, `type Record struct{ Tag Symbol; Fields Map }`, `type Uint uint64`, `type NegUint uint64`
  - `func Encode(v any) ([]byte, error)`
  - `func Decode(b []byte) (any, error)`
  - `func Validate(b []byte) error`
  - `func Format(v any) string`
  - `var ErrNotAValue, ErrDepth, ErrTrailing, ErrUTF8, ErrIntRange error`

- [ ] **Step 1: Write the failing golden test**

The first assertion is the node's own published golden, so a passing Go encoder is byte-compatible with the shipped VM by construction.

```go
// internal/cval/cval_test.go
package cval

import (
	"bytes"
	"encoding/hex"
	"testing"
)

// The node's own golden, from nsl-tests/14_cbor/02_golden_hex.nsl:
//   Map with: #n -> 42 with: #s -> 'hi'
// Symbols are tag 39; map keys sort bytewise over ENCODED key bytes.
func TestEncodeNodeGolden(t *testing.T) {
	m := Map{{Symbol("s"), "hi"}, {Symbol("n"), int64(42)}} // deliberately unsorted on input
	got, err := Encode(m)
	if err != nil {
		t.Fatalf("Encode: %v", err)
	}
	want, _ := hex.DecodeString("a2d827616e182ad8276173626869")
	if !bytes.Equal(got, want) {
		t.Fatalf("golden mismatch\n got %x\nwant %x", got, want)
	}
}

// A Record is tag 27 -> array(2) -> [Symbol, Map]. The NAME IS A SYMBOL,
// not a text string — this is where a standard tag-27 implementation diverges.
func TestEncodeRecordNameIsSymbol(t *testing.T) {
	r := Record{Tag: "ack", Fields: Map{{Symbol("hw"), int64(5)}}}
	got, err := Encode(r)
	if err != nil {
		t.Fatalf("Encode: %v", err)
	}
	want, _ := hex.DecodeString("d81b82d8276361636ba1d827626877" + "05")
	if !bytes.Equal(got, want) {
		t.Fatalf("record mismatch\n got %x\nwant %x", got, want)
	}
}

func TestRoundTrip(t *testing.T) {
	orig := Record{Tag: "reading", Fields: Map{
		{Symbol("id"), int64(4294967296)},
		{Symbol("name"), "sensor"},
		{Symbol("ok"), true},
		{Symbol("xs"), List{int64(1), int64(2), int64(3)}},
		{Symbol("raw"), Bytes{0xde, 0xad}},
		{Symbol("none"), nil},
	}}
	enc, err := Encode(orig)
	if err != nil {
		t.Fatalf("Encode: %v", err)
	}
	back, err := Decode(enc)
	if err != nil {
		t.Fatalf("Decode: %v", err)
	}
	re, err := Encode(back)
	if err != nil {
		t.Fatalf("re-Encode: %v", err)
	}
	if !bytes.Equal(enc, re) {
		t.Fatalf("not byte-stable\n first %x\nsecond %x", enc, re)
	}
}

func TestDecodeIsClosed(t *testing.T) {
	cases := map[string]struct {
		hex  string
		want error
	}{
		"unknown tag 42":  {"d82a01", ErrNotAValue},
		"indefinite text": {"7f6161ff", ErrNotAValue},
		"trailing bytes":  {"0101", ErrTrailing},
		"bad utf-8":       {"62c328", ErrUTF8},
	}
	for name, tc := range cases {
		t.Run(name, func(t *testing.T) {
			b, _ := hex.DecodeString(tc.hex)
			if _, err := Decode(b); err != tc.want {
				t.Fatalf("Decode(%s) = %v, want %v", tc.hex, err, tc.want)
			}
		})
	}
}

func TestDepthBound(t *testing.T) {
	v := any(int64(1))
	for i := 0; i < 40; i++ {
		v = List{v}
	}
	if _, err := Encode(v); err != ErrDepth {
		t.Fatalf("Encode deep = %v, want ErrDepth", err)
	}
}

// The L7 band is ±(2^64−1), which does not fit int64. It must survive exactly.
func TestIntegerBandEdges(t *testing.T) {
	for _, v := range []any{Uint(1<<64 - 1), NegUint(1<<64 - 1), int64(-1), int64(0)} {
		enc, err := Encode(v)
		if err != nil {
			t.Fatalf("Encode(%v): %v", v, err)
		}
		back, err := Decode(enc)
		if err != nil {
			t.Fatalf("Decode(%x): %v", enc, err)
		}
		if re, _ := Encode(back); !bytes.Equal(enc, re) {
			t.Fatalf("band value %v not stable: %x vs %x", v, enc, re)
		}
	}
}
```

- [ ] **Step 2: Run it to confirm it fails**

Run: `cd ~/workspace/porta && go test ./internal/cval/ -v`
Expected: FAIL — the package does not exist.

- [ ] **Step 3: Implement the types**

```go
// internal/cval/value.go
// Copyright (c) 2026 Ekorau LLC

// Package cval implements the CBOR encoding of the nsl message algebra,
// byte-compatible with the node's shipped codec (nsl-tuvm/lib/kernel.nsl,
// class Cbor). It is hand-rolled rather than layered on a general CBOR
// library for two reasons: the algebra's map keys are themselves tagged
// values (Symbols, tag 39), and decoding must be CLOSED — an unknown tag is
// an error, never a pass-through, so a mis-versioned peer cannot smuggle a
// value the node would faithfully relay.
package cval

import "errors"

// Kind mirrors SPACE_KIND_* in tuvm/src/space.h. The numbers are shared with
// the VM and travel on the wire; do not renumber them.
type Kind int

const (
	KindUnset  Kind = 0
	KindStream Kind = 1
	KindCell   Kind = 2
)

// The nine ranks of the algebra, as Go types:
//   nil, bool, int64/Uint/NegUint, float64, string, Bytes, Symbol, List, Map, Record
type (
	Bytes  []byte
	Symbol string
	List   []any
	// Map is an ordered slice, not a Go map: keys are arbitrary algebra
	// values (commonly Symbols) and Go maps cannot key on them.
	Map []Pair
	// Uint is a positive integer too large for int64 (up to 2^64−1).
	Uint uint64
	// NegUint is the negative integer −1−n, for values below math.MinInt64.
	NegUint uint64
)

type Pair struct{ K, V any }

type Record struct {
	Tag    Symbol
	Fields Map
}

// The error vocabulary mirrors the node's four decode failures exactly.
var (
	ErrNotAValue = errors.New("not a value")
	ErrDepth     = errors.New("message depth exceeded")
	ErrUTF8      = errors.New("invalid utf-8")
	ErrIntRange  = errors.New("integer out of range")
	ErrTrailing  = errors.New("trailing bytes")
)

const maxDepth = 32
```

- [ ] **Step 4: Implement encode**

```go
// internal/cval/encode.go
// Copyright (c) 2026 Ekorau LLC

package cval

import (
	"bytes"
	"math"
	"sort"
)

// Encode produces deterministic CBOR (RFC 8949 §4.2): shortest-form heads,
// definite lengths only, map keys sorted bytewise over their ENCODED bytes.
// Byte-identical to the node's Cbor>>encode:.
func Encode(v any) ([]byte, error) {
	var buf bytes.Buffer
	if err := enc(&buf, v, 1); err != nil {
		return nil, err
	}
	return buf.Bytes(), nil
}

func head(w *bytes.Buffer, major byte, arg uint64) {
	mt := major << 5
	switch {
	case arg < 24:
		w.WriteByte(mt | byte(arg))
	case arg < 1<<8:
		w.WriteByte(mt | 24)
		w.WriteByte(byte(arg))
	case arg < 1<<16:
		w.WriteByte(mt | 25)
		w.Write([]byte{byte(arg >> 8), byte(arg)})
	case arg < 1<<32:
		w.WriteByte(mt | 26)
		w.Write([]byte{byte(arg >> 24), byte(arg >> 16), byte(arg >> 8), byte(arg)})
	default:
		w.WriteByte(mt | 27)
		for i := 56; i >= 0; i -= 8 {
			w.WriteByte(byte(arg >> uint(i)))
		}
	}
}

func enc(w *bytes.Buffer, v any, depth int) error {
	if depth > maxDepth {
		return ErrDepth
	}
	switch x := v.(type) {
	case nil:
		w.WriteByte(0xf6)
	case bool:
		if x {
			w.WriteByte(0xf5)
		} else {
			w.WriteByte(0xf4)
		}
	case int64:
		if x >= 0 {
			head(w, 0, uint64(x))
		} else {
			head(w, 1, uint64(-1-x))
		}
	case Uint:
		head(w, 0, uint64(x))
	case NegUint:
		head(w, 1, uint64(x))
	case float64:
		// The node emits shortest-form floats via a C leaf; f64 is always
		// legal and always round-trips. Narrowing is a later optimisation
		// and must never change a value.
		w.WriteByte(0xfb)
		bits := math.Float64bits(x)
		for i := 56; i >= 0; i -= 8 {
			w.WriteByte(byte(bits >> uint(i)))
		}
	case string:
		head(w, 3, uint64(len(x)))
		w.WriteString(x)
	case Bytes:
		head(w, 2, uint64(len(x)))
		w.Write(x)
	case Symbol:
		head(w, 6, 39)
		head(w, 3, uint64(len(x)))
		w.WriteString(string(x))
	case List:
		head(w, 4, uint64(len(x)))
		for _, e := range x {
			if err := enc(w, e, depth+1); err != nil {
				return err
			}
		}
	case Map:
		return encMap(w, x, depth)
	case Record:
		head(w, 6, 27)
		head(w, 4, 2)
		if err := enc(w, x.Tag, depth+1); err != nil {
			return err
		}
		return enc(w, x.Fields, depth+1)
	default:
		return ErrNotAValue
	}
	return nil
}

func encMap(w *bytes.Buffer, m Map, depth int) error {
	type kv struct{ k, v []byte }
	pairs := make([]kv, 0, len(m))
	for _, p := range m {
		var kb, vb bytes.Buffer
		if err := enc(&kb, p.K, depth+1); err != nil {
			return err
		}
		if err := enc(&vb, p.V, depth+1); err != nil {
			return err
		}
		pairs = append(pairs, kv{kb.Bytes(), vb.Bytes()})
	}
	// RFC 8949 §4.2: bytewise over the ENCODED key bytes. This is NOT the
	// Map's own iteration order — they diverge whenever keys differ in
	// encoded length.
	sort.SliceStable(pairs, func(i, j int) bool { return bytes.Compare(pairs[i].k, pairs[j].k) < 0 })
	head(w, 5, uint64(len(pairs)))
	for _, p := range pairs {
		w.Write(p.k)
		w.Write(p.v)
	}
	return nil
}
```

- [ ] **Step 5: Implement decode and Validate**

```go
// internal/cval/decode.go
// Copyright (c) 2026 Ekorau LLC

package cval

import (
	"encoding/binary"
	"math"
	"unicode/utf8"
)

// Decode parses exactly one value. It is CLOSED: any major or tag outside the
// algebra is ErrNotAValue, indefinite lengths are ErrNotAValue, and trailing
// bytes after the first complete value are ErrTrailing.
func Decode(b []byte) (any, error) {
	v, n, err := dec(b, 1)
	if err != nil {
		return nil, err
	}
	if n != len(b) {
		return nil, ErrTrailing
	}
	return v, nil
}

// Validate reports whether b is exactly one well-formed value.
func Validate(b []byte) error {
	_, err := Decode(b)
	return err
}

// readHead returns (major, argument, bytes consumed).
func readHead(b []byte) (byte, uint64, int, error) {
	if len(b) == 0 {
		return 0, 0, 0, ErrNotAValue
	}
	major := b[0] >> 5
	ai := b[0] & 0x1f
	switch {
	case ai < 24:
		return major, uint64(ai), 1, nil
	case ai == 24:
		if len(b) < 2 {
			return 0, 0, 0, ErrNotAValue
		}
		return major, uint64(b[1]), 2, nil
	case ai == 25:
		if len(b) < 3 {
			return 0, 0, 0, ErrNotAValue
		}
		return major, uint64(binary.BigEndian.Uint16(b[1:])), 3, nil
	case ai == 26:
		if len(b) < 5 {
			return 0, 0, 0, ErrNotAValue
		}
		return major, uint64(binary.BigEndian.Uint32(b[1:])), 5, nil
	case ai == 27:
		if len(b) < 9 {
			return 0, 0, 0, ErrNotAValue
		}
		return major, binary.BigEndian.Uint64(b[1:]), 9, nil
	default:
		// 28–30 reserved, 31 indefinite — both closed out.
		return 0, 0, 0, ErrNotAValue
	}
}

func dec(b []byte, depth int) (any, int, error) {
	if depth > maxDepth {
		return nil, 0, ErrDepth
	}
	major, arg, n, err := readHead(b)
	if err != nil {
		return nil, 0, err
	}
	switch major {
	case 0:
		if arg <= math.MaxInt64 {
			return int64(arg), n, nil
		}
		return Uint(arg), n, nil
	case 1:
		if arg < 1<<63 {
			return -1 - int64(arg), n, nil
		}
		return NegUint(arg), n, nil
	case 2:
		if uint64(len(b)-n) < arg {
			return nil, 0, ErrNotAValue
		}
		out := make(Bytes, arg)
		copy(out, b[n:uint64(n)+arg])
		return out, n + int(arg), nil
	case 3:
		if uint64(len(b)-n) < arg {
			return nil, 0, ErrNotAValue
		}
		s := b[n : uint64(n)+arg]
		if !utf8.Valid(s) {
			return nil, 0, ErrUTF8
		}
		return string(s), n + int(arg), nil
	case 4:
		out := make(List, 0, arg)
		off := n
		for i := uint64(0); i < arg; i++ {
			e, m, err := dec(b[off:], depth+1)
			if err != nil {
				return nil, 0, err
			}
			out = append(out, e)
			off += m
		}
		return out, off, nil
	case 5:
		out := make(Map, 0, arg)
		off := n
		for i := uint64(0); i < arg; i++ {
			k, m, err := dec(b[off:], depth+1)
			if err != nil {
				return nil, 0, err
			}
			off += m
			v, m2, err := dec(b[off:], depth+1)
			if err != nil {
				return nil, 0, err
			}
			off += m2
			out = append(out, Pair{k, v})
		}
		return out, off, nil
	case 6:
		return decTag(b, arg, n, depth)
	case 7:
		switch b[0] {
		case 0xf4:
			return false, 1, nil
		case 0xf5:
			return true, 1, nil
		case 0xf6:
			return nil, 1, nil
		case 0xfb:
			if len(b) < 9 {
				return nil, 0, ErrNotAValue
			}
			return math.Float64frombits(binary.BigEndian.Uint64(b[1:])), 9, nil
		}
		return nil, 0, ErrNotAValue
	}
	return nil, 0, ErrNotAValue
}

// decTag handles the only two tags in the algebra: 39 (Symbol) and 27
// (Record). Every other tag is ErrNotAValue — that is what "closed" means.
func decTag(b []byte, tag uint64, n, depth int) (any, int, error) {
	switch tag {
	case 39:
		v, m, err := dec(b[n:], depth+1)
		if err != nil {
			return nil, 0, err
		}
		s, ok := v.(string)
		if !ok {
			return nil, 0, ErrNotAValue
		}
		return Symbol(s), n + m, nil
	case 27:
		v, m, err := dec(b[n:], depth+1)
		if err != nil {
			return nil, 0, err
		}
		l, ok := v.(List)
		if !ok || len(l) != 2 {
			return nil, 0, ErrNotAValue
		}
		// The record NAME is a Symbol, not a text string.
		name, ok := l[0].(Symbol)
		if !ok {
			return nil, 0, ErrNotAValue
		}
		fields, ok := l[1].(Map)
		if !ok {
			return nil, 0, ErrNotAValue
		}
		return Record{Tag: name, Fields: fields}, n + m, nil
	}
	return nil, 0, ErrNotAValue
}
```

- [ ] **Step 6: Implement Format (display only)**

```go
// internal/cval/format.go
// Copyright (c) 2026 Ekorau LLC

package cval

import (
	"fmt"
	"strings"
)

// Format renders a value for operator display. It is NEVER a storage format:
// the CBOR bytes are the source of truth (design D4.1), so this may be lossy
// for display purposes without consequence.
func Format(v any) string {
	switch x := v.(type) {
	case nil:
		return "nil"
	case bool:
		return fmt.Sprintf("%t", x)
	case int64:
		return fmt.Sprintf("%d", x)
	case Uint:
		return fmt.Sprintf("%d", uint64(x))
	case NegUint:
		return "-" + fmt.Sprintf("%d", uint64(x)+1)
	case float64:
		return fmt.Sprintf("%g", x)
	case string:
		return fmt.Sprintf("%q", x)
	case Bytes:
		return fmt.Sprintf("0x%x", []byte(x))
	case Symbol:
		return "#" + string(x)
	case List:
		parts := make([]string, len(x))
		for i, e := range x {
			parts[i] = Format(e)
		}
		return "[" + strings.Join(parts, " ") + "]"
	case Map:
		parts := make([]string, len(x))
		for i, p := range x {
			parts[i] = Format(p.K) + ": " + Format(p.V)
		}
		return "{" + strings.Join(parts, ", ") + "}"
	case Record:
		return "#" + string(x.Tag) + Format(x.Fields)
	}
	return "<?>"
}
```

- [ ] **Step 7: Run the tests**

Run: `cd ~/workspace/porta && go test ./internal/cval/ -v`
Expected: PASS, all seven tests. If `TestEncodeNodeGolden` fails, the encoder disagrees with the shipped VM — fix the encoder, never the golden.

- [ ] **Step 8: Re-confirm the Record fixture against the node's encoder**

The published golden covers only a Map. The Record fixture below was **generated from the node and verified 2026-08-01** — `Path A: PASS / Path B: PASS`, output `d81b82d8276361636ba1d82762687705`, which is the value already asserted in `TestEncodeRecordNameIsSymbol`. Re-run it to confirm nothing has drifted:

```bash
cat > /tmp/claude-1000/-home-david-workspace-tuvm/728b0719-a646-49f0-88c0-ddd9eeff8505/scratchpad/CvalFixture.nsl <<'EOF'
(*
expect:
d81b82d8276361636ba1d82762687705
*)
class CvalFixture packageUsing: manifest = () (
public main: platform args: args = (
  (platform Cbor encode: (platform Record tag: #ack with: #hw -> 5)) printHex println.
)
) : (
)
EOF
cd ~/workspace/nsl-tuvm && python3 tools/run-one.py /tmp/claude-1000/-home-david-workspace-tuvm/728b0719-a646-49f0-88c0-ddd9eeff8505/scratchpad/CvalFixture.nsl
```

If the node prints anything other than the hex above, **the node is right and this plan is wrong** — update the test's `want` and the encoder, never the other way round.

- [ ] **Step 9: Commit**

```bash
gofmt -l . && go test ./internal/cval/
git add internal/cval
git commit -m "feat(cval): the nsl message algebra's CBOR codec, in Go

Hand-rolled rather than layered on a CBOR library: the algebra's map keys
are themselves tagged values (Symbols, tag 39), and decoding must be
CLOSED so a mis-versioned peer cannot smuggle a value through.

Two shapes are not standard tag-27 usage and are golden-tested against
the node's own encoder: a Record's NAME is a Symbol, not a text string;
and map keys sort bytewise over their ENCODED bytes, which is not the
Map's iteration order. The first golden is the node's published one from
nsl-tests/14_cbor/02_golden_hex.nsl."
```

---

### Task 3: `internal/space` — the store

**Files:**
- Create: `internal/space/schema.go`, `internal/space/store.go`
- Test: `internal/space/store_test.go`

**Interfaces:**
- Consumes: `cval.Kind`, `cval.Validate`.
- Produces:
  - `type Entry struct { Seq int64; Path string; Kind cval.Kind; Value []byte }`
  - `type Event struct { Entry; TsMs int64; Direction string; Actor string; Reason string }`
  - `func Open(path string, nowMs func() int64) (*Store, error)`
  - `func (*Store) Close() error`
  - `func (*Store) CountNodes() (int, error)`
  - `func (*Store) Write(nodeID, path string, kind cval.Kind, value []byte, actor, reason string) (int64, error)`
  - `func (*Store) OutboundSince(nodeID string, cursor int64, maxBytes int) ([]Entry, int64, error)`
  - `func (*Store) Ingest(nodeID string, bootID int64, entries []Entry) (int64, error)`
  - `func (*Store) RecordExchange(nodeID string, bootID, cursorIn, cursorOut, pushedLo, pushedHi int64, addr string) error`
  - `func (*Store) NoteCursor(nodeID string, cursor int64) error`
  - `func (*Store) NodeAddr(nodeID string) (string, error)`
  - `func (*Store) Tree(nodeID, prefix string) ([]Entry, error)`
  - `func (*Store) Events(nodeID string, limit int) ([]Event, error)`
  - `const MaxValueBytes = 256`, `const MaxBatchBytes = 512`

- [ ] **Step 1: Write the failing tests**

```go
// internal/space/store_test.go
package space

import (
	"testing"

	"github.com/davidg238/porta/internal/cval"
)

func testStore(t *testing.T) *Store {
	t.Helper()
	var clock int64 = 1_000
	st, err := Open(t.TempDir()+"/t.db", func() int64 { clock += 10; return clock })
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	t.Cleanup(func() { st.Close() })
	return st
}

func enc(t *testing.T, v any) []byte {
	t.Helper()
	b, err := cval.Encode(v)
	if err != nil {
		t.Fatalf("Encode: %v", err)
	}
	return b
}

// The outbound seq is per-node and monotonic (design D5).
func TestWriteAssignsMonotonicSeqPerNode(t *testing.T) {
	st := testStore(t)
	s1, _ := st.Write("n1", "goal/name", cval.KindCell, enc(t, "alpha"), "david", "")
	s2, _ := st.Write("n1", "goal/mode", cval.KindCell, enc(t, "fast"), "david", "")
	s3, _ := st.Write("n2", "goal/name", cval.KindCell, enc(t, "beta"), "david", "")
	if s1 != 1 || s2 != 2 {
		t.Fatalf("n1 seqs = %d,%d want 1,2", s1, s2)
	}
	if s3 != 1 {
		t.Fatalf("n2 seq = %d want 1 (per-node counter)", s3)
	}
}

// A superseded cell write is never delivered: the pull carries the CURRENT
// value only. This is what bounds a cursor-0 replay by tree size rather than
// by history length.
func TestOutboundCollapsesSupersededCellWrites(t *testing.T) {
	st := testStore(t)
	st.Write("n1", "goal/name", cval.KindCell, enc(t, "first"), "david", "")
	st.Write("n1", "goal/name", cval.KindCell, enc(t, "second"), "david", "")
	got, hi, err := st.OutboundSince("n1", 0, MaxBatchBytes)
	if err != nil {
		t.Fatalf("OutboundSince: %v", err)
	}
	if len(got) != 1 {
		t.Fatalf("got %d entries, want 1 (superseded write must not ride)", len(got))
	}
	v, _ := cval.Decode(got[0].Value)
	if v != "second" {
		t.Fatalf("got %v, want \"second\"", v)
	}
	if hi != 2 {
		t.Fatalf("hi = %d, want 2", hi)
	}
}

// Stream entries are delivered once each, in order — never collapsed.
func TestOutboundKeepsEveryStreamEntry(t *testing.T) {
	st := testStore(t)
	st.Write("n1", "goal/do/reboot", cval.KindStream, enc(t, cval.Record{Tag: "Reboot"}), "david", "")
	st.Write("n1", "goal/do/reboot", cval.KindStream, enc(t, cval.Record{Tag: "Reboot"}), "david", "")
	got, _, _ := st.OutboundSince("n1", 0, MaxBatchBytes)
	if len(got) != 2 {
		t.Fatalf("got %d stream entries, want 2", len(got))
	}
}

func TestOutboundRespectsCursor(t *testing.T) {
	st := testStore(t)
	st.Write("n1", "goal/a", cval.KindCell, enc(t, int64(1)), "david", "")
	st.Write("n1", "goal/b", cval.KindCell, enc(t, int64(2)), "david", "")
	got, _, _ := st.OutboundSince("n1", 1, MaxBatchBytes)
	if len(got) != 1 || got[0].Path != "goal/b" {
		t.Fatalf("got %+v, want only goal/b", got)
	}
}

// The batch cap is a radio constraint, not a preference (RECORDS §2.2).
func TestOutboundStopsAtBatchCap(t *testing.T) {
	st := testStore(t)
	big := enc(t, string(make([]byte, 200)))
	for _, p := range []string{"goal/a", "goal/b", "goal/c", "goal/d"} {
		st.Write("n1", p, cval.KindCell, big, "david", "")
	}
	got, hi, _ := st.OutboundSince("n1", 0, MaxBatchBytes)
	if len(got) >= 4 {
		t.Fatalf("got %d entries, want a partial batch under the cap", len(got))
	}
	if hi != got[len(got)-1].Seq {
		t.Fatalf("hi = %d, want the last delivered seq %d", hi, got[len(got)-1].Seq)
	}
}

// A push replayed after loss must not double-apply (dedup on high-water).
func TestIngestDedupesOnHighWater(t *testing.T) {
	st := testStore(t)
	batch := []Entry{
		{Seq: 1, Path: "tel/log", Kind: cval.KindStream, Value: enc(t, "one")},
		{Seq: 2, Path: "tel/log", Kind: cval.KindStream, Value: enc(t, "two")},
	}
	hw, err := st.Ingest("n1", 7, batch)
	if err != nil || hw != 2 {
		t.Fatalf("Ingest = %d, %v; want 2, nil", hw, err)
	}
	if hw, _ = st.Ingest("n1", 7, batch); hw != 2 {
		t.Fatalf("replayed Ingest = %d, want 2", hw)
	}
	evs, _ := st.Events("n1", 100)
	if len(evs) != 2 {
		t.Fatalf("got %d events after replay, want 2", len(evs))
	}
}

// Provenance is the thing v1's issued_by carried and §5.1 forgot (design D3).
func TestEventsCarryProvenance(t *testing.T) {
	st := testStore(t)
	st.Write("n1", "goal/name", cval.KindCell, enc(t, "alpha"), "agent-7", "rollout")
	evs, _ := st.Events("n1", 10)
	if len(evs) != 1 {
		t.Fatalf("got %d events, want 1", len(evs))
	}
	if evs[0].Actor != "agent-7" || evs[0].Reason != "rollout" || evs[0].Direction != "out" {
		t.Fatalf("provenance lost: %+v", evs[0])
	}
}

// Superseded intent survives in the log even though the cell shows only the
// latest value — the property a drained command queue could not offer.
func TestSupersededIntentSurvivesInTheLog(t *testing.T) {
	st := testStore(t)
	st.Write("n1", "goal/mode", cval.KindCell, enc(t, "X"), "david", "")
	st.Write("n1", "goal/mode", cval.KindCell, enc(t, "Y"), "david", "")
	tree, _ := st.Tree("n1", "goal/")
	if len(tree) != 1 {
		t.Fatalf("tree has %d rows, want 1", len(tree))
	}
	evs, _ := st.Events("n1", 10)
	if len(evs) != 2 {
		t.Fatalf("log has %d rows, want 2 (X must still be visible)", len(evs))
	}
}

func TestOversizeValueRejected(t *testing.T) {
	st := testStore(t)
	_, err := st.Write("n1", "goal/x", cval.KindCell, make([]byte, MaxValueBytes+1), "david", "")
	if err == nil {
		t.Fatal("oversize value accepted; the 256 B cap is a radio constraint")
	}
}

func TestExchangeRowRecorded(t *testing.T) {
	st := testStore(t)
	if err := st.RecordExchange("n1", 7, 0, 3, 1, 2, "fdde:ad00::1"); err != nil {
		t.Fatalf("RecordExchange: %v", err)
	}
	if n, _ := st.CountNodes(); n != 1 {
		t.Fatalf("CountNodes = %d, want 1", n)
	}
}

// The peer address is v1's nodes.source_addr, which the capability audit
// (design §4.2) caught with no successor. serverstat.TransportOf() reads it to
// produce the wifi-vs-Thread breakdown, so losing it loses a fleet view, not a
// display field.
func TestExchangeRecordsPeerAddress(t *testing.T) {
	st := testStore(t)
	st.RecordExchange("n1", 7, 0, 3, 1, 2, "fdde:ad00::1")
	got, err := st.NodeAddr("n1")
	if err != nil {
		t.Fatalf("NodeAddr: %v", err)
	}
	if got != "fdde:ad00::1" {
		t.Fatalf("NodeAddr = %q, want fdde:ad00::1", got)
	}
}

// An exchange recorded without a peer address must not erase a known one.
func TestEmptyPeerAddressDoesNotClobber(t *testing.T) {
	st := testStore(t)
	st.RecordExchange("n1", 7, 0, 3, 1, 2, "fdde:ad00::1")
	st.RecordExchange("n1", 7, 3, 4, 0, 0, "")
	if got, _ := st.NodeAddr("n1"); got != "fdde:ad00::1" {
		t.Fatalf("NodeAddr = %q after an addr-less exchange, want the address kept", got)
	}
}
```

- [ ] **Step 2: Run to confirm failure**

Run: `cd ~/workspace/porta && go test ./internal/space/ -v`
Expected: FAIL — package does not exist.

- [ ] **Step 3: Write the schema**

```go
// internal/space/schema.go
// Copyright (c) 2026 Ekorau LLC

// Package space is porta's per-node space mirror: the cell tree, the
// append-only write log, and the check-in log. It replaces v1's nine
// per-feature tables (see the design record §4.1) with four generic ones,
// because the unit of extension is now a path, not a table.
package space

const schema = `
PRAGMA journal_mode=WAL;

CREATE TABLE IF NOT EXISTS nodes (
  id            TEXT PRIMARY KEY,
  name          TEXT,
  first_seen_ms INTEGER,
  last_seen_ms  INTEGER,
  cadence_ms    INTEGER,
  boot_id       INTEGER,
  cursor_acked  INTEGER NOT NULL DEFAULT 0,
  node_seq_hw   INTEGER NOT NULL DEFAULT 0,
  out_seq       INTEGER NOT NULL DEFAULT 0,
  -- Successor to v1's nodes.source_addr, which the capability audit (design
  -- §4.2) caught with no home. It is not merely a display field: serverstat
  -- .TransportOf() reads it to produce the wifi-vs-Thread fleet breakdown.
  last_addr     TEXT
);

-- Latest value per path. Paths are NODE-RELATIVE (design D4.5): the
-- node/<id>/** prefix in PROTOCOL-NSL.md §3 is a view, never storage, so a
-- captured session can be re-pointed at a different node.
CREATE TABLE IF NOT EXISTS cells (
  node_id    TEXT NOT NULL,
  path       TEXT NOT NULL,
  seq        INTEGER NOT NULL,
  kind       INTEGER NOT NULL,
  value_cbor BLOB NOT NULL,
  ts_ms      INTEGER NOT NULL,
  PRIMARY KEY (node_id, path)
);
CREATE INDEX IF NOT EXISTS idx_cells_seq ON cells(node_id, seq);

-- Append-only, both directions, with provenance (design D2/D3). There is no
-- tombstone column: stopped state is written explicitly as a value, and
-- genuine removal is an action on a do/ stream.
CREATE TABLE IF NOT EXISTS events (
  id         INTEGER PRIMARY KEY AUTOINCREMENT,
  node_id    TEXT NOT NULL,
  path       TEXT NOT NULL,
  seq        INTEGER NOT NULL,
  kind       INTEGER NOT NULL,
  value_cbor BLOB NOT NULL,
  ts_ms      INTEGER NOT NULL,
  direction  TEXT NOT NULL CHECK (direction IN ('in','out')),
  actor      TEXT NOT NULL DEFAULT '',
  reason     TEXT NOT NULL DEFAULT ''
);
CREATE INDEX IF NOT EXISTS idx_events_node ON events(node_id, id);
CREATE INDEX IF NOT EXISTS idx_events_out ON events(node_id, direction, seq);

-- One row per check-in: the heartbeat AND the replay index (design D4.3).
-- Without it the batch boundary a value arrived on is unrecoverable.
CREATE TABLE IF NOT EXISTS exchanges (
  id         INTEGER PRIMARY KEY AUTOINCREMENT,
  node_id    TEXT NOT NULL,
  ts_ms      INTEGER NOT NULL,
  boot_id    INTEGER NOT NULL,
  cursor_in  INTEGER NOT NULL,
  cursor_out INTEGER NOT NULL,
  pushed_lo  INTEGER NOT NULL,
  pushed_hi  INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_exchanges_node ON exchanges(node_id, ts_ms);
`
```

- [ ] **Step 4: Implement the store**

```go
// internal/space/store.go
// Copyright (c) 2026 Ekorau LLC

package space

import (
	"database/sql"
	"fmt"
	"strings"

	"github.com/davidg238/porta/internal/cval"
	_ "github.com/mattn/go-sqlite3"
)

// The caps are a radio constraint, not a preference: 6LoWPAN fragments above
// ~102 B and a datagram survives only if every fragment does, so a 1 KB batch
// reaches the weakest bench node ~54% of the time (RECORDS §2.1/§2.2).
const (
	MaxValueBytes = 256
	MaxBatchBytes = 512
)

type Entry struct {
	Seq   int64
	Path  string
	Kind  cval.Kind
	Value []byte
}

type Event struct {
	Entry
	TsMs      int64
	Direction string
	Actor     string
	Reason    string
}

type Store struct {
	db    *sql.DB
	nowMs func() int64
}

func Open(path string, nowMs func() int64) (*Store, error) {
	db, err := sql.Open("sqlite3", path+"?_busy_timeout=5000")
	if err != nil {
		return nil, err
	}
	if _, err := db.Exec(schema); err != nil {
		db.Close()
		return nil, err
	}
	return &Store{db: db, nowMs: nowMs}, nil
}

func (s *Store) Close() error { return s.db.Close() }

func (s *Store) CountNodes() (int, error) {
	var n int
	err := s.db.QueryRow(`SELECT COUNT(*) FROM nodes`).Scan(&n)
	return n, err
}

// ensureNode inserts the node row if absent. Called on every path that can
// mention a node id, so a write to an unseen node is not an error — porta
// may stage goal state before a node has ever checked in.
func ensureNode(tx *sql.Tx, nodeID string, nowMs int64) error {
	_, err := tx.Exec(
		`INSERT INTO nodes (id, first_seen_ms) VALUES (?, ?) ON CONFLICT(id) DO NOTHING`,
		nodeID, nowMs)
	return err
}

// Write records an outbound (porta→node) write, allocating the node's next
// outbound seq. A cell write also updates the cell tree; a stream write only
// appends. Returns the assigned seq.
func (s *Store) Write(nodeID, path string, kind cval.Kind, value []byte, actor, reason string) (int64, error) {
	if len(value) > MaxValueBytes {
		return 0, fmt.Errorf("value is %d bytes, over the %d byte cap", len(value), MaxValueBytes)
	}
	if err := cval.Validate(value); err != nil {
		return 0, fmt.Errorf("value is not a well-formed algebra value: %w", err)
	}
	if kind != cval.KindCell && kind != cval.KindStream {
		return 0, fmt.Errorf("kind %d is neither cell nor stream", kind)
	}
	now := s.nowMs()
	tx, err := s.db.Begin()
	if err != nil {
		return 0, err
	}
	defer tx.Rollback()
	if err := ensureNode(tx, nodeID, now); err != nil {
		return 0, err
	}
	var seq int64
	if err := tx.QueryRow(
		`UPDATE nodes SET out_seq = out_seq + 1 WHERE id = ? RETURNING out_seq`,
		nodeID).Scan(&seq); err != nil {
		return 0, err
	}
	if kind == cval.KindCell {
		if _, err := tx.Exec(`
			INSERT INTO cells (node_id, path, seq, kind, value_cbor, ts_ms)
			VALUES (?, ?, ?, ?, ?, ?)
			ON CONFLICT(node_id, path) DO UPDATE SET
			  seq = excluded.seq, kind = excluded.kind,
			  value_cbor = excluded.value_cbor, ts_ms = excluded.ts_ms`,
			nodeID, path, seq, int(kind), value, now); err != nil {
			return 0, err
		}
	}
	if _, err := tx.Exec(`
		INSERT INTO events (node_id, path, seq, kind, value_cbor, ts_ms, direction, actor, reason)
		VALUES (?, ?, ?, ?, ?, ?, 'out', ?, ?)`,
		nodeID, path, seq, int(kind), value, now, actor, reason); err != nil {
		return 0, err
	}
	return seq, tx.Commit()
}

// OutboundSince returns entries the node has not yet consumed, oldest first,
// stopping before the batch would exceed maxBytes.
//
// Cells are read from the cell tree, so a SUPERSEDED cell write never rides
// the radio — the node needs the current value, not the history that produced
// it. This is also what bounds a cursor-0 replay by tree size rather than by
// history length. Streams come from the log and are delivered once each.
func (s *Store) OutboundSince(nodeID string, cursor int64, maxBytes int) ([]Entry, int64, error) {
	rows, err := s.db.Query(`
		SELECT seq, path, kind, value_cbor FROM cells
		  WHERE node_id = ? AND seq > ?
		UNION ALL
		SELECT seq, path, kind, value_cbor FROM events
		  WHERE node_id = ? AND direction = 'out' AND kind = ? AND seq > ?
		ORDER BY seq`,
		nodeID, cursor, nodeID, int(cval.KindStream), cursor)
	if err != nil {
		return nil, cursor, err
	}
	defer rows.Close()

	var out []Entry
	hi := cursor
	size := 0
	for rows.Next() {
		var e Entry
		var k int
		if err := rows.Scan(&e.Seq, &e.Path, &k, &e.Value); err != nil {
			return nil, cursor, err
		}
		e.Kind = cval.Kind(k)
		// 8 bytes of slack per entry for the seq/kind heads and the array head.
		cost := len(e.Value) + len(e.Path) + 8
		if size+cost > maxBytes && len(out) > 0 {
			break
		}
		out = append(out, e)
		hi = e.Seq
		size += cost
	}
	return out, hi, rows.Err()
}

// Ingest applies a node push. Entries at or below the stored high-water are
// dropped as duplicates — which is why the exchange needs no retransmit
// layer: a lost push is simply resent, and a resent push is idempotent.
func (s *Store) Ingest(nodeID string, bootID int64, entries []Entry) (int64, error) {
	now := s.nowMs()
	tx, err := s.db.Begin()
	if err != nil {
		return 0, err
	}
	defer tx.Rollback()
	if err := ensureNode(tx, nodeID, now); err != nil {
		return 0, err
	}
	var hw int64
	if err := tx.QueryRow(`SELECT node_seq_hw FROM nodes WHERE id = ?`, nodeID).Scan(&hw); err != nil {
		return 0, err
	}
	for _, e := range entries {
		if e.Seq <= hw {
			continue // duplicate; the node is re-sending an unacked batch
		}
		if len(e.Value) > MaxValueBytes {
			return 0, fmt.Errorf("node %s sent a %d byte value at %s, over the %d byte cap",
				nodeID, len(e.Value), e.Path, MaxValueBytes)
		}
		if err := cval.Validate(e.Value); err != nil {
			return 0, fmt.Errorf("node %s sent a malformed value at %s: %w", nodeID, e.Path, err)
		}
		if e.Kind == cval.KindCell {
			if _, err := tx.Exec(`
				INSERT INTO cells (node_id, path, seq, kind, value_cbor, ts_ms)
				VALUES (?, ?, ?, ?, ?, ?)
				ON CONFLICT(node_id, path) DO UPDATE SET
				  seq = excluded.seq, kind = excluded.kind,
				  value_cbor = excluded.value_cbor, ts_ms = excluded.ts_ms`,
				nodeID, e.Path, e.Seq, int(e.Kind), e.Value, now); err != nil {
				return 0, err
			}
		}
		if _, err := tx.Exec(`
			INSERT INTO events (node_id, path, seq, kind, value_cbor, ts_ms, direction, actor, reason)
			VALUES (?, ?, ?, ?, ?, ?, 'in', ?, '')`,
			nodeID, e.Path, e.Seq, int(e.Kind), e.Value, now, nodeID); err != nil {
			return 0, err
		}
		hw = e.Seq
	}
	if _, err := tx.Exec(
		`UPDATE nodes SET node_seq_hw = ?, boot_id = ?, last_seen_ms = ? WHERE id = ?`,
		hw, bootID, now, nodeID); err != nil {
		return 0, err
	}
	return hw, tx.Commit()
}

func (s *Store) NoteCursor(nodeID string, cursor int64) error {
	now := s.nowMs()
	tx, err := s.db.Begin()
	if err != nil {
		return err
	}
	defer tx.Rollback()
	if err := ensureNode(tx, nodeID, now); err != nil {
		return err
	}
	if _, err := tx.Exec(
		`UPDATE nodes SET cursor_acked = ?, last_seen_ms = ? WHERE id = ?`,
		cursor, now, nodeID); err != nil {
		return err
	}
	return tx.Commit()
}

// RecordExchange logs one check-in. addr is the peer address the datagram
// arrived from; an EMPTY addr never clobbers a known one, which is v1's own
// COALESCE discipline (v1 store.go:267) kept deliberately — a caller that does
// not know the peer (the pure Handle path, a replayed session) must not erase
// what the UDP loop learned.
func (s *Store) RecordExchange(nodeID string, bootID, cursorIn, cursorOut, pushedLo, pushedHi int64, addr string) error {
	now := s.nowMs()
	tx, err := s.db.Begin()
	if err != nil {
		return err
	}
	defer tx.Rollback()
	if err := ensureNode(tx, nodeID, now); err != nil {
		return err
	}
	if _, err := tx.Exec(`
		INSERT INTO exchanges (node_id, ts_ms, boot_id, cursor_in, cursor_out, pushed_lo, pushed_hi)
		VALUES (?, ?, ?, ?, ?, ?, ?)`,
		nodeID, now, bootID, cursorIn, cursorOut, pushedLo, pushedHi); err != nil {
		return err
	}
	if _, err := tx.Exec(
		`UPDATE nodes SET last_seen_ms = ?, last_addr = COALESCE(NULLIF(?, ''), last_addr) WHERE id = ?`,
		now, addr, nodeID); err != nil {
		return err
	}
	return tx.Commit()
}

// NodeAddr returns the peer address of the node's last datagram, or "" if it
// has never been seen.
func (s *Store) NodeAddr(nodeID string) (string, error) {
	var addr sql.NullString
	err := s.db.QueryRow(`SELECT last_addr FROM nodes WHERE id = ?`, nodeID).Scan(&addr)
	if err == sql.ErrNoRows {
		return "", nil
	}
	return addr.String, err
}

// Tree returns the current cell values under prefix, path-ordered — the
// operator's view. goal/X vs obs/X is a diff over two of these, which is why
// self-heal disappears as a separate mechanism.
func (s *Store) Tree(nodeID, prefix string) ([]Entry, error) {
	rows, err := s.db.Query(`
		SELECT seq, path, kind, value_cbor FROM cells
		WHERE node_id = ? AND path LIKE ? ESCAPE '\' ORDER BY path`,
		nodeID, escapeLike(prefix)+"%")
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []Entry
	for rows.Next() {
		var e Entry
		var k int
		if err := rows.Scan(&e.Seq, &e.Path, &k, &e.Value); err != nil {
			return nil, err
		}
		e.Kind = cval.Kind(k)
		out = append(out, e)
	}
	return out, rows.Err()
}

// Events returns the most recent log rows, newest first — porta log.
func (s *Store) Events(nodeID string, limit int) ([]Event, error) {
	rows, err := s.db.Query(`
		SELECT seq, path, kind, value_cbor, ts_ms, direction, actor, reason
		FROM events WHERE node_id = ? ORDER BY id DESC LIMIT ?`,
		nodeID, limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []Event
	for rows.Next() {
		var e Event
		var k int
		if err := rows.Scan(&e.Seq, &e.Path, &k, &e.Value, &e.TsMs, &e.Direction, &e.Actor, &e.Reason); err != nil {
			return nil, err
		}
		e.Kind = cval.Kind(k)
		out = append(out, e)
	}
	return out, rows.Err()
}

func escapeLike(s string) string {
	r := strings.NewReplacer(`\`, `\\`, `%`, `\%`, `_`, `\_`)
	return r.Replace(s)
}
```

- [ ] **Step 5: Run the tests**

Run: `cd ~/workspace/porta && go test ./internal/space/ -v`
Expected: PASS, all ten tests.

- [ ] **Step 6: Commit**

```bash
gofmt -l . && go test ./internal/space/
git add internal/space
git commit -m "feat(space): the per-node space mirror — cells, log, exchanges

Four tables replace v1's nine. Five of those nine were the same two
shapes (a queue with a delivered marker, or an append log keyed
device+seq), which is the multiplicative-extension problem showing up in
the schema; once the unit of extension is a path, a feature adds rows.

Three properties the tests pin:
- a superseded cell write never rides the radio, so a cursor-0 replay is
  bounded by tree size rather than history length
- superseded INTENT still survives in the log, which a drained command
  queue could not offer
- every write carries an actor (design D3 — the column §5.1 forgot)"
```

---

### Task 4: `internal/wire` — the pull/push exchange

**Files:**
- Create: `internal/wire/message.go`, `internal/wire/server.go`
- Test: `internal/wire/wire_test.go`

**Interfaces:**
- Consumes: `cval`, `space`.
- Produces:
  - `func NewServer(st *space.Store, logf func(string, ...any)) *Server`
  - `func (*Server) Handle(req []byte) []byte` — pure bytes→bytes; never returns nil
  - `func (*Server) HandleFrom(req []byte, peer string) []byte` — same, plus the peer address to stamp on the exchange (design §4.2). `Handle(req)` is `HandleFrom(req, "")`, and an empty peer never clobbers a known address
  - `func (*Server) ListenAndServe(ctx context.Context, addr string) error`
  - `func EncodePull(id string, cursor, bootID, hw int64) ([]byte, error)`
  - `func EncodePush(id string, bootID int64, entries []space.Entry) ([]byte, error)`
  - `func DecodeBatch(b []byte) (hi int64, entries []space.Entry, err error)`
  - `func DecodeAck(b []byte) (hw int64, err error)`

Wire shapes, as nsl Records (no second codec, RECORDS §1):

```
pull  = #pull  { #id: str, #cursor: int, #bootId: int, #hw: int }
batch = #batch { #hi: int, #entries: [ [seq, path, kind, value], … ] }
push  = #push  { #id: str, #bootId: int, #entries: [ … ] }
ack   = #ack   { #hw: int }                     ; #hint reserved, not emitted
err   = #err   { #msg: str }
```

- [ ] **Step 1: Write the failing tests**

```go
// internal/wire/wire_test.go
package wire

import (
	"testing"

	"github.com/davidg238/porta/internal/cval"
	"github.com/davidg238/porta/internal/space"
)

func testServer(t *testing.T) (*Server, *space.Store) {
	t.Helper()
	var clock int64 = 1_000
	st, err := space.Open(t.TempDir()+"/t.db", func() int64 { clock += 10; return clock })
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	t.Cleanup(func() { st.Close() })
	return NewServer(st, func(string, ...any) {}), st
}

func mustEnc(t *testing.T, v any) []byte {
	t.Helper()
	b, err := cval.Encode(v)
	if err != nil {
		t.Fatal(err)
	}
	return b
}

func TestPullDeliversGoalWrite(t *testing.T) {
	srv, st := testServer(t)
	st.Write("n1", "goal/name", cval.KindCell, mustEnc(t, "alpha"), "david", "")

	req, _ := EncodePull("n1", 0, 7, 0)
	hi, entries, err := DecodeBatch(srv.Handle(req))
	if err != nil {
		t.Fatalf("DecodeBatch: %v", err)
	}
	if hi != 1 || len(entries) != 1 || entries[0].Path != "goal/name" {
		t.Fatalf("hi=%d entries=%+v", hi, entries)
	}
	v, _ := cval.Decode(entries[0].Value)
	if v != "alpha" {
		t.Fatalf("value = %v, want alpha", v)
	}
}

// A pull at the high-water is the idle heartbeat: empty, and not an error.
func TestPullAtHighWaterIsEmpty(t *testing.T) {
	srv, st := testServer(t)
	st.Write("n1", "goal/name", cval.KindCell, mustEnc(t, "alpha"), "david", "")
	req, _ := EncodePull("n1", 1, 7, 0)
	hi, entries, err := DecodeBatch(srv.Handle(req))
	if err != nil {
		t.Fatalf("DecodeBatch: %v", err)
	}
	if len(entries) != 0 || hi != 1 {
		t.Fatalf("hi=%d entries=%d, want 1 and 0", hi, len(entries))
	}
}

func TestPushIngestsAndAcks(t *testing.T) {
	srv, st := testServer(t)
	req, _ := EncodePush("n1", 7, []space.Entry{
		{Seq: 1, Path: "obs/health", Kind: cval.KindCell, Value: mustEnc(t, int64(42))},
		{Seq: 2, Path: "tel/log", Kind: cval.KindStream, Value: mustEnc(t, "hello")},
	})
	hw, err := DecodeAck(srv.Handle(req))
	if err != nil {
		t.Fatalf("DecodeAck: %v", err)
	}
	if hw != 2 {
		t.Fatalf("hw = %d, want 2", hw)
	}
	tree, _ := st.Tree("n1", "obs/")
	if len(tree) != 1 || tree[0].Path != "obs/health" {
		t.Fatalf("obs tree = %+v", tree)
	}
}

// The property that removes the need for a retransmit layer.
func TestReplayedPushIsIdempotent(t *testing.T) {
	srv, st := testServer(t)
	req, _ := EncodePush("n1", 7, []space.Entry{
		{Seq: 1, Path: "tel/log", Kind: cval.KindStream, Value: mustEnc(t, "one")},
	})
	srv.Handle(req)
	srv.Handle(req)
	evs, _ := st.Events("n1", 100)
	if len(evs) != 1 {
		t.Fatalf("got %d events after a replayed push, want 1", len(evs))
	}
}

// Every exchange is logged — the heartbeat and the replay index (D4.3).
func TestExchangeIsLogged(t *testing.T) {
	srv, st := testServer(t)
	req, _ := EncodePull("n1", 0, 7, 0)
	srv.Handle(req)
	var n int
	if err := st.DB().QueryRow(`SELECT COUNT(*) FROM exchanges WHERE node_id='n1'`).Scan(&n); err != nil {
		t.Fatalf("count: %v", err)
	}
	if n != 1 {
		t.Fatalf("exchanges = %d, want 1", n)
	}
}

// A hostile or mis-versioned peer must get an error record, never a panic
// and never a partial apply.
func TestGarbageRequestAnswersErr(t *testing.T) {
	srv, _ := testServer(t)
	for _, junk := range [][]byte{nil, {0xff}, {0x01}, mustEnc(t, "not a record")} {
		resp := srv.Handle(junk)
		v, err := cval.Decode(resp)
		if err != nil {
			t.Fatalf("response to junk is not decodable: %v", err)
		}
		r, ok := v.(cval.Record)
		if !ok || r.Tag != "err" {
			t.Fatalf("response to junk = %v, want an #err record", cval.Format(v))
		}
	}
}
```

`Store.DB()` is needed by `TestExchangeIsLogged`; add it to Task 3's store in this task:

```go
// DB exposes the handle for tests and for the operator surface's ad-hoc
// queries. Not part of the protocol path.
func (s *Store) DB() *sql.DB { return s.db }
```

- [ ] **Step 2: Run to confirm failure**

Run: `cd ~/workspace/porta && go test ./internal/wire/ -v`
Expected: FAIL — package does not exist.

- [ ] **Step 3: Implement the message layer**

```go
// internal/wire/message.go
// Copyright (c) 2026 Ekorau LLC

// Package wire is the node-facing space-sync exchange: two node-initiated
// CBOR datagrams over plain UDP.
//
// There is no retransmit layer and that is deliberate. The exchange is
// idempotent by cursor in both directions — a lost pull response means the
// next check-in re-pulls the same range, and a lost push is resent and
// deduped on the node-seq high-water. Loss is absorbed by convergence, not
// by retransmission (design D1).
package wire

import (
	"errors"
	"fmt"

	"github.com/davidg238/porta/internal/cval"
	"github.com/davidg238/porta/internal/space"
)

var errShape = errors.New("malformed message")

func field(m cval.Map, name string) (any, bool) {
	for _, p := range m {
		if k, ok := p.K.(cval.Symbol); ok && string(k) == name {
			return p.V, true
		}
	}
	return nil, false
}

func intField(m cval.Map, name string) (int64, error) {
	v, ok := field(m, name)
	if !ok {
		return 0, fmt.Errorf("%w: missing #%s", errShape, name)
	}
	n, ok := v.(int64)
	if !ok {
		return 0, fmt.Errorf("%w: #%s is not an integer", errShape, name)
	}
	return n, nil
}

func strField(m cval.Map, name string) (string, error) {
	v, ok := field(m, name)
	if !ok {
		return "", fmt.Errorf("%w: missing #%s", errShape, name)
	}
	s, ok := v.(string)
	if !ok {
		return "", fmt.Errorf("%w: #%s is not a string", errShape, name)
	}
	return s, nil
}

// entriesToList renders [[seq, path, kind, value], …]. The value rides as an
// already-encoded value, so it is decoded here only to nest it structurally.
func entriesToList(entries []space.Entry) (cval.List, error) {
	out := make(cval.List, 0, len(entries))
	for _, e := range entries {
		v, err := cval.Decode(e.Value)
		if err != nil {
			return nil, fmt.Errorf("stored value at %s is not decodable: %w", e.Path, err)
		}
		out = append(out, cval.List{e.Seq, e.Path, int64(e.Kind), v})
	}
	return out, nil
}

func listToEntries(v any) ([]space.Entry, error) {
	l, ok := v.(cval.List)
	if !ok {
		return nil, fmt.Errorf("%w: #entries is not a list", errShape)
	}
	out := make([]space.Entry, 0, len(l))
	for _, item := range l {
		row, ok := item.(cval.List)
		if !ok || len(row) != 4 {
			return nil, fmt.Errorf("%w: an entry is not a 4-element list", errShape)
		}
		seq, ok1 := row[0].(int64)
		path, ok2 := row[1].(string)
		kind, ok3 := row[2].(int64)
		if !ok1 || !ok2 || !ok3 {
			return nil, fmt.Errorf("%w: entry header types", errShape)
		}
		raw, err := cval.Encode(row[3])
		if err != nil {
			return nil, err
		}
		out = append(out, space.Entry{Seq: seq, Path: path, Kind: cval.Kind(kind), Value: raw})
	}
	return out, nil
}

func EncodePull(id string, cursor, bootID, hw int64) ([]byte, error) {
	return cval.Encode(cval.Record{Tag: "pull", Fields: cval.Map{
		{cval.Symbol("id"), id},
		{cval.Symbol("cursor"), cursor},
		{cval.Symbol("bootId"), bootID},
		{cval.Symbol("hw"), hw},
	}})
}

func EncodePush(id string, bootID int64, entries []space.Entry) ([]byte, error) {
	l, err := entriesToList(entries)
	if err != nil {
		return nil, err
	}
	return cval.Encode(cval.Record{Tag: "push", Fields: cval.Map{
		{cval.Symbol("id"), id},
		{cval.Symbol("bootId"), bootID},
		{cval.Symbol("entries"), l},
	}})
}

func encodeBatch(hi int64, entries []space.Entry) ([]byte, error) {
	l, err := entriesToList(entries)
	if err != nil {
		return nil, err
	}
	return cval.Encode(cval.Record{Tag: "batch", Fields: cval.Map{
		{cval.Symbol("hi"), hi},
		{cval.Symbol("entries"), l},
	}})
}

func encodeAck(hw int64) ([]byte, error) {
	// #hint (the §4.4 back-pressure hint) is reserved and deliberately not
	// emitted, so the shape does not move when it lands.
	return cval.Encode(cval.Record{Tag: "ack", Fields: cval.Map{{cval.Symbol("hw"), hw}}})
}

func encodeErr(msg string) []byte {
	b, err := cval.Encode(cval.Record{Tag: "err", Fields: cval.Map{{cval.Symbol("msg"), msg}}})
	if err != nil {
		// An unencodable error record would leave a node with no reply at
		// all; a constant is always encodable.
		b, _ = cval.Encode(cval.Record{Tag: "err", Fields: cval.Map{{cval.Symbol("msg"), "internal"}}})
	}
	return b
}

func DecodeBatch(b []byte) (int64, []space.Entry, error) {
	v, err := cval.Decode(b)
	if err != nil {
		return 0, nil, err
	}
	r, ok := v.(cval.Record)
	if !ok || r.Tag != "batch" {
		return 0, nil, fmt.Errorf("%w: not a #batch (%s)", errShape, cval.Format(v))
	}
	hi, err := intField(r.Fields, "hi")
	if err != nil {
		return 0, nil, err
	}
	ev, ok := field(r.Fields, "entries")
	if !ok {
		return 0, nil, fmt.Errorf("%w: missing #entries", errShape)
	}
	entries, err := listToEntries(ev)
	return hi, entries, err
}

func DecodeAck(b []byte) (int64, error) {
	v, err := cval.Decode(b)
	if err != nil {
		return 0, err
	}
	r, ok := v.(cval.Record)
	if !ok || r.Tag != "ack" {
		return 0, fmt.Errorf("%w: not an #ack (%s)", errShape, cval.Format(v))
	}
	return intField(r.Fields, "hw")
}
```

- [ ] **Step 4: Implement the server**

```go
// internal/wire/server.go
// Copyright (c) 2026 Ekorau LLC

package wire

import (
	"context"
	"fmt"
	"net"

	"github.com/davidg238/porta/internal/cval"
	"github.com/davidg238/porta/internal/space"
)

type Server struct {
	st   *space.Store
	logf func(string, ...any)
}

func NewServer(st *space.Store, logf func(string, ...any)) *Server {
	if logf == nil {
		logf = func(string, ...any) {}
	}
	return &Server{st: st, logf: logf}
}

// Handle turns one request datagram into one response datagram. It is pure
// (bytes in, bytes out) so the whole protocol is testable without a socket,
// and it NEVER returns nil: a node that gets no reply cannot distinguish a
// rejection from a lost datagram, and would retry forever.
func (s *Server) Handle(req []byte) []byte { return s.HandleFrom(req, "") }

// HandleFrom is Handle plus the address the datagram arrived from, stamped on
// the exchange so the fleet keeps the transport breakdown v1 read out of
// nodes.source_addr (design §4.2). An empty peer is legal and never clobbers.
func (s *Server) HandleFrom(req []byte, peer string) []byte {
	resp, err := s.handle(req, peer)
	if err != nil {
		s.logf("porta: rejecting datagram: %v", err)
		return encodeErr(err.Error())
	}
	return resp
}

func (s *Server) handle(req []byte, peer string) ([]byte, error) {
	v, err := cval.Decode(req)
	if err != nil {
		return nil, err
	}
	r, ok := v.(cval.Record)
	if !ok {
		return nil, fmt.Errorf("%w: not a record", errShape)
	}
	switch r.Tag {
	case "pull":
		return s.handlePull(r, peer)
	case "push":
		return s.handlePush(r, peer)
	}
	return nil, fmt.Errorf("%w: unknown message #%s", errShape, r.Tag)
}

func (s *Server) handlePull(r cval.Record, peer string) ([]byte, error) {
	id, err := strField(r.Fields, "id")
	if err != nil {
		return nil, err
	}
	cursor, err := intField(r.Fields, "cursor")
	if err != nil {
		return nil, err
	}
	bootID, err := intField(r.Fields, "bootId")
	if err != nil {
		return nil, err
	}
	// A pull carries the cursor the node has ALREADY applied, so recording
	// it here is what makes "the node's cursor passed seq N" better evidence
	// of delivery than v1's delivered_at ever was.
	if err := s.st.NoteCursor(id, cursor); err != nil {
		return nil, err
	}
	entries, hi, err := s.st.OutboundSince(id, cursor, space.MaxBatchBytes)
	if err != nil {
		return nil, err
	}
	if err := s.st.RecordExchange(id, bootID, cursor, hi, 0, 0, peer); err != nil {
		return nil, err
	}
	return encodeBatch(hi, entries)
}

func (s *Server) handlePush(r cval.Record, peer string) ([]byte, error) {
	id, err := strField(r.Fields, "id")
	if err != nil {
		return nil, err
	}
	bootID, err := intField(r.Fields, "bootId")
	if err != nil {
		return nil, err
	}
	ev, ok := field(r.Fields, "entries")
	if !ok {
		return nil, fmt.Errorf("%w: missing #entries", errShape)
	}
	entries, err := listToEntries(ev)
	if err != nil {
		return nil, err
	}
	lo := int64(0)
	if len(entries) > 0 {
		lo = entries[0].Seq
	}
	hw, err := s.st.Ingest(id, bootID, entries)
	if err != nil {
		return nil, err
	}
	if err := s.st.RecordExchange(id, bootID, 0, 0, lo, hw, peer); err != nil {
		return nil, err
	}
	return encodeAck(hw)
}

// ListenAndServe runs the UDP loop. One datagram in, one datagram out; no
// connection state, so a sleepy node costs the gateway nothing between
// check-ins.
func (s *Server) ListenAndServe(ctx context.Context, addr string) error {
	pc, err := net.ListenPacket("udp", addr)
	if err != nil {
		return err
	}
	defer pc.Close()
	go func() { <-ctx.Done(); pc.Close() }()

	buf := make([]byte, 1500) // the IPv6 MTU bounds a legal datagram
	for {
		n, peer, err := pc.ReadFrom(buf)
		if err != nil {
			if ctx.Err() != nil {
				return nil
			}
			return err
		}
		req := make([]byte, n)
		copy(req, buf[:n])
		resp := s.HandleFrom(req, peer.String())
		if _, err := pc.WriteTo(resp, peer); err != nil {
			s.logf("porta: reply to %s failed: %v", peer, err)
		}
	}
}
```

- [ ] **Step 5: Run the tests**

Run: `cd ~/workspace/porta && go test ./internal/wire/ -v`
Expected: PASS, all six tests.

- [ ] **Step 6: Commit**

```bash
gofmt -l . && go test ./...
git add internal/wire internal/space
git commit -m "feat(wire): the pull/push exchange over plain UDP + CBOR

Two node-initiated datagrams, both idempotent, no retransmit layer. A
lost pull response means the next check-in re-pulls the same range; a
lost push is resent and deduped on the node-seq high-water. Loss is
absorbed by convergence rather than retransmission, which is why the
control plane needs neither CoAP's CON/ACK nor TFTP's blocking (D1).

Handle is pure bytes-to-bytes so the protocol is testable without a
socket, and it never returns nil: a node that gets no reply cannot tell
a rejection from a lost datagram and would retry forever."
```

---

### Task 5: `porta serve` + the operator verbs, and the loopback gate

**Files:**
- Modify: `internal/portacli/serve.go`, `internal/portacli/root.go`
- Create: `internal/portacli/get.go`, `internal/portacli/put.go`, `internal/portacli/log.go`
- Test: `internal/portacli/loopback_test.go`

**Interfaces:**
- Consumes: `space`, `wire`, `cval`, `httpsrv`.
- Produces: `porta serve --db --udp --http`; `porta get <node> [prefix]`; `porta put <node> <path> <value>`; `porta log <node>`.

- [ ] **Step 1: Write the failing loopback gate**

This is the task's deliverable: a synthetic node driving a real UDP socket against a live server.

```go
// internal/portacli/loopback_test.go
package portacli

import (
	"context"
	"net"
	"testing"
	"time"

	"github.com/davidg238/porta/internal/cval"
	"github.com/davidg238/porta/internal/space"
	"github.com/davidg238/porta/internal/wire"
)

// A synthetic node: exactly the exchange the nsl bridge job will run, over a
// real socket, so the framing is proven before any hardware is involved.
type fakeNode struct {
	t      *testing.T
	conn   net.Conn
	id     string
	bootID int64
	cursor int64
	seq    int64
}

func (n *fakeNode) roundTrip(req []byte) []byte {
	n.t.Helper()
	if _, err := n.conn.Write(req); err != nil {
		n.t.Fatalf("write: %v", err)
	}
	n.conn.SetReadDeadline(time.Now().Add(2 * time.Second))
	buf := make([]byte, 1500)
	m, err := n.conn.Read(buf)
	if err != nil {
		n.t.Fatalf("read: %v", err)
	}
	return buf[:m]
}

func (n *fakeNode) pull() []space.Entry {
	n.t.Helper()
	req, err := wire.EncodePull(n.id, n.cursor, n.bootID, 0)
	if err != nil {
		n.t.Fatal(err)
	}
	hi, entries, err := wire.DecodeBatch(n.roundTrip(req))
	if err != nil {
		n.t.Fatalf("DecodeBatch: %v", err)
	}
	n.cursor = hi // persisted only after apply, exactly as the bridge job will
	return entries
}

func (n *fakeNode) push(path string, kind cval.Kind, v any) int64 {
	n.t.Helper()
	raw, err := cval.Encode(v)
	if err != nil {
		n.t.Fatal(err)
	}
	n.seq++
	req, err := wire.EncodePush(n.id, n.bootID, []space.Entry{{Seq: n.seq, Path: path, Kind: kind, Value: raw}})
	if err != nil {
		n.t.Fatal(err)
	}
	hw, err := wire.DecodeAck(n.roundTrip(req))
	if err != nil {
		n.t.Fatalf("DecodeAck: %v", err)
	}
	return hw
}

func TestLoopbackGate(t *testing.T) {
	var clock int64 = 1_000
	st, err := space.Open(t.TempDir()+"/t.db", func() int64 { clock += 10; return clock })
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	defer st.Close()

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	srv := wire.NewServer(st, func(string, ...any) {})
	// Port 0 so the test never collides with a running gateway.
	pc, err := net.ListenPacket("udp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("listen: %v", err)
	}
	addr := pc.LocalAddr().String()
	pc.Close()
	go srv.ListenAndServe(ctx, addr)
	time.Sleep(50 * time.Millisecond)

	conn, err := net.Dial("udp", addr)
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	defer conn.Close()
	node := &fakeNode{t: t, conn: conn, id: "c6-01", bootID: 1}

	// 1. A cell written at porta reaches the node.
	if _, err := st.Write("c6-01", "goal/name", cval.KindCell, mustEncode(t, "shipsbells"), "david", "bring-up"); err != nil {
		t.Fatalf("Write: %v", err)
	}
	got := node.pull()
	if len(got) != 1 || got[0].Path != "goal/name" {
		t.Fatalf("pull got %+v", got)
	}
	if v, _ := cval.Decode(got[0].Value); v != "shipsbells" {
		t.Fatalf("goal/name = %v", v)
	}

	// 2. An idle pull is the heartbeat: empty, not an error.
	if got := node.pull(); len(got) != 0 {
		t.Fatalf("second pull returned %d entries, want 0", len(got))
	}

	// 3. A cell pushed by the node reaches porta's tree.
	node.push("obs/health", cval.KindCell, cval.Record{Tag: "Health", Fields: cval.Map{
		{cval.Symbol("uptimeMs"), int64(12345)},
		{cval.Symbol("bootId"), int64(1)},
	}})
	tree, _ := st.Tree("c6-01", "obs/")
	if len(tree) != 1 || tree[0].Path != "obs/health" {
		t.Fatalf("obs tree = %+v", tree)
	}

	// 4. Stream entries arrive exactly once across a duplicated datagram —
	//    the property that lets the wire carry no retransmit layer.
	node.seq++
	raw, _ := cval.Encode("boot complete")
	req, _ := wire.EncodePush(node.id, node.bootID, []space.Entry{
		{Seq: node.seq, Path: "tel/log", Kind: cval.KindStream, Value: raw},
	})
	node.roundTrip(req)
	node.roundTrip(req) // the same datagram again, as a lossy radio would
	evs, _ := st.Events("c6-01", 100)
	n := 0
	for _, e := range evs {
		if e.Path == "tel/log" {
			n++
		}
	}
	if n != 1 {
		t.Fatalf("tel/log appears %d times after a duplicated push, want 1", n)
	}

	// 5. Every exchange is logged.
	var xn int
	if err := st.DB().QueryRow(`SELECT COUNT(*) FROM exchanges WHERE node_id='c6-01'`).Scan(&xn); err != nil {
		t.Fatal(err)
	}
	if xn != 5 {
		t.Fatalf("exchanges = %d, want 5 (2 pulls + 3 pushes)", xn)
	}
}

func mustEncode(t *testing.T, v any) []byte {
	t.Helper()
	b, err := cval.Encode(v)
	if err != nil {
		t.Fatal(err)
	}
	return b
}
```

- [ ] **Step 2: Run to confirm failure**

Run: `cd ~/workspace/porta && go test ./internal/portacli/ -run TestLoopbackGate -v`
Expected: FAIL — compile error, the CLI does not import these yet.

- [ ] **Step 3: Wire `serve` to the store and the UDP listener**

```go
// internal/portacli/serve.go
func serveCmd() *cobra.Command {
	var dbPath, udpAddr, httpAddr string
	cmd := &cobra.Command{
		Use:   "serve",
		Short: "Run the porta gateway: UDP space listener + operator HTTP surface",
		RunE: func(c *cobra.Command, _ []string) error {
			st, err := space.Open(dbPath, func() int64 { return time.Now().UnixMilli() })
			if err != nil {
				return err
			}
			defer st.Close()

			ctx, cancel := signal.NotifyContext(c.Context(), os.Interrupt, syscall.SIGTERM)
			defer cancel()

			errc := make(chan error, 2)
			go func() { errc <- wire.NewServer(st, log.Printf).ListenAndServe(ctx, udpAddr) }()
			go func() {
				srv, err := httpsrv.New(httpsrv.Config{Addr: httpAddr}, st)
				if err != nil {
					errc <- err
					return
				}
				errc <- srv.ListenAndServe(ctx)
			}()
			return <-errc
		},
	}
	cmd.Flags().StringVar(&dbPath, "db", "porta.db", "SQLite database path")
	cmd.Flags().StringVar(&udpAddr, "udp", ":6969", "node-facing UDP listen address")
	cmd.Flags().StringVar(&httpAddr, "http", ":6970", "operator HTTP listen address")
	return cmd
}
```

- [ ] **Step 4: Add the read verbs**

```go
// internal/portacli/get.go
// Copyright (c) 2026 Ekorau LLC

package portacli

import (
	"fmt"
	"time"

	"github.com/davidg238/porta/internal/cval"
	"github.com/davidg238/porta/internal/space"
	"github.com/spf13/cobra"
)

// getCmd dumps a node's cell tree. goal/X against obs/X is the convergence
// view AND the self-heal query — the same diff, which is why self-heal is no
// longer a mechanism.
func getCmd() *cobra.Command {
	var dbPath string
	cmd := &cobra.Command{
		Use:   "get <node> [prefix]",
		Short: "Dump a node's cell tree",
		Args:  cobra.RangeArgs(1, 2),
		RunE: func(c *cobra.Command, args []string) error {
			prefix := ""
			if len(args) == 2 {
				prefix = args[1]
			}
			st, err := space.Open(dbPath, func() int64 { return time.Now().UnixMilli() })
			if err != nil {
				return err
			}
			defer st.Close()
			rows, err := st.Tree(args[0], prefix)
			if err != nil {
				return err
			}
			for _, e := range rows {
				v, err := cval.Decode(e.Value)
				if err != nil {
					fmt.Fprintf(c.OutOrStdout(), "%-32s seq=%-6d <undecodable: %v>\n", e.Path, e.Seq, err)
					continue
				}
				fmt.Fprintf(c.OutOrStdout(), "%-32s seq=%-6d %s\n", e.Path, e.Seq, cval.Format(v))
			}
			return nil
		},
	}
	cmd.Flags().StringVar(&dbPath, "db", "porta.db", "SQLite database path")
	return cmd
}
```

```go
// internal/portacli/log.go
// Copyright (c) 2026 Ekorau LLC

package portacli

import (
	"fmt"
	"time"

	"github.com/davidg238/porta/internal/cval"
	"github.com/davidg238/porta/internal/space"
	"github.com/spf13/cobra"
)

// logCmd is v1's command audit, generalised: the write log for a node, in
// both directions, with the actor that caused each write.
func logCmd() *cobra.Command {
	var dbPath string
	var limit int
	cmd := &cobra.Command{
		Use:   "log <node>",
		Short: "Show the write log for a node, newest first",
		Args:  cobra.ExactArgs(1),
		RunE: func(c *cobra.Command, args []string) error {
			st, err := space.Open(dbPath, func() int64 { return time.Now().UnixMilli() })
			if err != nil {
				return err
			}
			defer st.Close()
			rows, err := st.Events(args[0], limit)
			if err != nil {
				return err
			}
			for _, e := range rows {
				dir := "→"
				if e.Direction == "in" {
					dir = "←"
				}
				v, err := cval.Decode(e.Value)
				text := "<undecodable>"
				if err == nil {
					text = cval.Format(v)
				}
				fmt.Fprintf(c.OutOrStdout(), "%s %s %-28s seq=%-6d %-12s %s\n",
					time.UnixMilli(e.TsMs).Format(time.RFC3339), dir, e.Path, e.Seq, e.Actor, text)
			}
			return nil
		},
	}
	cmd.Flags().StringVar(&dbPath, "db", "porta.db", "SQLite database path")
	cmd.Flags().IntVar(&limit, "limit", 50, "maximum rows")
	return cmd
}
```

- [ ] **Step 5: Add the write verb**

```go
// internal/portacli/put.go
// Copyright (c) 2026 Ekorau LLC

package portacli

import (
	"fmt"
	"os"
	"os/user"
	"strconv"
	"time"

	"github.com/davidg238/porta/internal/cval"
	"github.com/davidg238/porta/internal/space"
	"github.com/spf13/cobra"
)

// putCmd is the general path write. The operator-friendly verbs (porta app
// run, porta mode, …) are sugar over this — verbs live at the CLI and
// nowhere else (design D2).
func putCmd() *cobra.Command {
	var dbPath, kind, reason, actor string
	cmd := &cobra.Command{
		Use:   "put <node> <path> <value>",
		Short: "Write a value to a node's goal tree",
		Args:  cobra.ExactArgs(3),
		RunE: func(c *cobra.Command, args []string) error {
			k := cval.KindCell
			if kind == "stream" {
				k = cval.KindStream
			} else if kind != "cell" {
				return fmt.Errorf("--kind must be cell or stream, got %q", kind)
			}
			raw, err := cval.Encode(parseScalar(args[2]))
			if err != nil {
				return err
			}
			if actor == "" {
				actor = currentActor()
			}
			st, err := space.Open(dbPath, func() int64 { return time.Now().UnixMilli() })
			if err != nil {
				return err
			}
			defer st.Close()
			seq, err := st.Write(args[0], args[1], k, raw, actor, reason)
			if err != nil {
				return err
			}
			fmt.Fprintf(c.OutOrStdout(), "wrote %s seq=%d\n", args[1], seq)
			return nil
		},
	}
	cmd.Flags().StringVar(&dbPath, "db", "porta.db", "SQLite database path")
	cmd.Flags().StringVar(&kind, "kind", "cell", "cell or stream")
	cmd.Flags().StringVar(&reason, "reason", "", "why this write is being made (recorded in the log)")
	cmd.Flags().StringVar(&actor, "actor", "", "who is making this write (defaults to the OS user)")
	return cmd
}

// parseScalar covers the scalar cases an operator types. Records and nested
// values are not typeable on a command line and arrive through the HTTP
// surface (plan 4); a string is the honest fallback.
func parseScalar(s string) any {
	if s == "true" {
		return true
	}
	if s == "false" {
		return false
	}
	if s == "nil" {
		return nil
	}
	if len(s) > 1 && s[0] == '#' {
		return cval.Symbol(s[1:])
	}
	if n, err := strconv.ParseInt(s, 10, 64); err == nil {
		return n
	}
	if f, err := strconv.ParseFloat(s, 64); err == nil {
		return f
	}
	return s
}

func currentActor() string {
	if u, err := user.Current(); err == nil && u.Username != "" {
		return u.Username
	}
	if h, err := os.Hostname(); err == nil {
		return h
	}
	return "unknown"
}
```

Register all three in `root.go` alongside `serve`.

- [ ] **Step 6: Run the gate**

Run: `cd ~/workspace/porta && go test ./internal/portacli/ -run TestLoopbackGate -v`
Expected: PASS.

- [ ] **Step 7: Run everything and exercise the CLI by hand**

```bash
cd ~/workspace/porta
gofmt -l . && go build ./... && go test ./...
rm -f /tmp/claude-1000/-home-david-workspace-tuvm/728b0719-a646-49f0-88c0-ddd9eeff8505/scratchpad/demo.db
go run ./cmd/porta put c6-01 goal/name shipsbells --db /tmp/claude-1000/-home-david-workspace-tuvm/728b0719-a646-49f0-88c0-ddd9eeff8505/scratchpad/demo.db --reason "bring-up"
go run ./cmd/porta get c6-01 --db /tmp/claude-1000/-home-david-workspace-tuvm/728b0719-a646-49f0-88c0-ddd9eeff8505/scratchpad/demo.db
go run ./cmd/porta log c6-01 --db /tmp/claude-1000/-home-david-workspace-tuvm/728b0719-a646-49f0-88c0-ddd9eeff8505/scratchpad/demo.db
```

Expected: `get` prints `goal/name  seq=1  "shipsbells"`; `log` prints one `→` row with your username as the actor and `bring-up` recorded.

- [ ] **Step 8: Commit**

```bash
git add internal/portacli
git commit -m "feat(cli): serve, get, put, log — and the loopback gate

The gate is a synthetic node driving a real UDP socket against a live
server: a goal cell reaches the node, an idle pull is an empty heartbeat,
an obs cell reaches porta's tree, a duplicated tel/log push lands exactly
once, and every exchange is logged.

put is the general path write; the operator-friendly verbs are sugar over
it, because verbs live at the CLI and nowhere else (D2). Every write
records an actor and an optional reason (D3)."
```

---

## Self-Review

**Spec coverage.** D1 transport → Task 4. D2 verbs at the CLI only → Task 5 (`put`), Task 3 (no verb column). D3 provenance → Task 3 schema + `TestEventsCarryProvenance`. D4.1 raw CBOR → Task 3 (`value_cbor` BLOB, `Format` marked display-only). D4.2 ms timestamps → Task 3 schema. D4.3 exchange log → Task 3 + `TestExchangeIsLogged`. D4.4 `(bootId, monoMs)` → `exchanges.boot_id`. D4.5 node-relative paths → Task 3 schema comment. D5 one monotonic seq per side → Task 3 `nodes.out_seq` + `TestWriteAssignsMonotonicSeqPerNode`. D6 leaf kind → Task 3 stores kind per path, never infers from prefix. D7 demolition first → Task 1. D8 `obs/build` → RECORDS, already committed; porta needs no code since it is another cell. §4 schema → Task 3. §5 wire → Task 4. §9 gate items 1–3 → Task 5 loopback, then plan 3 on hardware.

**Not covered here, by design:** the operator HTTP/web/MCP surface (plan 4), the node side (plan 2), the hardware gate (plan 3). §7's deferrals and §8's hazards are recorded, not built.

**Placeholder scan.** No TBDs. Every code step carries real code; every test step carries real assertions.

**Type consistency.** `cval.Kind` is used uniformly in `space.Entry`, `wire`, and the CLI. `space.Entry` is the single entry type across all three packages. `Store.DB()` is introduced in Task 4 step 1 where its first consumer appears. `httpsrv.NodeCounter` is defined in Task 1 and satisfied by `*space.Store.CountNodes` in Task 3 — checked: same signature `(int, error)`.

**Both golden fixtures are verified against the shipped VM, not inferred.** The Map golden is the node's own published one (`nsl-tests/14_cbor/02_golden_hex.nsl`). The Record golden was generated from the node on 2026-08-01 — `Path A: PASS / Path B: PASS`, `d81b82d8276361636ba1d82762687705` — confirming that a Record's name really is a Symbol (tag 39) nested inside tag 27, which is where a standard tag-27 implementation would have silently diverged.
