# Zephyr Model A/B Build Knob Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a `TUVM_MODEL=A|B` knob to the tuvm Zephyr app plus a `zephyr/run.sh` that owns bake → build → flash → capture, so an ESP32 script change stops costing a ~50-minute rebuild.

**Architecture:** Model A (today) links the generated `src/solidified_image.c`. Model B links `src/oimg_loader.c` + `src/kernel_boot.c` instead, and gets its program from a `.oimg` blob `.incbin`'d into flash `.rodata` via `zephyr/blob.S`. The two link sets are mutually exclusive by construction — `solidified_image.c` XOR `kernel_boot.c` — which is the entirety of the "clash" that has kept Model B off Zephyr. One `#ifdef` fork in `zephyr/src/main.c`, one CMake option, one wrapper script.

**Tech Stack:** Zephyr 4.2.0 (`~/zephyr-esp32` west workspace), Xtensa GCC 12.2.0 (`~/zephyr-sdk-0.17.4`), CMake/ninja, esptool, Python 3 (`nsl-tuvm` compiler), bash.

**Design:** `docs/design/2026-07-16-zephyr-model-ab-build-knob-design.md`

## Global Constraints

- **Board:** `esp32_devkitc/esp32/procpu`. App source dir is `zephyr/`, **never** the repo root (`.` has no `CMakeLists.txt`).
- **Env exports, all five, load-bearing on every build** (a `-p always` build discards the CMake cache that would otherwise carry them):
  `OEVM_HEAP_SIZE=65536 METHOD_CACHE_SIZE=256 PRIM_CACHE_SIZE=256 VM_STACK_INIT_WORDS=4096 OEVM_MARK_STACK_SIZE=512`
- **Compiler path:** `~/workspace/nsl-tuvm/tools/ns_compile_oevm.py`. **NOT** `~/workspace/ns-oevm` — that is a *different, stale* repo (`bench.sh:8` still hardcodes it; out of scope here, file it).
- **`CONFIG_ESP32_USE_UNSUPPORTED_REVISION=y`** is required on this rev-1.0 silicon in *every* `prj*.conf`, including throwaway apps. Without it the bootloader halts at `soc_init` and the app never runs.
- **Never call `fflush` in Zephyr-side code.** `esp32.rom.newlib-funcs.ld` force-binds `fflush` to ROM newlib as a STRONG symbol; ROM `fflush` → ROM `__getreent` → NULL deref (EXCCAUSE 28). See commit `596b208`.
- **`src/solidified_image.c` is tracked** (committed as the `MethodCallTest` fixture). Any bake dirties it; it must be restored and never committed.
- **Never use `west flash`** to reflash — it triggers a rebuild. Use esptool directly at `0x1000` (Zephyr ESP32 is a single image).
- **Incremental by default.** `-p always` forces a full ~50 min rebuild.

## File Structure

| File | Responsibility |
|---|---|
| `zephyr/layoutdump/CMakeLists.txt` (create) | Throwaway Zephyr app that compiles `tools/dump_layout.c` for the target ABI |
| `zephyr/layoutdump/prj.conf` (create) | Minimal console config for that app |
| `zephyr/layout-esp32.json` (create, committed) | The banked on-target struct layout; input to `--format=oimg` |
| `zephyr/CMakeLists.txt` (modify) | `TUVM_MODEL` option → selects the link set |
| `zephyr/src/main.c` (modify) | The one `#ifdef TUVM_MODEL_B` fork: stub globals (file scope) + boot calls (in `main`) |
| `zephyr/blob.S` (create) | `.incbin`s the `.oimg` into flash `.rodata`; exports `tuvm_blob_start`/`_end` |
| `zephyr/run.sh` (create) | The loop: bake → build → flash → capture, `--model A\|B` |

---

### Task 1: `run.sh` — the loop, Model A only

Build the wrapper first and prove it against **known-good Model A**. This isolates the script's bugs from Model B's bugs, and it independently retires every papercut hit on 2026-07-16 (wrong cwd, unset `$BUILD`, stale compiler path, forgotten bake, forgotten env).

**Files:**
- Create: `zephyr/run.sh`

**Interfaces:**
- Consumes: nothing.
- Produces: `zephyr/run.sh --model A|B --node <file.ns> [--flash] [--capture SECS] [--pristine] [--build-dir DIR]`. Task 4 extends it with the `B` branch. Build dir defaults to `build-esp32-<MODEL>`.

