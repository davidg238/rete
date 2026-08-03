# L7 — Integer semantics across 32/64-bit platforms

**Status:** DECIDED 2026-07-04 (ratified by David). Supersedes the pre-code recommendation
in `tuvm/review/lib/integers.md` (2026-07-03). This is VM design, not library code.

**Why this supersedes the recommendation:** `integers.md` was written *before* Milestone A
was built. It proposed strict signed-64 semantics (range −2^63 … 2^63−1, **trap** past
2^63−1, an 8-byte `Int64` box). Milestone A actually shipped a wider and — for this
language's domain — better model: a fixed-width sign+magnitude bignum with range
**±(2^64−1)**. This document ratifies what shipped, keeps L7's real invariants, and
records why the pre-code proposal was *not* adopted. The integer that the Milestone-B
message algebra and CBOR codec carry is the integer defined here.

## The contract (language-level, platform-independent)

- **One integer type.** Its value range is **−(2^64−1) … +(2^64−1)**, identical on host
  (x86-64) and target (Cortex-M4). A program observes the same integer behaviour on both.
- **Overflow traps, never wraps.** An arithmetic result whose magnitude would exceed
  2^64−1 aborts the VM with `LargeInteger overflow (magnitude exceeds 2^64-1)` (see
  errors.md). Silent wrap-around never happens.
- **Fixed width, not arbitrary precision.** There are no growing bignums. The type is a
  deliberately bounded ±(2^64−1) integer; past the bound is a trap, not a promotion.
- **Wrapping arithmetic, when wanted** (checksums, ring counters), is explicit library
  surface decided later. Masking works today: `x bitAnd: 16rFFFFFFFF`.

## Representation (canonical, per platform)

Every integer value has **one canonical representation on a given platform**:

| Value fits the platform's tagged range? | Representation |
|---|---|
| yes | **SmallInteger (SMI)** — tagged, ~62 bits on 64-bit host, ~30 bits on 32-bit target |
| no | **LargeInteger** — heap object: `sign` (±1) + `mag` (`uint64`), a leaf object (no pointers) |

- Arithmetic **always normalises**: a result that fits the SMI range *is* an SMI
  (`sm_to_int` demotes; `dispatch.c`), a result that does not *is* a LargeInteger.
  Consequently, **the SMI and LargeInteger value-sets are disjoint on any single
  platform** — no value has two representations at once. This is the key invariant the
  equality/hash contract rests on (below).
- The SMI boundary is `sizeof(intptr_t)`-derived, so it adapts to the platform
  automatically (≈2^62 host, ≈2^30 target). The *language* range ±(2^64−1) does not.

## Equality & hash (the subtle part, stated precisely)

The requirement is the usual one: **equal values must be equal (`=`) and must hash
equally**, or Dictionary keys break the moment a value crosses the SMI boundary.

How it is upheld is worth recording, because the two representations use *different* hash
formulas and yet the contract holds:

- `=` / ordered compare (`prim_compare`) compares integers **exactly via sign+magnitude**,
  SMI and LargeInteger alike, never round-tripped through `double` (which would lose
  precision above 2^53 and mis-order on soft-`double` targets like nRF52840).
- Hash formulas differ by representation: an **SMI hashes to itself**; a **LargeInteger
  hashes to the low 30 bits of its magnitude** (sign ignored, `dispatch.c` `PRIM_hash`).
- These two formulas never disagree for the *same value* **because canonical
  representation makes their domains disjoint** — within one platform a value is either an
  SMI or a LargeInteger, never both, so no equal pair ever straddles the two formulas.
  The contract is upheld by *representation canonicalisation*, not by making the formulas
  agree.
- Note the LargeInteger hash **ignores sign**, so `+X` and `−X` collide into one bucket.
  That is correct (collisions are permitted; `=` disambiguates), and is exercised
  deliberately by the corpus test below.

**Fix applied during ratification:** the shipped `=` primitive (`PRIM_eq`) did value
equality only for `SMI`/`Float`; two distinct LargeInteger boxes of equal value fell
through to *identity* and compared unequal — so `<`/`>` (which already compared by value)
disagreed with `=`, and LargeInteger Dictionary keys silently failed. `dispatch.c` now
routes any integer/integer pair through exact sign+magnitude comparison (`prim_compare`),
matching the reference VM. Staged on branch `milestone-b-prework`, verified by the corpus
test below on both backends. The existing `04` test never caught this because it only
prints large integers, never compares them.

