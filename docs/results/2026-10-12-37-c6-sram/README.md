# rete#37 — the C6 hot path in SRAM (2026-10-12)

**Verdict: kept (Opt #14). AWFY 0.951, STIC 0.997 on both C6 chips (identical). Towers 0.847, CD 0.830, DeltaBlue
0.885, collectionWrite 0.947; no row worse than +1.5 %.**

Why: L6 (`../2026-10-12-37-l6/`) moved C6 rows it does not touch by 2–8 % (Towers +11 %), while the WeAct, whose hot
path runs from SRAM, moved them by under 1 %. The C6 was the last XIP target running its interpreter from flash
through the cache: `OEVM_HOT` placed code in RAM only on the ESP32/S2 (IRAM) and the RP2350 (`.ramfunc`).

The change: `zephyr/CMakeLists.txt` defines `OEVM_HOT_SECTION_IRAM` for `CONFIG_SOC_SERIES_ESP32C6` (default on,
`OEVM_IRAM_HOT=0` opts out); the C6 linker script collects `.iram.text` into SRAM, and RISC-V has no literal pools, so
the Xtensa `-mtext-section-literals` is not needed. `run.sh` passes the knob for the C6 and its placement report
recognises the C6's SRAM (`0x4080`–`0x4087`): `run_interpreter` moves from 0x42002bfa to 0x408037e2.

With L6 and SRAM together, Towers is 449 ms against 478 before L6: the same −6 % L6 showed on the WeAct, which flash
placement had hidden. Revisions: flash = master `bc3274c4` (L6 merged), SRAM = `a04ffea9`. One run per image.

See `tables.md`; `c6spare/`, `c6b/` hold the console logs.

The WeAct half of `board_build.sh` needs the uncommitted harness patch (`~/stic-harness.patch` on msi) to apply; this
change moved its context, so it was rebased (same 220 lines; the old copy is `~/stic-harness.patch.bak-20261012`).
