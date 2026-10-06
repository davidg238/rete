# The debugger's stack and names — one method identity (rete#323)

_2026-10-05. Decision record for #323, parts 1 (backtrace) and 2 (names). Part 3 (values
beyond `printString`) is #359. #340 (the debug page) renders what this builds._

## Decisions (David, 2026-10-05)

| | Decision |
|---|---|
| Order | #323 before #340: the debugger's data first, in the node verb and the CLI. |
| Scope | Backtrace and names only; one-level slot expansion is #359. |
| Identity | **B**: a method is its blob offset everywhere. The registry is deleted. |

## Why one identity

Today the debugger names a method by a **registry id**, minted by walking the entry
class's vtable chain (`Debugger>>buildRegistry`, `supervisor.nsl`). Kernel methods and
blocks are not in it, so a backtrace by registry id answers `-1` for most of a real stack.
The profiler already names a method by `ptr - oimg_blob_base` (`src/vm.c`, the `mprof:`
lines), and `.oimg.map.json` is keyed by that same offset (`protocol/IMAGE-ARTIFACTS.md`
§3). One identity means custos needs no registry, a frame in a block resolves like any
other, and the profile and debug readers share one lookup.

## 1. The VM: two mirror primitives (`src/mirror.c`)

- `blobOffsetOf: ctx` — the offset of the context's CompiledMethod from
  `oimg_blob_base`, as a SmallInteger.
- `methodAtBlobOffset: n` — the CompiledMethod at `oimg_blob_base + n`, or `nil` when
  `n` is out of the blob or the oop there does not have the CompiledMethod vtable.

With no blob (`oimg_blob_base == NULL`, a solidified Path A image) both answer `nil`, and
the Debugger answers every positional verb with `dbg:error no-blob`.

## 2. The node verbs (`nodus/nsl/lib/supervisor.nsl`)

| Verb | Today | After |
|---|---|---|
| `dbg:break <off> <pc>` / `dbg:clear` | registry id + pc | blob offset + pc; `dbg:error no-method` if `methodAtBlobOffset:` is nil |
| `dbg:paused <why> <off> <pc>` | registry id | blob offset |
| `dbg:backtrace` | — | one line: `dbg:frames <off>@<pc> <off>@<pc> …`, innermost first |
| `dbg:inspect N` | `dbg:stack off=… r0=… r1=…` | unchanged, plus `c0=… c1=…` for frame N's Context slots when the context register holds one |
| `dbg:methods` | the registry listing | deleted, with `buildRegistry`, `methodIdOfCtx:`, `listMethods` |

One flat line per answer, as `dbg:inspect` already does. Breakpoints inside a block become
possible, because a block method has its own offset and its own `lines` in the map; the
refusal custos gives today ("a line inside a block is refused by name") is removed.

## 3. Names in `.oimg.map.json` (contract: `protocol/IMAGE-ARTIFACTS.md` §3)

Each method entry gains `vars`, written by `ns_compile_oevm.py` from `MethodCompiler`'s
own allocation (register 0 `self`, args `1..n`, the Context at `n+1`, temps from `n+2`):

```json
"vars": [
  {"name": "n",   "reg": 1},
  {"name": "sum", "ctx": 0},
  {"name": "i",   "reg": 5, "from": 12, "to": 40}
]
```

- `reg` alone: an argument or method temp, live for the whole method.
- `reg` with `from`/`to` (bytecode offsets, `to` exclusive): a temp or param of an
  **inlined** block, whose register is reused once its scope closes
  (`compile_inlined_block` restores `next_free_reg`).
- `ctx`: a captured local, in slot `k` of the frame's Context (`capture_slot`); it does
  not live in a register.
- `self` is not listed; register 0 is always `self`.

A real (non-inlined) block is its own method with its own `vars`. A name it reads from an
enclosing frame is shown in that frame, not repeated in the block's entry.

## 4. custos (`custos/internal/debug`)

- `SourceMap` keys on the blob offset; `LoadRegistry`/`HasRegistry` and the `dbg:methods`
  exchange at attach are deleted. `Resolve` answers an offset+pc, `Where` takes one.
- `bt` — sends `dbg:backtrace`, prints one `file:line  Class>>sel` per frame, numbered
  from 0.
- `locals [N]` — sends `dbg:inspect N`, prints `name = value` for the vars of frame N's
  method that are live at its pc (`reg` within `from`/`to`, `ctx` from the `c` fields).
  `self` first.

## 5. Tests, and the fate of what it touches

| Item | Fate |
|---|---|
| `nodus/tests/04_somns_basic/46_debug_target.nsl` | grows a captured local and an inlined `to:do:` temp, reached through a block |
| `custos/internal/debug/*_test.go` | ported to offsets; new cases for `bt`, `locals`, `vars` liveness |
| `nodus/nsl/tools/test_custos_debug.py` | the block-refusal case becomes a break in a block; adds `bt` and `locals` |
| `nodus/nsl/tools/test_debugger_integration.py` | Path B half ported to offsets; **Path A half deleted** — a solidified image has no blob (regression, named: the debugger needs a `.oimg`) |
| `nodus/oevm/zephyr/tools/dbg_session.py` | deleted; the board check is `custos debug -port` on `c6spare` (the `--hold` past appLiveness is done by hand at a stop) |
| `protocol/IMAGE-ARTIFACTS.md`, `protocol/fixtures/image/` | `vars` documented; fixture regenerated |
| `docs/development.md`, `nodus/nsl/docs/NEWSPEAK_DIALECT.md` | verb grammar updated |

Done means: the suite green with its aux line (the debugger gates PASS, not SKIP), the Go
gates green, and one `custos debug -port` session on `c6spare` showing `bt` through a block
and `locals` with a named captured local, output on #323.

## Not done here

- #359: values beyond `printString`.
- Send-by-send stepping (#322) — stepping stays per line.
- The debug page (#340).
- Promotion at close: the verb grammar to the debugger's mainline doc, `vars` to
  `protocol/IMAGE-ARTIFACTS.md` (done in-change).
