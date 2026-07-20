# ESP32-C6 heap-ceiling bisect — Task 3

`tools/heap-ceiling.sh` bisects the largest `OEVM_HEAP_SIZE` that **links**, board by
board and radio-config by radio-config. `gc.c:25` declares
`static oevm_align_cell oevm_heap_store[OEVM_HEAP_CELLS]`, sized from the
`OEVM_HEAP_SIZE` macro at compile time, so an oversized value fails at link with a
region-overflow diagnostic — the ceiling is a pure build-time property, discoverable
without a board attached. Grain: 4096 B (finer is noise).

## Results

Task 5 adds the **boots** column below. It records an actual on-target boot, not an
inference from the link result — "links" and "boots" are different claims (a value that
links is not necessarily one that boots; see the framing section). Only cell (c) was
boot-tested in Task 5, which is scoped to the C6 (see "For context" in the task-5 brief);
cells (a)/(b)/(d) are left as `not boot-tested this task` rather than inferred, even
where indirect production evidence exists (e.g. cell (a)'s 53248 is the value
HttpServerNode/ThermalHttpNode already run at live on the WROOM-32 in production — see
`zephyr/run.sh`'s comment on the `esp32` profile default — but that is a different node,
board config and measurement methodology than this bisect, so it is noted here rather
than folded into the table as if it were the same claim).

| Cell | Board    | Radio config                              | max-linkable `OEVM_HEAP_SIZE` | boots? |
|------|----------|--------------------------------------------|-------------------------------|--------|
| (a)  | WROOM-32 | WiFi (default `prj_esp32.conf`)             | **53248**                     | not boot-tested this task (indirect production evidence only — see note above) |
| (b)  | WROOM-32 | no radio (WiFi block disabled, net stack kept) | **106496**                 | not boot-tested this task |
| (c)  | ESP32-C6 | no radio (`prj_esp32c6.conf`)               | **319488**                    | **yes — confirmed on hardware, Task 5. Boot ceiling = link ceiling (319488); no bisect-down was needed.** |
| (d)  | ESP32-C6 | OpenThread/IPv6 (`prj_esp32c6_thread.conf`) | **262144**                    | not boot-tested this task (link ceiling only; decision 7 does not require it — see below) |

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
  shape as cell (b)'s split, just against a larger region.

  **Corrected — the earlier "~100 B alignment residual" claim was wrong, and the truth
  is a stronger check.** Recomputing `region_size + overflow − H` from this document's
  own cited figures gives 124416 for H=524288 and 118144 for H=323584 — a **6272 B**
  gap, not ~100 B. The gap is not alignment noise: it is the **GC allocation bitmap**.
  `src/gc.c:33-34` declares

      static uint8_t heap_bitmap[OEVM_HEAP_SIZE / sizeof(oop) / 8u + 1u];

  as a **separate static array outside the arena**, one bit per 4-byte granule.
  `sizeof(oop)` is 4 on both 32-bit targets, so it is exactly `H/32`. Asking for
  `OEVM_HEAP_SIZE=H` therefore costs **`H + H/32` (1.03125×)** of static RAM — at
  H=319488 an extra 9984 B that naive accounting misses.

  With the bitmap included, `used(H) − H − H/32` is constant at **exactly 108032 bytes**
  across all four failing probes (524288, 397312, 331776, 323584). Constant to the byte
  across a 200 KB span is far better evidence that the bisect measured capacity — and
  only capacity — than the approximate residual originally claimed. Nothing here suggests
  a misclassified non-capacity failure or a search that ran past a real boundary.
- 319488 is exactly 6× cell (a)'s 53248 and 3× cell (b)'s 106496. Flagged, not
  suppressed: cell (b)'s note above already records a coincidental exact-2× relationship
  to cell (a) on this same codebase, so a further round multiple is consistent with how
  this SoC's linker sections happen to fall at 4 KB grain rather than a sign of a bug —
  but it is exactly the kind of "too clean" number the brief warns to distrust, so it is
  called out explicitly here rather than passed over.

