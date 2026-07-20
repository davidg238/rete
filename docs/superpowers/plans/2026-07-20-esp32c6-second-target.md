# ESP32-C6 Second Zephyr Target — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stand up `esp32c6_devkitc/esp32c6/hpcore` as a peer ESP32 target alongside the WROOM-32, and measure how much more VM heap its unified SRAM affords.

**Architecture:** `zephyr/run.sh` grows a `--board esp32|esp32c6` selector whose profile block owns every board-coupled wrong-by-default together — board target, Kconfig file, build dir, esptool chip/offset/flash-size/port, the Xtensa ROM-patch gate, the layout JSON path, and the env-knob defaults. The WROOM-32 profile reproduces today's behaviour exactly. A new `tools/heap-ceiling.sh` bisects the max linkable `OEVM_HEAP_SIZE` per board — build-only, no hardware.

**Tech Stack:** Zephyr 4.2.0 (`~/zephyr-esp32`, upstream — NOT NCS), RISC-V `rv32imac_zicsr`, esptool, bash, Python (the nsl bake toolchain).

**Spec:** `~/workspace/tuvm/docs/design/2026-07-20-esp32c6-second-target-design.md`
**Tracking issue:** #57. Motivated by #53; unlocks the debug leg of #56.

## Global Constraints

- **Repo:** `~/workspace/tuvm` only. This rung touches no nsl source, so `nsl-tuvm` / `nsl-tests` stay untouched. Work on branch **`feat/esp32c6-target`**, cut from `master`.
- **Zephyr workspace is `~/zephyr-esp32`**, not `~/ncs`. ESP32 is first-class upstream and absent from NCS's manifest.
- **The `esp32` profile must reproduce today's behaviour byte-for-byte.** This is the only step that can break working code. Gate it at Task 1 and never revisit.
- **`OEVM_IRAM_HOT` must NOT be exported for `esp32c6`.** `zephyr/CMakeLists.txt:141-145` gates it on `CONFIG_XTENSA` and raises `FATAL_ERROR` if forced on a non-Xtensa target.
- **`CONFIG_FPU` must NOT be set for `esp32c6`.** The ISA is `rv32imac_zicsr` (`esp32c6_common.dtsi:37`) — no `f`, no `d`. Soft-float is accepted.
- **`CONFIG_ESP32_USE_UNSUPPORTED_REVISION` is ESP32-silicon-specific.** It must not appear in any C6 Kconfig file.
- **Every config must carry a net stack.** `src/udp.c` and the Zephyr UDP/HTTP backends are in `target_sources` unconditionally, because `dispatch.c`'s `send()` fast path probes `udp_object()` on every send. Without `CONFIG_NET_SOCKETS` the app does not link. Discovered at Task 2; "no radio" therefore means net stack without a radio driver, never without sockets.
- **WiFi is out of scope on the C6** (decision, 2026-07-20). The board yaml declares `netif:openthread` and does not declare `wifi`. Thread/IPv6 is the C6's radio path.
- **The C6's linkable SRAM region is `sram0_0_seg` at 437512 B (~427 KB)**, not the DTS node's 512 KB — `soc/espressif/esp32c6/memory.h` and `default.ld:65` subtract icache and bootloader reservations.
- **C6 flash offset is `0x0`, not `0x1000`.** Flashing a C6 image at `0x1000` produces a silent boot loop, not an error.
- **Console on the UART bridge; flash on native USB.** Per #57: the native port re-enumerates on target reset and breaks continuous readers; the bridge chip never reboots. The board's default `zephyr,console = &uart0` is therefore already correct — **no console overlay.**
- **Pin ports by `/dev/serial/by-id/`,** never `ttyUSB0`/`ttyACM0`. The C6's CP210x now claims `/dev/ttyUSB0`, the exact node `run.sh` hard-pins for the WROOM-32.
- **Never `-g` a solidified image**, and **never run two nsl suites at once** (shared `/tmp`).
- **A newly-added devicetree overlay needs `-p always`** — CMake caches overlay discovery.

---

## File Structure

**Create:**
- `zephyr/prj_esp32c6.conf` — C6 boot-&-run Kconfig: net stack (no radio), no sensor, no FPU.
- `zephyr/prj_esp32c6_thread.conf` — the Task 6 OpenThread/IPv6 radio layer.
- `zephyr/boards/esp32c6_devkitc_esp32c6_hpcore.overlay` — enables the `ieee802154` node.
- `zephyr/layoutdump/prj_c6.conf` — layoutdump's C6 Kconfig (the existing `prj.conf` carries the ESP32-only revision opt-in).
- `tools/heap-ceiling.sh` — build-only bisect of max linkable `OEVM_HEAP_SIZE`.
- `zephyr/layout-esp32c6.json` — **generated at Task 4, not authored.**
- `docs/results/2026-07-20-c6-heap-ceiling.md` — the four-cell table.

**Modify:**
- `zephyr/run.sh` — the `--board` selector and profile blocks.

---

## Task 1: `run.sh` board profiles, with a byte-identical WROOM-32 gate

**Files:**
- Modify: `zephyr/run.sh`

**Interfaces:**
- Produces: `run.sh --board esp32|esp32c6` (default `esp32`, preserving every existing invocation). Profile variables consumed by later tasks: `BOARD`, `CONF_FILE`, `BUILD_DIR`, `LAYOUT_JSON`, `ESPTOOL_CHIP`, `ESPTOOL_OFFSET`, `ESPTOOL_FLASH_SIZE`, `ESPTOOL_PORT`, `ROM_PATCH` (`1`/`0`).

- [ ] **Step 1: Capture the pre-refactor baseline**

Build the current WROOM-32 image and bank its fingerprint before touching anything.

```bash
cd ~/workspace/tuvm
git checkout -b feat/esp32c6-target master
bash zephyr/run.sh --model B --node TimerNode.nsl --pristine
mkdir -p /tmp/c6-baseline
cp zephyr/build-esp32-B/zephyr/zephyr.bin   /tmp/c6-baseline/
cp zephyr/build-esp32-B/zephyr/.config      /tmp/c6-baseline/
md5sum /tmp/c6-baseline/zephyr.bin | tee /tmp/c6-baseline/bin.md5
```

Expected: a build that completes, and an md5 recorded.

- [ ] **Step 2: Add the `--board` flag and profile blocks**

In `zephyr/run.sh`, add `BOARD_ID=esp32` to the defaults line and a `--board` case to the arg loop:

```bash
MODEL=B NODE=TimerNode.nsl DO_FLASH=0 CAPTURE=0 PRISTINE=0 BUILD_DIR="" BOARD_ID=esp32
```

```bash
    --board)     BOARD_ID="$2"; shift 2 ;;
```

Add validation next to the existing `MODEL` check:

```bash
case "$BOARD_ID" in esp32|esp32c6) ;; *) echo "--board must be esp32 or esp32c6" >&2; exit 2 ;; esac
```

