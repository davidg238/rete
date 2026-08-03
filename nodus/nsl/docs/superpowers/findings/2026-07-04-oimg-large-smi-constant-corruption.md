# FINDING (2026-07-04): oimg backend corrupts large SMI integer constants

**Status:** diagnosed, NOT fixed. Ready to file as a GitHub issue on `nsl-tuvm` once
David approves (issue-creation is reserved for his word). Found while doing the
Milestone-B pre-work (https://github.com/davidg238/nsl-tuvm/issues/2, L7 integer decision). Separate from — and not a
regression of — the LargeInteger `=` fix on branch `milestone-b-prework`.

## Summary
Integer **literal constants** whose magnitude is in `[2^61, 2^62)` are loaded with the
**wrong sign** on the `.oimg` backend (Path B), while the solidified backend (Path A) is
correct. On the 32-bit target the exposed range is far wider (`[2^30, 2^62)`), because the
compiler's SMI/LargeInteger threshold is a fixed host-width constant.

## Repro (host, 64-bit)
```newspeak
(2305843009213693952 printString) println.                     "2^61"
((2305843009213693952 + 2305843009213693952) printString) println.
```
- Path A (`solidified_image.c` → `oevm_runner`): `2305843009213693952`, `4611686018427387904` ✓
- Path B (`--format=oimg` → `oevm_img`): `-2305843009213693952`, `-4611686018427387904` ✗

`u32 × u32` (`4294967295 * 4294967295 = 18446744065119617025`), `1e12`, and LargeInteger
**literals** (`≥ 2^62`) all round-trip correctly on both paths — only large **SMI
constants** corrupt. (The corpus test `06_limit_tests/06_largeint_dict_keys.ns` is
deliberately written to avoid this range so it stays green on both paths.)

## Root cause
The oimg SMI ref-word uses **2 tag bits**: `RefWord.smi(v) = (v << 2) | TAG_SMI` in an
`oop`-width word (`tools/oimg_serialize.py:31`). A signed value therefore has capacity
`|v| < 2^(oop*8-3)` = `2^61` on a 64-bit oop. But:

1. `tools/oimg_serialize.py:222-226` `_smi()` bounds `v` by `2^(oop*8-2)` = `2^62`, one bit
   too lenient — so `v = 2^61` passes the guard, then `(2^61 << 2) = 2^63` overflows onto
   the sign bit; the C loader's arithmetic `>> 2` sign-extends it to `-2^61`. (The guard
   should `raise`, not silently corrupt.)
2. `tools/ns_compile_oevm.py:813` emits `SMI:` for `-(1<<62) <= ival <= (1<<62)-1` and
   `LARGEINT:` otherwise — the same off-by-one, and it's a fixed host-width bound
   regardless of target `oop`, so a 32-bit-target image would mislabel every literal in
   `[2^30, 2^62)` as SMI.

## Suggested fix
- `oimg_serialize.py:_smi()`: bound by `1 << (self.oop*8 - 3)` (magnitude `< 2^61` on
  64-bit, `< 2^29` on 32-bit) and keep the `raise` as a hard guard.
- `ns_compile_oevm.py:813`: lower the SMI threshold to match the ref-word's true capacity
  (ideally derived from the target `oop` width, not a fixed `1<<62`), so wider literals
  become `LARGEINT` (which round-trips correctly).
- Add a corpus test covering a literal in the corrupting band on both paths.

## Severity
Silent numeric corruption of integer literal constants on the shipped image path, and
host/target divergence in exactly the "develop-on-host, deploy-.oimg-to-target" workflow
L7 protects. Not currently hit by the corpus (no existing test uses a literal in the
band), but a live hazard for Milestone B message/int work.
