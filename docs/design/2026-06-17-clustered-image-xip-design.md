# Design: Decoupled image + execute-in-place clustered format

Status: **approved design**, 2026-06-17. Implements the "next architectural
step" from `nodus/oevm/docs/deployment-model.md` (Model B): decouple the image from the VM
build, define an oevm-native clustered image format, and give the VM an
on-device loader. Supersedes nothing; the existing solidify-to-C path (Model A)
is retained as an opt-in appliance build.

## Goal & scope

Move oevm from Model A (image `#include`d into the VM, fused into one `gcc`
binary) to Model B (resident VM firmware that runs a separately-delivered
program image). This chunk delivers:

1. **Decouple** the image from the VM build — `oevm <image.oimg>` is standalone
   firmware; `src/solidified_image.c` is no longer `#include`d in the default
   build.
2. **Define** the `.oimg` format — clustered serialization (à la Fuel/Parcels)
   realized as an **execute-in-place (XIP)** blob.
3. **Loader** — the VM maps the blob read-only and runs it with no
   decode/relocate step.

### In scope
- Host-first proof: serialize on host → standalone VM maps and runs `.oimg` →
  identical output to Model A.
- **32-bit portability**, via **per-target builds** (one `.oimg` per target ABI).
  One blob serving both 32- and 64-bit simultaneously is explicitly **not** a
  requirement.
