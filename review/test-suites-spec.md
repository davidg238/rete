# nsl-tests — verification suites for the first step (proposed)

**Status:** proposed 2026-07-03 by external design review; parked in `review/` until
David approves and Milestone A lands. Intended home: `nodus/tests/` (behavioral suites) and
`nodus/tuvm/tools/` (harness gates).

**Purpose.** The project owner is not auditing the VM C code. Trust therefore comes from
**black-box behavioral tests that encode the spec's claims**, plus tooling that catches
the bug classes invisible at the nsl level. Every §7 success criterion in the first-step
spec must map to named tests below (the *claims manifest*, T0.4). A criterion without a
green named test is unproven, full stop.

---

## T0 — Cross-cutting gates (highest value; independent of milestones)

**T0.1 Sanitizer gate.** The full conformance suite runs under two extra host build
configs: ASAN(+LSAN) and UBSAN. Any sanitizer report is a suite failure. This is the
single cheapest defense against the failure class (memory corruption, UB) that
nsl-level tests cannot see. Wire into `run_tests_oevm.py` as `OEVM_BUILD=asan|ubsan`
modes; run at minimum before every task-final review.

**T0.2 Differential oracle.** The frozen originals (`~/workspace/oevm`,
`~/workspace/ns-oevm`) still run the pared subset. A harness mode runs every shared test
on both the frozen VM and the seeded `tuvm`, and diffs stdout. Divergence = the deletion
changed semantics; the diff is the review artifact. (Tests that exercise deleted features
are excluded by the pared-subset manifest, which already exists.)

**T0.3 Ratchet tests.** `footprint.sh` compares against the banked baseline and FAILS if:
- engine **text** grows beyond baseline + 2%;
- **RAM** (data + bss + configured heap, under the MCU-shaped config of addendum A2)
  grows beyond its banked baseline + 2%.
Deliberate growth requires re-banking the baseline in the same commit, with rationale.

**T0.4 Claims manifest.** `review/tests/CLAIMS.md`: a table mapping each first-step spec §7
bullet to the test files that prove it. Reviewed at each milestone gate; "all named tests
green" is the definition of proven.

## T-A — Milestone A: the deletion changed nothing but the deleted

- **A.1 NLR rejection diagnostics.** `^` inside an escaping (stored/passed) closure is a
  compile error carrying the correct source line and a message naming the rule. Cases:
  block assigned to a variable; block passed to a user-defined (non-inlined) selector;
  nested block two levels deep. Test asserts on the diagnostic text/line, not merely
  "compile failed" — the error message is UI.
- **A.2 `^` in inlined blocks still works.** `^` inside `ifTrue:`/`whileTrue:` arms
  returns from the enclosing method with the right value (the sanctioned local-return
  semantics), including from a nested `ifTrue:` inside a `whileTrue:`.
- **A.3 Combinator short-circuit proof.** `detect:ifNone:` / `anySatisfy:` /
  `allSatisfy:` over a 100-element collection with a side-effect counter in the block:
  assert the counter equals the match index + 1 (iteration truly stopped), the result is
  correct, and the `ifNone:` arm fires on no-match. These combinators are the no-NLR
  early-exit story; their semantics must be pinned, not assumed.
- **A.4 Stack exhaustion fails safe.** A self-recursive method driven thousands of frames
  deep produces a clean language-level error (and, once Milestone D exists, a contained
  handler crash) — never a C-level crash. Run under ASAN (T0.1) to prove no out-of-bounds
  on the fixed-cap stack boundary. This is the test that justifies keeping the flat loop.
- **A.5 Pared-subset coverage audit.** For each test excluded by the pared manifest,
  record in the manifest whether its *functionality* (not its mechanism) is: (a) deleted
  with the feature — fine; or (b) still-meaningful behavior that must be re-expressed
  combinator-style. Every (b) gets a replacement test. Prevents coverage silently
  narrowing exactly where the dialect changed.

## T-B — Milestone B: the message algebra is airtight

- **B.1 Depth-bound edges.** A nested message at exactly the depth bound constructs; one
  level deeper is rejected with a clean error. Both list-nesting and map-nesting paths.
- **B.2 Total mutation rejection.** For **each** algebra type (list, map, record, string,
  bytes): every mutating selector in the image is rejected. Enumerate mechanically if
  mirrors allow (walk the vtable), else maintain an explicit selector list per type in
  the test. Include the reflective path: `perform:` of a mutating selector must also be
  rejected, not bypass the check.
