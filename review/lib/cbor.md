# L6 — CBOR wire codec (the TISON-analog decision; design frozen, now implemented)

**Status:** design frozen 2026-07-03; **implemented (rung 3) 2026-07-10**. Landed as an nsl
`Cbor` class in kernel.ns (encode/decode walk) backed by 7 C leaf prims. Frozen-then-built
because messages.md, porta's server side, and test fixtures all depend on the choice. The
decision: **adopt CBOR (RFC 8949); do not invent a format.**

## Mapping (algebra ↔ CBOR)

| algebra | encode as | decode from |
|---|---|---|
| nil | `0xf6` | `0xf6` |
| bool | `0xf4`/`0xf5` | same |
| int (see integers.md: ratified L7 range ±(2^64−1)) | major 0 (0..2^64−1) / major 1 (−1..−2^64), shortest form, through both SMI and LargeInteger | majors 0/1, shortest form; **−2^64 and bignum tags 2/3 → `integer out of range`** (the algebra holds ±(2^64−1); only −2^64 overflows it) |
| float | major 7: shortest of f16/f32/f64 that round-trips | any of f16/f32/f64 |
| string | major 3, definite length (payload already valid UTF-8) | major 3; **re-validate UTF-8** |
| bytes | major 2, definite length | major 2 |
| symbol | **tag 39** (identifier) + major-3 text | tag 39 + text → interned symbol |
| list | major 4, definite length | major 4 |
| map | major 5, definite length | major 5 |
| record | **tag 27** (language-object) + `[tag-symbol, fields-map]` | tag 27 + 2-array |

Tags 27 and 39 are IANA-registered with exactly these meanings — generic CBOR tools
(`cbor.me`, python `cbor2`) render porta traffic readably with zero custom code.

## Rules (each is a test)

1. **Deterministic encoding** (RFC 8949 §4.2): shortest-form integers and floats, definite
   lengths only, map keys sorted **bytewise over the ENCODED key bytes** (RFC 8949 §4.2, not
   the source-order or a locale collation). ⇒ encode(x) is byte-identical everywhere ⇒
   fixtures can be hex literals and porta can hash messages. Symbol encodes as **tag 39** +
   UTF-8 text; Record encodes as **tag 27** + `[tag39(tagSymbol), fieldsMap]`.
2. **Depth bound enforced at decode** (same bound as construction, messages.md): a
   hostile/buggy peer cannot recurse the node's C stack. Decoder is depth-counted,
   error `message depth exceeded`.
3. **Closed algebra at decode**: unknown tag (≠27/39), indefinite length, half-parsed/
   truncated/malformed input, duplicate map key, undefined (`0xf7`), or NaN float →
   `not a value`; depth > 32 → `message depth exceeded`; malformed UTF-8 text → `invalid
   utf-8`; `−2^64` or bignum tags 2/3 → `integer out of range`. Never pass-through. Decode
   is total and closed: any byte sequence either yields exactly one valid message or
   exactly one error, routed to `ifInvalid:`.
4. **No float ↔ int coercion** either direction (1.0 ≠ 1 in the algebra; encode must not
   collapse them).

## Seam shape (as-built: architecture (B) — nsl walk + C leaf prims)

The encode/decode tree-walk lives in **nsl** (`Cbor` class, kernel.ns), not in a monolithic C
`cbor_encode`/`cbor_decode` pair. The walk calls down to 7 small C **leaf** prims for the
byte-level, non-recursive work; recursion (and the depth counter) is nsl-side.

C leaf prims (ids 97-103, append-only after `pumpEvents`=96):

| id | prim | purpose |
|---|---|---|
| 97 | `cborEncodeInt:` | shortest-form major 0/1 integer encode |
| 98 | `cborEncodeFloat:` | shortest-of-f16/f32/f64 float encode |
| 99 | `cborReadHead:at:` | read one item's major/argument head |
| 100 | `cborIntFromMajor:arg:` | reconstruct an integer from major+argument |
| 101 | `cborReadFloat:at:` | read an f16/f32/f64 payload |
| 102 | `utf8Valid:` | validate a byte range as UTF-8 |
| 103 | `asSymbol:` | String→Symbol (interning), used by tag-39 decode |

- Encoder: nsl recursive descent with depth counter (bound 32 ⇒ worst-case C-stack use stays
  small and static — fits the 2–8 KB Zephyr stack analysis).
- Decoder allocates algebra values bottom-up — so decoded messages satisfy
  by-construction immutability with no special path.
