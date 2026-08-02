# Rung 3 — CBOR + Int64 wire seam (design)

**Status:** design 2026-07-09; approved by David 2026-07-09 (scope, one-codec-both-platforms,
`integer out of range` message, architecture **(B)** nsl-walk + C-leaves, **RFC-bytewise**
map order). Phase 2, rung 3 of `tuvm/review/program-sequence.md` ("Wire seam: CBOR + Int64").
Implementation to follow via the writing-plans skill.

**Owners this rung reconciles:** `tuvm/review/lib/cbor.md` (L6 — CBOR codec, design frozen
2026-07-03, implementation deferred to here) and the ratified integer contract
`nsl-tuvm/docs/design/2026-07-04-L7-integer-semantics-decision.md`.

**Base (branch off master):** nsl-tuvm branch `rung3-cbor-int64` (this spec, `c31a816`);
tuvm + nsl-tests branch at implementation start. (Confirm actual master tips at branch-cut.)

---

## 1. Purpose & scope

Build the CBOR wire codec — the one deferred piece of the message algebra — proven on the
differential (Path A vs Path B) harness and reachable from nsl. A node can serialise a
message algebra value to canonical CBOR bytes and reconstruct it, and generic CBOR tooling
(`cbor2`, `cbor.me`) reads the traffic with zero custom code.

The "Int64" half of the rung's name is **already shipped**: Milestone A built the
`±(2^64−1)` sign+magnitude `LargeInteger`, and the L7 ratification fixed `=`/hash. What
remains for integers here is (a) encoding/decoding that full range correctly on the wire,
and (b) retiring two stale documentation rules that predate the shipped model by one day.

### In scope

1. **CBOR value codec** — recursive walk in nsl (§2, §3) with a small set of C leaf
   byte-primitives; deterministic, depth-bounded, closed-algebra, total decode.
2. **Integer reconciliation** — the codec's integer path implements the ratified
   `±(2^64−1)` contract; the design corrects the stale `> 2^63−1 → error` rule in
   `cbor.md` and the stale `overflow past 2^63−1` line in `errors.md`.
3. **nsl surface** — a `Cbor` utility (module sibling of `List`/`Map`/`Record`):
   `Cbor encode: msg` → `Bytes`, `Cbor decode: bytes ifInvalid: [ :err | … ]`, exercised by
   a new `nsl-tests/14_cbor` both-path gate.
4. **Test corpus** — golden-hex fixtures + rejection corpus + truncation fuzz-lite, with
   `cbor2` / `zcbor` used as independent external oracles.
5. **Co-scheduled (independent) fix** — the `.oimg` large-SMI-constant sign-corruption bug
   (findings `2026-07-04-oimg-large-smi-constant-corruption.md`), §7.

### Out of scope (explicit)

- **Porta wire framing / length-prefix / transport.** This rung is the *value* codec only
  (`encode(msg) → bytes`, `decode(bytes) → msg`). The porta-wire step is Phase 3.
- **Wrapping / masking arithmetic surface** (L7's deferred "option 3"). Tracked, not built
  — see §8. The concrete adjacent bug is tuvm#4 (bitops silently return `TAG_SMI(0)` on
  `LargeInteger` operands), which makes L7's "masking works today" false for large values.
- **`zcbor` in the shipping path.** Used only as a test oracle (§6), never linked into a
  VM. Rationale in §2.

---

## 2. Architecture: nsl walk + C leaf byte-primitives (decision B)

**Decision (B):** the recursive descent lives in **nsl** (a `Cbor` utility class); it drops
to a small set of **C leaf primitives** only for the byte-level work that genuinely needs C
and must be byte-identical across platforms. This is still "one codec compiled for both host
and target" (nsl runs on both; byte-identity comes from the shared C leaves and the shared
nsl walk). `zcbor` (vendored in the Zephyr build) and python `cbor2` serve as independent
test oracles only.

### 2.1 Why the walk is in nsl, not C (representation reality)

The frozen `cbor.md` assumed a pure-C `~1–2 KB` seam over a **C-visible** algebra. Two of
its premises no longer hold, so the pure-C shape was reconsidered and rejected:

