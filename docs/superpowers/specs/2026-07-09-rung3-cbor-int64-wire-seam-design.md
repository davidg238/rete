# Rung 3 — CBOR + Int64 wire seam (design)

**Status:** design 2026-07-09; approved by David 2026-07-09. Phase 2, rung 3 of
`tuvm/review/program-sequence.md` ("Wire seam: CBOR + Int64"). Implementation to follow
via the writing-plans skill.

**Owners this rung reconciles:** `tuvm/review/lib/cbor.md` (L6 — CBOR codec, design frozen
2026-07-03, implementation deferred to here) and the ratified integer contract
`nsl-tuvm/docs/superpowers/specs/2026-07-04-L7-integer-semantics-decision.md`.

**Base (branch off master):** tuvm `8eb6091`-line + rung-2 merge, nsl-tuvm `b12a085`,
nsl-tests `9cf5fd1`. (Confirm actual master tips at branch-cut.)

---

## 1. Purpose & scope

Build the CBOR wire codec — the one deferred piece of the message algebra — as a single
hand-written C seam compiled into **both** the host and target VMs, exposed to nsl, and
proven end-to-end on the differential (Path A vs Path B) harness. This is the "second
step's" wire seam: a node can serialise a message algebra value to canonical CBOR bytes
and reconstruct it, and generic CBOR tooling (`cbor2`, `cbor.me`) reads the traffic with
zero custom code.

The "Int64" half of the rung's name is **already shipped**: Milestone A built the
`±(2^64−1)` sign+magnitude `LargeInteger`, and the L7 ratification fixed `=`/hash. What
remains for integers here is (a) making the codec read/write that full range correctly,
and (b) retiring two stale documentation rules that predate the shipped model by one day.

### In scope

1. **CBOR value codec** — `cbor_encode` / `cbor_decode` C seam (`tuvm/src/cbor.c` + `.h`),
   recursive descent, depth-bounded, deterministic, closed-algebra, total decode.
2. **Integer reconciliation** — the codec's integer path implements the ratified
   `±(2^64−1)` contract; the design corrects the stale `> 2^63−1 → error` rule in
   `cbor.md` and the stale `overflow past 2^63−1` line in `errors.md`.
3. **nsl surface** — `platform cborEncode:` and `platform cborDecode:ifInvalid:` prims,
   Kernel forwarders, exercised by a new `nsl-tests/14_cbor` both-path gate.
4. **Test corpus** — C golden-hex fixtures + rejection corpus + truncation fuzz-lite, with
   `cbor2` / `zcbor` used as independent external oracles.
5. **Co-scheduled (independent) fix** — the `.oimg` large-SMI-constant sign-corruption bug
   (findings `2026-07-04-oimg-large-smi-constant-corruption.md`), §7.

### Out of scope (explicit)

- **Porta wire framing / length-prefix / transport.** This rung is the *value* codec only
  (`encode(msg) → bytes`, `decode(bytes) → msg`). The porta-wire step is Phase 3.
- **Wrapping / masking arithmetic surface** (L7's deferred "option 3"). Tracked, not built
  — see §8. The concrete adjacent bug is tuvm#4 (bitops silently return `TAG_SMI(0)` on
  `LargeInteger` operands), which makes L7's "masking works today" false for large values.
- **`zcbor` in the shipping path.** Used only as a test oracle (§6), never compiled into a
  VM. Rationale in §2.

---

## 2. Approach: one hand-written codec, both platforms

**Decision:** a single hand-written recursive-descent codec (~1–2 KB) in `tuvm/src/cbor.c`,
compiled into both the host and the Zephyr-target VM. `zcbor` (vendored in the Zephyr build
tree) and python `cbor2` serve as independent test oracles only.

**Why not `zcbor` in the shipping path:**

- Our messages are a *dynamic recursive algebra* (arbitrary nested list/map/record/symbol
  with tags 27/39), not a fixed CDDL schema — so `zcbor`'s codegen path does not apply; we
  would call its low-level primitives and **hand-write the recursive walk regardless**.
  `zcbor` would replace only the leaf varint byte-twiddling (the easy part) while adding
  its own runtime object code. The ~1–2 KB estimate already includes the walk; net ROM win
  is marginal-to-negative.