- nsl surface: `Cbor encode: aMessage` → `Bytes`; `Cbor decode: aBytes ifInvalid: aBlock` →
  the decoded message, or `aBlock value: errString` on the one error path (errors.md exact
  strings). **Not** a `platform` primitive — the codec is class-side nsl backed by the leaf
  prims above.

## Test fixtures (as-built: `nodus/tests/14_cbor/`)

Golden pairs generated with python `cbor2` at fixture-authoring time, stored as hex:
- each scalar type, ± edge values (int at ±SMI boundary, **±(2^64−1)**; f32-exact vs f64 float)
- symbol and record tags round-tripped through `cbor2` to prove interop
- deterministic-order map with unsorted-input keys
- rejection corpus: indefinite-length items, unknown tag, depth 33, duplicate keys,
  truncated buffers at every prefix length of one golden message (fuzz-lite), `−2^64` and
  bignum tags 2/3 (→ `integer out of range`)

Landed as:
- `01_roundtrip` — encode/decode identity across scalar and structural types
- `02_golden_hex` — byte-identical against `cbor2`-generated fixtures
- `03_map_order` — RFC 8949 bytewise key order over unsorted input
- `04_reject` — the rejection corpus above, each routed to `ifInvalid:` with the exact string
- `05_int_band` — the ±(2^64−1) boundary, both SMI and LargeInteger paths
- `06_float_roundtrip` — f16/f32/f64 shortest-form round-trip

Both-path (Path A solidified / Path B `.oimg`) verified A==B.

## Addendum 2026-07-22 — two device-only failures the host tests did not catch (tuvm#72)

Both surfaced only on a wifi node with a 49 KB VM arena, and both were **silent**: the
node reported a written result and the gateway received nothing.

### A decoded Symbol did not `=` an image literal (tuvm#75)

There is no symbol intern table. `platform asSymbol:` (`src/platform.c:703`) mints a FRESH
Symbol per tag-39 decode, and bare `Symbol>>=` fell through to
`Object>>= other = ( ^ self == other )` (`kernel.nsl:30`) — **identity**. So every symbol
arriving over the wire compared unequal to the literal of the same name.

`asSymbol:`'s own comment claimed the opposite ("Symbol equality is VALUE-based… so this
non-interned Symbol `=` the image-literal #foo"). It conflated the **value-object**
machinery — `kernel.nsl:2131` byteCompares rank-6 symbols, which is why Map *key* lookup
of a decoded `#state` succeeds — with bare `Symbol>>=` dispatch, which is `Object>>=`.

Fixed by intercepting `=` for Symbol receivers in `send_resolve` (`src/dispatch.c`),
comparing names. **`==` deliberately remains identity**, so the type now has two equality
rules; interning is the durable fix and is tracked as tuvm#75.

Two traps for anyone touching this:
- For a Symbol in the `.oimg` blob, `->name` is an **encoded offset, not a pointer**
  (`src/oimg.h:54`). It must go through `symbol_name()`; dereferencing directly segfaults
  on exactly the image literals the comparison exists to match.
- **`printString` renders a symbol's name regardless of identity.** A probe printing
  `#running` proves the value arrived and proves NOTHING about whether a comparison
  against it fires. Assert the comparison. This cost a full session of misdiagnosis:
  a console reading `DIAG CTL state=#running` sat directly beside an `st = #running`
  test that was returning false.

Pinned by `nodus/tests/19_latency/10_ctl_cbor_decoded_drives_capture.nsl`.

### The encode accumulator was 4 bytes per byte

`Cbor>>encode:` accumulated into an `OrderedCollection` of byte SMIs. A mutable Array slot
costs **four bytes per byte**, and the backing Array doubles. Encoding a 949-byte span
push therefore asked for a 1024-slot Array and took the node out:

```
VM GC Error: Out of Memory (requested 4104, used 49152 of 49152) -> abort()
```

A **hard abort, not a catchable condition** — nothing reached `sys/crash`, so no probe or
crash observer could see it. Now accumulates into `ByteBuilder` (packed, 1 byte/byte,
exact-size frozen output). The dominant cost was never the top-level accumulator but
`encodeMap:`, which holds a separate accumulator for **every key and every value
simultaneously** before sorting — previously a minimum 8-slot Array each, however few
bytes it held.

`02_golden_hex` and `03_map_order` still pass, so emitted bytes are unchanged: this is a
memory-shape change only, not a wire change.
