# Stop, patch, restart, resume — desktop spike for rete#313

**Question (0.1 buildmap 5, `docs/overview.md`):** can a program stopped at a breakpoint have one method replaced,
the frame restarted, and resume into the new code?

**Answer: yes, on the desktop and on a C6 over its one cable**, for a method whose new version uses only what the
running image already holds. `nodus/nsl/tools/test_patch_restart.py` demonstrates it on the desktop and is the suite's
`patch-restart` aux gate; the board run is in [On the board](#on-the-board-c6spare-2026-10-06-2301-mst).

## What was built

| Piece | Where | What it does |
|---|---|---|
| Method override table | `nodus/oevm/src/dispatch.c` (`method_override_install`, `lookup`, `lookup_defining_vtable`) | An image's vtables are read-only (the blob is mapped `PROT_READ`; on a board it is flash), so a replacement goes in a RAM table consulted before a vtable's own bindings, at each level of the delegate walk. Installing flushes the method cache. `vtable:bind:to:` on an image vtable now uses it instead of aborting: one mechanism. GC roots it (`gc.c`). |
| `patch:` (mirror) | `nodus/oevm/src/vm.c` `primitive_patch` | Parses the patch text (one debug line, the same on every target; it was a file path until the board step), builds the CompiledMethod (malloc'd, never freed: a replaced method may still be running) and its closure, installs the override, and records old closure → new in `method_patches`. Refuses a line that is not the format, a cut one included, before installing anything. |
| `restart:` (mirror) | `vm.c` `primitive_restart_frame`, `apply_restart` | Restarts the stopped frame from its first instruction, into its method's replacement when it has one. Receiver and arguments are kept; every other register is cleared. The primitive only records the request. The interpreter applies it when the debugger hook returns, because only then is the stopped frame the top of the stack, so a replacement with more registers can grow it in place. All three hook sites reload `method` and `pc_end` from the frame afterwards. |
| `dbg:patch <text>`, `dbg:restart` | `nodus/nsl/lib/supervisor.nsl` | The two commands. The stop is kept; `dbg:continue` runs the new code. |
| `patch_method.py` | `nodus/nsl/tools/` | Compiles the old and the edited source. The old compile must reproduce the running image byte for byte, which makes its offsets the running image's. Takes the changed method from the new compile and re-encodes each constant through the old offsets. `compile_source` and `serialize_oimg` were extracted from `ns_compile_oevm.py`'s `main()` for it; a compile before and after the refactor is byte-identical, image and map. |

## The demonstration

`48_patch_restart_target.nsl` calls `report` twice, and `report` prints `value 1`. The harness breaks `report` on
its println line, after `v:: self value` has already run. There it sends `dbg:patch` with `report` from
`48_patch_restart_target_v2.nsl` (which prints `patched 42`, and needs 11 registers where the frame has 9), then
`dbg:restart` and `dbg:continue`:

```
stopped (dbg:paused break 7872 4), patched, restarted, resumed: ['patched 42', 'patched 42']
a new string refused: patch_method: refused: the running image holds no ('STRING', 'never seen ') (str:never seen )
```

The first line comes from the restarted frame; the second from the next send, which finds the override.
**Falsified:** with `apply_restart` disabled, the same run prints `value 1` then `patched 42`, and the harness fails.
The patch alone changes later sends; the restart is what changes the stopped frame.

## Limits (each refused by name, not silently wrong)

| Limit | Why | What would lift it |
|---|---|---|
| The new method may use only constants the running image holds: no new symbol, string, class or float | Selectors match by identity and symbols are not interned at runtime | RAM interning of symbols and strings |
| No method that makes a block | A block is its own CompiledMethod, a constant of its method | Patch the block methods alongside, by the same re-encoding |
| Only the stopped frame restarts, and only a method's frame | The frame must be the top of the stack to grow; a block's frame belongs to its home | Deeper frames need the C activations from `on:do:` and `ensure:` unwound |
| The old source must reproduce the running image exactly | It is how the old offsets are known | Have the map carry the offsets (the image map already names every method by offset) |
| The patched method has no blob offset | `blobOffsetOf:` answers nil, so breakpoints, `bt` and the source map do not see it | Give RAM methods a name the map can join on |
| A patch must fit one debug line | The VM reads a line into `platform.c`'s 512-byte buffer (`PRIM_pf_readLine`); `patch_method.py` refuses a longer patch by name (`LINE_BYTES`). This method's is 233 B | Several lines into one buffer, when a method that needs it exists |

## Cost, and a gate it turns red

Measured by the suite's footprint gate (`footprint(T0.3)`), against the run of the same suite on #367's branch the same
day:

| Measure | Before | With this spike | Banked |
|---|---|---|---|
| mcu text | 40871 (ratchet ok) | 42178 (**RATCHET FAIL**) | 40229 |
| mcu data+bss | 67820 | 68540 (+720: the two patch tables and the restart request) | 67804 |
| host text | 71942 (already red, #358) | 74641 | 69939 |

**Decided (David, 2026-10-06): hot patching is a debug facility, not production.** It is compiled out with the
other debugger hooks (`OEVM_NO_DEBUG_HOOKS`, which a standard Zephyr image defines), so a production image carries
none of it, and `vtable:bind:to:` on an image vtable aborts there as before. The footprint gate's builds do not define
that macro: it measures the hooks build, so the numbers above stand and the gate stays red until #358's re-bank.

## On the board (c6spare, 2026-10-06 23:01 MST)

The transport is the debug line itself: `dbg:patch <text>`, so the desktop and the board share one parser and the
file path is gone. Firmware: `OEVM_DEBUG_HOOKS=1 run.sh --board esp32c6 --node 48_patch_restart_target.nsl
--build-dir build-esp32c6-dbg313 --flash` (build-id `porta-v1-final-1466-g09b61e1-dirty-20261006T224950`, program
sha256 `141143b8…`, run.sh borrowing the cable from `rete-custos`). The patch came from `patch_method.py` with
`layout-esp32c6.json`; the old source reproduced the board's image byte for byte, so its offsets are the board's.
Everything was sent through custos, `POST /board/c6spare/debug/do`, with no other reader on the cable:

```
break 21 → dbg:ok break; continue → dbg:paused break 3952 4   (report, its println line)
dbg:patch oevm-patch 2 …   → patch: malformed / dbg:error patch   (refused, nothing installed)
dbg:patch oevm-patch 1 …   → dbg:ok patch
dbg:restart                → dbg:ok restart
clear 21; continue         → patched 42
                             patched 42
```

**Control (falsified), same image after a reset:** break, patch, clear, continue, without `dbg:restart`, prints
`value 1` then `patched 42`: the stopped frame finishes in the old code and the next send finds the replacement.

**Cost of the board step** (footprint gate, the hooks build): mcu text 42178 → 43244 (+1066 B), host text 74641 →
75214. The mcu build had compiled `patchFrom:` as a stub (no file I/O there); it now carries the text parser, which is
the price of patching a board at all. A standard image (`OEVM_NO_DEBUG_HOOKS`) carries none of it. Suite 290/0.

**The custos verb (D17), 2026-10-07 10:47 MST, same board and program:** `POST /board/c6spare/debug/patch?method=
PatchRestartTarget>>report` with the edited source as the body, through the service `rete-custos` (master `b9de042`).
A source that needs a new string was refused by name (`the running image holds no ('STRING', 'never seen ')`) with
the program still stopped; v2 then answered OK, `restart` and `continue` printed `patched 42` twice. The map now
carries its layout (`image.layout`), so the verb needs only the map and the two sources.