This was, as of Task 3, a **link** ceiling only (binding constraint 5) — it said nothing
about whether 319488 boots on real C6 hardware; that confirmation was left to a separate,
later on-device step and was not claimed at the time. Task 5 (below) closes that gap.

#### Boot confirmation (Task 5)

`OEVM_HEAP_SIZE=319488 bash zephyr/run.sh --board esp32c6 --model B --node TimerNode.nsl
--pristine --flash` was flashed to the bench C6
(`/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_40:4C:CA:45:47:A4-if00`) and
observed over the UART-bridge console
(`/dev/serial/by-id/usb-Silicon_Labs_CP2102N_USB_to_UART_Bridge_Controller_ac2dce6f529ced118f43fd6be259fb3e-if00-port0`).

Build report at this heap size: `sram0_0_seg: 437504 B / 437512 B — 100.00% used` (8 B
spare) — the ceiling links right at the edge of the region, as expected of a
bisect-to-the-grain result.

Console output (verbatim, ROM banner and boot log elided to the load-bearing lines):

```
*** Booting Zephyr OS build v4.2.0 ***
TIMER OK
```

`TIMER OK` only prints after three 200 ms timer fires and an explicit cancel (see
`TimerNode.nsl`'s header comment), so this is a real multi-tick run, not an
immediate-crash false positive. **The boot ceiling equals the link ceiling: 319488
boots.** No downward bisection was required — decision 4's fallback path (bisect
down, report both numbers) was not needed on this hardware.

> ### 319488 is a ceiling, not a recommended default
>
> **Do not quote this as "the C6 gives you 312 KB of heap."** Three reasons, all
> visible in the numbers above:
>
> 1. **8 bytes of slack.** At 319488 the region is 100.00% full. That is precisely the
>    condition #53 describes on the WROOM-32 (99.58%, ~816 B spare) and calls a hard
>    ceiling with no headroom for any new static allocation. Shipping at the measured
>    maximum reproduces the problem this rung set out to escape, on a bigger part.
> 2. **It was measured under `TimerNode`**, which allocates almost nothing. A real
>    workload's live set is additional to the arena, not carved out of it.
> 3. **The GC is collect-when-full**, so heap high-water pegs to the arena ceiling
>    regardless of transient size. A 312 KB arena means rarer but proportionally longer
>    collections, and a high-water figure that always reads ~100% — losing the signal
>    that made the WROOM-32's ~52.8 KB observation useful.
>
> The honest reading of this table is that **the wall moved**, not that the wall should
> be leaned on. Picking a sensible C6 default is a separate decision informed by a real
> workload's live set; the ceiling only establishes how much room there is to choose
> within.

One incidental observation, present on every C6 flash in this task (Steps 1, 4 and 5
alike) and not a new finding: `esptool`'s "Image has only RAM segments visible. ROM
segments are hidden and SHA256 digest is not appended" note is followed at boot by a ROM
`SHA-256 comparison failed` / `Attempting to boot anyway` pair. This is a benign artifact
of the RAM-load image format Zephyr's `esp32c6` build produces here (no SHA appended for
esptool to check against), not a data-corruption signal — every boot proceeded normally
immediately afterward. Recorded for completeness since Task 5's own decision 2 asked
console output to be reported verbatim and this line appears on literally every boot.

### Cell (d): ESP32-C6 + OpenThread — 262144

**Why Thread, not WiFi.** `esp32c6_devkitc_hpcore.yaml` declares `netif:openthread` in its
`supported:` list and does **not** declare `wifi` — confirmed directly by reading the file
(`~/zephyr-esp32/zephyr/boards/espressif/esp32c6_devkitc/esp32c6_devkitc_hpcore.yaml`).
WiFi is out of scope on this board by decision. Thread is also IPv6-only, matching
`prj_esp32c6.conf`'s existing IPv6-only net stack, so the Thread layer adds a radio and
nothing else.