- **The algebra containers are nsl objects, not C structs.** `List = |items depth|`,
  `Map = |keys vals depth|`, `Record = |tag fields depth|` are heap kernel classes in
  `nsl-tuvm/lib/kernel.ns`; only `String`/`Symbol`/`Float`/`LargeInteger`/`RawBytes` are
  C-visible. A C codec would have to cache their vtables *and* either reach into their
  private ivar layout or re-enter the interpreter to build them — coupling or heavy
  mid-decode GC rooting.
- **"Depth bound protects the C stack" is now moot.** Post-derecursification, nsl recursion
  runs on a **growable heap stack**, not the C stack — so an nsl-level recursive walk is
  *safer* against a hostile deep message than deep C recursion. The depth bound (32) is
  still enforced; it just guards a heap stack now.
- **Porta shares fixtures, not code.** `cbor.md`'s "write once, use in nsl-tests AND porta's
  suite" refers to the golden **hex fixtures**. Porta speaks standard CBOR with any library;
  it does not need our codec's implementation, so "keep it in C for porta" is not a real
  constraint.

The Milestone-B `Map` doc already anticipated this: *"Duplicate keys in construction: LAST
WINS — pinned because future CBOR decoders construct by overwrite."* The decoder is meant to
build maps **through the nsl `Map` constructor**, which already does canonical sort + dedup +
depth. Architecture (B) uses exactly that, keeping the codec DRY.

### 2.2 Why not `zcbor` in the shipping path

Our messages are a *dynamic recursive algebra* (arbitrary nested list/map/record/symbol with
tags 27/39), not a fixed CDDL schema — so `zcbor`'s codegen path does not apply. Its fidelity
(deterministic canonical form, our closed-algebra rules) is not free either; we implement
those regardless. And the load-bearing determinism guarantee — `encode(x)` byte-identical on
host and target (RFC 8949 §4.2) — is best served by one shared implementation, not two.

### 2.3 Division of labour

**nsl `Cbor` class owns:** the recursive walk; dispatch on `valueRank`/`isList`/`isMap`/
`isRecord`; container structure (major-4 list, major-5 map, tag-27 record, tag-39 symbol);
the depth counter (bound 32); deterministic map-key sort (§4.2); duplicate-key and
closed-algebra decode checks; construction via `List with:` / `Map fromPairs:` (overwrite ⇒
dedup) / `Record tag:fields:`; nil/bool and byte-copy of String/Bytes payloads; UTF-8
re-validation on decode (via the C leaf below).

**C leaf primitives own** only what needs C and must be byte-identical (§5): integer
value ↔ major 0/1 bytes (reading through SMI *and* `LargeInteger`, full `±(2^64−1)`), float
value ↔ shortest of f16/f32/f64, CBOR head (major + argument varint) reading, and UTF-8
validation. These are pure computational prims (no capability/I-O), siblings of `sqrt`/
`string:`.