Then replace the single `BOARD=`/`ESPTOOL_PORT=` lines with a profile block. Place it
after `ROOT`/`NSL`/`ZDIR` are computed and before the env-knob exports:

```bash
# ---- board profiles -------------------------------------------------------
# Each profile owns EVERY board-coupled wrong-by-default together: the board
# target, Kconfig, build dir, layout JSON, the esptool triple, the port, and
# whether the Xtensa ROM linker patch applies. Adding a board means adding a
# block here and nothing else.
#
# Ports are pinned by /dev/serial/by-id/ rather than ttyUSB0/ttyACM0: those are
# allocation-order names, and the C6's CP210x now claims /dev/ttyUSB0 — the very
# node this script pinned for the WROOM-32 after esptool auto-detect clobbered the
# jag board on 2026-07-16. Same bug, re-armed. ESPTOOL_PORT still overrides.
case "$BOARD_ID" in
  esp32)
    BOARD="esp32_devkitc/esp32/procpu"
    CONF_FILE="prj_esp32.conf"
    LAYOUT_JSON="$ZDIR/layout-esp32.json"
    ESPTOOL_CHIP="esp32"
    ESPTOOL_OFFSET="0x1000"
    ESPTOOL_FLASH_SIZE="4MB"
    ROM_PATCH=1
    # WROOM-32 DRAM ceiling: see #53 (99.58%, ~816 B spare) and the long note below.
    : "${OEVM_HEAP_SIZE:=53248}"
    : "${BUILD_DIR:=build-esp32-$MODEL}"
    : "${ESPTOOL_PORT:=$(ls /dev/serial/by-id/*CP210*  2>/dev/null | head -1)}"
    : "${ESPTOOL_PORT:=/dev/ttyUSB0}"
    ;;
  esp32c6)
    BOARD="esp32c6_devkitc/esp32c6/hpcore"
    CONF_FILE="prj_esp32c6.conf"
    LAYOUT_JSON="$ZDIR/layout-esp32c6.json"
    ESPTOOL_CHIP="esp32c6"
    # RISC-V ESP32 parts moved the bootloader to 0x0. At 0x1000 the C6 boot-loops
    # SILENTLY — no error, no console output. This offset is load-bearing.
    ESPTOOL_OFFSET="0x0"
    ESPTOOL_FLASH_SIZE="8MB"
    # patch-esp32-rom-ld.sh rewrites esp32.rom.newlib-funcs.ld, an Xtensa artifact.
    # The C6 has a different ROM export table; the patch is meaningless here.
    ROM_PATCH=0
    # Provisional. Task 3 replaces this with the measured ceiling.
    : "${OEVM_HEAP_SIZE:=53248}"
    : "${BUILD_DIR:=build-esp32c6-$MODEL}"
    # Flash on the NATIVE USB port (303a:1001). Console lives on the UART bridge,
    # which stays enumerated across target resets — see #57.
    : "${ESPTOOL_PORT:=$(ls /dev/serial/by-id/*Espressif* 2>/dev/null | head -1)}"
    : "${ESPTOOL_PORT:=/dev/ttyACM0}"
    ;;
esac
```

- [ ] **Step 3: Make the knob exports profile-aware**

`OEVM_HEAP_SIZE` is now set by the profile, so delete the standalone
`: "${OEVM_HEAP_SIZE:=53248}"` line and leave the export.

**Move the existing multi-line comment explaining why 53248 is the WROOM-32 value into the
`esp32` profile block, verbatim.** It records the DRAM/WiFi-heap/net-buf reasoning and the
`/stats` high-water observation — that provenance is the most valuable text in the file and
must not be lost in the refactor. The plan's profile block shows a one-line placeholder
comment; replace that placeholder with the real comment from the current `run.sh`.

`OEVM_IRAM_HOT` must not leak to the C6:

```bash
export OEVM_HEAP_SIZE METHOD_CACHE_SIZE=256 PRIM_CACHE_SIZE=256 \
       VM_STACK_INIT_WORDS=4096 OEVM_MARK_STACK_SIZE=512
export OEVM_FLOAT_ARENA_SLAB_CELLS=512 OEVM_FLOAT_ARENA_SLAB_BUDGET=4
# OEVM_IRAM_HOT is Xtensa-only: zephyr/CMakeLists.txt:141-145 gates it on
# CONFIG_XTENSA and FATAL_ERRORs if it is set on a non-Xtensa target. Never
# export it for esp32c6 — unset any inherited value.
[ "$BOARD_ID" = esp32c6 ] && unset OEVM_IRAM_HOT
```

- [ ] **Step 4: Gate the ROM patch and generalise the build/flash calls**

Replace the unconditional patch call:

```bash
if [ "$ROM_PATCH" = 1 ]; then
  "$ROOT/tools/patch-esp32-rom-ld.sh"
fi
```

Replace the hardcoded layout path in the Model B branch:

```bash
  LAYOUT="$LAYOUT_JSON"
  [ -f "$LAYOUT" ] || { echo "missing $LAYOUT — run the layoutdump app (stage 0) for $BOARD_ID" >&2; exit 2; }
```

Replace the `CONF_FILE` in the build call:

```bash
( cd "$ZDIR" && west build -b "$BOARD" "${PRISTINE_ARG[@]}" -d "$BUILD_DIR" . \
    -- -DCONF_FILE="$CONF_FILE" -DTUVM_MODEL="$MODEL" "${BLOB_ARG[@]}" \
       "${EXTRA_CONF_ARG[@]}" )
```

Replace the flash call:

```bash
  echo "[flash] esptool $ESPTOOL_CHIP @ $ESPTOOL_OFFSET -> $ESPTOOL_PORT"
  "$ESPTOOL" --chip "$ESPTOOL_CHIP" --port "$ESPTOOL_PORT" --baud 921600 write_flash \
    --flash_mode dio --flash_freq 40m --flash_size "$ESPTOOL_FLASH_SIZE" \
    "$ESPTOOL_OFFSET" "$ZDIR/$BUILD_DIR/zephyr/zephyr.bin"
```

The WiFi overlay block is ESP32-specific today; scope it so a C6 build ignores it:

```bash
WIFI_CONF="$ZDIR/prj_esp32_wifi.local.conf"
EXTRA_CONF_ARG=()
if [ "$BOARD_ID" = esp32 ] && [ -f "$WIFI_CONF" ]; then
  EXTRA_CONF_ARG=(-DEXTRA_CONF_FILE=prj_esp32_wifi.local.conf)
  echo "[wifi]  overlay found -> $WIFI_CONF"
fi
```

- [ ] **Step 5: Verify the WROOM-32 path is unchanged**

