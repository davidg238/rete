# L6 — CBOR wire codec (C seam design; the TISON-analog decision, frozen)

**Status:** design frozen 2026-07-03; **implementation deferred** to the porta-wire step
(spec §8). Frozen now because messages.md, porta's server side, and test fixtures all
depend on the choice. The decision: **adopt CBOR (RFC 8949); do not invent a format.**

## Mapping (algebra ↔ CBOR)

| algebra | encode as | decode from |
|---|---|---|
| nil | `0xf6` | `0xf6` |
| bool | `0xf4`/`0xf5` | same |
| int (see integers.md: signed 64-bit) | major 0 / major 1, shortest form | majors 0/1; value > 2^63−1 → error |
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

1. **Deterministic encoding** (RFC 8949 §4.2): shortest-form integers, definite lengths
   only, map keys sorted bytewise. ⇒ encode(x) is byte-identical everywhere ⇒ fixtures
   can be hex literals and porta can hash messages.
2. **Depth bound enforced at decode** (same bound as construction, messages.md): a
   hostile/buggy peer cannot recurse the node's C stack. Decoder is depth-counted,
   error `message depth exceeded`.
3. **Closed algebra at decode**: unknown tag, indefinite length, half-parsed input,
   duplicate map key, or non-value CBOR (e.g. undefined) → decode error (`not a value`),
   never pass-through. Decode is total: any byte sequence either yields a valid message
   or one error.
4. **No float ↔ int coercion** either direction (1.0 ≠ 1 in the algebra; encode must not
   collapse them).

## Seam shape (C, ~1–2 KB)

```
size_t cbor_encode(oop msg, uint8_t *buf, size_t cap);   // 0 = overflow (caller grows/chunks)
oop    cbor_decode(const uint8_t *buf, size_t len, err *e);
```

- Encoder: recursive descent with depth counter (bound 32 ⇒ worst-case C-stack use is
  small and static — fits the 2–8 KB Zephyr stack analysis).
- Decoder allocates algebra values bottom-up — so decoded messages satisfy
  by-construction immutability with no special path.
- nsl surface (later, one pair): `platform cborEncode: msg` → Bytes,
  `platform cborDecode: bytes ifInvalid: [...]` per errors.md.

## Test fixtures (write once, use in nsl-tests AND porta's suite)

Golden pairs generated with python `cbor2` at fixture-authoring time, stored as hex:
- each scalar type, ± edge values (int at ±SMI boundary, ±2^63−1; f32-exact vs f64 float)
- symbol and record tags round-tripped through `cbor2` to prove interop
- deterministic-order map with unsorted-input keys
- rejection corpus: indefinite-length items, unknown tag, depth 33, duplicate keys,
  truncated buffers at every prefix length of one golden message (fuzz-lite)