- [ ] **Step 1: Write the script**

```bash
#!/usr/bin/env bash
# zephyr/run.sh — the ESP32 dev loop: bake -> build -> flash -> capture.
# Owns every step that has a wrong-by-default: cwd, the five env exports, the
# compiler path, the build dir, and restoring the tracked solidified_image.c.
set -euo pipefail

MODEL=A NODE=TimerNode.ns DO_FLASH=0 CAPTURE=0 PRISTINE=0 BUILD_DIR=""
while [ $# -gt 0 ]; do
  case "$1" in
    --model)     MODEL="$2"; shift 2 ;;
    --node)      NODE="$2"; shift 2 ;;
    --flash)     DO_FLASH=1; shift ;;
    --capture)   CAPTURE="$2"; shift 2 ;;
    --pristine)  PRISTINE=1; shift ;;
    --build-dir) BUILD_DIR="$2"; shift 2 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done
case "$MODEL" in A|B) ;; *) echo "--model must be A or B" >&2; exit 2 ;; esac

ZDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"   # zephyr/ — the app dir
ROOT="$(cd "$ZDIR/.." && pwd)"                          # repo root
NSL="$HOME/workspace/nsl-tuvm/tools/ns_compile_oevm.py" # NOT ~/workspace/ns-oevm (stale)
: "${BUILD_DIR:=build-esp32-$MODEL}"
BOARD="esp32_devkitc/esp32/procpu"
ESPTOOL="$HOME/.espressif/python_env/idf5.4_py3.12_env/bin/esptool.py"

[ -f "$ZDIR/$NODE" ] || { echo "no such node: $ZDIR/$NODE" >&2; exit 2; }
[ -f "$NSL" ]        || { echo "compiler not found: $NSL" >&2; exit 2; }

# The five load-bearing knobs. -p always discards the cache, so always export.
export OEVM_HEAP_SIZE=65536 METHOD_CACHE_SIZE=256 PRIM_CACHE_SIZE=256 \
       VM_STACK_INIT_WORDS=4096 OEVM_MARK_STACK_SIZE=512

# src/solidified_image.c is TRACKED (the MethodCallTest fixture). A Model A bake
# dirties it; restore on exit so it is never committed by accident.
restore_image() { git -C "$ROOT" checkout -- src/solidified_image.c 2>/dev/null || true; }
trap restore_image EXIT

echo "[bake]  $NODE  (model $MODEL)"
if [ "$MODEL" = A ]; then
  python3 "$NSL" "$ZDIR/$NODE" "$ROOT/src/solidified_image.c"
else
  echo "[bake]  model B not wired yet (Task 4)" >&2; exit 3
fi

PRISTINE_ARG=(); [ "$PRISTINE" = 1 ] && PRISTINE_ARG=(-p always)
echo "[build] -b $BOARD -d $BUILD_DIR ${PRISTINE_ARG[*]:-(incremental)}"
( cd "$ZDIR" && west build -b "$BOARD" "${PRISTINE_ARG[@]}" -d "$BUILD_DIR" . \
    -- -DCONF_FILE=prj_esp32.conf -DTUVM_MODEL="$MODEL" )

if [ "$DO_FLASH" = 1 ]; then
  echo "[flash] esptool 0x1000"
  # NOT `west flash` — that would rebuild.
  "$ESPTOOL" --chip esp32 --baud 921600 write_flash --flash_mode dio \
    --flash_freq 40m --flash_size 4MB 0x1000 "$ZDIR/$BUILD_DIR/zephyr/zephyr.bin"
fi

if [ "${CAPTURE:-0}" != 0 ]; then
  echo "[watch] ${CAPTURE}s"
  python3 "$ZDIR/capture_coredump.py" "$CAPTURE"
fi
```

- [ ] **Step 2: Run it and verify it rejects bad input**

```bash
cd ~/workspace/tuvm && chmod +x zephyr/run.sh
zephyr/run.sh --model Q --node TimerNode.ns
```
Expected: `--model must be A or B`, exit 2.

```bash
zephyr/run.sh --model A --node NoSuchNode.ns
```
Expected: `no such node: .../NoSuchNode.ns`, exit 2.