**Files added.**

`zephyr/boards/esp32c6_devkitc_esp32c6_hpcore.overlay` enables the radio node
`esp32c6_common.dtsi:85` ships disabled (confirmed at that exact line —
`ieee802154: ieee802154 { compatible = "espressif,esp32-ieee802154"; status = "disabled"; }`):

```
&ieee802154 {
	status = "okay";
};
```

The filename is the board-qualifier join (`esp32c6_devkitc/esp32c6/hpcore` →
`esp32c6_devkitc_esp32c6_hpcore.overlay`) — the brief's decision 1 is correct that CMake
caches overlay discovery, so every build in this cell used `-p always`/`--pristine`.

`zephyr/prj_esp32c6_thread.conf` copies only the networking block of
`zephyr/prj_dk_thread.conf` (the nRF52840-DK precedent), verbatim per the brief, and
deliberately omits that file's `CONFIG_NEWLIB_LIBC` and `CONFIG_FPU=y` (wrong here: the C6
uses picolibc and is `rv32imac_zicsr` — no FPU):

```
CONFIG_IEEE802154=y
CONFIG_IEEE802154_ESP32=y
CONFIG_NET_L2_OPENTHREAD=y
CONFIG_OPENTHREAD_MTD=y
CONFIG_OPENTHREAD_FTD=n
CONFIG_OPENTHREAD_MANUAL_START=y
CONFIG_OPENTHREAD_THREAD_STACK_SIZE=6240
CONFIG_NET_MAX_CONTEXTS=6
CONFIG_NET_PKT_RX_COUNT=8
CONFIG_NET_PKT_TX_COUNT=8
CONFIG_NET_BUF_RX_COUNT=16
CONFIG_NET_BUF_TX_COUNT=16
CONFIG_FLASH=y
CONFIG_FLASH_MAP=y
CONFIG_NVS=y
CONFIG_SETTINGS=y
CONFIG_SETTINGS_NVS=y
```

**A bug in the brief's own Step 3, found and worked around.** The brief's literal Step 3
command invokes `west build` directly:

```
west build -b esp32c6_devkitc/esp32c6/hpcore -p always -d build-c6-thread . \
  -- -DCONF_FILE=prj_esp32c6.conf -DEXTRA_CONF_FILE=prj_esp32c6_thread.conf \
     -DTUVM_MODEL=B
```

Run exactly as written, this **completed a link** — but overflowed `sram0_0_seg` by
**17,124,360 bytes**, not a plausible few-KB capacity result. Inspecting
`zephyr_pre0.map` showed the overflow was a single symbol:
`app/libapp.a(gc.c.obj)` contributed a **16,777,216-byte (16 MiB)** `.dram0.bss` entry —
`oevm_heap_store`, sized by `OEVM_HEAP_SIZE`. `src/gc.c:18-19` defines a *host* fallback
(`#ifndef OEVM_HEAP_SIZE #define OEVM_HEAP_SIZE (16u * 1024u * 1024u)`), and
`zephyr/CMakeLists.txt:93-95` only forwards `OEVM_HEAP_SIZE` into the build **from the
shell environment**, never as a bare `-D` (documented reason: NCS sysbuild swallows a
bare `-D`). The brief's raw `west build` invocation never exports the env var, so the
compile silently fell back to the 16 MiB host default. `zephyr/CMakeLists.txt:167-192`
has a Kconfig-gated safety net that forces sane defaults when the env is silent, but it
is scoped to `CONFIG_SOC_NRF52840` only — there is no equivalent for the C6, so this trap
is real and specific to this board. This is **not** an OpenThread/RISC-V build failure;
it is a missing-env-export bug in the brief's literal command, and it would have
misreported "OpenThread doesn't build on RISC-V" if taken at face value. Worked around by
using `zephyr/run.sh` (which exports the seven load-bearing knobs unconditionally) instead
of a bare `west build`, via the new `--conf-extra` flag described below. Re-run correctly:

