# nsl-tuvm — language sufficiency & library roadmap (proposed)

**Status:** proposed 2026-07-03 by external design review; parked in `review/`.
Nothing here is first-step scope except where marked; this is the post-Milestone-D
roadmap for making the language *sufficient for its user* — someone scripting Zephyr
drivers and coordination logic, not writing a VM.

**Build status (audited 2026-07-12) — this doc is a proposal; where it disagrees with
the code, the code has usually been ratified since. Check before re-litigating.**

| | | |
|---|---|---|
| L1 combinators | **SHIPPED** | landed 2026-07-12 — full set + `Interval`. One gap: the `1 to: n` sugar is not landable (integers are VM builtins with no reachable vtable), so ranges are built with `Interval from:to:[by:]` — tuvm#30 |
| L2 binary data | **PARTIAL** | 8/16-bit only; no u32/signed accessors, no `bitField:`; staged in `review/lib/bytes.ns` |
| L3 timers | **SHIPPED** | `Timers` in `kernel.ns`; real `k_timer` backend (rung 8) |
| L4 FSM | **SHIPPED** | `Protocol` in `kernel.ns`; `timeout:` sugar deliberately omitted — compose from `Timers` |
| L5 error convention | **SHIPPED** | plus `on:do:`/`ensure:` (Milestone E), which slot *above* it as L5.3 anticipated |
| L6 CBOR | **SHIPPED** | rung 3 — but built in nsl over 7 C leaf prims, the *inverse* of this doc's "codec in C" |
| L7 integers | **SHIPPED** | range **ratified wider** than proposed below — see L7 |
| L8 strings | **PARTIAL** | no `class String`; UTF-8 validity **not** enforced at construction; staged in `review/lib/strings.ns` |

Note `review/lib/` is a **staging** directory — `combinators.ns`, `bytes.ns` and
`strings.ns` are written but **not loaded by any harness**; only `fsm.ns` and `timers.ns`
have been landed into `nodus/nsl/lib/kernel.ns`.

**Sufficiency verdict.** For the coordinator role (jobs, channels, supervision, driver
facades), the language as specced is sufficient. The risks are (a) stdlib gaps that push
users back to C, and (b) three under-specified corners — integer width, string/bytes,
wire encoding — that get expensive if decided late. Those three are specified here.

---

## L1 — Combinators: the complete set (first-step adjacent; extend as Milestone A Task 2 touches them)

With NLR gone, combinators ARE the early-exit story; any gap here reads as a language
deficiency. The complete required set on sequenceable collections:

- Search/decide: `detect:ifNone:`, `anySatisfy:`, `allSatisfy:`, `count:`,
  `indexOf:ifAbsent:`
- Transform: `collect:`, `select:`, `reject:`, `with:collect:`, `copyFrom:to:`,
  `reverse`, `,` (concat)
- Reduce: `inject:into:`, `detectMax:`/`detectMin:`
- Iterate: `do:`, `do:separatedBy:`, `withIndexDo:`, `from:to:do:`
- Plus an `Interval` (`1 to: n` as a first-class collection) so numeric loops get the
  same protocol.

Rule: every combinator that can terminate early (`detect:`, `anySatisfy:`, …) must have a
pinned short-circuit test (test-suites-spec T-A.3).

## L2 — Binary data: the make-or-break library for the domain

Driver scripting is register masks and packet fields; this must be *pleasant*.