```bash
cd ~/workspace/tuvm
bash zephyr/run.sh --model B --node TimerNode.nsl --pristine
diff /tmp/c6-baseline/.config zephyr/build-esp32-B/zephyr/.config && echo "KCONFIG IDENTICAL"
md5sum zephyr/build-esp32-B/zephyr/zephyr.bin
cat /tmp/c6-baseline/bin.md5
```

Expected: `KCONFIG IDENTICAL`, and matching md5s.

**If the md5s differ but `.config` is identical**, do not wave it through — check whether
Zephyr embedded a build timestamp before accepting:

```bash
cmp -l /tmp/c6-baseline/zephyr.bin zephyr/build-esp32-B/zephyr/zephyr.bin | head
```

A handful of differing bytes in one contiguous run is a timestamp; a scattered diff is a
real regression. Record which it was.

- [ ] **Step 6: Verify the C6 profile at least selects correctly**

This must fail at the *layout* check, proving the profile resolved and got as far as the
Model B bake — not at argument parsing.

```bash
bash zephyr/run.sh --board esp32c6 --model B --node TimerNode.nsl 2>&1 | tail -3
```

Expected: `missing /home/david/workspace/tuvm/zephyr/layout-esp32c6.json — run the layoutdump app (stage 0) for esp32c6`

- [ ] **Step 7: Commit**

```bash
cd ~/workspace/tuvm
git add zephyr/run.sh
git commit -m "feat(zephyr): run.sh --board profiles; esp32 path byte-identical

Each profile owns every board-coupled wrong-by-default together: board
target, Kconfig, build dir, layout JSON, esptool chip/offset/flash-size,
port, and the Xtensa ROM-patch gate.

Ports now pin by /dev/serial/by-id/ — the C6's CP210x claims /dev/ttyUSB0,
the node this script hardcoded for the WROOM-32 after the 2026-07-16 jag
clobber. Same bug, re-armed.

esp32 output verified unchanged: identical .config and zephyr.bin.

Refs #57"
```

---

## Task 2: C6 Kconfig and a layoutdump build that links

**Files:**
- Create: `zephyr/prj_esp32c6.conf`, `zephyr/layoutdump/prj_c6.conf`

**Interfaces:**
- Consumes: Task 1's `--board esp32c6` profile (`CONF_FILE=prj_esp32c6.conf`).
- Produces: a C6 Kconfig that links, and a layoutdump binary ready to flash at Task 4.

- [ ] **Step 1: Write `zephyr/prj_esp32c6.conf`**

```
# tuvm/zephyr — ESP32-C6 (C6-DevKitC-1) boot-&-run Kconfig.
#
# Selected with:
#   bash zephyr/run.sh --board esp32c6 ...
# built against the upstream-Zephyr workspace ~/zephyr-esp32 (NOT NCS).
#
# The C6 counterpart to prj_esp32.conf. Three deliberate differences from the
# WROOM-32 config, each load-bearing:
#
# 1. NO CONFIG_FPU. The C6 ISA is rv32imac_zicsr
#    (dts/riscv/espressif/esp32c6/esp32c6_common.dtsi:37) — no 'f', no 'd'
#    extension, so there is no FPU. Every VM float becomes a soft-float libcall.
#    Accepted: this target is chosen for memory, not float throughput.
#
# 2. NO CONFIG_ESP32_USE_UNSUPPORTED_REVISION. That opt-in exists for the bench
#    WROOM-32's rev-1.0 Xtensa silicon; it is meaningless on the C6.
#
# 3. NO networking. Task 6 layers prj_esp32c6_wifi.conf on top. WiFi is
#    quarantined because esp32c6_devkitc_hpcore.yaml does not declare 'wifi' in
#    its supported: list (though the HAL blobs and the driver Kconfig both
#    suggest it should work) — see #57.
#
# Console: the board DTS already routes zephyr,console = &uart0
# (esp32c6_devkitc_hpcore.dts:20), i.e. the CP210x UART bridge. That is what we
# want and needs no overlay. The bridge stays enumerated across target resets
# because the bridge chip never reboots, so a continuous log survives every
# reset — unlike the native USB port, which re-enumerates. Flashing uses the
# native port instead. See #57.
CONFIG_CONSOLE=y
CONFIG_SERIAL=y
CONFIG_UART_CONSOLE=y
CONFIG_STDOUT_CONSOLE=y

# libc: picolibc, matching the ESP32 config. The VM needs malloc/realloc/free,
# setjmp, snprintf and FP printf. The VM's own arena is the static
# oevm_heap_store (gc.c:25), sized by OEVM_HEAP_SIZE at build time.
CONFIG_PICOLIBC=y
CONFIG_PICOLIBC_IO_FLOAT=y
CONFIG_CBPRINTF_FP_SUPPORT=y
CONFIG_HEAP_MEM_POOL_SIZE=16384
CONFIG_MAIN_STACK_SIZE=8192
```

- [ ] **Step 2: Write `zephyr/layoutdump/prj_c6.conf`**

The existing `prj.conf` carries `CONFIG_ESP32_USE_UNSUPPORTED_REVISION=y`, which is
ESP32-only. A separate file keeps both boards buildable.

```
# layoutdump for the ESP32-C6 ABI. Throwaway app: compiles tools/dump_layout.c
# and prints the struct layout JSON over the console. Output is banked as
# zephyr/layout-esp32c6.json and fed to the Model B bake as --layout.
#
# Separate from prj.conf because that file sets
# CONFIG_ESP32_USE_UNSUPPORTED_REVISION=y for the bench WROOM-32's rev-1.0
# silicon — an ESP32-only symbol.
#
# RV32 field offsets and alignment differ from Xtensa, so layout-esp32.json
# CANNOT be reused. This app is why Task 4 is the first C6 flash rather than
# TimerNode.
CONFIG_SERIAL=y
CONFIG_UART_CONSOLE=y
CONFIG_STDOUT_CONSOLE=y
CONFIG_PICOLIBC=y
```

- [ ] **Step 3: Build layoutdump for the C6**

```bash
cd ~/workspace/tuvm/zephyr/layoutdump
west build -b esp32c6_devkitc/esp32c6/hpcore -p always -d build-c6 . -- -DCONF_FILE=prj_c6.conf
```

Expected: the build completes and prints a memory report. Note the reported `sramhp`
total — it should show roughly 512 KB, not the WROOM-32's 192 KB `dram0_0_seg`.

- [ ] **Step 4: Verify the main app's C6 Kconfig links too**

Model B needs a layout JSON we do not have yet, so use Model A's path only as far as
Kconfig resolution. Instead, confirm `prj_esp32c6.conf` is well-formed by configuring
the app without building the blob:

```bash
cd ~/workspace/tuvm/zephyr
west build -b esp32c6_devkitc/esp32c6/hpcore -p always -d build-c6-probe . \
  -- -DCONF_FILE=prj_esp32c6.conf -DTUVM_MODEL=A 2>&1 | tail -20
```

