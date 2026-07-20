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
| (c)  | ESP32-C6 | no radio (`prj_esp32c6.conf`)               | **BLOCKED** — see below       |
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

### Cell (c): ESP32-C6, no radio — BLOCKED, not a build-time-only measurement yet

**This cell could not be measured.** `tools/heap-ceiling.sh` invokes `zephyr/run.sh`
with `--model B` (unconditionally, matching the brief's Step 1 script). Model B loads
`zephyr/layout-esp32c6.json` to bake the `.oimg` blob
(`run.sh`'s `LAYOUT="$LAYOUT_JSON"` / `[ -f "$LAYOUT" ] || { ... exit 2; }` check), and
that file **does not exist yet**. The first probe failed immediately:

```
[bake]  TimerNode.nsl  (model B)
missing /home/david/workspace/tuvm/zephyr/layout-esp32c6.json — run the layoutdump app (stage 0) for esp32c6
```

Per the implementation plan
(`~/workspace/tuvm/docs/superpowers/plans/2026-07-20-esp32c6-second-target.md`),
`zephyr/layout-esp32c6.json` is explicitly **"generated at Task 4, not authored"** —
Task 4's Step 1 heading is literally "**This is the first step requiring hardware**": it
flashes the `layoutdump` app (already built at Task 2 into
`zephyr/layoutdump/build-c6/`) onto a real C6, reads struct-layout JSON back over the
UART-bridge console, and banks it. Task 3 (this task) comes *before* Task 4 in the
plan's sequence.

This directly conflicts with this task's brief, which states "**No hardware is
involved**" and lists cell (c) as one of "three cells are yours" to measure build-only.
That framing is correct for cells (a) and (b) — Model B for the WROOM-32 has always had
a `layout-esp32.json` — but does not hold for a *fresh* board whose ABI layout has never
been captured. The gap was not visible until the bisect actually ran, because nothing
about the C6 Kconfig or `run.sh`'s `--board esp32c6` plumbing hints at it beforehand.

Two options exist to close this cell without deviating further: (1) accept the
dependency and measure cell (c) as part of/after Task 4 once the real
`layout-esp32c6.json` lands, or (2) use `--model A` instead of B, which needs no layout
JSON. Option (2) was **not attempted**: this codebase's own standing guidance is to
avoid Model A entirely for iterative work — `run.sh`'s own comment about Model A
(`cc1 on the result needs ~4.9 GB RSS and ~50 min, and HAS OOM-killed the desktop`) and
this project's broader "Model B is the device default" convention rule it out as a
bisect substitute, and a heap-ceiling bisect run against Model A would be that
iterative use, many times over, on the very board whose stability under memory pressure
is what's being tested.

**Recommendation:** measure cell (c) as an add-on immediately after Task 4 produces
`zephyr/layout-esp32c6.json`, using this same `tools/heap-ceiling.sh` unmodified
(`bash tools/heap-ceiling.sh --board esp32c6`). No script change is needed once the
layout file exists.

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

The same caveat applies here in the other direction: cells (a) and (b) are both fully
measured and *are* a clean no-radio-vs-WiFi comparison, but neither can yet be compared
to the C6 at all, since no C6 cell has been measured. The headline "does the C6's
unified SRAM remove the ceiling" question is **not yet answered** by this task — it
awaits cell (c), which awaits Task 4.

## Harness notes

`tools/heap-ceiling.sh` is the brief's Step-1 script, essentially verbatim — its
`--pristine` / `--build-dir "build-heapprobe-$BOARD_ID"` handling was already correct
(decision 1: `--pristine` on every probe is load-bearing because `OEVM_HEAP_SIZE`
reaches the compiler via `target_compile_definitions` at CMake *configure* time, and
`--build-dir` is passed explicitly rather than via an exported `BUILD_DIR`, which
`run.sh`'s defaults line would silently clobber). No bug fix was needed in the script
itself.

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
