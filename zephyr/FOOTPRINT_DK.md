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
size (49152)` ✓, `gc (5) >= 1` ✓ — the DK's 49152-word config-D arena forced
5 collections during the 3000-iteration soak, as expected for this arena size.

For context, a **host** run of the same baked image (large default host
arena, not the 49152-word config-D arena) had earlier sanity-checked the
line's shape and confirmed `main:args:` compiles and executes cleanly:

```
heapStats live 493472 / hw 493472 / free 16283744 / gc 0 / size 16777216
```

(`gc 0` there was expected on the host's generous default arena; on the DK's
much smaller config-D heap the soak indeed forced `gc >= 1`, confirmed above.)