- [ ] **Step 3: Verify Model A reproduces today's behaviour (incremental)**

Note `-DTUVM_MODEL=A` is passed but `zephyr/CMakeLists.txt` ignores unknown vars until Task 3 — harmless.

```bash
cd ~/workspace/tuvm && zephyr/run.sh --model A --node TimerNode.ns --flash --capture 12 2>&1 | tee /tmp/run-a.log
```
Expected: bake → build → flash → `TIMER OK` in the capture.

- [ ] **Step 4: Verify the tracked fixture was restored**

```bash
cd ~/workspace/tuvm && git status --short src/solidified_image.c
```
Expected: **empty output** (the `trap` restored it).

- [ ] **Step 5: Commit**

```bash
cd ~/workspace/tuvm
git add zephyr/run.sh
git commit -m "feat(zephyr): run.sh owns the ESP32 loop — bake, build, flash, capture

Model A only; the B branch lands with the knob. Owns the five env exports, the
correct nsl-tuvm compiler path (bench.sh's ~/workspace/ns-oevm is a stale repo),
the app cwd, per-model build dirs, esptool-not-west-flash, and restoring the
tracked src/solidified_image.c on exit."
```

---

### Task 2: Stage 0 — bank the on-target layout

The `.oimg` is width- **and ABI**-specific. `--format=oimg` needs a layout JSON from `tools/dump_layout.c`, which must **run** on the target ABI. An Xtensa binary cannot run on the host, and the `-m32` proxy is rejected: i386 SysV aligns `double` to 4 bytes, Xtensa to 8, and `CONFIG_FPU=y` means floats are live. A wrong layout yields a blob that loads and then *misbehaves* — worse than one that fails loudly.

`tools/dump_layout.c` is reusable as-is: `int main(void)`, `printf`/`offsetof`/`sizeof` only, links nothing, and never calls `fflush` (which would be fatal — see Global Constraints).

**Files:**
- Create: `zephyr/layoutdump/CMakeLists.txt`, `zephyr/layoutdump/prj.conf`
- Create: `zephyr/layout-esp32.json` (committed output)

**Interfaces:**
- Consumes: `tools/dump_layout.c` (existing, unmodified).
- Produces: `zephyr/layout-esp32.json` — the `--layout` argument for Task 4's bake.

- [ ] **Step 1: Write the throwaway app**

`zephyr/layoutdump/CMakeLists.txt`:
```cmake
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(tuvm_layoutdump)
# tools/dump_layout.c is `int main(void)` and links nothing — it only prints
# sizeof/offsetof for the TARGET ABI. Compiling it as the Zephyr app IS the test.
target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/dump_layout.c)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
```

`zephyr/layoutdump/prj.conf`:
```
CONFIG_SERIAL=y
CONFIG_UART_CONSOLE=y
CONFIG_STDOUT_CONSOLE=y
CONFIG_PICOLIBC=y
# rev-1.0 silicon: without this the bootloader HALTS at soc_init and main never runs.
CONFIG_ESP32_USE_UNSUPPORTED_REVISION=y
```

- [ ] **Step 2: Build and flash it**

```bash
cd ~/workspace/tuvm/zephyr/layoutdump
export OEVM_HEAP_SIZE=65536 METHOD_CACHE_SIZE=256 PRIM_CACHE_SIZE=256 \
       VM_STACK_INIT_WORDS=4096 OEVM_MARK_STACK_SIZE=512
west build -b esp32_devkitc/esp32/procpu -d build .
~/.espressif/python_env/idf5.4_py3.12_env/bin/esptool.py --chip esp32 --baud 921600 \
  write_flash --flash_mode dio --flash_freq 40m --flash_size 4MB 0x1000 build/zephyr/zephyr.bin
```
Expected: builds in well under a minute (no `solidified_image.c`), flashes.

- [ ] **Step 3: Capture the JSON**

```bash
cd ~/workspace/tuvm/zephyr && python3 capture_coredump.py 8 | tee /tmp/layout-raw.log
```
Expected: a JSON object beginning `{ "oop_size": 4, "structs": { ...`.

- [ ] **Step 4: Extract, validate, bank**