- The fidelity that matters is *ours*, not generic CBOR: deterministic canonical form
  (sorted keys, shortest form) and the closed-algebra decode rules (§4). `zcbor` provides
  none of these for free — they layer on top either way.
- **The determinism guarantee is the load-bearing reason.** The design rests on `encode(x)`
  being **byte-identical on host and target** (RFC 8949 §4.2). One source compiled for both
  platforms is identical *by construction*. Two codecs (zcbor on target, hand-rolled on
  host) would create two accept/reject surfaces to keep in perpetual lockstep; any
  divergence manifests **only on target** — precisely the failure mode host-as-CI and the
  frozen-oracle differential model exist to prevent. Byte-identity is what lets fixtures be
  hex literals, host be the oracle for target, and porta hash messages.

---

## 3. Seam shape (C)

```c
/* tuvm/src/cbor.h */
typedef struct { const char *msg; } cbor_err;   /* msg = errors.md vocabulary, or NULL */

size_t cbor_encode(oop msg, uint8_t *buf, size_t cap);   /* bytes written; 0 = overflow */
oop    cbor_decode(const uint8_t *buf, size_t len, cbor_err *e);  /* NULL on error, e set */
```

- **Encoder:** recursive descent with a depth counter (bound 32). Returns the byte count,
  or `0` if `cap` is too small — the caller grows and retries (§5). Writes canonical
  shortest-form, definite-length, sorted-key CBOR by construction.
- **Decoder:** allocates algebra values **bottom-up**, so decoded messages satisfy
  by-construction immutability with no special freeze path. Depth-counted identically to
  construction.
- **GC rooting (called out because it is the recurring hazard in this codebase):** every
  partially-built container (list-so-far, map-so-far, record tag/fields) and every
  child result must be GC-rooted across the *subsequent* child allocations inside
  `cbor_decode`. The implementation plan tasks this explicitly and the review must verify
  it on both backends, the way the pulse/exception rungs did.
- **Depth bound = 32** — the same bound `messages.md` enforces at construction. A
  hostile/buggy peer therefore cannot recurse the node's C stack (worst-case stack use is
  small and static, fits the 2–8 KB Zephyr stack analysis). Error: `message depth exceeded`.

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
| Map | major 5, definite length, keys sorted (§4.2) | major 5 |
| Record | **tag 27** (language-object) + `[tagSymbol, fieldsMap]` | tag 27 + 2-array |

Tags 27 and 39 are IANA-registered with exactly these meanings, so `cbor2` / `cbor.me`
render porta traffic readably. **No float ↔ int coercion** in either direction (`1.0 ≠ 1`
in the algebra; encode must not collapse them).

### 4.1 Integer reconciliation (the "Int64" half)

The codec's integer path implements the **ratified `±(2^64−1)` contract**, superseding two
stale docs written one day earlier.

- **Encode:** read the value through **both** representations — SMI *and* `LargeInteger` —
  keyed on the numeric **value**, not the representation. Consequence: `2^40` (an SMI on
  64-bit host, a `LargeInteger` on 32-bit target) encodes to **byte-identical CBOR on both
  platforms**. Shortest-form always.
- **Range mapping:** the full `−(2^64−1) … +(2^64−1)` fits standard CBOR — **major 0**
  carries `0 … 2^64−1`, **major 1** carries `−1 … −2^64`. Every value we hold produces
  valid, shortest-form CBOR that any 64-bit peer or generic tool decodes as the exact
  integer. Wire-compatible in both directions.
- **Decode rejects**, with the distinct error **`integer out of range`**, the two cases
  that are legal-CBOR integers unrepresentable for us: `−2^64` (major 1 with argument
  `2^64−1`, one below our floor), and any CBOR **bignum** (tags 2/3 — a legal but
  too-wide integer encoding). David's ruling: a distinct message, not the umbrella
  `not a value`, because these are legal CBOR from a 64-bit peer rather than garbage, and
  a distinct message aids debugging. (Tags 2/3 are handled as the integer-range case, not
  as generic unknown tags.)
