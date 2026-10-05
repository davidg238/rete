# The nsl compiler

The nsl compiler turns Newspeak/SOMns-derived source (`.nsl`) into something the OEVM
interpreter can run — either a **solidified C image** fused into the VM binary, or an
**`.oimg`** the resident VM loads at boot. This doc is the map to the code; the durable
*rationale* lives in `nodus/oevm/docs/topics.md`, the perf and conformance log in
`nodus/oevm/docs/optimizations.md`, and the task-oriented guide is
`programming_with_nsl.md`. Deployment — which backend ships and why — is
`nodus/oevm/docs/deployment-model.md`.

**The extension is `.nsl`** (since 2026-07-18). A bare `.ns` in this tree means a frozen
oracle, an archived file or a generated one — never live source.

## Pipeline

`.nsl` source → **Ohm parse** → JSON AST → **+ kernel + supervisor** → **compile** →
bytecode + constants → **emit** → either `src/solidified_image.c` (→ gcc → fused binary)
or `<name>.oimg` (loaded by the resident VM), with its image map `<name>.oimg.map.json` beside it.

1. **Parse** — `grammar/ns.ohm` (Ohm grammar, `NsLite`) + `grammar/ns-parse.js`
   (the AST builder, run via `node`). `ns_compile_oevm.py main()` shells out to
   `node ns-parse.js <src>` and reads a JSON AST from stdout. Every top-level node is
   tagged with the file it came from, which is what lets a `CompileError` report
   `file:line:col` and print the offending line (#247). A top-level script (no `module`)
   is wrapped in a synthetic `ScriptApp` module whose `main:args:` holds the statements —
   so a `^` in a script is handled exactly as in any other method.
2. **Concatenate the library** — `lib/kernel.nsl` and then `lib/supervisor.nsl` are parsed
   and appended **after** the app, as top-level siblings rather than nested classes. Order
   matters: the entry class is the first declared class, so the app must come first. Being
   siblings is why `resolve_class_ref` never resolves a kernel class in app scope — **an
   app reaches the kernel only through its `platform` argument**, which is a Kernel
   instance, and that is the seam that keeps most builtins out of an app's reach.
3. **Compile** — `tools/ns_compile_oevm.py`. `OevmCompiler` walks the AST
   (`map_classes`/`map_nested`), computes per-class slot layouts (`collect_slots`), then
   emits bytecode per method/block/initializer via a `MethodCompiler`.
4. **Emit** — `tools/solidify.py`'s `SolidifyCompiler` renders vtables, interned symbols,
   compiled methods (instruction + constant arrays) and globals into C; or, for
   `--format=oimg`, `tools/oimg_serialize.py`'s `OimgSerializer` serializes the same graph
   to a blob with an entry descriptor that mirrors the C path's
   `run_solidified_entry_point` exactly — so both backends agree on the init selector,
   argc and slot count.

## The two backends

```
python3 tools/ns_compile_oevm.py <src.nsl> [out]            # Path A — C, the default
python3 tools/ns_compile_oevm.py <src.nsl> out.oimg \
        --format=oimg --layout=<layout.json>                # Path B — the shipping form
```

| | Path A (`--format=c`) | Path B (`--format=oimg`) |
|---|---|---|
| output | `src/solidified_image.c`, compiled into the VM | an `.oimg` blob the resident VM loads |
| oop width | the host's, 8 bytes | `layout["oop_size"]` — the **target's** |
| `--layout` | ignored | **required** |

`--layout` is not a convenience: it is loaded *before* compilation because the
SMI/LARGEINT split in `compile_expr` depends on the target's oop width, so a Path B image
built without it would misclassify integer literals. The layout JSON is a captured target
ABI that no host gate can see go stale — check its provenance before trusting a build.

**Two standing traps on Path A.** `out` defaults to the literal `src/solidified_image.c`,
relative to the cwd — run from `nodus/oevm` that is
**`nodus/oevm/src/solidified_image.c`, a tracked file**. Any bake with no output argument
overwrites it, and the committed copy is a canonical placeholder rather than anyone's
build. And the suite runs **Path B only** (`OEVM_PATH` defaults to `B`), so pass `B`
explicitly to `run-one.py` — its own default is `AB`, which bakes Path A over that same
fixture.

## Key structures (`ns_compile_oevm.py`)

- **`OevmCompiler`** — top level. Owns `all_classes`, `class_slots`,
  `nested_classes`/`nested_classes_flat`, the `class_stack` (lexical scope during
  compilation), and the `SolidifyCompiler` instance. `resolve_class_ref(name)` resolves a
  bare class name against the lexical stack and returns
  `(qname, hops, container_qname)` where **`hops`** = how many lexical levels up
  from the current class the defining container is (the count of `outer` walks
  needed at runtime). `vt_id(qname)` = the C-safe identifier.
- **`MethodCompiler`** — compiles one method/block/initializer to bytecode.
  Register allocator: `alloc_reg`/`free_reg` over `next_free_reg`, tracking
  `max_regs_used`. R0 is `self` in a normal frame. Entry points:
  `compile_statement`, `compile_expr`, `compile_initializer`.
- **`Scope`** — name → (register, frame-depth) resolution within and across
  frames. `resolve(name)` returns `(reg, frame_depth)`; `frame_depth 0` = same
  frame (a plain register), `>0` = an enclosing block frame (reached reflectively
  via `home`/`regAt:` for non-inlined blocks). See
  `topics.md#scope-vs-frame` and `topics.md#why-standalone-scope-class`.

`resolve_name(name)` (in `MethodCompiler`) classifies a bare name into:
`local` (this frame's register), `lexical` (enclosing block frame),
`slot` (instance var of the current or an enclosing class, with a `depth` of
outer hops), `nested_class`, or `implicit_send` (fallback: `self name`).

## Bytecode

**The opcode numbers are not reproduced here.** They live in exactly two places —
`enum Opcode` in `nodus/oevm/src/oevm.h` (the VM decodes it) and the `OP_*` constants at
the top of `tools/ns_compile_oevm.py` (the compiler emits it) — and the two are mirrored by
hand, so a third copy in prose is a copy that goes wrong silently. Read one of those. This
page had `OP_RET=4` and an `OP_NLR=5` for weeks after Milestone A deleted non-local return
and renumbered everything below it; anyone decoding a dump against that table would have
misread every instruction in it.

**There is no `OP_NLR`.** A `^` inside an *inlined* control-flow block compiles to
`OP_RET`; inside an *escaping* closure it is a `CompileError` at the emit site. That is the
no-NLR model (spec §2), not a missing feature.

Each instruction is one `uint32`:

```
op = inst & 0xFF;  A = (inst>>8)&0xFF;  B = (inst>>16)&0xFF;  C = (inst>>24)&0xFF
```

For wide-operand forms (`emit_ABx`) the operand is `Bx = (inst>>16)&0xFFFF`.
Operand conventions worth memorizing when reading a dump:

| op | operands |
|----|----------|
| `LD_REG` | A=dest, B=src |
| `LD_CONST` | A=dest, Bx=const index |
| `LD_SLOT` | A=dest, B=slot index, C=object reg |
| `ST_SLOT` | A=value reg, B=slot index, C=object reg |
| `SEND` / `SEND_IMPLICIT` | A=argc, B=selector const, C=receiver reg (result lands back in C) |
| `NEW` | A=dest, B=register holding the vtable, C=slot count |

Jump offset encoding (verified against the VM decoder): offset =
`target-(idx+1)`; `JMP (off<<8)|op`, `JMP_IF_FALSE (off<<16)|(cond<<8)|op`.

## Calling convention & object layout — invariants

These are easy to break and were the source of two benchmark bugs; the full
reasoning is in `topics.md#calling-convention-invariants`.

- **Send arguments are contiguous above the receiver** (`rcvr+1 … rcvr+argc`).
  Because evaluating an argument can be an expression that bumps `next_free_reg`
  (nested send, block, inlined loop), each arg loop resets
  `next_free_reg = areg + 1` after compiling an argument, so the next one stays
  contiguous and the VM never reads a stale-scratch gap.
- **Slot layout**: a nested class's slot 0 is `outer`; then params, then declared
  slots, then nested-class refs. Parent slots are merged in front for inheriting
  classes (`collect_slots`).
- **`outer` (slot 0) of a new nested instance is the enclosing-CLASS instance**,
  reached by walking `self`'s outer chain `hops` times (from `resolve_class_ref`),
  not always `self`. `hops==0` stores `self` directly.
- **`self` is `get_self_reg()`, never raw R0, in anything `compile_expr` emits.** R0 is
  `self` only in a method frame; in a non-inlined block's frame R0 is the closure, and
  `get_self_reg()` emits `OP_LD_SELF` (the closure's `captured_self`) there. Raw R0 is
  legal only where no block frame can be the emitter — the synthesized accessors and
  `compile_initializer`'s own slot stores. The class-receiver send path broke this for
  the outer link of a new nested instance and for a class-side `self` send (rete#133):
  built inside a reified block, the instance was linked to the closure, and the first
  walk of that link aborted with `Invalid receiver for LD_SLOT`. Gate:
  `nodus/tests/03_blocks/05_nested_class_new_in_block.nsl`.

## Initializers (`compile_initializer`)

For each class an `init_<sig>` method is synthesized. Order: (0) inline
single-level parent-slot init if there is a `superInit`/superclass; (1) store
params to their slots; (2) evaluate declared slot initializers; (3) store nested
class vtables into their slots; (4) run the **primary-factory body statements**
(executable code after the slot section, e.g. `1 to: 4 do: [...]`); then
`RET self`. Limitations (single-level inheritance; parent-before-child source
order) are documented in `topics.md#block-inlining` (Slot-layout fix).

## Class-side / metaclass methods

Methods after the `: ( ... )` class-side section (`classMethods` in the AST) are
sent to the class OBJECT, not an instance. Each class that — or whose ancestor —
declares one gets a `Meta_<vt>` vtable holding the compiled class-side methods;
`solidify.add_vtable(..., meta=...)` points the class vtable's header
(`base.vtable`) at it, and `Meta_<vt>` delegates to the superclass's metaclass
for class-side inheritance. A send `ClassName sel` routes to the class object
(via `class_side_has`, which walks the super chain) when `sel` is a class-side
method, otherwise falls through to the instantiation path. Inside a class-side
method `self` IS the class (`MethodCompiler.class_side_qname`), so `self new`
instantiates the enclosing class with the outer slot left nil. Classes with no
class-side methods keep `meta=None` (header = `VtableVtable`), so their codegen
is byte-identical. **Known limitation:** an inherited class-side `self new`
instantiates the DEFINING class, not the receiver class (no metaclass-`self`
polymorphism). See `nodus/oevm/docs/optimizations.md` (richards, 2026-06-17).

## Block inlining

Control-flow blocks (`ifTrue:`/`ifFalse:`/`and:`/`or:`/`whileTrue:`/`to:do:` …)
with literal block arguments compile *inline* into the enclosing frame (direct
registers, no closure, no `home`/`regAt:`). Non-inlined blocks become separate
compiled methods (closures). See `topics.md#block-inlining` and
`nodus/oevm/docs/optimizations.md` Phases A–E for the full account.

## The image map (`.oimg.map.json`)

Beside every `<name>.oimg` the compiler writes `<name>.oimg.map.json`: every method in
the image by its blob offset, with its name, its source file and its line table, and the
image's SHA-256 so a map from another build is refused. The contract is
`protocol/IMAGE-ARTIFACTS.md` (rete#337); custos reads it (`custos debug`, #339;
`custos profile`, #312). A Path A (`.c`) build writes no map — the
map is keyed by `.oimg` offsets.

Line tables are recorded at statement boundaries, threaded through the compiler via
`compile_statement` and registered per qualified name (`register_source_map`). Each
vtable's source file is recorded as it is declared (`vtable_files`).
`tools/image_map.py::build_image_map` joins these with the serializer's offsets
(`OimgSerializer.method_quals`) and writes the file. A method with no source statements
(a synthesized initializer, a nested-class accessor) is listed with empty `lines`.

## Tooling

- `tools/run_tests_oevm.py` — the conformance gate, run from `nodus/nsl`. Compiles each
  `nodus/tests/**/*.nsl` to an `.oimg` (Path B), runs it, and compares stdout to the
  inline `(* expect: ... *)` block at the top of the file. It also owns the aux gates,
  and since #275 the `C_UNIT_TESTS` registry — every C unit test is one row in that one
  table, plus the `ENG_A`/`ENG_B` link sets and the `OEVM_BUILD=asan|ubsan` knob. **Read
  the aux line, not just the pass count**: it can be red while `Failed: 0` reads green.
- `tools/run-one.py <file.nsl> B` — the fast inner loop for one test, on the same code
  paths as the suite. Pass `B`; the default is `AB`, and Path A bakes over the tracked
  `src/solidified_image.c`.
- `tools/snapshot_images.py <outdir>` — compiles every test to a per-test C
  image; `diff -rq` two snapshots (before/after a change, e.g. via `git stash`)
  to **prove a change is a no-op** or bound its blast radius. This is the
  required exit criterion for structural refactors
  (`topics.md#no-op-first-rollout`).

## Where the rationale lives

- `../../oevm/docs/topics.md` — durable design rationale (scope vs frame, calling
  convention, dispatch caching, no-op-first rollout, …). Start at
  `../../oevm/docs/vm.md`.
- `../../oevm/docs/optimizations.md` — chronological perf + conformance log.
- `../../oevm/docs/deployment-model.md` — which backend ships, and why.
- `NEWSPEAK_DIALECT.md` — what the dialect is and is not; `programming_with_nsl.md` — the
  task-oriented guide.
- the program-level `docs/design/` and `docs/superpowers/plans/` — dated decision records
  and execution plans.