Strip the Zephyr boot banner and any log prefixes, keeping only the JSON:
```bash
cd ~/workspace/tuvm/zephyr
sed -e 's/\x1b\[[0-9;]*m//g' /tmp/layout-raw.log \
  | sed -n '/^{/,/^}/p' > layout-esp32.json
python3 -c "import json; d=json.load(open('layout-esp32.json')); assert d['oop_size']==4, d['oop_size']; print('oop_size', d['oop_size'], '| structs:', len(d['structs']))"
```
Expected: `oop_size 4 | structs: <N>` and no traceback.

- [ ] **Step 5: Diff against the -m32 layout to see whether the ABI worry was real**

This is evidence for the design's rejection of the proxy — record the answer either way.
```bash
cd ~/workspace/tuvm
gcc -m32 -std=c99 -Isrc tools/dump_layout.c -o /tmp/dl32 && /tmp/dl32 > /tmp/layout32.json
diff <(python3 -m json.tool /tmp/layout32.json) <(python3 -m json.tool zephyr/layout-esp32.json) \
  && echo "IDENTICAL — the -m32 proxy would have worked" \
  || echo "DIFFERENT — the on-target dump was necessary (record which fields moved)"
```
Expected: either answer is a valid result. If DIFFERENT, note the fields in the commit body — that is the justification for stage 0 existing.

- [ ] **Step 6: Commit**

```bash
cd ~/workspace/tuvm
git add zephyr/layoutdump/CMakeLists.txt zephyr/layoutdump/prj.conf zephyr/layout-esp32.json
git commit -m "feat(zephyr): bank the on-target ESP32 struct layout (stage 0)

.oimg is width- AND ABI-specific and --format=oimg needs a layout JSON from
tools/dump_layout.c, which must RUN on the target ABI. An Xtensa binary cannot run
on the host, so dump_layout is compiled as a tiny Zephyr app and its JSON captured
over serial. The -m32 proxy was rejected up front: i386 SysV aligns double to 4,
Xtensa to 8, and CONFIG_FPU=y means floats are live — a wrong layout would yield a
blob that loads and then misbehaves, which is worse than one that fails loudly.

Regenerate only when a struct or the toolchain changes."
```

---

### Task 3: The `TUVM_MODEL` knob + the `main.c` fork

**Files:**
- Modify: `zephyr/CMakeLists.txt`
- Modify: `zephyr/src/main.c` (stub globals at ~113-116; boot calls at ~151-163)
- Create: `zephyr/blob.S`

**Interfaces:**
- Consumes: `zephyr/layout-esp32.json` (Task 2).
- Produces: CMake option `TUVM_MODEL` (`A` default). Under B: `-DTUVM_MODEL_B -DOEVM_NO_FILE_IO`, links `src/oimg_loader.c` + `src/kernel_boot.c` + `zephyr/blob.S`, expects `<node>.oimg` beside `blob.S`. Exports `tuvm_blob_start[]`, `tuvm_blob_end[]`.
- Existing signatures used (from `oevm_image_main.c`): `void kernel_boot(void)`, `int oimg_load_blob(const uint8_t *base, size_t size)`, `oop oimg_root(void)`, `oop oimg_bootstrap(oop entry_vtable)`.

- [ ] **Step 1: Write `zephyr/blob.S`**

`.incbin` is a byte-for-byte copy at assemble time — no parsing, no literal pools. Verified 2026-07-16: 0.00 s vs `solidified_image.c`'s ~46 min, landing in `.rodata` (flash, XIP-able, which is what `is_in_blob()`/`oimg_resolve()` need).

```asm
/* zephyr/blob.S — the Model B program, copied verbatim into flash .rodata.
   Deliberately NOT `xxd -i` into a C array: a byte array re-enters the
   assembler through the front door, which is the exact cost Model B exists to
   escape (946 KB of C -> 941.7 MB / 71M lines of Xtensa asm -> ~46 min in `as`).
   .incbin assembles the same bytes in 0.00 s. TUVM_BLOB_FILE is -D'd by CMake. */
    .section .rodata
    .balign 4
    .global tuvm_blob_start
tuvm_blob_start:
    .incbin TUVM_BLOB_FILE
    .global tuvm_blob_end
tuvm_blob_end:
```

- [ ] **Step 2: Add the option to `zephyr/CMakeLists.txt`**