- Execute-in-place from a read-only mapping (mmap on host; the embedded ideal for
  the keeper's deep-sleep/re-poll lifecycle on device).

### Out of scope (deferred)
- ESP32/Zephyr hardware bring-up and the C-keeper↔VM seam (named in
  `deployment-model.md` open questions).
- Bignum / SMI-overflow promotion (`04_integer_overflow` — already failing,
  unrelated). Consequence: SMI literals must fit a signed `(wordbits−2)`-bit
  immediate (30-bit on a 32-bit target); the serializer errors on overflow.
- Inline caching (`OP_SEND_IC`) and unboxed floats — the perf levers that XIP's
  per-access cost makes load-bearing. Follow-on work, not part of this chunk.
- Partial-graph inter-container messages (the format is designed to extend to
  them later, but they are not built here).
- Programs that mutate baked-in literals (see Immutability constraint).

## Key decisions (resolved during brainstorming)

| # | Decision | Rationale |
|---|----------|-----------|
| Scope | Host proof **+ 32/64 portability via per-target builds** | One blob for both arches dropped as a requirement; frees us to use native layout. |
| Image roots | `.oimg` carries **only the app graph**; firmware owns builtins | Smallest payload; matches the partial-graph-message future; primitives are C functions that can't be serialized anyway. |
| References | **A2 — byte-offset references** | Offset-shaped refs make the run-in-place future a loader change, not a format change. |
| Load model | **Execute-in-place from a read-only mapping** | Zero per-wake work across deep-sleep/re-poll; the embedded ideal. |
| Test path | **Parallel paths, both gated** | Model A stays a stable backstop while the loader matures. |

## Architecture: the kernel/app split

`solidified_image.c` today mixes two object populations:

- **Kernel objects** (identical for every app): `IntegerVtable`, `FloatVtable`,
  `MethodContextVtable`, `FiberVtable`, the root `Object` vtable, their compiled
  methods (e.g. `MethodContextVtable>>unwindTo:value:`), and the core/reflection
  symbols (`=`, `new`, `printString`, …).
- **App objects** (per-program): user class vtables, their compiled methods +
  closures, user symbols/selectors, float literals, any literal instances.

Model B splits them:

```
  ┌─────────────────────── VM firmware (built once per target) ──────────────────────┐
  │  interpreter (vm.c, dispatch.c, gc.c)                                              │
  │  kernel_boot(): constructs kernel objects in RAM → fills builtin_registry[]        │
  │  loader: mmap .oimg read-only → header/version check → set blob_base → bootstrap   │
  └────────────────────────────────────────────────────────────────────────────────────┘
                                        │ maps & runs (no decode/relocate)
                                        ▼
              app.oimg  (clustered, native-layout, XIP; every reference to a
                         kernel object is a builtin-index ref)
```

The serializer walks the app graph and classifies every reference as immediate
SMI, singleton, kernel object (→ builtin-index ref), or another app object (→
byte-offset ref). The VM never rebuilds kernel objects.

## The `.oimg` format

Execute-in-place: the blob **is** the heap-resident program. No decode/relocate
step. Mapped read-only at `blob_base` (mmap on host; XIP flash on device).

- **Native layout, ABI-matched.** Blob objects are laid out as the target's
  actual C structs. The serializer consumes a **layout descriptor** dumped from a
  per-target build (extend `tests/test_layout.c` → `dump_layout` →
  `layout.json`: `sizeof`/`offsetof` for every serialized struct). Per-target
  builds follow from this.
- **Little-endian, 4-byte aligned.** Format invariants; not negotiated per-file.
- **References = tagged offset-words** (word-sized in native layout: 4 bytes on
  32-bit, 8 on 64-bit). Low 2 bits = tag, upper bits = payload:

  | tag | meaning | payload |
  |-----|---------|---------|
  | `00` | app-object | byte offset into blob → `blob_base + offset` |
  | `01` | builtin | index into firmware `builtin_registry[]` |
  | `10` | small immediate SMI | sign-extended value → `TAG_SMI(v)` |
  | `11` | singleton | id → firmware `nil`/`true`/`false` |

  SMI immediates limited to signed-(wordbits−2); literals that overflow are a
  serialize-time error (ties to deferred bignum).

### File layout

```
Header (fixed):
  u8  magic[4] = "OIMG"
  u16 format_version
  u16 flags                    // reserved; LE & 4-align are format invariants
  u32 builtin_registry_version // must == firmware BUILTIN_REGISTRY_VERSION, else reject
  u32 blob_size                // bytes of the object region
  u32 cluster_count
  uN  root_ref                 // ref-word → entry-class vtable (app-object); word-sized
  (pad to alignment)

Cluster directory (cluster_count entries):
  u16 kind        // SYMBOL|VTABLE|METHOD|CLOSURE|FLOAT|INSTANCE|BOXEDBINDING
  u16 reserved
  u32 count
  u32 body_offset // from blob base; first record of this cluster

Object region (clusters grouped by kind; each record native-layout, 4-aligned):
  SYMBOL       : struct Symbol      (interned string)
  VTABLE       : struct Vtable      (delegate ref, meta ref, bindings: (sym ref, method ref)*)
  METHOD       : struct CompiledMethod (num_regs, num_args, instructions[], constants[] of refs)
  CLOSURE      : struct Closure     (method ref; home_ctx = nil singleton for image closures)
  FLOAT        : struct Float       (f64 IEEE-754 LE)
  INSTANCE     : struct ObjectInstance (vtable ref, slots[] of refs)
  BOXEDBINDING : struct BoxedBinding (sym ref, method ref)
```

An object's **byte offset is its identity** — that is what app-object ref-words
point at. Clustering by kind keeps decode/scan branchless and groups type info
(the Fuel/Parcels property). `instructions[]` is raw bytecode (our own fixed
32-bit encoding), read straight from flash with no resolution; constant-pool
entries are ref-words, resolved on read.

### Reference resolution

Resolution happens at the **field-read boundary**. When the interpreter reads a
ref field *out of a blob-resident object* (`LD_SLOT`, `LD_CONST`, vtable
`delegate`/binding lookup, `closure->method`, constant-pool reads), it resolves
the tagged word to a **canonical `oop`**: real pointer for app/builtin,
`TAG_SMI` for smi, firmware global for singleton. Everything downstream
(registers, stack, RAM instances) holds canonical `oop`s exactly as today.

The accessor is gated by an **address-range check** `is_in_blob(ptr)` — a range
compare against `[blob_base, blob_base+blob_size)`; on ESP32 the XIP flash range
is a constant. The resolution surface is localized to this accessor.

Selector identity is preserved across the kernel/app boundary: a user method's
selector constant that equals a core symbol is emitted as a builtin-tag ref, so
both the constant and the vtable binding resolve to the same registry pointer.

### Three consequences (accepted)

1. **Immutability constraint.** Blob objects are never written. Fine for
   vtables/methods/symbols/floats (immutable in our model); runtime instances are
   `gc_allocate`d in RAM as today. A program that mutates a *literal* (e.g.
   `at:put:` into a baked-in Array) is unsupported under XIP and needs RAM
   promotion. No current conformance program does this; add a guard/diagnostic.
2. **GC skips the blob.** Blob objects are never freed or moved; the collector
   stops tracing at the blob boundary (blob refs reach only blob/builtin/smi, all
   permanent). RAM→blob pointers are stable forever. Modest change to `gc.c`.
3. **Hot-path cost.** Per-field-read range-check + tag-dispatch on blob objects
   is real interpreter overhead — the reason inline caching and unboxed floats
   become load-bearing under Model B (deferred, see Out of scope).

## Components

### Builtin registry (single source of truth)
`builtins.def` lists, in frozen order, every firmware-owned object an image may
reference (singletons, kernel vtables, core symbols). One generator emits both:
- firmware: `builtin_registry[]` (filled at kernel boot) + `BUILTIN_REGISTRY_VERSION`;
- serializer: a `name → index` map.

Generated from the same file so they cannot drift; version bumps when the set
changes; loader rejects a mismatched image at the header check.

### Layout dumper
Extend `tests/test_layout.c` into a `dump_layout` build target that emits
`layout.json` (`sizeof`/`offsetof` per serialized struct) for the current target
ABI. `test_layout`'s assertions extend to cover the 32-bit ABI.

### Serializer (host)
A new binary backend on `solidify.py`'s existing in-memory graph. Given the
target `layout.json`:
1. assign each app object a blob offset (clustered by kind, 4-aligned);
2. classify and write each reference as `smi`/`singleton`/`builtin`/`app-offset`;
3. write native-layout records, header, cluster directory, `root_ref`.

`ns_compile_oevm.py` gains `--format=oimg --layout=layout.json` alongside the
existing C path.

### Kernel boot (firmware)
`kernel_boot.c` constructs the kernel objects (moved out of the per-app
`solidified_image.c`) in RAM at startup and populates `builtin_registry[]`.

### Standalone VM entry + loader
```
main(argv): parse image path → mmap read-only → header & registry-version check
    → set blob_base → kernel_boot() → resolve root_ref
    → bootstrap: instantiate entry class, send init:manifest:(nil,nil), main:args:(nil,nil)
```
`solidified_image.c` is no longer `#include`d by the default build. **Model A is
retained as an opt-in appliance build** (build flag preserves the fused-image
path).

## Testing & verification

Parallel paths, both gated:
- **Path A (default, backstop):** existing solidify-to-C build; `run_tests_oevm.py`
  unchanged; stays the full 36/42 gate.
- **Path B (new):** per test, build `.oimg` → run `oevm` on the blob → compare
  output against the same 36/42 expectations. Both paths green.
- **32-bit portability:** add a 32-bit (`-m32`) build of the VM + `dump_layout`,
  serialize a 32-bit `.oimg`, run it under the 32-bit `oevm`. A small dedicated
  subset (not the whole suite) proves ABI-parameterized serialization on both
  word sizes.
- **No-op gate for Path A:** the kernel hoist must not change Path A output
  (kernel objects are the same, just relocated in the build). Snapshot/diff per
  the conformance-workstreams no-op proof technique.

## Risks & open questions

- **Hot-path overhead** may regress the steady-state gap to Soup measurably;
  quantify via `../ns-tests/00_perf` once Path B runs. Mitigation is the deferred
  perf levers.
- **Literal mutation** under XIP is unsupported; the guard must fail loudly
  rather than corrupt flash semantics.
- **Bootstrap convention** is currently hardcoded (`init:manifest:` then
  `main:args:` with nil/nil); kept as firmware behavior. If apps need a richer
  entry contract later, `root_ref` can grow into a small root descriptor.
- **Deferred to deployment-model open questions:** GC/ROMable-cluster interaction
  at scale, NVS-survival/versioning across OTA, shared-heap vs isolate-per-
  container, and the exact C-keeper↔VM seam.