Expected: either a completed build, or a failure that is clearly about the *solidified
image* rather than Kconfig. Record which. A Kconfig error here (unknown symbol, missing
dependency) must be fixed before proceeding.

- [ ] **Step 5: Confirm no FPU and no IRAM_HOT leaked in**

```bash
grep -E "^CONFIG_FPU=" zephyr/build-c6-probe/zephyr/.config || echo "NO CONFIG_FPU — correct"
grep -rn "OEVM_HOT_SECTION_IRAM" zephyr/build-c6-probe/ | head -3 || echo "NO IRAM pinning — correct"
```

Expected: both report the "correct" message.

- [ ] **Step 6: Commit**

```bash
cd ~/workspace/tuvm
git add zephyr/prj_esp32c6.conf zephyr/layoutdump/prj_c6.conf
git commit -m "feat(zephyr): ESP32-C6 Kconfig + layoutdump C6 config

prj_esp32c6.conf drops CONFIG_FPU (ISA is rv32imac_zicsr — no FPU, so
soft-float), drops the ESP32-only rev-1.0 opt-in, and omits networking
(Task 6 layers it). Console stays on the board's default uart0 — the UART
bridge survives target resets, unlike the native USB port (#57), so no
overlay is needed.

layoutdump gets its own C6 conf because prj.conf carries the ESP32-only
CONFIG_ESP32_USE_UNSUPPORTED_REVISION.

Refs #57"
```

---

## Task 2b: Give `prj_esp32c6.conf` a net stack (unblocks Task 3)

**Files:**
- Modify: `zephyr/prj_esp32c6.conf`

**Why this task exists.** Task 2 committed `prj_esp32c6.conf` exactly as the plan specified
— with no networking — and then discovered the main app cannot link that way.
`zephyr/CMakeLists.txt` adds `src/udp.c`, `src/http.c`, `src/http_core.c` and their Zephyr
backends to `target_sources(app PRIVATE ...)` **unconditionally**, outside any board or
model branch, because `dispatch.c`'s `send()` fast path probes `udp_object()` on every
send. Without `CONFIG_NET_SOCKETS` the link fails on `zsock_*` undefined references. Every
Task 3 probe would have hit this before ever exercising a heap size.

**Interfaces:**
- Consumes: Task 2's `zephyr/prj_esp32c6.conf`.
- Produces: a C6 config the main app actually links against — the precondition for Task 3.

- [ ] **Step 1: Reproduce the link failure**

```bash
cd ~/workspace/tuvm/zephyr
source ~/zephyr-esp32/.venv/bin/activate
west build -b esp32c6_devkitc/esp32c6/hpcore -p always -d build-c6-netprobe . \
  -- -DCONF_FILE=prj_esp32c6.conf -DTUVM_MODEL=B 2>&1 | grep -E "undefined reference|error:" | head
```

Expected: `undefined reference to 'zsock_socket'` (and siblings). Confirm before changing
anything — if the link now succeeds, stop and report, because the premise has changed.

- [ ] **Step 2: Add the minimal net stack, no radio**

Append to `zephyr/prj_esp32c6.conf`:

```
# Net stack WITHOUT a radio driver.
#
# NOT optional, despite this target having no network peer to talk to:
# zephyr/CMakeLists.txt adds src/udp.c, src/http.c, src/http_core.c and their
# zephyr backends to target_sources UNCONDITIONALLY, because dispatch.c's send()
# fast path probes `receiver == udp_object()` on every send. Without
# CONFIG_NET_SOCKETS the app fails to link on zsock_* undefined references.
# The same reasoning is recorded in prj_esp32.conf's networking comment.
#
# IPv6 rather than IPv4: the C6's radio path is Thread (prj_esp32c6_thread.conf),
# and Thread is IPv6-only. Keeping the base config IPv6 means the Thread layer
# adds a radio and nothing else.
CONFIG_NETWORKING=y
CONFIG_NET_IPV6=y
CONFIG_NET_UDP=y
CONFIG_NET_SOCKETS=y
CONFIG_NET_CONFIG_SETTINGS=n

# Small pools: this config has no radio, so nothing but the loopback path uses
# them. The Thread layer raises them.
CONFIG_NET_MAX_CONTEXTS=6
CONFIG_NET_PKT_RX_COUNT=8
CONFIG_NET_PKT_TX_COUNT=8
CONFIG_NET_BUF_RX_COUNT=16
CONFIG_NET_BUF_TX_COUNT=16
```

- [ ] **Step 3: Verify it links**

```bash
cd ~/workspace/tuvm/zephyr
west build -b esp32c6_devkitc/esp32c6/hpcore -p always -d build-c6-netprobe . \
  -- -DCONF_FILE=prj_esp32c6.conf -DTUVM_MODEL=B 2>&1 | tail -25
```

Expected: either a completed link with a memory report, **or** a failure that is clearly a
*region overflow* (the default un-exported heap arena is large). A region overflow is an
acceptable pass here — it proves the `zsock_*` references resolved. Any remaining undefined
reference is a real failure.

Record the memory report verbatim if one is produced.

- [ ] **Step 4: Add the missing `.gitignore` entries**

Task 2 left untracked build dirs; Task 3 will add more.

```bash
cd ~/workspace/tuvm
cat >> .gitignore <<'EOF'
zephyr/build-c6-probe/
zephyr/build-c6-netprobe/
zephyr/build-esp32c6-*/
zephyr/build-heapprobe-*/
zephyr/layoutdump/build-c6/
EOF
git status --porcelain zephyr/ | head
```

Expected: no untracked build directories listed.

- [ ] **Step 5: Commit**

```bash
cd ~/workspace/tuvm
git add zephyr/prj_esp32c6.conf .gitignore
git commit -m "fix(zephyr): C6 conf needs a net stack to link at all

The design said prj_esp32c6.conf carries no networking. That is not
possible: zephyr/CMakeLists.txt adds src/udp.c and the zephyr UDP/HTTP
backends to target_sources unconditionally, because dispatch.c's send()
fast path probes udp_object() on every send. Without CONFIG_NET_SOCKETS
the link fails on zsock_* undefined references, and every Task 3 heap
probe would have hit it before measuring anything.

Adds the net stack with NO radio driver, IPv6 so the Thread layer adds
only a radio. Also gitignores the C6 build dirs.

Refs #57"
```


---

## Task 3: The heap-ceiling bisect — the headline deliverable

**Files:**
- Create: `tools/heap-ceiling.sh`, `docs/results/2026-07-20-c6-heap-ceiling.md`

**Interfaces:**
- Consumes: Task 1's `--board` profiles; Task 2's `prj_esp32c6.conf`.
- Produces: the four-cell max-linkable-`OEVM_HEAP_SIZE` table. **No hardware required.**

