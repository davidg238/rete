# ESP32-C6 heap-ceiling bisect — Task 3

`tools/heap-ceiling.sh` bisects the largest `OEVM_HEAP_SIZE` that **links**, board by
board and radio-config by radio-config. `gc.c:25` declares
`static oevm_align_cell oevm_heap_store[OEVM_HEAP_CELLS]`, sized from the
`OEVM_HEAP_SIZE` macro at compile time, so an oversized value fails at link with a
region-overflow diagnostic — the ceiling is a pure build-time property, discoverable
without a board attached. Grain: 4096 B (finer is noise).

## Results

| Cell | Board    | Radio config                              | max-linkable `OEVM_HEAP_SIZE` |
|------|----------|--------------------------------------------|-------------------------------|
| (a)  | WROOM-32 | WiFi (default `prj_esp32.conf`)             | **53248**                     |
| (b)  | WROOM-32 | no radio (WiFi block disabled, net stack kept) | **106496**                 |
| (c)  | ESP32-C6 | no radio (`prj_esp32c6.conf`)               | **319488**                    |
| (d)  | ESP32-C6 | OpenThread/IPv6 (`prj_esp32c6_thread.conf`) | pending Task 6                |

### Cell (a): WROOM-32 + WiFi — 53248

First failing diagnostic, probe 57344 (next grain step above the ceiling):

```
/home/david/zephyr-sdk-0.17.4/xtensa-espressif_esp32_zephyr-elf/bin/../lib/gcc/xtensa-espressif_esp32_zephyr-elf/12.2.0/../../../../xtensa-espressif_esp32_zephyr-elf/bin/ld.bfd: zephyr/zephyr_pre0.elf section `.dram0.noinit' will not fit in region `dram0_0_seg'
/home/david/zephyr-sdk-0.17.4/xtensa-espressif_esp32_zephyr-elf/bin/../lib/gcc/xtensa-espressif_esp32_zephyr-elf/12.2.0/../../../../xtensa-espressif_esp32_zephyr-elf/bin/ld.bfd: region `dram0_0_seg' overflowed by 3408 bytes
```

**Sanity check:** this is the documented, hardware-verified WROOM-32 ceiling
(`zephyr/run.sh`'s `esp32` profile default, and #53's ~816 B DRAM-spare figure). The
bisect landed on it exactly — validates the whole method.

### Cell (b): WROOM-32, no radio — 106496

Derived `prj_esp32.conf` by disabling only the six WiFi-driver / WiFi-mgmt lines
(`CONFIG_WIFI`, `CONFIG_WIFI_ESP32`, `CONFIG_NET_L2_WIFI_MGMT`, `CONFIG_NET_MGMT`,
`CONFIG_NET_MGMT_EVENT`, `CONFIG_NET_L2_ETHERNET`) on a scratch copy — see "Harness
notes" below. `CONFIG_NETWORKING`, `CONFIG_NET_SOCKETS`, `CONFIG_NET_UDP`,
`CONFIG_NET_TCP`, `CONFIG_NET_IPV4`, `CONFIG_NET_DHCPV4` and every net-buf pool line
were left untouched — the config still carries a full net stack, just no WiFi driver
or its ~40 KB ESP-WIFI system-heap reservation.

First failing diagnostic, probe 110592:

```
/home/david/zephyr-sdk-0.17.4/xtensa-espressif_esp32_zephyr-elf/bin/../lib/gcc/xtensa-espressif_esp32_zephyr-elf/12.2.0/../../../../xtensa-espressif_esp32_zephyr-elf/bin/ld.bfd: zephyr/zephyr_pre0.elf section `.dram0.noinit' will not fit in region `dram0_0_seg'
/home/david/zephyr-sdk-0.17.4/xtensa-espressif_esp32_zephyr-elf/bin/../lib/gcc/xtensa-espressif_esp32_zephyr-elf/12.2.0/../../../../xtensa-espressif_esp32_zephyr-elf/bin/ld.bfd: region `dram0_0_seg' overflowed by 1272 bytes
```

Gain over cell (a): 106496 − 53248 = 53248 B — coincidentally exactly double 53248 (not
a designed relationship; the arena floor and the driver-heap/net-buf savings just land
there on this part).

### Cell (c): ESP32-C6, no radio — 319488

The blocker recorded in the earlier draft of this section is closed: `zephyr/
layout-esp32c6.json` was generated from a real on-device `layoutdump` capture and
committed at `9076ba9`, so `zephyr/run.sh --board esp32c6 --model B` now bakes the
`.oimg` blob without hitting the missing-layout guard. Measured with the fixed
`tools/heap-ceiling.sh` (see "Harness notes" below):

```
bash tools/heap-ceiling.sh --board esp32c6
```

