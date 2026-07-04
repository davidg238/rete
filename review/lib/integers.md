# L7 — Integer semantics across 32/64-bit platforms (VM design)

> **SUPERSEDED 2026-07-04 → `nsl-tuvm/docs/superpowers/specs/2026-07-04-L7-integer-semantics-decision.md`.**
> This file is the pre-code *recommendation* (signed-64 + trap-at-2^63 + 8-byte `Int64`
> box). Milestone A shipped a different, wider model — a ±(2^64−1) sign+magnitude
> LargeInteger — which better serves the register-math domain (it holds `u32 × u32`,
> which strict signed-64 would trap). The ratified decision reconciles L7's real
> invariants (platform-independent width, trap-not-wrap, fixed width) to what shipped.
> Kept here only for provenance; do not treat this file as the contract.

**Status:** recommended 2026-07-03; must be **decided before Milestone B** (the message
algebra's `int` is this integer). This is VM work, not library code.

## The contract (language-level, platform-independent)

- One integer type, **signed 64-bit semantics on every platform**: values in
  −2^63 … 2^63−1 behave identically on host (x86-64) and target (Cortex-M4).
- Overflow past the 64-bit range **traps** (`integer overflow` crash per errors.md) —
  never silent wrap.
- Wrapping arithmetic, when wanted (checksums, ring counters), is explicit library
  surface decided later (`bitAnd: 16rFFFFFFFF` masks work today).
- Algebra `int` = this integer; CBOR major types 0/1 carry it exactly.

## Implementation shape

| Platform | Fast path | Fallback |
|---|---|---|
| 64-bit host | SMI (tagged, ~62 bits) | boxed `Int64` for the rare top band |
| 32-bit target | SMI (tagged, ~30 bits) | boxed `Int64` heap object (8-byte payload) |

- Boxed `Int64`: one heap object type — header + 8-byte little-endian payload. No
  bignums, no `Int32` twin, no unsigned twin. GC: leaf object (no pointers).
- Every arithmetic/comparison primitive: SMI×SMI fast path with overflow check →
  box on overflow of the *SMI* range; trap only past the *64-bit* range.
- `=`/`hash` must agree across representations (SMI 5 = boxed 5, same hash) — required
  or Dictionary keys break the moment a value crosses the SMI boundary on target.
- Printing, `printString`, and the CBOR codec read through both representations.

## Why (recorded so it is not re-litigated)

Driver code needs u32 register values and ms-timestamps; 30-bit target SMIs cannot hold
them. If integer width were platform-dependent, programs would pass on host and break on
target — destroying the develop-on-host workflow, the frozen-oracle differential testing
(T0.2), and host-as-CI. The cost is one boxed leaf type and an overflow check the SMI
fast path already mostly pays.

**Rejected:** "int = platform SMI width" (platform-dependent semantics — unacceptable,
above); bignums (flash + complexity, no embedded use case); wrap-by-default (silent
corruption in the exact domain — register math — this language targets).

## Test hooks (feed T-B and the future on-target suite)

- `16r7FFFFFFF + 1`, `16rFFFFFFFF` (u32 max), `1000000 * 1000000` — same printString on
  host and target.
- Dictionary with keys straddling the 30-bit boundary — lookups stable.
- `(2 raisedTo: 62) + huge` → `integer overflow` trap, both platforms.
- nsl-tests `06_limit_tests/04_integer_overflow.ns` exists in the corpus — re-point its
  expectations at this contract when landing.