**Why this works without a board:** `gc.c:25` declares
`static oevm_align_cell oevm_heap_store[OEVM_HEAP_CELLS]`, sized from the
`OEVM_HEAP_SIZE` macro at compile time. An oversized value fails at **link** with a
region-overflow diagnostic, so the ceiling is discoverable by building alone.

- [ ] **Step 1: Write `tools/heap-ceiling.sh`**

```bash
#!/usr/bin/env bash
# tools/heap-ceiling.sh — bisect the largest OEVM_HEAP_SIZE that LINKS.
#
# gc.c:25 declares `static oevm_align_cell oevm_heap_store[OEVM_HEAP_CELLS]`,
# sized from the OEVM_HEAP_SIZE macro at compile time. Too large -> the linker
# reports a region overflow. So the ceiling is a pure build-time property and
# this script needs NO hardware.
#
# A value that links is not automatically a value that boots. This script
# reports the LINK ceiling; boot confirmation is a separate, on-device step.
#
# Usage: tools/heap-ceiling.sh --board esp32|esp32c6 [--node X.nsl] [--lo N] [--hi N]
set -euo pipefail

BOARD_ID=esp32 NODE=TimerNode.nsl LO=16384 HI=524288
while [ $# -gt 0 ]; do
  case "$1" in
    --board) BOARD_ID="$2"; shift 2 ;;
    --node)  NODE="$2";     shift 2 ;;
    --lo)    LO="$2";       shift 2 ;;
    --hi)    HI="$2";       shift 2 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GRAIN=4096          # bisect to the nearest 4 KB; finer is noise
LAST_FAIL_MSG=""

try() {   # $1 = candidate heap size; 0 = links, 1 = does not
  local size="$1" log
  log="$(mktemp)"
  # --build-dir, NOT an exported BUILD_DIR: run.sh's defaults line sets
  # BUILD_DIR="" unconditionally before its arg loop, so an exported value is
  # silently clobbered.
  #
  # --pristine on EVERY probe is load-bearing. OEVM_HEAP_SIZE reaches the
  # compiler through target_compile_definitions(... $ENV{OEVM_HEAP_SIZE}),
  # evaluated at CMake CONFIGURE time. An incremental build reuses the cached
  # definition, so probes 2..N would all silently measure probe 1's heap size
  # and the bisect would return a plausible-looking wrong number. Slow but
  # correct: ~7 full rebuilds for a 16 KB..512 KB range at 4 KB grain.
  if OEVM_HEAP_SIZE="$size" \
       bash "$ROOT/zephyr/run.sh" --board "$BOARD_ID" --model B --node "$NODE" \
       --pristine --build-dir "build-heapprobe-$BOARD_ID" \
       >"$log" 2>&1; then
    rm -f "$log"; return 0
  fi
  LAST_FAIL_MSG="$(grep -E "overflowed|region .* overflow|will not fit" "$log" | head -2 || true)"
  [ -n "$LAST_FAIL_MSG" ] || LAST_FAIL_MSG="(no overflow diagnostic — see $log)"
  echo "    fail @ $size: $LAST_FAIL_MSG" >&2
  rm -f "$log"; return 1
}

echo "[bisect] board=$BOARD_ID node=$NODE range=[$LO,$HI] grain=$GRAIN"
try "$LO" || { echo "FLOOR $LO does not even link — widen downward" >&2; exit 1; }

while [ $((HI - LO)) -gt $GRAIN ]; do
  MID=$(( (LO + HI) / 2 / GRAIN * GRAIN ))
  [ "$MID" -le "$LO" ] && break
  echo "  probe $MID"
  if try "$MID"; then LO="$MID"; else HI="$MID"; fi
done

echo
echo "RESULT board=$BOARD_ID node=$NODE max-linkable-OEVM_HEAP_SIZE=$LO"
echo "first-failing-diagnostic: $LAST_FAIL_MSG"
```

- [ ] **Step 2: Make it executable and sanity-check the floor**

```bash
cd ~/workspace/tuvm
chmod +x tools/heap-ceiling.sh
bash tools/heap-ceiling.sh --board esp32 --lo 16384 --hi 20480 2>&1 | tail -3
```

Expected: a `RESULT` line. This only proves the harness runs; the number is meaningless
at this range.

- [ ] **Step 3: Measure the WROOM-32, no WiFi**

`prj_esp32.conf` pulls in WiFi unconditionally, so a no-WiFi WROOM-32 cell requires
temporarily commenting the networking block. Do this on a scratch copy rather than
editing the tracked file:

```bash
cd ~/workspace/tuvm
cp zephyr/prj_esp32.conf /tmp/prj_esp32_nowifi.conf
sed -i 's/^CONFIG_NET/# CONFIG_NET/; s/^CONFIG_WIFI/# CONFIG_WIFI/' /tmp/prj_esp32_nowifi.conf
cp /tmp/prj_esp32_nowifi.conf zephyr/prj_esp32_nowifi.conf
```

Temporarily point the `esp32` profile at it, run the bisect, then restore:

```bash
bash tools/heap-ceiling.sh --board esp32 2>&1 | tail -3   # with CONF_FILE=prj_esp32_nowifi.conf
```

Record the number. Then `rm zephyr/prj_esp32_nowifi.conf` and restore the profile.

- [ ] **Step 4: Measure the remaining cells**

```bash
bash tools/heap-ceiling.sh --board esp32   2>&1 | tail -3   # WROOM-32 + WiFi (default prj_esp32.conf)
bash tools/heap-ceiling.sh --board esp32c6 2>&1 | tail -3   # C6, net stack, no radio
```

The C6-with-Thread cell is deferred to Task 6, since `prj_esp32c6_thread.conf` does not
exist yet. Record it as "pending Task 6" rather than leaving the cell blank.

- [ ] **Step 5: Write up the table**

Create `docs/results/2026-07-20-c6-heap-ceiling.md` with the four cells, each accompanied
by the first failing linker diagnostic. Include this framing verbatim, because the number
is easy to quote misleadingly:

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

- [ ] **Step 6: Commit**

```bash
cd ~/workspace/tuvm
git add tools/heap-ceiling.sh docs/results/2026-07-20-c6-heap-ceiling.md
git commit -m "feat(tools): heap-ceiling bisect + the four-cell table

Bisects the largest OEVM_HEAP_SIZE that links. Build-only: gc.c:25's
oevm_heap_store is a static array sized from the macro, so an oversized
value fails at link and the ceiling needs no hardware.

Table reports {WROOM-32, C6} x {no-WiFi, WiFi} because the documented
53248 is WiFi-loaded and quoting it against a no-WiFi C6 number would
overstate the gain by ~40 KB of WiFi driver heap plus net-buf pools.

Refs #57, #53"
```

---

## Task 4: Flash layoutdump on the C6 and capture the layout JSON

**Files:**
- Create: `zephyr/layout-esp32c6.json` (generated)

**Interfaces:**
- Consumes: Task 2's `build-c6` layoutdump binary.
- Produces: `zephyr/layout-esp32c6.json`, consumed by Task 5's Model B bake.