Rationale for the split: integer byte-decomposition **cannot** be done in nsl today — nsl
bitops on `LargeInteger` are broken (tuvm#4, §8), so a `LargeInteger`'s 64-bit magnitude
bytes are unreachable from nsl. Float IEEE bit-layout and shortest-form selection likewise
need C. Everything else is natural, safe nsl.

---

## 3. Seam shape

### 3.1 nsl surface (`Cbor`, module sibling of `List`/`Map`/`Record`)

```newspeak
Cbor encode: aMessage                      "→ Bytes (frozen RawBytes); depth/closed by construction"
Cbor decode: aBytes ifInvalid: aBlock      "→ message, or aBlock value: errText on any decode error"
```

- `encode:` walks the value bottom-up, appending encoded fragments into a growable byte
  accumulator (a mutable `RawBytes`/Array buffer, frozen to `Bytes` at the end). Depth-counted
  (bound 32) — a cyclic or over-deep value cannot occur (algebra values are acyclic and
  depth-capped at construction), but the counter is asserted defensively.
- `decode:ifInvalid:` is **total**: any byte sequence yields a valid message or exactly one
  error routed to `aBlock` with the error text (errors.md vocabulary). No partial parse, no
  pass-through.

*(This supersedes `cbor.md`'s indicative `platform cborEncode:` spelling: the codec is pure
computation, not a `platform` capability, so it lives on an nsl `Cbor` class; only the C
leaves are `platform` prims.)*

### 3.2 C leaf primitives (representative; exact ids/signatures finalised in the plan)

Appended to `PRIM_NAMES` after `pumpEvents`(=96), so no pre-existing id shifts; each id = its
1-based `PRIM_NAMES` position; `prim_ids.h` mirrors updated. Two `platform_dispatch` cases per
prim, mirroring the `string:`/`basicNewBytes:` idiom.

```
platform cborEncodeInt: anInteger              → Bytes   "major 0/1, shortest form; SMI + LargeInteger; full ±(2^64−1)"
platform cborEncodeFloat: aFloat               → Bytes   "major 7, shortest of f16/f32/f64 that round-trips"
platform cborReadHead: aBytes at: anIndex      → Array{ majorSMI, argInteger, nextIndexSMI }  "argInteger may be LargeInteger for 8-byte args; nextIndex past the head"
platform cborIntFromMajor: m arg: anInteger ifOutOfRange: aBlock  → Integer  "major 0→arg; major 1→ −1−arg; −2^64 → aBlock value"
platform cborReadFloat: aBytes at: anIndex width: w  → Float       "w ∈ {2,4,8} from the head; IEEE decode"
platform utf8Valid: aBytes                     → Boolean "RFC 3629 well-formedness (dialect: byte-granular String needs re-validation)"
```

- `cborEncodeInt:` / `cborEncodeFloat:` allocate a fresh frozen `RawBytes` of exactly the
  encoded length (`basicNewBytes:` + fill + `freezeBytes:` idiom).
- `cborReadHead:` returns the three parsed head fields; the nsl walk dispatches on `major` and
  advances by `nextIndex`. It signals malformed/truncated heads (e.g. length runs past
  `aBytes size`) by answering a distinguished sentinel the nsl walk turns into `not a value`.
- GC: each leaf does at most one allocation from already-rooted args; the nsl walk holds the
  accumulator and partially-built values in its own frame regs (heap-stack scanned by GC) —
  the pure-C bottom-up-rooting hazard of the rejected (A) design does not arise here.

### 3.3 Depth bound

Bound **32** — the same bound `messages.md` enforces at construction. Enforced in the nsl
walk on both encode and decode. A hostile/buggy peer therefore cannot drive unbounded
recursion; error `message depth exceeded`.

---

## 4. Algebra ↔ CBOR mapping

Per the frozen `cbor.md` table. Algebra (messages.md): `nil, Boolean, Integer, Float,
String, Bytes, Symbol, List, Map, Record`.

| algebra | encode as | decode from |
|---|---|---|
| nil | `0xf6` | `0xf6` |
| Boolean | `0xf4` / `0xf5` | same |
| Integer | major 0 / major 1, shortest form (§4.1) | majors 0/1; out-of-range → error (§4.1) |
| Float | major 7: shortest of f16/f32/f64 that round-trips | any of f16/f32/f64 |
| String | major 3, definite length (payload already valid UTF-8) | major 3; **re-validate UTF-8** |
| Bytes | major 2, definite length | major 2 |
| Symbol | **tag 39** (identifier) + major-3 text | tag 39 + text → interned Symbol |
| List | major 4, definite length | major 4 |
| Map | major 5, definite length, keys bytewise-sorted (§4.2) | major 5 |
| Record | **tag 27** (language-object) + `[tagSymbol, fieldsMap]` | tag 27 + 2-array |

Tags 27 and 39 are IANA-registered with exactly these meanings, so `cbor2` / `cbor.me`
render porta traffic readably. **No float ↔ int coercion** in either direction (`1.0 ≠ 1`
in the algebra; encode must not collapse them; a major-7 float that happens to be integral
decodes to `Float`, and a major-0/1 int decodes to `Integer`).

### 4.1 Integer reconciliation (the "Int64" half)

The codec's integer path implements the **ratified `±(2^64−1)` contract**, superseding two
stale docs written one day earlier.

- **Encode** (`platform cborEncodeInt:`): read the value through **both** representations —
  SMI *and* `LargeInteger` — keyed on the numeric **value**, not the representation.
  Consequence: `2^40` (an SMI on 64-bit host, a `LargeInteger` on 32-bit target) encodes to
  **byte-identical CBOR on both platforms**. Shortest-form always.
- **Range mapping:** the full `−(2^64−1) … +(2^64−1)` fits standard CBOR — **major 0**
  carries `0 … 2^64−1`, **major 1** carries `−1 … −2^64`. Every value we hold produces
  valid, shortest-form CBOR that any 64-bit peer or generic tool decodes exactly.
- **Decode** (`platform cborIntFromMajor:arg:ifOutOfRange:`): rejects, with the distinct
  error **`integer out of range`**, the two legal-CBOR integers unrepresentable for us —
  `−2^64` (major 1 with argument `2^64−1`, one below our floor), and any CBOR **bignum**
  (tags 2/3 — a legal but too-wide integer encoding). David's ruling: a distinct message,
  not the umbrella `not a value`, because these are legal CBOR from a 64-bit peer, and a
  distinct message aids debugging. (Tags 2/3 are handled as the integer-range case in the
  nsl walk, not as generic unknown tags.)
- **Doc corrections this rung lands:**
  - `tuvm/review/lib/cbor.md` — replace the mapping row's `value > 2^63−1 → error` with the
    `±(2^64−1)` rule + `integer out of range` on `−2^64`/bignum.
  - `tuvm/review/lib/errors.md` — the `integer overflow past 2^63−1` line is stale; the VM
    traps at `±(2^64−1)` with `LargeInteger overflow (magnitude exceeds 2^64-1)`. Correct the
    row and add the decode-side `integer out of range` entry.

### 4.2 Deterministic encoding (RFC 8949 §4.2) — bytewise map order

Shortest-form integers, **definite lengths only**, and **map keys sorted bytewise by their
encoded key bytes** (David's ruling — true RFC-canonical, so any conformant encoder including
porta's reproduces identical bytes and cross-implementation message hashing is robust).

Because `Map` stores entries in the algebra's `valueCompare:` order — which is *not* the same
as bytewise-over-encoded-keys (they diverge when keys differ in encoded length) — the `encode:`
walk must **re-sort at emit time**: encode each key to its CBOR bytes, sort the (keyBytes,
valBytes) entry pairs bytewise by keyBytes, then emit in that order. This is a small in-nsl
sort over an already-encoded, typically-small key set. ⇒ `encode(x)` is byte-identical
everywhere ⇒ fixtures are hex literals and porta can hash messages. This is a per-rule test
(§9): unsorted-input map ⇒ bytewise-sorted output.

---

## 5. Decode is total and closed

`Cbor decode:ifInvalid:` is **total**: any byte sequence yields either one valid algebra
message or exactly one error routed to the block — never a partial parse, never pass-through.
Rejections (each a test), enforced in the nsl walk:

| condition | error (errors.md vocabulary) |
|---|---|
| indefinite-length item | `not a value` |
| unknown tag (any tag other than 27/39; bignum tags 2/3 handled below) | `not a value` |
| duplicate map key | `not a value` |
| non-value CBOR (e.g. `undefined`) | `not a value` |
| half-parsed / truncated input | `not a value` |
| tree deeper than bound 32 | `message depth exceeded` |
| String bytes not valid UTF-8 | `invalid utf-8` |
| integer `−2^64`, or CBOR bignum (tags 2/3) | `integer out of range` |

`not a value` is the umbrella for closed-algebra violations (per frozen `cbor.md` rule 3);
`message depth exceeded`, `invalid utf-8`, and `integer out of range` are the three distinct
sub-cases carried through for diagnosability.

**Duplicate-key note:** rule 3 forbids duplicate map keys → `not a value`. This is stricter
than `Map`'s construction "last-wins overwrite." As the decode walk collects the map's
key/value pairs, it checks each freshly-decoded key against those already collected (exact
algebra `=`) and rejects on the first collision — *before* handing the pairs to the `Map`
constructor. It does not silently rely on last-wins. (Last-wins remains the constructor's
behaviour for nsl-authored maps; the wire decoder is stricter by spec.)

---

## 6. nsl surface & prims — registration

- **nsl:** the `Cbor` class in `nsl-tuvm/lib/kernel.ns` (module sibling of `List`/`Map`/
  `Record`), with `encode:` and `decode:ifInvalid:` and the private recursive walk.
- **Prims (append-only):** last prim is `PRIM_pf_pumpEvents = 96` (`prim_ids.h:98`). The C
  leaf prims (§3.2) are appended as ids `97, 98, …` — enum + `PRIM_NAMES` + `prim_ids.h`
  mirror, `builtins.def` untouched. `platform_dispatch` cases switch on `pid`, mirroring the
  gpio prims. The plan re-counts append-only ids and the review re-verifies mechanically (as
  every prior prim rung did).
- **No new capability object.** The leaves are pure computational prims on the existing
  `platform` primitive receiver (like `string:`, `sqrt`); `Cbor` is ordinary nsl code and
  needs no capability wiring.

---

## 7. Co-scheduled: `.oimg` large-SMI-constant fix (independent)

Bounded and mechanically independent of the codec; co-scheduled because it is the one
integer-wire-fidelity gap and, once fixed, lets the CBOR fixtures include the corrupting band
as a regression rather than tiptoeing around it. Root cause (findings doc): an off-by-one in
the oimg SMI ref-word tag-capacity bound.

- `tools/oimg_serialize.py` `_smi()` — bound magnitude by `1 << (self.oop*8 − 3)`
  (`< 2^61` on 64-bit, `< 2^29` on 32-bit) and **keep the `raise` as a hard guard** (never
  silently corrupt).
- `tools/ns_compile_oevm.py:813` — lower the `SMI:` threshold to the ref-word's true capacity
  (ideally derived from the target `oop` width, not a fixed `1<<62`) so wider literals emit
  `LARGEINT:` (which already round-trips correctly on both paths).
- Add a corpus test with an integer **literal in `[2^61, 2^62)`** exercised on both paths
  (A==B), proving the sign no longer corrupts.

This is a separate work-item in the plan, not entangled with the codec tasks.

---

## 8. Deferred, tracked

- **Wrapping / masking arithmetic surface** (L7 option 3) — not built this rung. At close,
  file a tracking issue (or annotate tuvm#4) so the deferral is recorded, not lost. The
  concrete substance is **tuvm#4**: the integer bitop family lacks a `LargeInteger` path
  (silent `TAG_SMI(0)` on boxed operands), which makes L7's "masking works today (`bitAnd:`)"
  false for exactly the large values that land on the heap path. Compute-path bug, orthogonal
  to the wire codec — does not block this rung. (It is also *why* integer byte-decomposition
  must be a C leaf, §2.3.)

---

## 9. Test plan

### C unit tests (`tuvm/tests`, run_c_tests harness) — the leaf prims

- **`cborEncodeInt:` edges:** `0`, `1`, `23`/`24` (1-byte↔2-byte head boundary), `255`/`256`,
  `65535`/`65536`, `2^32−1`, `2^32`, ±SMI boundary, `+(2^64−1)`, `−1`, `−(2^64−1)` — each a
  golden hex from `cbor2`; shortest-form asserted.
- **`cborIntFromMajor:…` rejects:** `−2^64` (major 1, arg `2^64−1`) → `integer out of range`;
  a bignum tag → `integer out of range`.
- **`cborEncodeFloat:`:** f32-exact value → 5-byte f32; f64-only value → 9-byte f64; a
  half-representable value → 3-byte f16; assert no int coercion (`1.0` → major 7, not 0).
- **`cborReadHead:`** on truncated/over-long length → sentinel; **`utf8Valid:`** accept/reject
  corpus (valid multibyte, overlong, lone continuation, truncated tail).
- **External oracle cross-check:** feed leaf output to `cbor2` and assert it decodes to the
  expected scalar.

### nsl-tests (`14_cbor`, differential both-path A==B)

- **Round-trip** a representative message — a `Record` with a symbol-keyed `Map` containing a
  large integer (`> host SMI`), a string, a bytes, a float, a nested `List`, `nil`, and both
  booleans — through `Cbor encode:` then `Cbor decode:ifInvalid:`; assert structural `=` to
  the original. **A==B.**
- **Golden hex:** assert `Cbor encode:` of a fixed small message equals a `cbor2`-authored hex
  literal (byte-identity, both paths).
- **Deterministic map order:** build a `Map` whose keys, if emitted in insertion or
  `valueCompare:` order, would differ from bytewise (keys of differing encoded length); assert
  encoded bytes match the bytewise-sorted golden.
- **Rejection corpus:** `Cbor decode:ifInvalid:` on indefinite-length, unknown-tag, depth-33,
  duplicate-key, and truncated buffers runs the block; assert the block's error text
  (non-tautological — assert the value, not just absence of crash).
- **Integer band (post-§7):** a golden round-trip using a literal in `[2^61, 2^62)`.

### Gates

Whole-suite both-path gate green (controller-owned); `14_cbor` PASS; the four named gates
(grant / debugger-integration / portability-32 / boot-gc-asan) PASS. The new C leaf source
(if a new `.c` file rather than added to `dispatch.c`/`platform.c`) must be added to **every**
VM-build source list — the recurring harness-linkage gap: `tools/run_tests_oevm.py` build
lists, `test_supervisor_grant.py`, `test_debugger_integration.py`, `tests/test_portability_32.sh`,
`bench.sh`, `tests/test_load_run.sh`, `tests/test_oimg_blob.sh`, and the mcu `footprint.sh`.
(If the leaves live inside existing `dispatch.c`/`platform.c`, this gap does not arise — a
point in favour of not adding a new file.)

---

## 10. Risks & mitigations

- **Byte-identity host vs target** — guaranteed by the shared nsl walk + shared C leaf byte
  emission (value-keyed, not representation-keyed). The deterministic-order + int-edge golden
  fixtures are the proof, run on both paths.
- **Integer leaf correctness on `LargeInteger`** — the one genuinely C-hard part. Mitigation:
  exhaustive `cborEncodeInt:` edge fixtures incl. `±(2^64−1)` and the head-size boundaries,
  cross-checked against `cbor2`.
- **`valueCompare:` vs bytewise map divergence** — real; handled by the emit-time re-sort
  (§4.2) with a dedicated divergent-length test, not by assuming `Map`'s order.
- **Harness linkage** — prefer placing the leaves in existing `dispatch.c`/`platform.c` to
  avoid the ≥8-site build-list gap; if a new file is unavoidable, enumerate all sites (§9) and
  the final review greps for completeness.
- **Prim-id drift** — append-only from 97, `prim_ids.h` mirror; re-counted in plan,
  mechanically re-verified in review.

---

## 11. Deliverables

- **nsl:** `Cbor` class + recursive walk in `nsl-tuvm/lib/kernel.ns`.
- **C leaves:** the §3.2 prims in `tuvm/src/dispatch.c` + `tuvm/src/platform.c` +
  `prim_ids.h` (preferred: no new `.c` file).
- **Tests:** `tuvm/tests` C unit tests + `cbor2` fixtures; `nsl-tests/14_cbor` both-path suite.
- **`.oimg` fix:** `tools/oimg_serialize.py` + `tools/ns_compile_oevm.py` + band regression
  test.
- **Doc updates:** `cbor.md` (implemented; int rule corrected; note the nsl-`Cbor` surface and
  bytewise order), `errors.md` (int rows corrected + `integer out of range`),
  `program-sequence.md` rung-3 marked done at close, and a rung-close note.
- **Tracking issue** for the deferred wrapping surface (§8).
