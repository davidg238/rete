# Fast-path opcodes: OP_LT/LE/GT/GE + OP_AT/OP_AT_PUT — decision record

**Status:** implemented and device-proven on trial branches (tuvm `perf/op-cmp-at`
b9f893f, combined `perf/activation-plus-ops` 8e70ba5; paired `nsl-tuvm
perf/op-cmp-at`), 2026-07-17. **Not merged** — awaiting David's review. Results:
`~/workspace/tuvm/zephyr/docs/results.md` E6. Working journal:
`../superpowers/plans/2026-07-16-perf-trials-pushframe-opat.md`. Tracker: tuvm#37.

## Decision 1 — new opcodes reuse OP_SEND's layout and FALL INTO its handler

All six opcodes (0x19–0x1E) carry `a=argc, b=selector-const, c=receiver base reg`,
identical to OP_SEND. The VM handler tries the fast path; any miss does
`goto do_send` into the unmodified OP_SEND body. Consequences, and why this shape
was chosen over an OP_ADD-style dedicated encoding:

- **Zero duplicated semantics.** Float/LargeInteger/String comparisons, non-Array
  receivers, bounds errors — all take the exact code that ran before the opcode
  existed. The fallback isn't "equivalent"; it is the same instructions.
- The compiler diff is a selector→opcode map at the send-emission sites; the
  serializer is opcode-agnostic (1:1 word replacement, image sizes unchanged).
- The selector constant stays in the instruction, so the fallback needs no side
  table and disassembly stays readable.

## Decision 2 — `<  <=  >  >=` are lowered; `=` is NOT

The four ordering comparisons are numeric-only in practice and their SMI×SMI case
dominates (every inlined loop bound — the compiler emits a full send per iteration
otherwise, which E5 showed was the single hottest waste). `=` is object equality
with a broad polymorphic surface (strings, symbols, collections, user classes);
lowering it buys little (Dictionary probes are its hot user and mostly miss the
SMI×SMI case) and risks much. dictionaryWrite staying flat in E6 is the accepted
cost of this caution.

## Decision 3 — `basicAt:`/`basicAt:put:` are lowered; public `at:` is NOT

`at:` is polymorphic (Interval has SMI slot0 AND its own `at:` — a shape-gated
public `at:` fast path would silently corrupt it). `basicAt:` already runs on a
documented emitter-discipline + shape gate (`dispatch.c` PRIM_ix_basicAt), so
OP_AT with the same gate adds zero new risk. The discipline is **widened** from
"kernel Array methods on self" to "kernel collection internals on their
Array-typed slots": OrderedCollection `do:/at:/at:put:/add:` now index `contents`
via `basicAt:`, deleting the Array>>at: method frame per element (the −62% on
E6's ocDo). The dispatch.c comment records the widened discipline.

## Decision 4 — activation-path trims are semantics-free and separable

`perf/activation-trim` (no ISA change): `is_truthy` identity fast path against the
static boolean symbols (strcmp fallback retained for blob-side `true` literals),
send_resolve's three capability gates cached as pointers (addresses are static),
push_frame arg-copy clamp, OP_BLOCK_INVOKE selector load moved into its fallback.
Kept as its own branch so the two ideas can be adopted or reverted independently.

## Invalidated this session

- "push_frame is the elephant" (the going-in hypothesis): activation ≈ send
  (5.8 vs 7.1 µs on target) and the deltas the trims target are real but small on
  Xtensa; the elephant was sends-per-element and per-iteration bound-check sends.
- "hand index loops beat do:": true on host under the branch, **false on target**
  (ocAt is still 2.4× ocDo — OC>>at:'s frame per element dominates). On ESP32 the
  combinator is the fast way to iterate an OC.
- Host absolutes as evidence: floatMath read +15% on host and −2% on device.
  Within-binary deltas only; see the E5/E6 caveats and the benchmark-discipline
  memory.