**This is the first step requiring hardware.**

- [ ] **Step 1: Confirm both ports and identify them by stable path**

```bash
ls -l /dev/serial/by-id/
```

Expected: two entries — a `*CP210*` (UART bridge, console) and an `*Espressif*` /
`*JTAG*` (native USB, flashing). Record both paths.

- [ ] **Step 2: Start a console reader on the UART bridge**

In a separate terminal, so the log is running *before* the flash completes:

```bash
python3 -c "
import serial,sys
p=sorted(__import__('glob').glob('/dev/serial/by-id/*CP210*'))[0]
s=serial.Serial(p,115200,timeout=1)
sys.stderr.write('reading %s\n'%p)
while True:
    d=s.read(4096)
    if d: sys.stdout.write(d.decode('utf8','replace')); sys.stdout.flush()
" | tee /tmp/c6-layoutdump.log
```

- [ ] **Step 3: Flash layoutdump**

```bash
cd ~/workspace/tuvm
ESPTOOL="$HOME/.espressif/python_env/idf5.4_py3.12_env/bin/esptool.py"
PORT="$(ls /dev/serial/by-id/*Espressif* | head -1)"
"$ESPTOOL" --chip esp32c6 --port "$PORT" --baud 921600 write_flash \
  --flash_mode dio --flash_freq 40m --flash_size 8MB \
  0x0 zephyr/layoutdump/build-c6/zephyr/zephyr.bin
```

Expected: `Hash of data verified.` **Note the `0x0` offset** — `0x1000` silently
boot-loops on the C6.

- [ ] **Step 4: Capture and bank the JSON**

The console log now holds the layout JSON. Extract it:

```bash
cd ~/workspace/tuvm
python3 - <<'PY'
import json, re
raw = open('/tmp/c6-layoutdump.log').read()
start = raw.index('{'); depth = 0
for i, c in enumerate(raw[start:], start):
    depth += (c == '{') - (c == '}')
    if depth == 0:
        blob = raw[start:i+1]; break
obj = json.loads(blob)                      # fails loudly if the capture is truncated
open('zephyr/layout-esp32c6.json','w').write(json.dumps(obj, indent=2) + '\n')
print('oop_size', obj['oop_size'], 'structs', len(obj['structs']))
PY
```

Expected: valid JSON written, `oop_size 4`.

- [ ] **Step 5: Confirm the RV32 layout actually differs from Xtensa**

If these are identical, either the wrong binary was flashed or the capture came from the
wrong board — investigate before trusting it.

```bash
cd ~/workspace/tuvm
diff <(python3 -c "import json;d=json.load(open('zephyr/layout-esp32.json'));print('\n'.join(f'{k} {v[\"size\"]}' for k,v in sorted(d['structs'].items())))") \
     <(python3 -c "import json;d=json.load(open('zephyr/layout-esp32c6.json'));print('\n'.join(f'{k} {v[\"size\"]}' for k,v in sorted(d['structs'].items())))") \
  && echo "WARNING: layouts identical — verify the right board was flashed"
```

Record whether any struct sizes differ, and which. `struct Float` is the one to watch:
the design predicts it stays 16 B under the RISC-V ilp32 ABI.

- [ ] **Step 6: Commit**

```bash
cd ~/workspace/tuvm
git add zephyr/layout-esp32c6.json
git commit -m "feat(zephyr): bank layout-esp32c6.json from on-device layoutdump

Captured from the C6 over the UART-bridge console after flashing
layoutdump at offset 0x0 via the native USB port.

RV32 field offsets and alignment differ from Xtensa, so layout-esp32.json
cannot be reused — this is why layoutdump is the first C6 flash rather
than TimerNode.

Refs #57"
```

---

## Task 5: TimerNode on the C6 — the bench gate

**Files:**
- Create: `zephyr/FloatSmokeNode.nsl`
- Modify: none

**Interfaces:**
- Consumes: Task 4's `layout-esp32c6.json`; Task 3's measured ceiling.
- Produces: the rung's bench gate — a running VM on RISC-V.

- [ ] **Step 1: Bake and flash TimerNode for the C6**

```bash
cd ~/workspace/tuvm
bash zephyr/run.sh --board esp32c6 --model B --node TimerNode.nsl --pristine --flash
```

Expected: bake, build, and `Hash of data verified.`

- [ ] **Step 2: Watch the console for ticks**

With the UART-bridge reader from Task 4 Step 2 still running:

Expected: periodic timer-tick output, matching what `TimerNode` prints on the WROOM-32.

