# ns-oevm compiler

The ns-oevm compiler turns Newspeak/SOMns source (`.ns`) into a C image
(`solidified_image.c`) that links against the OEVM interpreter. This doc is the
map to the code that lives in this repo; the durable *rationale* (why things are
the way they are) lives in the OEVM repo's `docs/topics.md` and the perf/
conformance log in `docs/optimizations.md` — linked by anchor below rather than
duplicated here.

## Pipeline

`.ns` source → **Ohm parse** → JSON AST → **compile** → bytecode + constants →
**solidify** → `src/solidified_image.c` → (gcc) → runnable image.

1. **Parse** — `grammar/ns.ohm` (Ohm grammar, `NsLite`) + `grammar/ns-parse.js`
   (the AST builder, run via `node`). `ns_compile_oevm.py main()` shells out to
   `node ns-parse.js <src>` and reads a JSON AST from stdout. A top-level script
   (no `module`) is wrapped in a synthetic `ScriptApp` module; a `^` inside a
   top-level script block is rejected (no home method to return from).
2. **Compile** — `tools/ns_compile_oevm.py`. `Compiler` walks the AST
   (`map_classes`/`map_nested`), computes per-class slot layouts
   (`collect_slots`), then emits bytecode per method/block/initializer via a
   `MethodCompiler`.
3. **Solidify** — `tools/solidify.py` renders vtables, interned symbols,
   compiled methods (instruction + constant arrays), and globals into C.

`main(src, [out])` defaults `out` to `src/solidified_image.c`. Note that file is
a **generated build artifact**: the test runner overwrites it on every compile,
so never commit its churn — the committed copy is a canonical placeholder.

## Key structures (`ns_compile_oevm.py`)

- **`Compiler`** — top level. Owns `all_classes`, `class_slots`,
  `nested_classes`/`nested_classes_flat`, the `class_stack` (lexical scope during
  compilation), and the `Solidify` instance. `resolve_class_ref(name)` resolves a
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

Opcodes are defined at the top of `ns_compile_oevm.py` (`OP_LD_CONST=0`,
`OP_LD_REG=1`, `OP_SEND=2`, `OP_RET=4`, `OP_NLR=5`, arithmetic 6–9, `OP_JMP=10`,
`OP_JMP_IF_FALSE=11`, `OP_LD_SLOT=12`, `OP_ST_SLOT=13`, `OP_NEW=14`,
`OP_SEND_IMPLICIT=15`). Each instruction is one `uint32`:

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
| `NEW` | A=dest, B=vtable-const reg, C=#slots |

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
polymorphism). See `docs/optimizations.md` (richards, 2026-06-17).

## Block inlining

Control-flow blocks (`ifTrue:`/`ifFalse:`/`and:`/`or:`/`whileTrue:`/`to:do:` …)
with literal block arguments compile *inline* into the enclosing frame (direct
registers, no closure, no `home`/`regAt:`). Non-inlined blocks become separate
compiled methods (closures). See `topics.md#block-inlining` and
`docs/optimizations.md` Phases A–E for the full account.

## Source maps (.stmap)

The compiler emits a **sidecar source-map file** (`.stmap` JSON) alongside each
compiled image (`solidified_image.c`), written to
`<image-base>.stmap` (e.g., `foo.c` → `foo.stmap`). This map ties bytecode
offsets to source lines, enabling **debug-aware tools** (such as `jast-gw`) to
display source during inspection.

**Format:** The `.stmap` JSON matches the contract st-berry produces, so
`jast-gw` consumes ns-oevm and st-berry maps identically:

```json
{
  "source": "<basename.ns>",
  "functions": {
    "<Vtable>>selector>": [[offset, line], ...]
  }
}
```

Each function maps to an array of `[bytecode_offset, line_number]` pairs,
recorded at statement boundaries (statement-level granularity).

**Scope:** v1 includes instance methods, class-side methods, and non-inlined
blocks. Synthetic methods without source (e.g., default initializers, accessors)
are intentionally omitted; empty maps are dropped.

**Implementation:** `tools/stmap.py::build_stmap(source_filename, source_maps)`
serializes the collected per-method line maps to JSON. Maps are threaded through
the compiler via `compile_statement` and registered per qualified name
(`register_source_map`), then emitted to disk via `Solidify`.

## Tooling

- `tools/run_tests_oevm.py` — the conformance gate: compiles each
  `ns-tests/**/*.ns`, builds the runner (`-O2`), runs it (30s timeout), compares
  stdout to the `(* expect: ... *)` block.
- `tools/snapshot_images.py <outdir>` — compiles every test to a per-test C
  image; `diff -rq` two snapshots (before/after a change, e.g. via `git stash`)
  to **prove a change is a no-op** or bound its blast radius. This is the
  required exit criterion for structural refactors
  (`topics.md#no-op-first-rollout`).

## Where the rationale lives

- `../oevm/docs/topics.md` — durable design rationale (scope vs frame, calling
  convention, dispatch caching, no-op-first rollout, …).
- `../oevm/docs/optimizations.md` — chronological perf + conformance log.
- `docs/design/` and `docs/superpowers/plans/` — feature specs/plans
  (block inlining).
