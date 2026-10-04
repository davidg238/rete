# #297 — Cbor>>encode: as a native walk, dispatched through the value classes' own vtables

*Written 2026-10-04 on `297-cbor-native-walk` (off `329-c6-speed`). Decision record for
rete#297. Spike: `docs/spikes/2026-10-04-cbor-prim-arms/` (VM half on branch `297-cbor-spike`).*

## Why

#297 states the forcing function: cabot's 50 Hz tick makes the codec the budget (~80 B/tick at
~104–125 µs/B is ~8.5 ms of 20 ms), and every porta node's check-in pays the same encode
(`pushEncode` ~53 ms on a C6 for the 425 B frame). The 2026-08-09 shape result
(`docs/results/2026-08-09-cbor-encode-shape.md`) says there is nothing left to fix in nsl.

## What the spike measured (host, Path B, ns per encode, min of 20; all arms byte-identical)

| payload | N today | B nsl walk + C bytes | A all C | A′ C walk, OEOM dispatch |
|---|---|---|---|---|
| str256 (259 B) | 75,200 | 1,530 | 374 | 338 |
| ints64 (64 elts) | 47,750 | 28,120 | 798 | 815 |
| map8 | 43,900 | 19,620 | 688 | 729 |
| mixed (81 B) | 64,250 | 29,315 | 931 | 988 |
| **batch (425 B)** | **260,350** | **86,355** | **1,913** | **2,040** |

- **B ÷ A = 45× on `batch`.** Moving the bytes into C while the walk stays in nsl leaves ~400 ns
  of interpreted sends per element. The walk is the cost, not the bytes, so the walk goes to C.