```
bash zephyr/run.sh --board esp32c6 --model B --node TimerNode.nsl \
  --pristine --build-dir build-c6-thread --conf-extra prj_esp32c6_thread.conf
```

completed cleanly at the profile's provisional default `OEVM_HEAP_SIZE=53248`:

```
Memory region         Used Size  Region Size  %age Used
     sram0_0_seg:      221072 B     437512 B     50.53%
```

— i.e. **OpenThread built for RISC-V in this tree without incident**; the brief's
"unproven going in" line was the right caution, but the actual outcome is a clean build.

**Driver-presence verification (Step 4), on the exact build that produced the measured
ceiling** — not a separate throwaway build:

```
$ grep -E "^CONFIG_(IEEE802154_ESP32|NET_L2_OPENTHREAD|OPENTHREAD_MTD)=" \
    zephyr/build-heapprobe-esp32c6-prj_esp32c6_thread/zephyr/.config
CONFIG_OPENTHREAD_MTD=y
CONFIG_IEEE802154_ESP32=y
CONFIG_NET_L2_OPENTHREAD=y
```

All three present and `=y` — the overlay took effect and the driver is genuinely built
in, not silently absent.

**Harness change: `--conf-extra` added to both `zephyr/run.sh` and
`tools/heap-ceiling.sh`.** Neither script had a way to layer an `EXTRA_CONF_FILE` for an
arbitrary board before this task (`run.sh` only auto-detected the ESP32 WiFi
credentials overlay). `run.sh --conf-extra FILE.conf` now forwards
`-DEXTRA_CONF_FILE=FILE.conf` to `west build`, stacking (semicolon-joined) with the
existing WiFi-overlay auto-detection if both happen to apply. `heap-ceiling.sh
--conf-extra FILE.conf` forwards straight through to `run.sh`'s new flag on every probe,
and gives a `--conf-extra` bisect its own `build-heapprobe-$BOARD_ID-<conf-basename>`
build directory so it can never be confused with (or collide with) the no-radio bisect's
directory for the same board. `--pristine` (already unconditional on every probe) also
happens to satisfy decision 1's "-p always on the first build after adding the overlay"
requirement by construction — every probe is pristine, so there is no "first build"
distinction to get wrong.

**Measurement:**

```
bash tools/heap-ceiling.sh --board esp32c6 --conf-extra prj_esp32c6_thread.conf
```

Full bisect log:

```
[bisect] board=esp32c6 node=TimerNode.nsl range=[16384,524288] grain=4096 conf-extra=prj_esp32c6_thread.conf
  probe HI=524288 (must NOT link)
    fail @ 524288 (capacity): ... region `sram0_0_seg' overflowed by 269320 bytes
  probe 270336
    fail @ 270336 (capacity): ... region `sram0_0_seg' overflowed by 7432 bytes
  probe 143360
  probe 204800
  probe 237568
  probe 253952
  probe 262144
  probe 266240
    fail @ 266240 (capacity): ... region `sram0_0_seg' overflowed by 3208 bytes

RESULT board=esp32c6 node=TimerNode.nsl conf-extra=prj_esp32c6_thread.conf max-linkable-OEVM_HEAP_SIZE=262144
```

First failing diagnostic, probe 266240 (next grain step above the ceiling):

```
/home/david/zephyr-sdk-0.17.4/riscv64-zephyr-elf/bin/../lib/gcc/riscv64-zephyr-elf/12.2.0/../../../../riscv64-zephyr-elf/bin/ld.bfd: zephyr/zephyr_pre0.elf section `.dram0.bss' will not fit in region `sram0_0_seg'
/home/david/zephyr-sdk-0.17.4/riscv64-zephyr-elf/bin/../lib/gcc/riscv64-zephyr-elf/12.2.0/../../../../riscv64-zephyr-elf/bin/ld.bfd: region `sram0_0_seg' overflowed by 3208 bytes
```