- **Doc corrections this rung lands:**
  - `tuvm/review/lib/cbor.md` — replace the mapping row's `value > 2^63−1 → error` with the
    `±(2^64−1)` rule + `integer out of range` on `−2^64`/bignum.
  - `tuvm/review/lib/errors.md` — line ~59 `integer overflow past 2^63−1` is stale; the VM
    traps at `±(2^64−1)` with `LargeInteger overflow (magnitude exceeds 2^64-1)`. Correct
    the row and add the decode-side `integer out of range` entry.

### 4.2 Deterministic encoding (RFC 8949 §4.2)

Shortest-form integers, **definite lengths only**, **map keys sorted bytewise** by their
encoded key bytes. ⇒ `encode(x)` is byte-identical everywhere ⇒ fixtures are hex literals
and porta can hash messages. This is a per-rule test (§6).

---

## 5. Decode is total and closed

`cbor_decode` is **total**: any byte sequence yields either one valid algebra message or
exactly one error — never a partial parse, never pass-through. Rejections (each a test):

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
`message depth exceeded`, `invalid utf-8`, and `integer out of range` are the three
distinct sub-cases carried through for diagnosability.

---

## 6. nsl surface & prims

- `platform cborEncode: msg` → `Bytes` (frozen `RawBytes` leaf).
- `platform cborDecode: bytes ifInvalid: [ :err | … ]` → the reconstructed message, or the
  block's value on any decode error (block receives the error text per errors.md
  `…ifInvalid:` convention).
- **Prim registration (append-only):** last prim is `PRIM_pf_pumpEvents = 96`
  (`prim_ids.h:98`). New: `PRIM_pf_cborEncode = 97`, `PRIM_pf_cborDecode = 98` — enum +
  `PRIM_NAMES` + `prim_ids.h` mirror, `builtins.def` untouched. Two `platform_dispatch`
  cases (switch on `pid`, mirroring the gpio prims). The plan re-counts append-only ids and
  the review re-verifies mechanically (as every prior prim rung did).
- **Encode buffer strategy:** `cbor_encode` returns `0` on overflow. The `cborEncode:` prim
  starts at a modest cap, grows-and-retries until the message fits, then builds a frozen
  `RawBytes` of exactly the written length (`basicNewBytes:` + `freezeBytes` idiom).
- **Kernel forwarders:** two, mirroring the existing `platform`-facility forwarders (e.g.
  the pulse/timer accessors), with no nsl-side state (C owns the codec).

---

## 7. Co-scheduled: `.oimg` large-SMI-constant fix (independent)

Bounded and mechanically independent of the codec; co-scheduled because it is the one
integer-wire-fidelity gap and, once fixed, lets the CBOR fixtures include the corrupting
band as a regression rather than tiptoeing around it. Root cause (findings doc): an
off-by-one in the oimg SMI ref-word tag-capacity bound.

- `tools/oimg_serialize.py` `_smi()` — bound magnitude by `1 << (self.oop*8 − 3)`
  (`< 2^61` on 64-bit, `< 2^29` on 32-bit) and **keep the `raise` as a hard guard** (never
  silently corrupt).
- `tools/ns_compile_oevm.py:813` — lower the `SMI:` threshold to the ref-word's true
  capacity (ideally derived from the target `oop` width, not a fixed `1<<62`) so wider
  literals emit `LARGEINT:` (which already round-trips correctly on both paths).
- Add a corpus test with an integer **literal in `[2^61, 2^62)`** exercised on both paths
  (A==B), proving the sign no longer corrupts.

This is a separate work-item in the plan, not entangled with the codec tasks.

---

## 8. Deferred, tracked