**If output stops immediately after the first line**, that is the forced-ROM-newlib
signature the WROOM-32 gave (see `prj_esp32.conf`'s long comment). The C6 would then need
its own equivalent of `tools/patch-esp32-rom-ld.sh`. Record the exact last line printed —
it identifies which libc call faulted.

**If there is no output at all**, check the flash offset was `0x0` and that the reader is
on the CP210x path, not the Espressif one.

- [ ] **Step 3: Write the float smoke test**

`TimerNode` boxes no floats, so soft-float and the float-arena knobs are otherwise
unexercised. This node exists specifically to exercise them.

Create `zephyr/FloatSmokeNode.nsl`. **Model its shape on `zephyr/TimerNode.nsl`** —
`packageUsing: manifest`, platform arriving as the `main:` argument, an `expect:` block in
the header comment, `println` for output, and `^ 0` to return. Float idioms are taken from
`nsl-tests/05_somns_benchmarks/05_nbody.nsl` (`0.0 asFloat`, `1 to: N do:`):

```
(*
FloatSmokeNode — exercises the boxed-Float path on a target with NO FPU.

The C6 ISA is rv32imac_zicsr: no 'f', no 'd' extension, so every float
operation here is a soft-float libcall. This node exists because TimerNode
(the bench gate) boxes no floats at all, leaving both soft-float AND the
float-arena knobs (OEVM_FLOAT_ARENA_SLAB_CELLS / _SLAB_BUDGET) completely
unexercised on this target.

The arena knobs are carried over from Xtensa on the PREDICTION that struct
Float stays 16 B under the RISC-V ilp32 ABI. Task 4 confirmed the size, but
size is not the same as the arena behaving correctly under churn — hence a
loop that boxes several hundred intermediates rather than a single add.

Grants no capability, so no `net:` header. No radio, no timer, no hardware.
expect:
float-smoke ok
*)
class FloatSmokeNode packageUsing: manifest = (
| sum ::= 0.0 asFloat. |
) (
  public main: platform args: args = (
    1 to: 200 do: [ :i | sum:: sum + (i asFloat / 3.0) ].
    ('float-smoke sum=', sum printString) println.
    'float-smoke ok' println.
    ^ 0
  )
) : (
)
```

**Verify it on the host before flashing it.** The device loop is slow and a syntax error
should not cost a flash cycle:

```bash
python3 ~/workspace/nsl-tuvm/tools/run-one.py ~/workspace/tuvm/zephyr/FloatSmokeNode.nsl
```

Expected: `float-smoke sum=` with a plausible value (~6700) and `float-smoke ok`. If the
dialect differs from what is written above, **fix it against `TimerNode.nsl` and the
nbody benchmark rather than guessing** — this snippet is derived from those sources, not
copied from a compiled node.

- [ ] **Step 4: Run the float smoke test on the C6**

```bash
cd ~/workspace/tuvm
bash zephyr/run.sh --board esp32c6 --model B --node FloatSmokeNode.nsl --pristine --flash
```

Expected on the console: a `float-smoke sum=` line with a plausible value (~6700) and
`float-smoke ok`.

**If it aborts with "VM Float Arena Error: Out of Memory"**, the carried-over
`OEVM_FLOAT_ARENA_SLAB_CELLS=512` / `BUDGET=4` do not suit the C6 and need re-tuning —
this is exactly the risk the design flagged. Record the failing values.

- [ ] **Step 5: Confirm the measured heap ceiling actually boots**

Re-flash `TimerNode` at the Task 3 ceiling:

```bash
cd ~/workspace/tuvm
OEVM_HEAP_SIZE=<task-3-c6-ceiling> bash zephyr/run.sh --board esp32c6 --model B \
  --node TimerNode.nsl --pristine --flash
```

Expected: ticks, as in Step 2. If it links but does not boot, bisect downward and record
**both** numbers — the reported ceiling is the largest that *boots*.

- [ ] **Step 6: Update the results doc with boot confirmation**

Add a "boots" column to `docs/results/2026-07-20-c6-heap-ceiling.md` distinguishing the
link ceiling from the boot ceiling for each cell measured on hardware.

- [ ] **Step 7: Commit**

```bash
cd ~/workspace/tuvm
git add zephyr/FloatSmokeNode.nsl docs/results/2026-07-20-c6-heap-ceiling.md
git commit -m "feat(zephyr): TimerNode + float smoke on ESP32-C6 (bench gate)

TimerNode runs on RISC-V under Model B against layout-esp32c6.json.
FloatSmokeNode exists because TimerNode boxes no floats, leaving
soft-float and the float-arena knobs otherwise unexercised on a part with
no FPU.

Results doc now separates the link ceiling from the boot ceiling.

Refs #57"
```

---

## Task 6: Thread / IPv6 on the C6

**Files:**
- Create: `zephyr/prj_esp32c6_thread.conf`, `zephyr/boards/esp32c6_devkitc_esp32c6_hpcore.overlay`

**Interfaces:**
- Consumes: Task 5's working C6 VM; Task 2b's net stack.
- Produces: the fourth table cell (C6 with radio) and a Thread-capable C6 image.

**Why Thread and not WiFi.** `esp32c6_devkitc_hpcore.yaml` declares `netif:openthread` in
its `supported:` list and does **not** declare `wifi`. Thread is the board's supported
radio path, it is IPv6-only (which Task 2b's base config already matches), and there is an
in-repo precedent to copy from. WiFi is out of scope on the C6 by decision (2026-07-20).

**Unproven going in:** OpenThread on RISC-V in this tree. The driver
(`drivers/ieee802154/ieee802154_esp32.c`), the Kconfig (`CONFIG_IEEE802154_ESP32`) and the
DTS node all exist — none of that is a green link.

- [ ] **Step 1: Write the devicetree overlay**

`esp32c6_common.dtsi:85` ships the radio node disabled. Create
`zephyr/boards/esp32c6_devkitc_esp32c6_hpcore.overlay`:

```
/* Enable the C6's built-in 802.15.4 radio for OpenThread.
 *
 * esp32c6_common.dtsi:85 declares:
 *     ieee802154: ieee802154 { compatible = "espressif,esp32-ieee802154";
 *                              status = "disabled"; };
 * CONFIG_IEEE802154_ESP32 depends on DT_HAS_ESPRESSIF_ESP32_IEEE802154_ENABLED,
 * so the driver does not build until this node is enabled.
 *
 * Filename is derived from the board qualifiers (esp32c6_devkitc/esp32c6/hpcore
 * -> esp32c6_devkitc_esp32c6_hpcore.overlay). CMake CACHES overlay discovery, so
 * a newly-added overlay needs -p always on the first build after adding it.
 */
&ieee802154 {
	status = "okay";
};
```

- [ ] **Step 2: Write `zephyr/prj_esp32c6_thread.conf`**

Copy only the networking block of the in-repo precedent `zephyr/prj_dk_thread.conf`. **Do
not copy the whole file** — it also sets `CONFIG_NEWLIB_LIBC` and `CONFIG_FPU=y`, both
wrong for the C6 (we use picolibc, and the C6 has no FPU).

```
# OpenThread / IPv6 radio layer for the C6, applied on top of prj_esp32c6.conf
# via EXTRA_CONF_FILE.
#
# Thread rather than WiFi: esp32c6_devkitc_hpcore.yaml declares netif:openthread
# in its supported: list and does NOT declare wifi. Thread is also IPv6-only,
# which prj_esp32c6.conf already matches, so this layer adds a radio and nothing
# else.
#
# Values are the networking block of zephyr/prj_dk_thread.conf (proven on the
# nRF52840 DK). That file's CONFIG_NEWLIB_LIBC and CONFIG_FPU=y are deliberately
# NOT copied: the C6 uses picolibc and has no FPU (rv32imac_zicsr).
#
# Requires boards/esp32c6_devkitc_esp32c6_hpcore.overlay to enable the
# ieee802154 node — the driver does not build otherwise.
CONFIG_IEEE802154=y
CONFIG_IEEE802154_ESP32=y
CONFIG_NET_L2_OPENTHREAD=y
CONFIG_OPENTHREAD_MTD=y
CONFIG_OPENTHREAD_FTD=n
CONFIG_OPENTHREAD_MANUAL_START=y
CONFIG_OPENTHREAD_THREAD_STACK_SIZE=6240

# Raised from prj_esp32c6.conf's radio-less values now that a real radio exists.
CONFIG_NET_MAX_CONTEXTS=6
CONFIG_NET_PKT_RX_COUNT=8
CONFIG_NET_PKT_TX_COUNT=8
CONFIG_NET_BUF_RX_COUNT=16
CONFIG_NET_BUF_TX_COUNT=16

# Thread stores its network credentials in settings; NVS backs them.
CONFIG_FLASH=y
CONFIG_FLASH_MAP=y
CONFIG_NVS=y
CONFIG_SETTINGS=y
CONFIG_SETTINGS_NVS=y
```

- [ ] **Step 3: Build with Thread**

`-p always` is required — the overlay is newly added and CMake caches overlay discovery.

```bash
cd ~/workspace/tuvm/zephyr
source ~/zephyr-esp32/.venv/bin/activate
west build -b esp32c6_devkitc/esp32c6/hpcore -p always -d build-c6-thread . \
  -- -DCONF_FILE=prj_esp32c6.conf -DEXTRA_CONF_FILE=prj_esp32c6_thread.conf \
     -DTUVM_MODEL=B 2>&1 | tail -25
```

Expected: a completed build with a memory report, or a region overflow (acceptable — the
heap arena default is large; Step 4 measures the real ceiling).

**If it fails on undefined OpenThread symbols or a missing RISC-V OpenThread port**,
capture the exact error and report it. Do not attempt to port OpenThread.

- [ ] **Step 4: Verify the radio driver actually built in**

A silently-absent driver would make the Thread cell measure the wrong thing.

```bash
cd ~/workspace/tuvm
grep -E "^CONFIG_(IEEE802154_ESP32|NET_L2_OPENTHREAD|OPENTHREAD_MTD)=" \
  zephyr/build-c6-thread/zephyr/.config
```

Expected: all three present and `=y`. If `CONFIG_IEEE802154_ESP32` is absent, the overlay
did not take effect — rebuild with `-p always`.

- [ ] **Step 5: Measure the fourth table cell**

```bash
cd ~/workspace/tuvm
bash tools/heap-ceiling.sh --board esp32c6 --conf-extra prj_esp32c6_thread.conf 2>&1 | tail -3
```

If `tools/heap-ceiling.sh` has no `--conf-extra` flag, add one that forwards
`-DEXTRA_CONF_FILE=` through `run.sh`, mirroring how the board profile forwards
`-DCONF_FILE=`. Record the number.

**This is the C6's with-radio cell.** It is not directly comparable to the WROOM-32's 53248
— WiFi and OpenThread have different footprints — so the results doc must say so where it
reports both.

- [ ] **Step 6: Commit**

```bash
cd ~/workspace/tuvm
git add zephyr/prj_esp32c6_thread.conf zephyr/boards/esp32c6_devkitc_esp32c6_hpcore.overlay \
        tools/heap-ceiling.sh docs/results/2026-07-20-c6-heap-ceiling.md
git commit -m "feat(zephyr): OpenThread/IPv6 on the C6 + fourth table cell

Thread rather than WiFi: the board yaml declares netif:openthread and
does not declare wifi. Overlay enables the ieee802154 node, which
esp32c6_common.dtsi:85 ships disabled and which CONFIG_IEEE802154_ESP32
depends on.

Kconfig copies only the networking block of prj_dk_thread.conf — that
file's CONFIG_NEWLIB_LIBC and CONFIG_FPU=y are wrong for the C6.

Refs #57"
```

---

## Task 7: Close-out — promote the durable knowledge

**Files:**
- Modify: `review/program-sequence.md`
- Modify: `zephyr/README.md`

**Per CLAUDE.md, the rung is not closed until its durable knowledge is promoted to a
mainline document of record.** The dated design and results docs stay behind as the
historical "why".

- [ ] **Step 1: Promote to the ladder**

Add a rung entry to `~/workspace/tuvm/review/program-sequence.md` capturing what a future
reader must know, independent of these documents:

- The C6 board target is `esp32c6_devkitc/esp32c6/hpcore`; its overlay filename would be
  `boards/esp32c6_devkitc_esp32c6_hpcore.overlay` (derived from board qualifiers).
- The C6 has no FPU (`rv32imac_zicsr`); tuvm runs soft-float there.
- C6 flash offset is `0x0`; `0x1000` boot-loops silently.
- Console on the UART bridge (survives target reset), flashing on native USB
  (re-enumerates). Not the intuitive assignment.
- `layout-*.json` is ABI-specific — a new architecture needs its own layoutdump run before
  Model B works at all.
- The measured heap ceilings, with the WiFi/no-WiFi distinction stated.

- [ ] **Step 2: Document the dev loop**

Add a short section to `zephyr/README.md` covering `run.sh --board esp32c6`, which port
does what, and where the layout JSON comes from.

- [ ] **Step 3: Confirm no CLAIMS.md row moves**

This rung adds no VM semantics and no nsl-visible behaviour, so no claim should change.

```bash
cd ~/workspace/nsl-tuvm && git status --porcelain review/tests/CLAIMS.md 2>/dev/null || true
```

Expected: no output.

- [ ] **Step 4: Run the full suite**

The rung touches no nsl source, so this should be unchanged at the current floor.

```bash
cd ~/workspace/nsl-tuvm && python3 tools/run_tests_oevm.py 2>&1 | tail -6
```

Expected: `Passed: 202  Failed: 0  Skipped: 14`.

- [ ] **Step 5: Update the issues**

Comment on #57 with the measured table and the go/no-go on promoting the C6 to primary.
If the ceiling is a large improvement, comment on #53 that the C6 path relieves it, and on
#58 that Task 9's trigger thread is now affordable.

- [ ] **Step 6: Commit**

```bash
cd ~/workspace/tuvm
git add review/program-sequence.md zephyr/README.md
git commit -m "docs(ladder): promote the ESP32-C6 rung's durable lessons

Closes the C6 target rung: board target and overlay naming, no-FPU
soft-float, the 0x0 flash offset, the console-on-bridge/flash-on-native
port split, layout JSON being ABI-specific, and the measured heap
ceilings with the WiFi/no-WiFi distinction.

Refs #57"
```

---

## Self-Review Notes

**Spec coverage.** All five design hazards map to tasks: no-FPU → Task 2 Step 1 + Task 5
Steps 3-4; port split → Task 2 Step 1 + Task 4 Steps 1-3; ROM patch gating → Task 1 Step 4
+ Task 5 Step 2's failure branch; flash offset → Task 1 Step 2 + Task 4 Step 3; layout ABI
→ Tasks 2 and 4. The `OEVM_IRAM_HOT` non-export is Task 1 Step 3, verified at Task 2
Step 5. The four-cell table is Task 3, completed at Task 6 Step 4. Port pinning by
`by-id` is Task 1 Step 2.

**Known soft spots, flagged rather than hidden:**
- Task 3 Step 3's no-WiFi WROOM-32 cell requires temporarily repointing the profile at a
  scratch conf. That is manual and slightly awkward; if it proves fiddly in practice,
  promoting `prj_esp32_nowifi.conf` to a tracked file is a reasonable deviation.
- Task 5 Step 3's `FloatSmokeNode.nsl` skeleton is written from the general nsl idiom, not
  copied from a verified node. The step says to read `TimerNode.nsl` and mirror it —
  follow that instruction rather than pasting the skeleton.
- Task 2 Step 4 configures the app with `TUVM_MODEL=A`, which may fail for reasons
  unrelated to Kconfig. The step says to record which; a Model A failure that is clearly
  about the solidified image is an acceptable pass.