First failing diagnostic, probe 323584 (next grain step above the ceiling):

```
/home/david/zephyr-sdk-0.17.4/riscv64-zephyr-elf/bin/../lib/gcc/riscv64-zephyr-elf/12.2.0/../../../../riscv64-zephyr-elf/bin/ld.bfd: zephyr/zephyr_pre0.elf section `.dram0.bss' will not fit in region `sram0_0_seg'
/home/david/zephyr-sdk-0.17.4/riscv64-zephyr-elf/bin/../lib/gcc/riscv64-zephyr-elf/12.2.0/../../../../riscv64-zephyr-elf/bin/ld.bfd: region `sram0_0_seg' overflowed by 4216 bytes
```

**Sanity check:** 319488 is large enough on a 512 KB-SRAM part to be worth distrusting
on sight (binding constraint 6), so it was checked rather than published on faith.

- *Both ends were actually probed, not inferred.* The bisect's own log shows LO=319488
  as the last probe that **succeeded** and HI=323584 (LO+GRAIN, the very next 4 KB step)
  as the last probe that **failed** with a genuine region-overflow diagnostic — exactly
  the "reported ceiling links, next grain step doesn't" check the brief asks for, and it
  falls out of the bisect's normal convergence rather than needing a rerun.
- *The `--hi` fix caught the search range itself.* The new up-front HI=524288 probe
  failed (`overflowed by 211192 bytes`) before the bisect started narrowing, confirming
  524288 was genuinely out of reach and the result wasn't an artifact of a too-narrow
  default range silently reporting `HI`.
