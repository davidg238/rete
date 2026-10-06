# Image artifacts — what the compiler writes and custos reads

> **In force (rete#337, decided by David 2026-10-04)** — except the word width in
> `flags` (§2, decision 3), which waits for #330's `format_version` 11 to merge and then
> lands as version 12. Design: `docs/design/2026-10-04-custos-peer-design.md` §6a step 1.

The nsl compiler (`nodus/nsl/tools/ns_compile_oevm.py`, Python) writes an image and a
map that describes it. custos (Go) reads them to load a program (#310), to debug by
source line (#339), and to name the rows of a profile (#312). The compiler owns the
writer and custos owns the reader. This document and the fixture in §4 are what both
of them answer to. Neither side reads the other's code to learn the format.

## 1. The artifacts

| File | Written by | Read by | Purpose |
|---|---|---|---|
| `<name>.oimg` | `oimg_serialize.py` | the VM (`nodus/oevm/src/oimg_loader.c`); custos reads only the prefix (§2) | the program, executed in place |
| `<name>.oimg.map.json` | `ns_compile_oevm.py` | custos | every method in the image: its offset, its name, its source lines |

The map is written in the same directory as the image and named after it, so the two
files travel together. The map records the image's hash (§3), so a map that belongs to
a different build is refused instead of being trusted.

## 2. `.oimg`: the part custos reads

custos reads the 20-byte fixed prefix and the hash of the whole file. Nothing more.
Everything after the prefix (the descriptor block, the clusters, the classnames and
symbol tables) belongs to the VM. Its layout is defined in the comment block at
`OimgWriter.header_bytes` and mirrored in `oimg_loader.c`, and it may change without
any change to custos.

All values are little-endian.

| Offset | Width | Field | What custos does with it |
|---|---|---|---|
| 0 | 4 | magic `"OIMG"` | refuses the file if the magic does not match |
| 4 | 2 | `format_version` (u16) | shows it; **does not judge it**, because the VM does that and refuses any version it does not run |
| 6 | 2 | `flags` (u16) | bits 0–1 = log2(oop_size): `2` for 4-byte words, `3` for 8-byte words; all other bits 0. **Not yet written** (decision 3, #344): today `flags` is 0 and custos takes the width from the map's `image.oop_size` |
| 8 | 4 | `registry_version` (u32) | shows it; the VM refuses a mismatch with its firmware |
| 12 | 4 | `blob_size` (u32) | refuses the file unless this equals the file's length; it is also the size written to the program region (#310) |
| 16 | 4 | `cluster_count` (u32) | ignored |

**Image hash:** the SHA-256 of the whole `.oimg` file, written as lowercase hex. This
is how a map is tied to its image (§3). It is also how custos checks which program a
board is running: the node prints and reports the hash of its program region (§2a,
decision 2).

## 2a. The program region (rete#310)

A board that has a program region runs its program from there. The firmware links
none, so writing a new program never touches the firmware's build-id.

| Board | Partition label | Offset | Size | Writer | Readers |
|---|---|---|---|---|---|
| ESP32-C6 devkit | `program_partition` | `0x320000` | 512K | `nodus/oevm/zephyr/boards/esp32c6_devkitc_esp32c6_hpcore.overlay` | `zephyr/run.sh` (from the build's `zephyr.dts`); `custos/internal/load` (a copy that cites this row) |

`blob_size` (§2) is the number of bytes the region holds. An erased region reads
`0xFFFFFFFF` there, and the firmware's loader refuses it by name. After a load, the
firmware prints `oevm: program sha256 <hex> (<N> ms)` and reports the same hash as
`obs/apps/main`'s `#image`.

## 3. `.oimg.map.json`

```json
// 46_debug_target.nsl, C6 layout, 2026-10-05 (three of its 787 methods)
{
  "image":   { "sha256": "422b6570…2b428e", "size": 160776, "oop_size": 4 },
  "entry":   "DebugTargetTest",
  "files":   ["46_debug_target.nsl", "kernel.nsl", "supervisor.nsl"],
  "methods": {
    "3952":  { "name": "DebugTargetTest>>countTo:", "file": 0,
               "lines": [[0, 25], [2, 26], [9, 26], [12, 26], [19, 27]],
               "vars":  [{"name": "n", "reg": 1}, {"name": "sum", "reg": 3},
                         {"name": "i", "reg": 5, "from": 9, "to": 16}] },
    "7600":  { "name": "Kernel>>Bytes", "file": 1, "lines": [], "vars": [] },
    "10224": { "name": "Kernel_OrderedCollection>>reversed", "file": 1,
               "lines": [[0, 176], [6, 177], [10, 178], [17, 178], [31, 179]],
               "vars":  [{"name": "r", "reg": 2}, {"name": "n", "reg": 3},
                         {"name": "i", "reg": 6, "from": 17, "to": 28}] }
  }
}
```

- `image`: the image this map belongs to. `sha256` and `size` must match the `.oimg`
  exactly, or custos refuses to use the map. `oop_size` is the target word width.
- `entry`: the class the image runs. The debugger's registry (`dbg:methods`) lists
  this class's methods first, so custos needs `entry` to turn `<file>:<line>` into a
  method id (rete#311).
- `files`: the base names of the source files that went into the image. The app's
  file is always first, then `kernel.nsl`, then `supervisor.nsl`.
- `methods`: one entry per CompiledMethod in the image. Blocks are included, because a
  profile needs to show them (#37).
  - The **key** is the method's offset from the start of the blob, in bytes, written
    as a decimal string. The VM reports a method by this offset
    (`ptr - oimg_blob_base`), both in `mprof:` lines and as a debugger position.
  - `name`: `"<Vtable>>selector"`. A block is named
    `"<Home>_block<N>>value:"`, with no `Block_` prefix. Every method has exactly one
    name: `SolidifyCompiler.add_method` mints one method per (vtable, selector) and is
    the only place a binding is made, and the serializer asserts it.
  - `file`: an index into `files`, saying which source file `lines` refers to.
  - `lines`: `[bytecode_offset, line]` pairs in order of offset. A pair means that
    the bytecodes from this offset up to the next pair's offset belong to this line.
    The list is empty for a method that has no source, such as a synthesized
    initializer or a nested-class accessor. Every method that has source has lines,
    including a block whose whole body is one expression (#343). A block's first pair
    is at offset 1: offset 0 is its `OP_MK_CONTEXT` prologue, which has no line.
  - `vars`: the frame's named variables (rete#323), from the compiler's own register
    allocation (register 0 `self`, arguments `1..n`, the Context at `n+1`, temps
    after). The list is empty for a method with none. Each entry has a `name` and
    exactly one home:
    - `reg` alone: an argument or method temp, in that register for the whole method.
    - `reg` with `from` and `to`: a param or temp of an **inlined** block, whose
      register is reused once its scope closes. It is live for offsets `from` to
      `to`, `to` exclusive, counted like `lines` (instruction indices).
    - `ctx`: a captured argument or temp, in that slot of the frame's Context. Its
      register is stale after the prologue copies it, so only the slot is named. An
      inlined block's temps are never captured, so a `ctx` entry has no range.

    `self` is not listed. When two live entries share a name (an inlined scope
    shadowing an outer one), the one listed later wins. A real block is its own method
    with its own `vars`; a name it reads from an enclosing frame is listed there, not
    in the block's entry.

custos refuses a map that is missing any field listed here. Any extra field is an
error in the writer, not a way to extend the format (see the root `CLAUDE.md`: the
format changes by editing this document and rebuilding both sides).

## 4. The fixture

`protocol/fixtures/image/` holds:

- `Fixture.nsl`: a small program with an entry class, a nested class, a class-side
  method, a block the compiler keeps as a block, a captured local and an inlined
  loop variable;
- `Fixture.oimg` and `Fixture.oimg.map.json`, compiled for the C6 layout
  (`oop_size` 4), because a board is the target custos actually loads. Regenerate both
  together, from the repo root:

  ```
  python3 nodus/nsl/tools/ns_compile_oevm.py protocol/fixtures/image/Fixture.nsl \
      protocol/fixtures/image/Fixture.oimg --format=oimg \
      --layout=nodus/oevm/zephyr/layout-esp32c6.json
  ```

**The writer's test** (`nodus/nsl/tools/test_image_map.py`, in the `nsl-tools-py` aux
gate) compiles `Fixture.nsl` and checks every §3 rule on the output. It also requires the
fixture program's own methods (`file` 0) to match the checked-in map by name, file and
lines, and the checked-in pair to agree with each other. It does not compare bytes: the
image is mostly kernel, so a byte compare would fail on every `kernel.nsl` edit. The
fixture is regenerated when this contract changes, not when the kernel does.

**The reader's test** (`custos/internal/debug/sourcemap_test.go`, #339) parses the
checked-in files and checks facts written down by hand: the line a method's statement is
on resolves to that method and offset, a block's line, a nested class's and a class
side's are refused by name, and changing a single byte of the `.oimg` makes the map be
refused. It also refuses a map missing any §3 field or carrying an extra one.

## 5. What this replaced (master before rete#337, `format_version` 10)

There were two sidecar files, `<name>.stmap` (`build_stmap`, `stmap.py`) and
`<name>.oimg.methods.json` (`OimgSerializer.method_name_map`). I measured both on
`46_debug_target.nsl` with the C6 layout: the image has 770 methods and the `.stmap`
has 634 functions.

1. **The two files disagree on what a block is called.** `.methods.json` names a block
   `Block_Kernel_Collection_block14>>value::`. `.stmap` names the same block
   `Kernel_Collection_block14>>value::`. If the `Block_` prefix is removed, every
   `.stmap` key appears in `.methods.json`. The other 136 methods have no lines.
2. **The `.stmap` gives the wrong file for most of its lines.** It names one `source`
   (the app's file), but most of its functions are kernel or Supervisor methods, and
   their line numbers count lines in `kernel.nsl` or `supervisor.nsl`.
   `Kernel>>rankOf:` maps to line 3688, which is a line in `kernel.nsl`, not in
   `46_debug_target.nsl`. `dbg.py` only works because it filters down to the entry
   class. The compiler already tags every AST node with its `file`.
3. **Nothing ties either sidecar to its image.** The profile reader (then
   `mprof_resolve.py`, now `custos profile`) could only say "wrong build?". The node's `platform buildId` (#74) identifies the *firmware*. Once
   #310 loads the program into its own flash region, one firmware build will run many
   programs, so `buildId` no longer identifies the program.
4. **The image does not record its word width.** `flags` is always 0, and the loader
   checks only the magic, the version, the registry version and the size. A 64-bit
   image and a 32-bit image of the same program carry the same values in all four
   fields, so nothing in the prefix lets a reader tell them apart.
5. **The two files are named differently.** One is `splitext(out) + ".stmap"`, the
   other is `out + ".methods.json"`.
6. **Stale claims.** `stmap.py` says its format matches st-berry's, so that the
   jast-gw viewer can read both. That viewer is from a lineage that has been retired.
   `method_name_map` says `qual` is "absent for a method no vtable binds", but every
   method now has a `qual`, so the `id` field it kept for naming blocks named nothing.
   And no method can have more than one `qual` (see `name` in §3), so the list was a
   mechanism for a case the compiler cannot produce.

The contract fixes each of these: one map instead of two (fixes 1, 5 and
6); a file for each method (fixes 2); the image hash in the map (fixes 3); the word
width in `flags` (fixes 4).

## 6. Decisions (David, 2026-10-04)

1. **One map instead of two.** `.stmap`, `.oimg.methods.json` and `stmap.py` are deleted;
   `custos debug` (`-map`, #339, which deleted `dbg.py`) and `custos profile` (#312,
   which deleted `mprof_resolve.py`) read the map. A Path A (`.c`) build writes no map.
2. **The SHA-256 of the `.oimg` is the program's identity.** The map carries it, and
   `custos debug` refuses a host image whose hash or size differs. Since #310 the node reports the
   hash of its program region, the way it reports `buildId` today.
3. **log2(oop_size) goes in `flags` bits 0–1, and the VM refuses an image of another
   width.** `format_version` 12 (#344), after #330's 11 merges (the merge queue is
   `329-c6-speed` → `198-custos-views` → #330). The fixture is regenerated then.
4. **The fixture is checked in, built for the C6 only.**
5. **Order:** the map, the fixture and this document merge first; the header change
   follows #330.