Replace the unconditional `${TUVM_SRC}/src/solidified_image.c` line in `target_sources(app PRIVATE ...)` with nothing, and append after that block:

```cmake
# ---- Model A/B knob -------------------------------------------------------
# A (default): the per-program baked image, src/solidified_image.c.
# B: a generic VM + a .oimg blob .incbin'd into flash .rodata.
# The link sets are MUTUALLY EXCLUSIVE: solidified_image.c XOR kernel_boot.c —
# kernel_boot.c's static Vtable_* defs clash with the ones solidified_image.c
# bakes for THIS program. (The old comment here also blamed oimg_loader.c; that
# was wrong — the host's run_c_unit_tests() links oimg_loader.c together with
# solidified_image.c. oimg_loader.c's only conflict is that it owns the four
# globals src/main.c stubs, which the TUVM_MODEL_B ifdef there resolves.)
set(TUVM_MODEL "A" CACHE STRING "tuvm image model: A (solidified) or B (.oimg blob)")
set_property(CACHE TUVM_MODEL PROPERTY STRINGS A B)

if(TUVM_MODEL STREQUAL "A")
  target_sources(app PRIVATE ${TUVM_SRC}/src/solidified_image.c)
elseif(TUVM_MODEL STREQUAL "B")
  if(NOT DEFINED TUVM_BLOB)
    message(FATAL_ERROR "TUVM_MODEL=B requires -DTUVM_BLOB=<path to .oimg>")
  endif()
  if(NOT EXISTS "${TUVM_BLOB}")
    message(FATAL_ERROR "TUVM_BLOB does not exist: ${TUVM_BLOB}")
  endif()
  target_sources(app PRIVATE
    ${TUVM_SRC}/src/oimg_loader.c
    ${TUVM_SRC}/src/kernel_boot.c
    ${CMAKE_CURRENT_SOURCE_DIR}/blob.S)
  target_compile_definitions(app PRIVATE TUVM_MODEL_B OEVM_NO_FILE_IO)
  # blob.S is preprocessed (.S, capital) so TUVM_BLOB_FILE expands.
  set_property(SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/blob.S APPEND PROPERTY
    COMPILE_DEFINITIONS "TUVM_BLOB_FILE=\"${TUVM_BLOB}\"")
  # Rebuild when the blob changes.
  set_property(SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/blob.S APPEND PROPERTY
    OBJECT_DEPENDS "${TUVM_BLOB}")
else()
  message(FATAL_ERROR "TUVM_MODEL must be A or B, got: ${TUVM_MODEL}")
endif()
message(STATUS "tuvm: TUVM_MODEL=${TUVM_MODEL}")
```

- [ ] **Step 3: Fork `zephyr/src/main.c` — file scope**

Wrap the existing four stub globals (currently ~113-116). Under B, `src/oimg_loader.c` owns all four and defining both duplicate-defines — this is the most likely first-try link error.

```c
#ifndef TUVM_MODEL_B
/* XIP stubs — Model A never loads an .oimg blob; oimg.h's is_in_blob() checks
   oimg_blob_base first and returns 0 while it is NULL, so the XIP accessor
   paths are never taken. Under Model B these MUST NOT be compiled:
   src/oimg_loader.c defines all four itself. */
const uint8_t *oimg_blob_base = NULL;
const uint8_t *oimg_blob_end  = NULL;
oop builtin_registry[BUILTIN_COUNT] = {0};
oop oimg_singleton[3] = {0};
#else
extern const uint8_t tuvm_blob_start[], tuvm_blob_end[];   /* zephyr/blob.S */
extern void kernel_boot(void);
extern int  oimg_load_blob(const uint8_t *base, size_t size);
extern oop  oimg_root(void);
extern oop  oimg_bootstrap(oop entry_vtable);
#endif
```

- [ ] **Step 4: Fork `zephyr/src/main.c` — inside `main()`**

`gc_additional_roots_hook` and `oevm_millis` are already set above this point and are shared by both models — do not duplicate them. Replace the `init_solidified_vtables(); init_reflection_symbols();` pair and the `run_solidified_entry_point()` call:

```c
#ifdef TUVM_MODEL_B
    kernel_boot();                    /* ONCE, before load/bootstrap (oevm_image_main.c) */
    if (oimg_load_blob(tuvm_blob_start,
                       (size_t)(tuvm_blob_end - tuvm_blob_start)) != 0) {
        printk("tuvm: oimg_load_blob failed\n");   /* loud: never boot a bad blob silently */
        return 0;
    }
#else
    init_solidified_vtables();
    init_reflection_symbols();
#endif

    wire_pulse_gpio();

#ifdef CONFIG_NET_L2_OPENTHREAD
    thread_bringup();
#endif

#ifdef TUVM_MODEL_B
    oop result = oimg_bootstrap(oimg_root());
#else
    oop result = run_solidified_entry_point();
#endif
    (void)result;
```

- [ ] **Step 5: Verify Model A still builds and runs (no regression)**

The knob must not change today's behaviour.
```bash
cd ~/workspace/tuvm && zephyr/run.sh --model A --node TimerNode.ns --flash --capture 12 2>&1 | tail -20
```
Expected: `-- tuvm: TUVM_MODEL=A` in the CMake output, and `TIMER OK` in the capture.

- [ ] **Step 6: Verify B fails loudly without a blob**

```bash
cd ~/workspace/tuvm/zephyr && west build -b esp32_devkitc/esp32/procpu -d /tmp/bcheck . \
  -- -DCONF_FILE=prj_esp32.conf -DTUVM_MODEL=B 2>&1 | grep -i 'TUVM_BLOB'
```
Expected: `TUVM_MODEL=B requires -DTUVM_BLOB=<path to .oimg>`.

- [ ] **Step 7: Commit**

```bash
cd ~/workspace/tuvm
git add zephyr/CMakeLists.txt zephyr/src/main.c zephyr/blob.S
git commit -m "feat(zephyr): TUVM_MODEL=A|B knob + blob.S (.incbin)

A stays the default and is byte-for-byte today's build. B swaps the link set to
oimg_loader.c + kernel_boot.c + a .oimg .incbin'd into flash .rodata, and defines
OEVM_NO_FILE_IO so the loader's mmap path compiles out.

The link sets are mutually exclusive by construction: solidified_image.c XOR
kernel_boot.c. Corrects the old CMakeLists comment, which also blamed oimg_loader.c
— the host's run_c_unit_tests() links oimg_loader.c together with solidified_image.c;
oimg_loader.c's only conflict is owning the four globals main.c stubs, resolved by
the TUVM_MODEL_B ifdef there.

.incbin rather than xxd -i: a byte array would re-enter the assembler, which is the
cost B exists to escape. Measured: .incbin of 946 KB = 0.00 s vs ~46 min for
solidified_image.c."
```

---

### Task 4: Model B end-to-end on the device (the gate)

**Files:**
- Modify: `zephyr/run.sh` (the `B` bake branch from Task 1 Step 1)

**Interfaces:**
- Consumes: the knob (Task 3), `zephyr/layout-esp32.json` (Task 2).
- Produces: `zephyr/run.sh --model B --node <file.ns>` → a booting Model B image.

- [ ] **Step 1: Replace the Model B stub in `run.sh`**

Swap the `exit 3` branch for the real bake:

```bash
echo "[bake]  $NODE  (model $MODEL)"
if [ "$MODEL" = A ]; then
  python3 "$NSL" "$ZDIR/$NODE" "$ROOT/src/solidified_image.c"
else
  LAYOUT="$ZDIR/layout-esp32.json"
  [ -f "$LAYOUT" ] || { echo "missing $LAYOUT — run the layoutdump app (stage 0)" >&2; exit 2; }
  OIMG="$ZDIR/${NODE%.ns}.oimg"
  python3 "$NSL" "$ZDIR/$NODE" "$OIMG" --format=oimg --layout="$LAYOUT"
  echo "[blob]  $(stat -c %s "$OIMG") bytes -> .incbin"
fi
```

And pass the blob to CMake — extend the build invocation:
```bash
BLOB_ARG=(); [ "$MODEL" = B ] && BLOB_ARG=(-DTUVM_BLOB="$ZDIR/${NODE%.ns}.oimg")
( cd "$ZDIR" && west build -b "$BOARD" "${PRISTINE_ARG[@]}" -d "$BUILD_DIR" . \
    -- -DCONF_FILE=prj_esp32.conf -DTUVM_MODEL="$MODEL" "${BLOB_ARG[@]}" )
```

- [ ] **Step 2: Ignore the generated blob**

