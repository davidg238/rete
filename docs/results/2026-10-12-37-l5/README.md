# rete#37 — L5, the `send_resolve` head (2026-10-12)

**Verdict: kept (Opt #16). AWFY: C6 0.985, WeAct 0.963; STIC: C6 0.987, WeAct 0.978 (`run2/`, each pair of boards
within 0.002). MCU text 45,070 B (−11).** Richards −2.0 % (C6) / −6.5 % (WeAct), DeltaBlue −2.6 / −4.6 %, List
−2.1 / −5.7 %, stringCompare −4.4 / −6.6 %, alloc −2.3 / −3.7 %; the rows that do not send are level.

The change, inside `send_resolve` (the loop is untouched):

- The one-shot capture of the Kernel instance for the raise prims (`g_kernel_instance`) was a test on every send
  (`g_kernel_instance == NULL && … strcmp(selector, "main:args:")`). Both launch sites hold the instance, so they set
  it: `oimg_loader.c` (both branches) and the compiler's `run_solidified_entry_point`, after the kernel's and the
  entry's init, where the per-send capture used to fire.
- The receiver's vtable is read once at the top. The BoundClass and `Symbol =` gates become single compares on it,
  and the method lookup reuses it.

**The footprint.** The first cut (`85525dba`, `run1/`) read the vtable as `IS_SMI(r) ? IntegerVtable : r ? vt : NULL`
at the top. Under the footprint gate's `-Os` Cortex-M4 build, gcc threaded the constant `IntegerVtable` through the
primitive block and copied it: `send_resolve` 4,778 → 5,622 B, MCU text 45,930 (the ratchet failed). Moving the SMI
arm to just before the lookup gives 4,762 B. The copy was also a fast path: the first cut measured AWFY C6 0.978,
WeAct 0.949; STIC C6 0.980, WeAct 0.969. **What the 844 B bought: about 1.5 % more on every board.** Merged without
it (David: fix the footprint, then merge); re-banking the ratchet for it is the open alternative.

**The one-word arena header** (`38c9b557`, branch `37-alloc-hdr`, `run1/`): `struct HeapHeader` from three words to
one (the size with the mark in bit 0; a free block's link in its payload), 8 B less per object on the boards. Speed
level on every board (1.000 ± 0.002): the board profile's 256 KB arena hardly ever collects and leaves Storage out, so
the saving cannot show as time here. Not merged with L5; it is a memory lever, decided separately.

Gates (msi, `aad8a67b`): nsl 298/0, every aux gate PASS (footprint included), PORTA SYNC OK. Revisions: master
`c075cab1` (L4(c) merged). Builds `board_build.sh` on msi; runs `board_ab.sh` on opti7040, c6spare, c6b, WeActs usb
1-5 and 1-7, one run per image, interleaved. `tables.md` (`../2026-10-12-37-send-frame/boardtable.py`).

## The day on c6spare

| AWFY (ms) | `f6f6e472` (morning) | + L4(c) | + L5 | ÷ morning |
|---|--:|--:|--:|--:|
| Richards | 6949 | 5809 | 5693 | 0.819 |
| DeltaBlue | 863 | 619 | 603 | 0.699 |
| CD | 19368 | 14670 | 14454 | 0.746 |
| Towers | 605 | 443 | 436 | 0.721 |
| List | 394 | 333 | 326 | 0.827 |
