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

> PENDING — captured in the DK flash step (controller-driven).

This dispatch was scoped to the SAFE (no-device-write) static-image portion
of Task 5 only; flashing the DK and capturing the live `heapStats` console
line over `/dev/ttyACM0` is held for a separate controller-driven step. A
**host** run of the same baked image (large default host arena, not the
49152-word config-D arena) sanity-checked the line's shape and confirmed
`main:args:` compiles and executes cleanly:

```
heapStats live 493472 / hw 493472 / free 16283744 / gc 0 / size 16777216
```

(`gc 0` here is expected on the host's generous default arena; the DK's much
smaller 49152-word config-D heap is expected to force at least one collection
during the 3000-iteration soak, i.e. `gc >= 1` on hardware — to be confirmed
in the flash step.)

Once flashed, the expected on-hardware line has the same shape:

```
heapStats live <N> / hw <N> / free <N> / gc <N> / size 49152
```

Invariants to confirm on hardware: `hw <= size`, `free <= size`, `gc >= 1`.
