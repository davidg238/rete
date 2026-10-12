# rete#37 L6 — frameless accessor sends (2026-10-12)

**Verdict: kept (Opt #13). A send to a getter, a setter or `^ self` is answered in `send_resolve` with no frame.
WeAct AWFY 0.941 (Richards 0.866, DeltaBlue 0.871, CD 0.903, List 0.894, Towers 0.938; rows with no accessor
1.000–1.006), STIC 0.984. On the C6 the same rows gain (Richards 0.867), but rows L6 does not touch move 2–8 % either
way (STIC 1.016, Towers 1.109): the C6 runs the interpreter from flash through its cache, so any edit shifts placement.
The WeAct, whose hot path is in SRAM, is the control. Next: the C6 hot path in SRAM.**

Why: the counting build (`../2026-10-12-37-M-counts/`) found 56 % of Richards' frames, 46 % of DeltaBlue's and
64–93 % of CD's, Towers' and List's run a method that is one slot access or one constant. Toit runs such sends with no
frame (`INVOKE_VIRTUAL_GET/SET`).

## The mechanism

- `CompiledMethod.num_arguments` narrows to 16 bits and `shape` takes the other half (offset 14 on 32-bit, 22 on the
  host). No other offset moves, and an image written before L6 reads as all-ordinary. David: the header bit, not a
  method-cache field.
- The compiler states the shape (`ns_compile_oevm.py method_shape`, stamped on every method at the end of `compile()`):
  getter `LD_SLOT r <- self.slot; RET r`, setter `ST_SLOT self.slot <- arg1; RET self`, or `RET self`. The slot index
  rides in the high byte.
- `oevm_method_check` proves the claim against the instructions at load, so the send trusts it.
- `send_resolve`'s bytecode branch calls `frameless_send` (vm.c) for a shaped method and returns the result as a
  primitive's, with no method to push. So `run_interpreter` is unchanged. A test at the loop's three send sites cost
  10–20 % on host rows with no accessor (Sieve 1.18×), even out of line: the L2 lesson. A breakpoint or step, a
  NULL/SMI receiver, an image object and a setter sent no argument take the frame. Patched methods are ordinary.
- Constant-returning methods (`LD_CONST r; RET r`, e.g. `isNil`) are not shaped: the constant's canonicalisation would
  need a case of its own. They are 5 % of CD's frames and 45 % of List's.

Frames on the board profile: Richards 1,103,063 → 481,330, CD 2,100,603 → 828,403, DeltaBlue 112,793 → 61,526.

## Numbers

One run per image (the boards repeat within 0.1 %). The c6b and weact-1-5 lanes agree with c6spare and weact-1-7:
AWFY 0.984 / 0.942, STIC 1.016 / 0.984. Revisions: base `0e321529` (master), L6 `81320370`.

See `tables.md` (per-row, c6spare and weact-1-7) and `host-table.md`. Host (opti7040, three runs each, master's VM and
image against L6's): geomean 0.972; NBody 0.828, Towers 0.913, List 0.926, Richards 0.943, CD 0.941, DeltaBlue 0.949.

Footprint: MCU text 45,109 B (+507 against the bank of 44,602), host 77,680 (+751). The ratchet passes.

## Files

`board/<lane>/` the console logs from `board_ab.sh`; `host/` a-N (master) and b-N (L6) host logs.