```bash
cd ~/workspace/tuvm && printf 'zephyr/*.oimg\nzephyr/build-esp32-*/\nzephyr/layoutdump/build/\n' >> .gitignore
```
(Also covers the 136 MB `zephyr/build-esp32-coredump/` currently untracked.)

- [ ] **Step 3: THE GATE — Model B boots and prints `TIMER OK`**

```bash
cd ~/workspace/tuvm && time zephyr/run.sh --model B --node TimerNode.ns --pristine --flash --capture 12 2>&1 | tee /tmp/run-b.log
```
Expected: `TIMER OK` — **identical** output to Model A. This is the A==B equivalence the host already asserts, now on target.

If `oimg_load_blob failed`: the loader validates magic, `format_version == 8`, and `registry_version` against the firmware's `BUILTIN_REGISTRY_VERSION = 5`, and prints which check failed. A `registry_version` mismatch means the blob and firmware disagree about builtins — re-bake. A silent boot-then-misbehave instead points at `layout-esp32.json`.

- [ ] **Step 4: Bank the numbers**

```bash
grep -E 'real' /tmp/run-b.log
```
Expected: Model B pristine wall-clock **dramatically** below Model A's ~50 min (`as` on `solidified_image.c` is gone entirely). Record both figures for the commit body and for `review/metrics-spec.md`'s per-phase build line.

- [ ] **Step 5: Verify no dirty fixture and both models still work**

```bash
cd ~/workspace/tuvm && git status --short src/solidified_image.c
```
Expected: empty.

- [ ] **Step 6: Commit**

```bash
cd ~/workspace/tuvm
git add zephyr/run.sh .gitignore
git commit -m "feat(zephyr): Model B boots on ESP32 — TIMER OK from a .oimg blob

run.sh --model B bakes <node>.ns -> <node>.oimg with the banked on-target layout,
.incbin's it into flash .rodata, and links the generic VM (oimg_loader + kernel_boot)
with no solidified_image.c. Same TimerNode, same TIMER OK as Model A — the A==B
equivalence the host asserts, now proven on target.

Build time: <B> vs ~50 min for Model A."
```

---

### Task 5: Promote to the ladder

The rung is not closed until its durable knowledge reaches a mainline document of record (CLAUDE.md).

**Files:**
- Modify: `review/program-sequence.md` (Phase 2 rung 2 — still open)
- Modify: `review/DOCMAP.md` (regenerated)

- [ ] **Step 1: Add an in-rung finding under Phase 2 rung 2**

Not a rung-close — the rung stays open (stage 2 is unbuilt). Record: the ~50 min was `as` on 941.7 MB / 71M lines of Xtensa literal-pool asm from `solidified_image.c`; Model B removes the file so the cost disappears; `.incbin` = 0.00 s; the CMakeLists clash comment was wrong (only `kernel_boot.c` clashes); `oimg_load_blob` already existed for embedded; stage 0's on-target layout and what the `-m32` diff showed; and that stage 2 (flash partition, blob-only reflash) is the open item that answers **G1**.

- [ ] **Step 2: Regenerate DOCMAP after committing the doc**

`tools/docmap.sh` derives dates from `git log -1 --format=%as`, so it must run **after** the doc commit or it banks a stale date.
```bash
cd ~/workspace/tuvm
git add review/program-sequence.md && git commit -m "docs(zephyr): promote — Model B on ESP32, to the ladder's rung 2"
bash tools/docmap.sh && git add review/DOCMAP.md && git commit --amend --no-edit
```

- [ ] **Step 3: Consider a CLAIMS row**

`review/tests/CLAIMS.md`'s rule: *a criterion without a green named test is unproven.* Task 4 Step 3 is a manual device gate, not a named automated test. Add a row **only** if the gate is automated; otherwise leave it to the ladder and say so.

---

## Out of Scope

- **Stage 2** — flash partition / blob-only reflash / independent `.oimg` OTA. Its own rung; feeds G1.
- `bench.sh:8`'s stale `~/workspace/ns-oevm` path — file an issue.
- The nRF/`prj_dk.conf` side — the knob generalizes, but only ESP32 is proven here.
- The ROM-bound `setjmp`/`longjmp` thread (`vm.c`'s raise/ensure boundary on Xtensa).
