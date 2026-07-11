# nRF52840-DK on-target footprint + heap telemetry (rung-4)

Captured on the DK (`nrf52840dk/nrf52840`) with the recommended **config D**
(`OEVM_HEAP_SIZE=49152 METHOD_CACHE_SIZE=256 PRIM_CACHE_SIZE=256
VM_STACK_INIT_WORDS=4096 OEVM_MARK_STACK_SIZE=512`), image baked from
`zephyr/MeasureNodeDK.ns`. Recipe: `zephyr/README.md` "Build + flash + run".

Commit: tuvm `71d4ac4` (base; this measurement's image was baked from
`MeasureNodeDK.ns` on top of this commit)  ·  date 2026-07-10

## Static image (`west build -t ram_report` / `-t rom_report`)

| Region | Bytes  | % of nRF52840 |
| ------ | ------ | ------------- |
| ROM    | 289868 | 27.64 %       |
| RAM    | 118090 | 45.05 %       |

(nRF52840: 1 MB flash / 256 KB SRAM. The linker's own final memory-region
summary from `west build` agrees closely: FLASH 289892 B / 27.65 %, RAM
118160 B / 45.07 % — the small deltas vs. the footprint-tool totals above are
expected: `ram_report`/`rom_report` categorize regions slightly differently
than the linker's top-level region table.)

## Live heap telemetry under the allocation soak (prim 104 `heapStats`)

Captured directly off the flashed nRF52840-DK over `/dev/ttyACM0` at 115200
baud, immediately after `west flash --skip-rebuild --runner jlink` reset the
board and `MeasureNodeDK.ns` auto-ran at boot (no button press / host
interaction needed):

```
*** Booting nRF Connect SDK v3.2.4-4c3fc0d44534 ***
*** Using Zephyr OS v4.2.99-9673eec75908 ***
heapStats live 40092 / hw 49152 / free 9048 / gc 5 / size 49152
```

Invariant check on hardware: `hw (49152) <= size (49152)` ✓, `free (9048) <=
size (49152)` ✓, `gc (5) >= 1` ✓ — the DK's 49152-byte config-D arena forced
5 collections during the 3000-iteration soak, as expected for this arena size.

For context, a **host** run of the same baked image (large default host
arena, not the 49152-byte config-D arena) had earlier sanity-checked the
line's shape and confirmed `main:args:` compiles and executes cleanly:

```
heapStats live 493472 / hw 493472 / free 16283744 / gc 0 / size 16777216
```

(`gc 0` there was expected on the host's generous default arena; on the DK's
much smaller config-D heap the soak indeed forced `gc >= 1`, confirmed above.)

---

# Rung 7 — the OpenThread MTD image (`ThreadEchoNodeDK.ns`), 2026-07-11

The Layer-3 Thread node, same **config D** as above, built with
`-DCONF_FILE=prj_dk_thread.conf -DEXTRA_CONF_FILE=prj_dk_thread_local.conf`
(recipe: `zephyr/README.md` "Rung 7"). Commit: tuvm `341a5dd`.

| Region | Rung-4 loopback | **Rung-7 OpenThread MTD** | Delta |
| ------ | --------------- | ------------------------- | ----- |
| FLASH  | 289 868 B (27.64 %) | **436 300 B (41.94 %)** | **+146 432 B** |
| RAM    | 118 090 B (45.05 %) | **186 880 B (71.29 %)** | **+68 790 B** |

The delta is **OpenThread L2 + mbedTLS**, not the VM: the engine core is byte-identical
across the two builds (the `mcu-footprint` ratchet gate still reads text=29198 /
data+bss=69036, unmoved by this rung — it measures the engine, and rung 7 changed only
`zephyr/src/main.c`, the board Kconfig, and the node `.ns`).

**Headroom.** RAM at 71.29 % of the 256 KB part leaves ~75 KB free with the 48 KB VM
arena in place, so the MTD node fits comfortably; the arena remains the largest single
knob if a future rung needs to claw RAM back (`OEVM_HEAP_SIZE`: 49152 → 40960 → 32768).
**FTD would not fit** — this is the measured basis for the standing "MTD only, never
FTD" rule.

The nRF52840 **dongle** builds the same image against `prj_dongle_thread.conf`
(FLASH 42.55 % / RAM 74.83 % — the small increase is the USB-CDC stack): same SoC, same
256 KB, so the fit conclusion carries. See `docs/BOARD_PORTS.md`.