- **A′ ÷ A = 1.04–1.07 on `batch`, ≤ 1.12 on `map8`** (three runs; the emulation over-states it —
  a ≤4-way compare stands in for reading the binding's entry). That overhead buys the decision
  below: C does not know which classes are List/Map/Record/Bytes; each class's vtable says so.
- Found in passing: **#330**, Path B has two `true` objects (computed vs literal).

## Decisions

### D1. A class declares that its encoding is native (OEOM, made real)

- **nsl:** a method body may be `native <name>`. Each value class carries one line —
  `List`: `cborWalk = native cborList`, likewise `cborMap` (Map), `cborRecord` (Record),
  `cborBytes` (Bytes). One grammar rule (`ns.ohm` `MethodDef`), one compiler path.
- **Registry:** `builtins.def` gains a fourth row kind, `NATIVE` — a firmware-static
  `PrimitiveClosure` — plus a `SYMBOL` row for `cborWalk`, so image and firmware share one selector
  oop (no strcmp scan, unlike the Hanoi spike; no hand-built Symbol, #226). The compiler emits
  the binding's method as a registry reference (`TAG_BUILTIN`, already in the `.oimg`); **no
  format change**, `BUILTIN_REGISTRY_VERSION` 5 → 6 (and `gen_builtins.VERSION`). Path A emits
  `(oop)&Native_cborList`.
- **`cborWalk` is not sendable from nsl.** The closure's interpreter entry aborts with a VM error
  naming the native. The single entry is `platform cborEncode: v` (one new prim id).

Why not the alternatives: **A** (C compares against the four classes, passed as a `kinds`
array) hardcodes class identity in C; **B** fails the 10% bar by 45×; a runtime slot-reading prim
would be a send per slot (B's cost again) or need ivar names in the image.

### D2. The walk is C; any doubt falls back to the nsl oracle

- `Cbor>>encode: v` = `platform cborEncode: v`; if that answers *refused*, run
  `encodeOracle: v` — **today's nsl walk, renamed, otherwise unchanged**.
- C encodes leaves by a switch (nil, both booleans per #330, SMI, LargeInteger, Float, String,
  Symbol — tag 39). A container is encoded only when `lookup_method_cached(vtable, #cborWalk)`
  answers a cbor native; the native's C walk function is reached from its closure.
- **C refuses** — answers one sentinel, never an error text — on: no/other binding, depth > 32,
  NaN, anything else it does not recognise. The oracle then produces the exact bytes or the
  exact error. So the error texts, their ORDER, and #132's depth behaviour stay defined in one
  place (nsl), and a user class answering `valueRank 7` still encodes (via the oracle).
- Map keys: RFC 8949 §4.2 bytewise over encoded keys, stable insertion sort, as
  `sortPairsBytewise:` (Symbols are tag 39; Records are tag 27 over `[tag, fields]` —
  #297's text says "Symbol names in tag 27", which is wrong and is corrected here).

### D3. The class shape is written once (nsl) and checked at bake time

- Each `NATIVE` row declares the ivars its C reads, in order: `cborList: items depth`,
  `cborMap: keys vals depth`, `cborRecord: tag fields depth`, `cborBytes: raw`. C reads them as
  constants (slot 0 is `outer`, so the first ivar is slot 1; an Array's slot 0 is its length).
- The bake (both paths) **refuses, naming the native and the class**, an image where a class
  binding `native X` does not declare exactly X's ivars. A shape edit is a registry edit: bump
  the version, which the loader already refuses on mismatch.

### D4. Memory: two allocations sized by pass 1, no tunables

- Pass 1 counts output bytes, total map-key bytes and total map entries. Then two GC allocations,
  both before pass 2: the output RawBytes and one scratch RawBytes (key bytes + a sort index).
  No allocation happens during pass 2, so no GC runs mid-walk (the collector is non-moving and
  `v` is rooted by the prim's args). No `.bss` buffer, no C heap, no invented size limits.
- C recursion ≤ 32 levels (the language's bound). Its real stack cost is **measured on the C6**
  (8 KB main stack) by the board gate, not assumed.

### D5. Proof and price

- **Oracle gate (suite):** encode the #140 golden set + the spike's `mixed` set + nesting at
  depth 31/32/33 both ways (`encode:` vs `encodeOracle:`) and require byte equality; force one
  fallback (a class answering `valueRank 7`) and require identical output. A C unit test in
  `tests/` covers the refusal sentinel.
- **Falsify the guards:** swap two `List` ivars → the bake must refuse naming `cborList`; break
  one map-sort comparison → the oracle gate must fail.
- **Board, one flash:** `CborBenchNodeC6.nsl` on `c6spare` (fix #212's 1-byte `bid` in the same
  pass), then `bridge-health` for the check-in spans; read the VM stack high-water.

## Not in scope

Decode (`rawBytesOf:` is the same per-byte shape and unpriced — one profile window first, per
#297 point 2). Re-expressing the Hanoi spike (`src/hanoi_native.c`) as a `NATIVE` row. Fixing
#330 (C accepts both booleans meanwhile). #161 fix (1) is re-priced after, not done first. #28 is
not absorbed. The size law (512 B batch, 256 B value) is untouched — it is a radio fact.

## Costs and assumptions, written down

- One new language form (`native <name>`) with exactly one user (the cbor natives). It is the
  OEOM lever generalised; a second user is what would prove the generality.
- The C walk and the nsl oracle are two encoders that must agree; the oracle gate is the only
  thing keeping them so, and it runs on host only.
- Host ratios are not device numbers; the board run is the price.

## Promotion (close-out)

The `NATIVE` row and the bake shape check → `nodus/oevm/docs/vm.md`; the `native <name>` form →
`nodus/nsl/docs/NEWSPEAK_DIALECT.md` + `nodus/nsl/docs/compiler.md`; the C/oracle split →
the `Cbor` class comment in `kernel.nsl`. For David (review/ tree, not edited here): the
program-sequence 2026-07-17 OEOM addendum gains its first production user.

## Addendum 2026-10-04 — plan-time refinements (David approved the plan)

- **D1, unsendable entry:** the four natives share one entry function, so the abort names the
  *receiver's class* (and `cborWalk`), not the native. A native is recognised by
  `closure_entry == cbor_native_unsendable`; its walk is a field of `struct CborNative`.
- **Placement:** the walk lives in `src/platform.c` beside the leaf prims (reuses the half-float
  helpers; no new `.c`, so #63's nine source lists are untouched).
- **Registry:** a NATIVE row's name string is `"<native> <ivar>…"`; the generator emits natives in a
  separate `NATIVES` dict so a native name can never resolve as an app symbol.
- **Path A** is oracle-only: `Symbol_cborWalk` is assigned only by `kernel_boot` (Model B), so C refuses.
- Plan: `docs/superpowers/plans/2026-10-04-297-cbor-native-walk.md`.