- **`ByteArray`** (mutable, NOT part of the message algebra) and **`Bytes`** (immutable,
  IS the algebra's `bytes`): build in a ByteArray, freeze to Bytes to send. Freeze is a
  copy (or a neuter, later — spec §8 already defers ownership transfer).
- Accessors on both, offset-addressed, explicit width and endianness:
  `u8At:`, `u16At:`/`u16At:put:` (+ `Be` variants; LE default), `u32At:`, `s8At:`,
  `s16At:`, `s32At:`, and `copyFrom:to:`. All bounds-checked; out-of-range is an error
  (crash), never truncation.
- Integer bit operations: `bitAnd:`, `bitOr:`, `bitXor:`, `<<`, `>>` (logical),
  `bitAt:`, `bitField:from:to:` (extract) / `bitField:from:to:put:` (insert). The last
  two remove the mask-and-shift boilerplate that dominates register code.
- Later (Zephyr step): a declarative register/bitfield description idiom
  (`Register at: 0x40 field: #mode bits: (4 to: 6)`) built on the above — library, not
  language.

## L3 — Timers as channel events (Milestone D adjacent)

`after: ms post: aMessage on: channel` and `every: ms post:on:` — a C-seam timer source
posting into the space. No new semantics: a timer is just an event source. Covers most
"wait then act" cases without any suspension. Cancellation returns/consumes a token
(itself a small capability).

## L4 — State-machine idiom: the fiber replacement, treated as first-class

Ship a tiny FSM library **with the first driver examples, not after**. Shape:

```
machine := Protocol
  state: #idle    on: #start   do: [:m :e | ...begin conversion...] then: #converting;
  state: #converting on: #tick do: [:m :e | ...read result...]      then: #idle;
  timeout: #converting after: 50 then: #idle.
whenever: sensorChannel do: [:e | machine handle: e].
```

One object per protocol; events in, actions out; current state explicit and inspectable
(and — the deep-sleep payoff — trivially persistable across sleep). This is addendum A3's
sanctioned mechanism #1. Getting this idiom right is worth more UX than any VM feature.

## L5 — Error convention (decide NOW, before the stdlib grows)

With `on:do:` deferred (spec §8), the stdlib needs one convention immediately, or it
grows ad-hoc nil-returns:

1. **Expected absence/failure** takes an explicit continuation keyword:
   `at:ifAbsent:`, `detect:ifNone:`, `take:` → nil (pinned), `decode:ifInvalid:`.
   No sentinel returns without an `if…:` variant.
2. **Everything else crashes** — let-it-crash; the supervisor is the handler. Stdlib code
   never "recovers" internally.
3. When full exceptions land (§8), they slot in *above* this convention (supervisor-level
   and coarse job-level `on:do:`), not as a replacement for `ifAbsent:`-style APIs.

This is Smalltalk's best idea about errors; codifying it keeps exceptions rare and the
first-step semantics honest.

## L6 — Wire encoding: adopt CBOR (RFC 8949), don't invent a TISON-analog

The message algebra maps onto CBOR nearly one-to-one; a custom format buys nothing.

| algebra | CBOR |
|---|---|
| null / bool | simple values (0xf6 / 0xf4-f5) |
| int | major 0/1 |
| float | major 7 (encode shortest width that round-trips; decode any) |
| string | major 3 (UTF-8, definite length) |
| bytes | major 2 (definite length) |
| symbol | **tag 39** ("identifier", registered) wrapping a text string |
| list | major 4 (definite length) |
| map | major 5 (definite length) |
| record | **tag 27** ("language-object": [type-name, args]) — array of [record-name, field-map] |

Rules: deterministic encoding per RFC 8949 §4.2 (canonical map key order — enables
byte-comparison of encodings and reproducible test fixtures); definite lengths only;
depth bound enforced at **decode** as well as construction (a hostile peer must not
overflow the node); unknown tags/types are a decode error (closed algebra — reject,
don't pass through).

Payoffs: OpenThread's application layer (CoAP) is CBOR-native; off-the-shelf decoders
(`cbor.me`, python `cbor2`) debug porta traffic for free; porta's server side needs no
custom codec. Encoder+decoder in C at the seam (spec §2 already places the codec in C),
~1–2 KB code.

## L7 — Integers on 32- vs 64-bit platforms — **RATIFIED & SHIPPED** (Milestone B; range widened at rung 3, 2026-07-10)

The platforms diverge: host (x86-64) SMIs carry ~62/63 bits; nRF52840 (Cortex-M4, 32-bit
words) SMIs carry ~31. Driver code routinely needs unsigned 32-bit values (register
reads, `u32At:`), which do **not** fit a 31-bit SMI. Leaving this implicit means programs
that pass on host silently break on target — destroying the develop-on-host workflow and
the differential-oracle testing strategy.

**Decision (as built): integer semantics are platform-independent, with a boxed
fallback.**
- 64-bit host: SMI covers the full range in practice; no boxing.
- 32-bit target: SMI fast path (±2^30) + a boxed heap `LargeInteger` fallback, allocated
  only when a value exceeds SMI range. The SMI band is derived from `sizeof(intptr_t)`
  (`nodus/tuvm/src/dispatch.c`), so the *same* program boxes at ±2^30 on target and ±2^62 on
  host and computes the same answer. u32 register values and ms-since-boot timestamps fit
  without ceremony; the box is rare in real driver code (most values are small).
- **The ratified range is ±(2^64−1)** — sign + `uint64_t` magnitude (`struct LargeInteger`,
  `nodus/tuvm/src/oevm.h`), *not* signed-64. Overflow **traps (crash) at magnitude 2^64**, never
  wraps silently; only `−2^64` and the CBOR bignum tags (2/3) exceed it, both routing to
  `integer out of range`. Pinned by `nodus/tests/06_limit_tests/04_integer_overflow.ns` and
  by the rung-3 wire contract (`program-sequence.md` phase 2 item 3).
  *(This supersedes the original proposal of signed-64 semantics trapping at ±2^63. The
  wider band was ratified when CBOR landed: RFC 8949 major 0/1 carries the full unsigned
  64-bit range, and rejecting `2^63 … 2^64−1` at the wire would have made a conformant
  peer's encoding undecodable.)*
- Comparison is exact via sign+magnitude — deliberately **never** round-tripped through
  `double`, because the nRF52840 is soft-float (`dispatch.c`).
- Wrapping, when wanted (checksums, counters), stays explicit: `+w`, `bitAnd: 16rFFFFFFFF`,
  or a `Word32` view — library surface, still undecided (see L2).
- The message algebra's `int` = this integer, everywhere; CBOR major 0/1 carries it exactly.

Rejected alternative (documented so it isn't re-proposed): "int = platform SMI width."
Cheaper (no boxed type) but makes integer semantics platform-dependent — unacceptable
against the host-as-oracle test strategy, and 31 bits genuinely cannot hold a u32.

Cost, as paid: one boxed integer type on 32-bit targets (box, GC case, arithmetic
fallback). It is the price of "test on host, trust on target".

## L8 — Strings: minimal, UTF-8, firmly separated from bytes

- `String` = immutable, valid UTF-8 by construction. Part of the message algebra.
- Operations: `size` (bytes), `=`/`<` (byte-wise), `,` (concat), `copyFrom:to:`
  (byte-indexed, must not split a code point — error if it would), `asBytes`,
  code-point iteration (`codePointsDo:`); `format:`-style interpolation for logging.
- **No** case tables, normalization, collation, or grapheme segmentation — flash-budget
  poison; a node does not need them. Text *processing* happens on the porta side.
- `Bytes asString` validates UTF-8 (`asStringIfInvalid:` per L5); `String asBytes` is
  free (a view or copy). Binary protocol work lives entirely in L2's types — a string is
  for humans and logs, never a byte-buffer.

## Priority order

1. L5 (error convention) + L7 (integer semantics) — decisions, near-zero code, everything
   downstream depends on them; L7 gates Milestone B's `int`.
2. L1 (combinators) — completes the no-NLR story; partially exists.
3. L2 (binary data) — the domain-defining library.
4. L3 (timers) + L4 (FSM idiom) — the no-fibers ergonomic answer; land with Milestone D
   examples.
5. L6 (CBOR) — when porta wire integration un-defers (spec §8); the *decision* is now,
   the code is later.
6. L8 (strings) — formalize when the stdlib port reaches String; the constraint list
   above is the spec.
