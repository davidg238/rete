# rete#37 — the rest of L5: nothing ahead of the probe but the prim id (2026-10-12)

**Verdict: kept (Opt #18). AWFY C6 0.918, WeAct 0.926; STIC C6 0.941, WeAct 0.939 against master `336a2a22`
(`run1/`; each pair of boards within 0.001). MCU text 45,918 → 46,808 B (+890), inside the ratchet's 2 % (46,848),
not re-banked.** The rows that send gain 8–19 % (Richards 0.888 / 0.872, List 0.875 / 0.889, stringCompare 0.830 /
0.809, collectionWrite 0.890 / 0.871; C6 / WeAct); the rows that do not send (arrayWrite, floatMath, integerMath,
collectionIterate) are level.

What every send tested before the method cache probe after Opt #16, and where each test went (all in `send_resolve`
unless named; the loop is untouched):

| rev | change | AWFY C6 / WeAct | STIC C6 / WeAct |
|---|---|--:|--:|
| R1 `5a7b3d0b` | Symbol `=` (tuvm#75) answered inside the `=` prim's case, not gated ahead of every send; `platform_object()`, a cross-TU call on every prim send, resolved once | 0.977 / 0.984 | 0.979 / 0.986 |
| R2 `7c04f9a9` | the serial / UDP / mirror capabilities tested on the lookup miss: their vtables are empty, so every send to one misses, and none of their selectors is a prim name | 0.970 / 0.966 | 0.982 / 0.974 |
| R3 `e2fa47e3` | a send's selector constant is proved a Symbol at load (`oevm_inst_check`, for every opcode that names one: `OP_SEND`, `_IMPLICIT`, `_SUPER`, `OP_BLOCK_INVOKE`, the compare and `basicAt:` fast paths); the prim test is one load of `prim_id`; `send()` checks its C callers | 0.918 / 0.926 | 0.941 / 0.939 |

Cumulative, against master. R3 is the large step. Every compiled send's selector is an image literal, and
`obj_vtable` on a blob object is not a load: a blob range check, then `oimg_resolve`'s tag switch on the stored word
(`oimg.h`). That ran on every send to prove what the compiler always emits; the loader now proves it once.

**One behaviour change (R2).** A prim selector sent to a capability (`serial isNil`, `udp == x`) is now answered by the
prim; before, the capability's own dispatch aborted with its "doesNotUnderstand". A selector that is not a prim still
reaches the same abort. The one sharp edge: `basicAt:`, `basicAt:put:`, `basicSize` or `basicReplace…` sent to a
capability would read its (absent) slot 0 instead of aborting — a program error either way, no longer a loud one. The hole is older (any slotless
receiver, `platform` included, already reached it): #424.

**The footprint.** Per revision (`FOOTPRINT_CONFIG=mcu`, msi): master 45,918, R1 46,858, R2 46,474, R3 46,808. All of
R1's growth is `send_resolve` (5,622 → 6,562 B), the function where Opt #16 found `-Os` copying the primitive block for a
known receiver vtable. What R1 changed in that copy is not pinned down: it is not the Symbol arm itself, since putting
the arm in a `noinline` helper saved only 32 B of `send_resolve` and cost 76 B of helper (46,852). At R3: `send_resolve` +598, `send` +70 (its
check), the verifier the rest. The ratchet has 40 B of its 2 % left.

Gates (msi, `e2fa47e3`): nsl 298/0, every aux gate PASS (footprint, portability-32, boot-gc-asan included), PORTA SYNC
OK; Go: protocol and porta green, custos red only on #414 (red on master too).

Builds: `board_build.sh` on msi (from `~/rete-l6`), master and R1–R3; runs `board_ab.sh` on opti7040: c6spare, c6b,
WeActs usb 1-5 and 1-7, one run per image, interleaved. `tables.md` (`../2026-10-12-37-send-frame/boardtable.py`; C6
and WeAct 1-5 shown, c6b and WeAct 1-7 agree within 0.001).