- *Cross-checked against the linker's own region size.* `zephyr_pre0.map`'s Memory
  Configuration table gives `sram0_0_seg` a fixed length of `0x6ad08` = 437512 bytes —
  i.e. of the C6's 512 KB (524288 B) unified HP SRAM, ~86.8 KB is reserved ahead of
  `user_sram_org` for the 2nd-stage-bootloader / IRAM-loader segment (see
  `soc/espressif/esp32c6/default.ld`'s `user_sram_end` comment), leaving 437512 B as the
  actual linkable region. 319488 / 437512 ≈ 73% of that region goes to the VM heap
  array; the rest (~118 KB) is kernel, net stack, stacks and other bss/data — the same
  shape as cell (b)'s split, just against a larger region. The overflow arithmetic at
  both the HI probe and the 323584 probe is self-consistent with that 437512 B region
  size to within a small (~100 B) alignment residual, not off by an order of magnitude.
  Nothing here suggests a misclassified non-capacity failure or a search that ran past
  a real boundary.
- 319488 is exactly 6× cell (a)'s 53248 and 3× cell (b)'s 106496. Flagged, not
  suppressed: cell (b)'s note above already records a coincidental exact-2× relationship
  to cell (a) on this same codebase, so a further round multiple is consistent with how
  this SoC's linker sections happen to fall at 4 KB grain rather than a sign of a bug —
  but it is exactly the kind of "too clean" number the brief warns to distrust, so it is
  called out explicitly here rather than passed over.

This is a **link** ceiling only (binding constraint 5) — it says nothing about whether
319488 boots on real C6 hardware; that confirmation is a separate, later on-device step
and is not claimed here.

### Cell (d): ESP32-C6 + OpenThread — pending Task 6

`prj_esp32c6_thread.conf` does not exist yet (by design — Task 6's deliverable). Not
attempted, per the brief's decision 3.

## Framing (verbatim, per the brief)

> The WROOM-32's documented 53248 is a **radio-loaded** figure. Quoting it against a
> no-radio C6 number overstates the C6's gain by roughly the WiFi driver heap (~40 KB)
> plus the net-buf pools. The no-radio pair is the clean memory-map comparison.
>
> The with-radio cells are **not like-for-like**: the WROOM-32 carries WiFi, the C6
> carries OpenThread, and their footprints differ. That column compares each board
> against its own usable radio, not against each other. Both are reported; neither is
> quoted alone.
>
> "No radio" never means "no sockets" — `src/udp.c` is compiled unconditionally, so every
> config carries a net stack (see Task 2b).
>
> These are **link** ceilings. A value that links is not necessarily a value that boots;
> boot confirmation for the C6 lands at Task 5.

Cells (a), (b) and (c) are now all measured, and the clean no-radio-vs-no-radio
comparison the framing calls for is (b) vs (c): WROOM-32 no-radio 106496 vs. C6 no-radio
319488 — the C6's unified SRAM links roughly **3×** more VM heap than the WROOM-32 in
the matched (no-radio) configuration. That is a **link**-ceiling comparison only (per
the framing's last bullet); it does not by itself say the C6 boots at 319488, and it
does not speak to the with-radio (WiFi vs. OpenThread) comparison, which still awaits
cell (d) at Task 6. The with-radio pair remains the "not like-for-like" column the
framing already warns about — reported separately, never quoted against cell (a) alone.

## Harness notes

`tools/heap-ceiling.sh` is the brief's Step-1 script, essentially verbatim — its
`--pristine` / `--build-dir "build-heapprobe-$BOARD_ID"` handling was already correct
(decision 1: `--pristine` on every probe is load-bearing because `OEVM_HEAP_SIZE`
reaches the compiler via `target_compile_definitions` at CMake *configure* time, and
`--build-dir` is passed explicitly rather than via an exported `BUILD_DIR`, which
`run.sh`'s defaults line would silently clobber).

A later code review, ahead of measuring cell (c), found and fixed four bugs in the
bisect logic itself (none in the `--pristine`/`--build-dir` plumbing above, which was
correct as noted): (1) `try()` deleted the failure log on the same path that pointed
`LAST_FAIL_MSG` at it, so the one diagnostic you need when something goes wrong was
gone — this had already bitten the C6 layout-JSON failure recorded in an earlier draft
of this file, which had to be recovered by re-running `run.sh` by hand; (2) any
non-capacity build failure (toolchain crash, OOM, missing prerequisite) at an interior
probe was indistinguishable from a genuine region overflow and would silently narrow
the bisect to a too-low ceiling — probes now classify their failure and abort the whole
run loudly if it isn't a recognized overflow/`will not fit` diagnostic; (3) `HI` was
trusted as known-bad but never probed, so a caller's `--hi` that actually links would
have been reported as "the" ceiling — `HI` is now probed up front and the run aborts if
it links; (4) the `MID` bisect step is only provably correct for GRAIN-aligned `--lo`/
`--hi`, so un-aligned bounds are now rejected outright rather than silently rounded (a
decision-critical tool should not quietly substitute a value the caller didn't ask
for). Cell (a) was re-run against the fixed script and still landed on exactly 53248
with the same first-failing diagnostic shape — the regression check the brief asks for.

Step 3's `sed`, as written in the brief, **was buggy** and was not transcribed:
`sed -i 's/^CONFIG_NET/# CONFIG_NET/; s/^CONFIG_WIFI/# CONFIG_WIFI/'` comments out every
`CONFIG_NET*` line, including `CONFIG_NETWORKING`, `CONFIG_NET_SOCKETS`,
`CONFIG_NET_UDP` and the net-buf pool block — which would produce an unbuildable
config, since `src/udp.c` and the Zephyr UDP backend link unconditionally (`dispatch.c`'s
`send()` probes `udp_object()` on every send). The actual derivation (done with a small
Python script rather than `sed`, for precision) commented out exactly the six
WiFi-driver / WiFi-mgmt lines (`CONFIG_WIFI`, `CONFIG_WIFI_ESP32`,
`CONFIG_NET_L2_WIFI_MGMT`, `CONFIG_NET_MGMT`, `CONFIG_NET_MGMT_EVENT`,
`CONFIG_NET_L2_ETHERNET`) and left `CONFIG_NETWORKING`, `CONFIG_NET_IPV4/TCP/UDP/
SOCKETS/DHCPV4`, and every net-buf pool line untouched.

To point `run.sh`'s hardcoded `esp32` profile (`CONF_FILE="prj_esp32.conf"`) at the
no-WiFi variant without editing the tracked file's *committed* content, the tracked
file's *working-tree* content was temporarily overwritten with the no-WiFi variant for
the duration of cell (b)'s bisect, then restored with `git checkout -- zephyr/
prj_esp32.conf` immediately afterward (verified byte-identical via `diff` against a
pre-swap backup). This mirrors `run.sh`'s own `restore_image()` trap pattern for
`src/solidified_image.c`. Nothing was ever staged or committed in the swapped state.
`zephyr/prj_esp32_nowifi.conf` was created as a scratch file for the derivation and
removed afterward, per the brief's Step 3.

## Timing

With a warm `ccache` (71.8% hit rate going in — this tree has been built repeatedly
across Tasks 1–2), each pristine probe took roughly 15 seconds rather than the "few
minutes" a cold-cache full Zephyr rebuild would cost. The brief's own comment
("~7 full rebuilds ... Slow but correct") anticipates a much longer wall-clock time than
was actually observed here; a cold-cache run (fresh checkout, no `ccache`) should expect
minutes per probe, not seconds. Cell (a) (8 probes, full default range 16384–524288) and
cell (c)'s aborted first probe ran in parallel; cell (a) completed in under 3 minutes.
Cell (b) (7 probes) ran afterward (sequentially, since it required exclusive use of
`prj_esp32.conf`) in a comparable time.