- **B.3 Aliasing immutability.** Build a message, hold two references, exercise every
  read path on one while attempting mutation via the other (expected: rejection), then
  verify structural equality and identity of shared substructure — shared-by-reference,
  unchanged.
- **B.4 Cycle impossibility.** Attempt to construct a cyclic value through every
  construction API (list-of-self, map-containing-self, record field). Since construction
  is bottom-up over immutable values this should be *unrepresentable*; the test documents
  and pins that.
- **B.5 Generative round-trip.** A small deterministic pseudo-random generator (seeded,
  in-image) builds ~1000 random message trees within the depth bound; each is traversed
  and structurally compared to an independently-built copy. Later (wire codec step) the
  same generator feeds encode→decode round-trip testing — write it once, reuse forever.
- **B.6 Mixed mutable/immutable boundary.** A mutable object (a plain nsl object) is
  rejected at message-construction time — a message *containing* a mutable object must be
  unrepresentable, including nested deep inside an otherwise-valid tree.

## T-C — Milestone C: channels confine and the reactor is deterministic

- **C.1 Segment-aligned confinement (addendum A1).** Capability rooted at `sensor`
  cannot address `sensors/temp`; rooted at `a/b` addresses `a/b/c` but **not** `a/bc`;
  disjoint prefixes cannot cross. These are the exact collision cases; the obvious
  disjoint case alone is insufficient.
- **C.2 Canonicalization rejections.** Grants containing `a//b`, `./a`, `a/../b`, or a
  trailing separator are rejected at grant time (not silently normalized).
- **C.3 Attenuation monotonicity.** Attenuate `a` → `a/b` → `a/b/c`; the leaf capability
  fails on `a/b/other` and `a/x`. Attempting to *widen* (derive `a` from `a/b`) is
  impossible/rejected.
- **C.4 No ambient authority.** A job granted only one rooted capability cannot reach the
  space root, enumerate channels outside its prefix, or obtain a second capability except
  by supervisor grant. (The confinement claim of §4C, stated as a test.)
- **C.5 `take:` semantics.** `take:` on an empty channel returns nil immediately (never
  blocks); post-then-take returns the value exactly once (second take → nil).
- **C.6 Run-to-completion ordering.** A `whenever:` handler that posts to its own channel
  does **not** re-enter itself recursively; the new posting is dispatched only after the
  current firing completes. Multiple postings dispatch FIFO. This pins the reactor's core
  semantic promise — programs will be written against it.
- **C.7 Registration lifecycle.** A reaction registered during a firing takes effect for
  subsequent postings; posting to a channel with no reaction is a defined no-op (transient
  signals — pin that the signal is dropped, not queued).

## T-D — Milestone D: crashes are contained, the heap survives

- **D.1 Crash containment baseline.** N handlers on N channels; one crashes on every
  firing. The other N−1 accumulate correct counts across many rounds; the supervisor
  observes each crash; the node never exits.
- **D.2 Crash mid-allocation.** The crashing handler allocates in a tight loop (building
  nested messages) and then faults. After recovery, allocate heavily and run full GC
  cycles; verify heap coherence (allocations succeed, structures intact). Run under ASAN.
  This is the single test that proves the `setjmp` boundary and the GC agree — the
  likeliest place for a first-step VM bug to hide.
- **D.3 Crash at depth.** Handler faults inside deeply nested block invocations (and,
  separately, near the stack cap): unwind to the reactor is clean from any stack shape.
- **D.4 Restart policy stub.** Repeated crashes of one handler trigger the stub policy's
  observable behavior (log + restart; quarantine counter if implemented). Whatever the
  stub does, pin it.
- **D.5 Soak + leak plateau.** ≥100k events through several handlers with periodic
  deliberate crashes; sample heap usage each 10k. Usage must plateau (bounded variance
  after warm-up), not creep. This is the standing leak detector for the shared-heap
  tradeoff (a leaky job cannot be evicted by restart — creep must be caught in test, not
  in the field). Mark slow; run at milestone gates rather than every commit.
- **D.6 Event-source seam.** The host seam (timer/stdin) posting into the space is
  exercised end-to-end: `event → C seam → out: → reaction → out: → supervisor observes`
  (§4D's loop, as a scripted test with deterministic input).

## Execution notes

- Suites run **solo and foreground** (harness is not concurrency-safe — per the spec §7).
- Everything above is host-only, matching the first step. When the Zephyr target lands,
  T0.2/T0.3 carry over unchanged and the behavioral suites re-run on-target unmodified —
  that portability is itself the point of keeping tests black-box.
