# rete#37 — L4(c), `gen_tag` dropped; L3-lite (2026-10-12)

**Verdict: L4(c) kept (Opt #15). AWFY: C6 0.988, WeAct 0.981; STIC: C6 0.995, WeAct 0.978, on all four boards (each
pair agrees within 0.002). L3-lite is a dead end: level on the C6 (AWFY 1.000), +2.4 % AWFY and +3.7 % STIC on the
WeAct against L4(c).**

**L4(c)** (`880e3f62`): every frame was stamped with a fresh serial number (`++current_generation_tag`) that
`make_closure` and `OP_MK_CONTEXT` copied and nothing ever compared (send/frame design Q3). Deleted: the frame word
(the header is five words, `regs` at offset 5), `Closure.home_generation` (a Closure is 20 bytes on the boards),
`Context.home_generation`, the global. Every row that pushes frames gains about 1–2.5 % (DeltaBlue 0.976, Richards
0.988, CD 0.986 on the C6), integerMath 0.958/0.950. One row moves the other way on the C6 only: collectionIterate
1.028 (WeAct 0.972). MCU text 45,081 B (−28), host 77,595 (−85). Gates on msi: nsl 298/0, every aux gate PASS,
PORTA SYNC OK; Go green except `custos/internal/gate`, red on master too (#414).

**L3-lite** (`139812c6`, branch dropped): `push_frame` and the return read `vm_stack`, `vm_stack_ptr` and
`current_frame` once into locals, and the send sites take `ctx` from `push_frame`'s return instead of reloading
`current_frame`. The boards build with `-fno-strict-aliasing`, so each store through `ctx` reloads those globals; the
change removes the reloads without a new live register across the loop. The design's full L3 (the frame state in
the loop's registers, written back at every call-out) was not built: every frame-pushing send already calls
`send_resolve` out of line and every allocating opcode can reach the GC, so the write-backs would sit on nearly every
send. The C6 is level and the WeAct loses, the L2 pattern again: code moved in the loop is priced by each target's gcc.

Revisions: master `5781e09e` (L6 and the C6 SRAM hot path merged), L4(c) `880e3f62`, L3-lite `139812c6` on top of it.
Builds: `board_build.sh` on msi-7d43 (`~/rete-board`); runs: `board_ab.sh` on opti7040, c6spare (`/dev/ttyUSB1`), c6b
(`/dev/ttyUSB0`), WeActs usb 1-5 and 1-7, one run per image, revisions interleaved. `tables.md` (from
`../2026-10-12-37-send-frame/boardtable.py`); the console logs are in the board directories.

## The day on c6spare, and the other VMs

| AWFY (ms) | `f6f6e472` (morning) | master | + L4(c) | ÷ morning |
|---|--:|--:|--:|--:|
| Richards | 6949 | 5882 | 5810 | 0.836 |
| DeltaBlue | 863 | 634 | 619 | 0.717 |
| CD | 19368 | 14872 | 14670 | 0.757 |
| Towers | 605 | 449 | 443 | 0.732 |
| List | 394 | 337 | 333 | 0.845 |
| geomean (9 rows) | | 0.879 | 0.868 | |

STIC is level over the day (geomean 0.994; alloc 0.870). Against `../2026-10-12-37-c6-cross-vm/` (same VM as the
morning, `532a2391`): STIC geomean nsl ÷ Toit 0.63, ÷ MicroPython 0.75. Richards and DeltaBlue, the harness's own
sources, scaled by AWFY's ratio over the day (an estimate, not a run): Toit about 5.0× and 5.1× faster than nsl (from
6.0× and 7.1×); MicroPython 0.79 and 1.14 of nsl's time (from 0.94 and 1.59).
