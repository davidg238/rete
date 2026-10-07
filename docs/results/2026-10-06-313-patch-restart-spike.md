# Stop, patch, restart, resume — desktop spike for rete#313

**Question (0.1 buildmap 5, `docs/overview.md`):** can a program stopped at a breakpoint have one method replaced,
the frame restarted, and resume into the new code?

**Answer, on the desktop: yes**, for a method whose new version uses only what the running image already holds.
`nodus/nsl/tools/test_patch_restart.py` demonstrates it and is the suite's `patch-restart` aux gate. The board is
the next step.

## What was built

| Piece | Where | What it does |
|---|---|---|
| Method override table | `nodus/oevm/src/dispatch.c` (`method_override_install`, `lookup`, `lookup_defining_vtable`) | An image's vtables are read-only (the blob is mapped `PROT_READ`; on a board it is flash), so a replacement goes in a RAM table consulted before a vtable's own bindings, at each level of the delegate walk. Installing flushes the method cache. `vtable:bind:to:` on an image vtable now uses it instead of aborting: one mechanism. GC roots it (`gc.c`). |
| `patchFrom:` (mirror) | `nodus/oevm/src/vm.c` `primitive_patch_from` | Reads a patch file, builds the CompiledMethod (malloc'd, never freed: a replaced method may still be running) and its closure, installs the override, and records old closure → new in `method_patches`. Desktop only. |
| `restart:` (mirror) | `vm.c` `primitive_restart_frame`, `apply_restart` | Restarts the stopped frame from its first instruction, into its method's replacement when it has one. Receiver and arguments are kept; every other register is cleared. The primitive only records the request. The interpreter applies it when the debugger hook returns, because only then is the stopped frame the top of the stack, so a replacement with more registers can grow it in place. All three hook sites reload `method` and `pc_end` from the frame afterwards. |
| `dbg:patch <file>`, `dbg:restart` | `nodus/nsl/lib/supervisor.nsl` | The two commands. The stop is kept; `dbg:continue` runs the new code. |
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
| Desktop only | The patch file is read with `fopen` | Send the patch over the cable, as `dbg:*` lines; a line is capped at 512 bytes (`platform.c`) |

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

## Next, for the board

The VM side is the same code on the C6. What is missing is the transport: a patch file is about 300 bytes of text
for this method, so it can arrive as one or a few cable lines into the same parser. Then the same gate on `c6spare`,
with a debug-hooks image (#311).