Both ends were actually probed, not inferred: LO=262144 is the last probe that
**succeeded**, HI=266240 (LO+GRAIN) is the last probe that **failed** with a genuine
region-overflow diagnostic. The up-front HI=524288 probe also failed
(`overflowed by 269320 bytes`) before the bisect started narrowing, confirming the search
range was wide enough and 524288 was never trusted on faith.

**Comparison to cell (c) — the drop decision 4 predicted.** 319488 − 262144 = **57344
bytes (56 KB)** less linkable VM heap than the no-radio C6 config. This is the expected,
correct result, not a failure: cell (c)'s 319488 already links at `sram0_0_seg` 100.00%
used with 8 B spare (Task 5's boot-confirmation figure), so there was no slack for
OpenThread's ~6.2 KB dedicated thread stack, NVS/settings/flash-map machinery, and
OpenThread's own core library static state (`libopenthread-mtd.a`,
`crypto_platform.cpp.obj`, etc. all appear in the `.dram0.bss` map) to be absorbed
anywhere except by shrinking the arena. **Minor brief inaccuracy:** `prj_esp32c6_thread.conf`'s
own header comment (copied verbatim from the brief) describes the `NET_MAX_CONTEXTS` /
`NET_PKT_RX/TX_COUNT` / `NET_BUF_RX/TX_COUNT` block as "raised from prj_esp32c6.conf's
radio-less values now that a real radio exists" — checked directly against
`prj_esp32c6.conf`, and the five values (6/8/8/16/16) are byte-for-byte identical between
the two files, not raised. Harmless (the brief's own Step 2 code block gives the correct
literal values, which is what was transcribed), but the comment's claim doesn't match its
own numbers. 262144 is also a
suspiciously round number (256 KiB exactly) for a bisect-to-the-grain result — flagged in
keeping with this document's existing practice of calling out "too clean" numbers rather
than passing them over; nothing found suggests it is a search-convergence artifact rather
than a genuine boundary (the up-front HI probe and the immediate-next-grain HI=266240
probe both failed with real, distinct overflow diagnostics on either side of it).

**Not like-for-like with the WROOM-32's WiFi cell (a).** Per the brief and this
document's Framing section: 53248 (WiFi) and 262144 (Thread) are not directly
comparable — different boards, different radios, different footprints. Both are now
measured; neither should be quoted against the other as if they were.

**Boot not attempted, by design (decision 7).** This cell is a **link** ceiling like
(a)-(c). Booting Thread is explicitly not required for this task, and joining a Thread
network was explicitly out of scope (no border router on this bench). No flash attempt
was made for this cell; "boots?" is recorded as `not boot-tested this task`, consistent
with how (a) and (b) are recorded, not inferred from the link result.

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

All four cells are now measured. The clean no-radio-vs-no-radio comparison the framing
calls for is (b) vs (c): WROOM-32 no-radio 106496 vs. C6 no-radio 319488 — the C6's
unified SRAM links roughly **3×** more VM heap than the WROOM-32 in the matched
(no-radio) configuration. That was, as of Task 3, a **link**-ceiling comparison only (per
the framing's last bullet) that did not by itself say the C6 boots at 319488; Task 5's
"Boot confirmation" subsection above closes exactly that gap for cell (c) — 319488 is now
a confirmed **boot** ceiling on real C6 hardware, not just a link ceiling.

The with-radio pair — (a) WiFi 53248 vs. (d) OpenThread 262144 — is now also measured,
completing the table, but per the framing it is **not like-for-like** and is reported
here for exactly that reason, not to be quoted as "the C6 beats the WROOM-32 5×
with-radio." The two radios have different footprints on different boards; the pair
answers "how much does each board's own supported radio cost it," not "which radio is
cheaper." Cell (d) is a **link** ceiling only — per decision 7, booting Thread was not
required for this task and was not attempted (no border router on this bench to join
anyway), so unlike cell (c) there is no boot confirmation for cell (d) in this document.

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