- **Wrapping / masking arithmetic surface** (L7 option 3) — not built this rung. At close,
  file a tracking issue (or annotate tuvm#4) so the deferral is recorded, not lost. The
  concrete substance is **tuvm#4**: the integer bitop family lacks a `LargeInteger` path
  (silent `TAG_SMI(0)` on boxed operands), which makes L7's "masking works today
  (`bitAnd:`)" false for exactly the large values that land on the heap path. Compute-path
  bug, orthogonal to the wire codec — does not block this rung.

---

## 9. Test plan

### C unit tests (`tuvm/tests`, run_c_tests harness)

- **Golden hex fixtures** authored with python `cbor2` at fixture-authoring time, stored as
  hex, for each scalar type and container.
- **Integer edges:** ±SMI boundary, `±(2^64−1)` (round-trip), `−2^64` (**reject →
  `integer out of range`**), CBOR bignum (**reject**), and — now that §7 lands — a literal
  in the `[2^61, 2^62)` band.
- **Float:** f32-exact vs f64-only value (shortest-form selection); prove no int coercion
  (`1.0` encodes as major 7, not major 0).
- **Symbol / Record** round-tripped through `cbor2` to prove tag 39 / tag 27 interop.
- **Deterministic-order map:** unsorted-input keys ⇒ sorted-output bytes (byte-identical).
- **Rejection corpus:** indefinite-length items, unknown tag, depth 33, duplicate keys.
- **Truncation fuzz-lite:** every prefix length of one golden message decodes to an error,
  never a crash.
- **External oracle cross-check:** feed our encoder output to `cbor2` (and optionally
  `zcbor`) and assert it decodes to the expected value — "interop tested hard" without a
  second production codec.

### nsl-tests (`14_cbor`, differential both-path)

- Round-trip a representative message (nested record with a symbol-keyed map, a large
  integer, a string, a bytes, a float, nil/bool) through `platform cborEncode:` then
  `platform cborDecode:ifInvalid:`, assert structural `=` to the original — **A==B**.
- `cborDecode:ifInvalid:` on a corrupt buffer runs the block (non-tautological: assert the
  block's value, not just absence of crash).

### Gates

- Whole-suite both-path gate green (controller-owned); `14_cbor` PASS; the four named gates
  (grant / debugger-integration / portability-32 / boot-gc-asan) PASS. `cbor.c` added to
  every VM-build source list (the recurring harness-linkage gap — enumerate all sites:
  `tools/run_tests_oevm.py` build lists, `test_supervisor_grant.py`,
  `test_debugger_integration.py`, `tests/test_portability_32.sh`, `bench.sh`,
  `tests/test_load_run.sh`, `tests/test_oimg_blob.sh`, and the mcu `footprint.sh`).

---

## 10. Risks & mitigations

- **GC rooting during bottom-up decode** — the known-hard part. Mitigation: explicit
  per-allocation rooting, tasked in the plan, reviewed on both backends.
- **Host/target byte-identity** — guaranteed by one-source-both-platforms + value-keyed
  (not representation-keyed) integer/float encoding. The deterministic-order + int-edge
  fixtures are the proof.
- **Harness linkage** — `cbor.c` must be added to *every* VM-build list (≥8 sites, per prior
  rungs' repeated finding). Enumerated in §9 gates; final review greps for completeness.
- **Prim-id drift** — append-only 97/98, `prim_ids.h` mirror; re-counted in plan, mechanically
  re-verified in review.

---

## 11. Deliverables

- `tuvm/src/cbor.c`, `tuvm/src/cbor.h`; prims 97/98 in `dispatch.c` + `platform.c` +
  `prim_ids.h`; Kernel forwarders in `nsl-tuvm` kernel.
- `tuvm/tests` C unit tests + fixtures; `nsl-tests/14_cbor` both-path suite.
- `.oimg` fix in `tools/oimg_serialize.py` + `tools/ns_compile_oevm.py` + band regression
  test.
- Doc updates: `cbor.md` (implemented; int rule corrected), `errors.md` (int rows
  corrected + `integer out of range`), `program-sequence.md` rung-3 marked done at close,
  and a `MILESTONE`/rung close note.
- Tracking issue for the deferred wrapping surface (§8).