**Known adjacent bug (not this contract):** the `.oimg` backend corrupts the sign of large
*SMI constants* in `[2^61, 2^62)` — see
`docs/superpowers/findings/2026-07-04-oimg-large-smi-constant-corruption.md`. It does not
affect LargeInteger values, so it does not touch this contract; the corpus test below is
written to stay clear of the corrupting band.

**Residual cross-platform note:** a *literal* like `2^40` is an SMI on host but a
LargeInteger on target. Hashes are only required to be consistent within a single run, so
this is not a defect — but it means the LargeInteger-keyed path is the one that only fully
exercises **on target**. Host CI covers the LargeInteger path using values above the host
SMI boundary (≈2^62); the on-target suite must re-cover it at the 2^30 boundary.

## CBOR mapping (feeds the Milestone-B codec)

The ±(2^64−1) range maps cleanly onto CBOR's two integer major types: **major type 0
(unsigned)** carries 0 … 2^64−1, **major type 1 (negative)** carries −1 … −2^64. The
language integer fits inside both half-ranges (it stops one short of −2^64). Printing,
`printString`, and the CBOR codec all read through both SMI and LargeInteger
representations.

## Why (recorded so it is not re-litigated)

Driver code needs u32 register values and millisecond timestamps; a 30-bit target SMI
cannot hold them, and — critically — a **u32 × u32 product reaches ≈2^64**. If integer
width were platform-dependent, programs would pass on host and break on target, destroying
the develop-on-host workflow, the frozen-oracle differential testing (T0.2), and
host-as-CI. The shipped ±(2^64−1) model holds u32×u32 without trapping, honours L7's real
invariants (platform-independent width, trap-not-wrap, fixed width), and maps onto CBOR's
integer major types. The cost is one boxed leaf type and an overflow check the SMI fast
path already mostly pays.

**On-target performance caveat (feed the G3 GC decision, https://github.com/davidg238/nsl-tuvm/issues/3):** on the 32-bit
target the SMI boundary is ≈2^30, so the *motivating* values — u32 registers with the high
bit set, ms-timestamps past ~12 days of uptime, any u32×u32 — fall onto the LargeInteger
(heap) path routinely, not rarely. The GC/allocation strategy must make short-lived
LargeInteger boxes cheap (nursery / bump-pointer, or small-box reuse). This is a real
tension between the motivation and the "boxed for the rare top band" framing, and it
belongs in the G3 GC-strategy decision.

## Rejected alternatives

- **Strict signed-64 with a trap at 2^63−1 (the pre-code `integers.md` proposal).**
  Rejected: it **traps `u32 × u32`** (result up to ≈2^64, well past 2^63−1) — the exact
  register math it claimed to serve. Narrower range, no gain.
- **8-byte `Int64` two's-complement box (the pre-code proposal's representation).**
  Rejected: reshaping the shipped sign+magnitude `LargeInteger` is real VM work that
  re-freezes the differential-testing oracle for no behavioural benefit; sign+magnitude
  also maps more naturally onto CBOR's unsigned/negative major-type split.
- **`int` = platform SMI width** (platform-dependent semantics). Rejected: destroys
  develop-on-host, frozen-oracle differential testing, and host-as-CI.
- **Arbitrary-precision bignums.** Rejected: flash + complexity with no embedded use case;
  a bounded ±(2^64−1) integer with a trap is enough and keeps the box a fixed-size leaf.
- **Wrap-by-default.** Rejected: silent corruption in the exact domain — register math —
  this language targets. Wrapping is opt-in library surface (`bitAnd:`).

## Test hooks

- **Overflow boundary** — `nsl-tests/06_limit_tests/04_integer_overflow.ns` (already in the
  corpus) pins that values up to ±(2^64−1) are represented exactly and print identically:
  `9223372036854775807 + 1` → `9223372036854775808`,
  `9223372036854775807 * 2` → `18446744073709551614`. **Kept as-is** — it already encodes
  this ratified contract. (Under the rejected signed-64 proposal it would have had to be
  rewritten to expect a trap.)
- **Trap past ±(2^64−1)** — a product whose magnitude exceeds 2^64−1 aborts with the frozen
  `LargeInteger overflow (magnitude exceeds 2^64-1)` string. On-target suite hook.
- **`=` / hash across representations** —
  `nsl-tests/06_limit_tests/06_largeint_dict_keys.ns` (added with this decision):
  LargeInteger values as Dictionary keys, equal values from different construction paths
  hitting the same slot, and the sign-collision-but-distinct-by-`=` case.
- **On-target (future suite):** re-cover the LargeInteger-keyed Dictionary path at the
  2^30 boundary, where the target actually boxes.
