# The program in its own flash partition (#310) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** On the ESP32-C6, the nsl program lives in its own flash partition; `run.sh --flash` writes firmware and program, `custos load` replaces the program alone, and the node reports the SHA-256 of the program it runs.

**Architecture:** A C6 board overlay adds `program_partition` (512K at `0x320000`) carved from the two firmware slots. A Model B build whose devicetree has that partition links no program (`blob.S` drops out); `main.c` memory-maps the partition, hands it to the unchanged `oimg_load_blob`, hashes what it loaded, prints the hash and gives it to the ota seam, so the kernel's existing `publishApps` writes `obs/apps/main` with no nsl change. custos gains `internal/oimg` (the prefix reader `protocol/IMAGE-ARTIFACTS.md` §2 already specifies) and `internal/load` (the verb).

**Tech Stack:** C99 + Zephyr (ESP32-C6, `spi_flash_mmap`), bash (`zephyr/run.sh`), Go 1.26 (`custos`, `go.bug.st/serial`), esptool.

**Spec:** `docs/design/2026-10-04-custos-peer-design.md` §6a step 5, addendum 2026-10-05 (decided, Q1–Q2 as proposed). Spike evidence: the three comments on #310 dated 2026-10-05.

## Global Constraints

- The layout is: `image-0` `0x20000` 1536K, `image-1` `0x1A0000` 1536K, `program_partition` `0x320000` 512K. `storage`, the lpcore slots, scratch and coredump don't move.
- Simple boot stays. MCUboot is #233's step.
- The firmware carries no program on a board that has `program_partition`. Other boards keep the linked blob.
- The partition offset's writer is the overlay. `run.sh` reads it from the build's `zephyr/zephyr.dts`. custos carries the one copy, which cites `protocol/IMAGE-ARTIFACTS.md` §2a (Q1).
- The SHA-256 of the `.oimg` is the program's identity (`IMAGE-ARTIFACTS.md` decision 2).
- Only `c6spare` may be flashed. Never flash `c6bench` (it's in the soak) or the bell. The target harness refuses the bell; a hand `run.sh` doesn't.
- Every commit names its paths explicitly (never `-a`), says `refs #310`, and ends with the session's attribution lines.
- Gates (root `CLAUDE.md`): the nsl suite `288 passed, 0 failed` plus a green aux line; `for m in protocol porta custos; do (cd $m && go test ./...); done`; `bash tests/porta_sync_host.sh` → `PORTA SYNC OK`. Never run two of these concurrently.
- No host gate compiles `nodus/oevm/zephyr/src/*.c`. A firmware change is verified on `c6spare`.

## Review Focus

1. **An erased or never-written partition.** The boot logs the loader's refusal (`image too small`), and `custos load` still writes a program into it: the "before" boot has a build-id and a refusal, and no program. Pinned in Task 4 (`TestLoadIntoAnEmptyPartition`) and on the board in Task 5 step 4.
2. **An `.oimg` larger than the partition.** custos refuses before esptool runs (Task 4, `TestLoadRefusesAnImageLargerThanTheRegion`), and so does `run.sh` (Task 2 step 4).
3. **A collector holding the port.** The boot read times out with a message naming that cause, and esptool never runs (Task 4, `TestLoadStopsWhenTheBoardDoesNotBoot`).
4. **A stale `.oimg` the VM refuses** (a registry mismatch). `custos load` reports the loader's own refusal line and exits 1, rather than a confusing hash mismatch (Task 4, `TestLoadReportsTheLoadersRefusal`).
5. **The new `obs/apps/main` row on the first push after boot.** It must not crowd out other rows: `bridge-health` on `c6spare` with `dropped 0` (Task 5 step 2).

---

### Task 1: The ota seam answers the running program's hash on the C6

**Files:**
- Modify: `nodus/oevm/src/ota.c` (add `ota_sha256`; `staged_hash_is` calls it)
- Modify: `nodus/oevm/src/ota.h` (declare `ota_sha256`)
- Modify: `nodus/oevm/src/ota_backend.h` (declare `ota_note_running_hash`)
- Modify: `nodus/oevm/src/backend/zephyr/ota_zephyr.c` (`ota_env("OEVM_IMAGE_HASH")` answers the noted hash; correct the stale "staging slot is slot1" comment)
- Modify: `nodus/oevm/tests/ota_seam_test.c` (known-answer test of `ota_sha256`)
- Create: `nodus/oevm/tests/ota_zephyr_test.c`
- Modify: `nodus/nsl/tools/run_tests_oevm.py` (`C_UNIT_TESTS`: one row)

**Interfaces:**
- Produces: `void ota_sha256(const uint8_t *p, long n, uint8_t out[32]);` (ota.h), and `void ota_note_running_hash(const uint8_t h[32]);` (ota_backend.h, defined only by the zephyr backend). After a note, the zephyr `ota_env("OEVM_IMAGE_HASH")` answers 64 lowercase hex chars; before one, it answers NULL.

- [ ] **Step 1: Write the failing known-answer test.** In `tests/ota_seam_test.c`, `main()` already fills `blob_a`. Add this function above `main` and call it right after `blob_a` is filled:

```c
/* rete#310: ota_sha256 is the hash the C6 prints for the program it loaded.
   HASH_A_HEX was computed with python hashlib, so this is a conformance check. */
static void test_ota_sha256_known_answer(void) {
    uint8_t h[32];
    char hex[65];
    ota_sha256(blob_a, BLOB_LEN, h);
    for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", h[i]);
    assert(strcmp(hex, HASH_A_HEX) == 0);
    printf("ota_sha256 known answer: ok\n");
}
```

- [ ] **Step 2: Run it and watch it fail.**

Run: `cd nodus/oevm && gcc -Wall -std=c99 -Isrc tests/ota_seam_test.c src/ota.c src/backend/posix/platform_posix.c -lm -o /tmp/oevm_ct_ota_seam_test && /tmp/oevm_ct_ota_seam_test`
Expected: a build failure (`implicit declaration of function 'ota_sha256'`, or an undefined reference).

- [ ] **Step 3: Implement `ota_sha256`.** In `src/ota.c`, below `sha256_final`, add the following, and replace the file-header sentence "If a second one ever appears, promote it then — not in anticipation." with "rete#310 is the second user (the C6 hashes the program it loaded), so `ota_sha256` is public; the block functions stay static."

```c
void ota_sha256(const uint8_t *p, long n, uint8_t out[32]) {
    struct sha256 c;
    sha256_init(&c);
    sha256_update(&c, p, n);
    sha256_final(&c, out);
}
```

Declare it in `src/ota.h` next to `ota_running_hash`:

```c
/* SHA-256 of p[0..n). rete#310: the C6 hashes the program it loaded with this. */
void ota_sha256(const uint8_t *p, long n, uint8_t out[32]);
```

- [ ] **Step 4: Run the test again and watch it pass.** Run the same command as step 2.
Expected: `ota_sha256 known answer: ok`, then the test's existing output, exit 0.

- [ ] **Step 5: Write the failing zephyr-backend test.** Create `tests/ota_zephyr_test.c`:

```c
/* Copyright (c) 2026 Ekorau LLC */
/* rete#310 — the zephyr ota backend answers OEVM_IMAGE_HASH once main.c has
 * noted the hash of the program it loaded, and nothing before that.
 *
 * Link set (run_tests_oevm.py C_UNIT_TESTS): src/backend/zephyr/ota_zephyr.c
 * ONLY. That file includes no zephyr header on purpose, so it builds here. */
#include "../src/ota_backend.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    uint8_t h[32];
    for (int i = 0; i < 32; i++) h[i] = (uint8_t)(i * 9 + 1);

    assert(ota_env("OEVM_IMAGE_HASH") == NULL);      /* nothing noted: nil, as before */
    ota_note_running_hash(h);
    const char *hex = ota_env("OEVM_IMAGE_HASH");
    assert(hex != NULL);
    assert(strcmp(hex, "010a131c252e374049525b646d767f88919aa3acb5bec7d0d9e2ebf4fd060f18") == 0);
    assert(ota_env("OEVM_OTA_STAGE") == NULL);       /* every other name still answers nil */
    printf("ota_zephyr: ok\n");
    return 0;
}
```

The expected string is `bytes(i*9+1 & 0xff for i in range(32)).hex()`. Before you rely on it, check it: `python3 -c "print(bytes((i*9+1)&0xff for i in range(32)).hex())"`.

Add the row to `C_UNIT_TESTS` in `nodus/nsl/tools/run_tests_oevm.py`, directly below the `ota_seam_test` row:

```python
    ("ota_zephyr_test",       ["src/backend/zephyr/ota_zephyr.c"], [], None),
```

- [ ] **Step 6: Run it and watch it fail.**

Run: `cd nodus/oevm && gcc -Wall -std=c99 -Isrc tests/ota_zephyr_test.c src/backend/zephyr/ota_zephyr.c -o /tmp/oevm_ct_ota_zephyr_test && /tmp/oevm_ct_ota_zephyr_test`
Expected: an undefined reference to `ota_note_running_hash`.

- [ ] **Step 7: Implement the backend.** In `src/ota_backend.h`, after the `ota_env` declaration, add:

```c
/* rete#310, zephyr backend only: main.c notes the SHA-256 of the program it
   loaded from program_partition; ota_env("OEVM_IMAGE_HASH") then answers it as
   64 lowercase hex chars, so ota_running_hash() and the kernel's publishApps
   report it with no change above this seam. */
void ota_note_running_hash(const uint8_t h[32]);
```

In `src/backend/zephyr/ota_zephyr.c`, replace the `ota_env` body and add the note function:

```c
static char running_hex[65];   /* "" until main.c notes the loaded program (rete#310) */

void ota_note_running_hash(const uint8_t h[32]) {
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        running_hex[2 * i]     = digits[h[i] >> 4];
        running_hex[2 * i + 1] = digits[h[i] & 0xf];
    }
    running_hex[64] = '\0';
}

const char *ota_env(const char *name) {
    /* No runner environment on target. The one name that answers is the hash of
       the program main.c loaded from program_partition (rete#310). */
    if (running_hex[0] != '\0' && strcmp(name, "OEVM_IMAGE_HASH") == 0) return running_hex;
    return NULL;
}
```

Add `#include <string.h>` beside the existing `#include <stddef.h>`. Then correct the header comment:
- "ota_running_hash 0 (nil)" becomes "ota_running_hash nil until main.c notes the program it loaded (rete#310)".
- In the paragraph "What the real one becomes", replace "the staging slot is the inactive MCUboot flash partition (flash_area_* against FIXED_PARTITION_ID(slot1_partition))" with "the program is NOT staged in slot1_partition: that is the firmware's MCUboot slot, and the program has its own `program_partition` (rete#310, `docs/design/2026-10-04-custos-peer-design.md` §6a step 5; where a staged program goes is #233's Q2)".

- [ ] **Step 8: Run both C tests and watch them pass.** Run the commands from steps 2 and 6.
Expected: `ota_sha256 known answer: ok` and `ota_zephyr: ok`, both exit 0.

- [ ] **Step 9: Commit.**

```bash
git add nodus/oevm/src/ota.c nodus/oevm/src/ota.h nodus/oevm/src/ota_backend.h \
  nodus/oevm/src/backend/zephyr/ota_zephyr.c nodus/oevm/tests/ota_seam_test.c \
  nodus/oevm/tests/ota_zephyr_test.c nodus/nsl/tools/run_tests_oevm.py
git commit -m "ota: ota_sha256 public; the zephyr backend answers the noted program hash, refs #310"
```

---

### Task 2: The C6 boots its program from `program_partition`, and `run.sh` writes it

**Files:**
- Modify: `nodus/oevm/zephyr/boards/esp32c6_devkitc_esp32c6_hpcore.overlay` (append the partition block)
- Modify: `nodus/oevm/zephyr/CMakeLists.txt:188-212` (Model B: no blob when the devicetree has `program_partition`)
- Modify: `nodus/oevm/zephyr/src/main.c:1529-1536` (externs and `program_map`) and `:1866-1874` (load, hash, print)
- Modify: `nodus/oevm/zephyr/run.sh` (the Model B blob check at ~691-715; the esptool line at ~857-859)

**Interfaces:**
- Consumes: `ota_sha256`, `ota_note_running_hash`, `ota_env` (Task 1).
- Produces: the boot line `oevm: program sha256 <64 hex> (<N> ms)`, printed after `oevm: build-id <id>` when the program loaded. When it didn't, the loader's own `oimg_load_blob: <reason>` line appears, followed by `oevm: oimg_load_blob failed`. Task 4 parses these lines.

- [ ] **Step 1: Add the partition to the overlay.** Append to `boards/esp32c6_devkitc_esp32c6_hpcore.overlay`:

```dts
/* rete#310 — the nsl program in its own flash partition, carved from the two
 * firmware slots (1792K -> 1536K each) so image-1 stays whole for MCUboot (#233).
 * The firmware links no program on a board that has this node: run.sh writes the
 * .oimg here, and `custos load` replaces it alone. The offset is cited by
 * protocol/IMAGE-ARTIFACTS.md §2a and copied once, into custos/internal/load.
 * Sizes are a starting point (firmware 587 KB, largest program 170 KB, 2026-10-05);
 * a program over 512K or firmware over 1536K is what earns a resize.
 * Deleting and redefining the slots is what moves their unit addresses; the
 * chosen code partition is restated because it named the deleted node. */
/delete-node/ &slot0_partition;
/delete-node/ &slot1_partition;
&flash0 {
	partitions {
		slot0_partition: partition@20000 {
			label = "image-0";
			reg = <0x20000 DT_SIZE_K(1536)>;
		};
		slot1_partition: partition@1a0000 {
			label = "image-1";
			reg = <0x1A0000 DT_SIZE_K(1536)>;
		};
		program_partition: partition@320000 {
			label = "program";
			reg = <0x320000 DT_SIZE_K(512)>;
		};
	};
};
/ {
	chosen {
		zephyr,code-partition = &slot0_partition;
	};
};
```

- [ ] **Step 2: Drop the blob from CMake when the partition exists.** In `CMakeLists.txt`, replace the whole `elseif(OEVM_MODEL STREQUAL "B")` body (from `if(NOT DEFINED OEVM_BLOB)` through the `OBJECT_DEPENDS` property) with the following. Keep the existing comments above `blob.S`.

```cmake
elseif(OEVM_MODEL STREQUAL "B")
  target_sources(app PRIVATE
    ${OEVM_SRC}/src/oimg_loader.c
    ${OEVM_SRC}/src/kernel_boot.c
    ${OEVM_SRC}/src/hanoi_native.c)
  # OEVM_NO_FILE_IO compiles out oimg_loader.c's oimg_load() mmap/open path,
  # which has no meaning on this target.
  target_compile_definitions(app PRIVATE OEVM_MODEL_B OEVM_NO_FILE_IO)
  # rete#310: a board whose devicetree has program_partition links NO program.
  # run.sh writes the .oimg into that partition and `custos load` replaces it
  # alone; main.c maps it. The devicetree is the one source of that fact, so C
  # (FIXED_PARTITION_EXISTS) and this file cannot disagree.
  dt_nodelabel(OEVM_PROGRAM_PARTITION NODELABEL program_partition)
  if(OEVM_PROGRAM_PARTITION)
    message(STATUS "oevm: program_partition present — no program linked (rete#310)")
  else()
    if(NOT DEFINED OEVM_BLOB)
      # Model B is the device default; a bare `west build` (no run.sh, no
      # -DOEVM_BLOB) falls back to the committed TimerNode image so it just works.
      # run.sh bakes a fresh <node>.oimg and passes -DOEVM_BLOB, overriding this.
      set(OEVM_BLOB ${CMAKE_CURRENT_SOURCE_DIR}/TimerNode.oimg)
      message(STATUS "oevm: OEVM_BLOB defaulted to ${OEVM_BLOB}")
    endif()
    if(NOT EXISTS "${OEVM_BLOB}")
      message(FATAL_ERROR "OEVM_BLOB does not exist: ${OEVM_BLOB}")
    endif()
    target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/blob.S)
    # blob.S is preprocessed (.S, capital) so OEVM_BLOB_FILE expands.
    set_property(SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/blob.S APPEND PROPERTY
      COMPILE_DEFINITIONS "OEVM_BLOB_FILE=\"${OEVM_BLOB}\"")
    # Rebuild when the blob changes.
    set_property(SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/blob.S APPEND PROPERTY
      OBJECT_DEPENDS "${OEVM_BLOB}")
  endif()
```

- [ ] **Step 3: Map, load, hash and print in `main.c`.** Replace the Model B extern block (the `#else` branch at ~1529-1535) with:

```c
#else
/* Model B: the program is a .oimg blob in flash, not baked C.
   kernel_boot() comes from oevm.h and oimg_load_blob() from oimg.h, but
   oimg_root()/oimg_bootstrap() are declared in NO header (defined in
   src/oimg_loader.c, used by src/oevm_image_main.c), so they are externed here. */
extern oop  oimg_root(void);
extern oop  oimg_bootstrap(oop entry_vtable);
#include <zephyr/storage/flash_map.h>
#if FIXED_PARTITION_EXISTS(program_partition)
/* rete#310: the program has its own flash partition, written on its own over the
   cable (`custos load`) and later over the air (#233). The firmware links none.
   spi_flash_mmap is ESP-only: a board that adds program_partition without ESP
   flash fails to compile HERE, which is the right place to learn it maps its
   flash some other way. */
#include <spi_flash_mmap.h>
#include "ota.h"
#include "ota_backend.h"
static int program_map(const uint8_t **base, size_t *size) {
    const void *p;
    spi_flash_mmap_handle_t h;
    if (spi_flash_mmap(FIXED_PARTITION_OFFSET(program_partition),
                       FIXED_PARTITION_SIZE(program_partition),
                       SPI_FLASH_MMAP_DATA, &p, &h) != 0) {
        printk("oevm: program partition 0x%x would not map\n",
               (unsigned)FIXED_PARTITION_OFFSET(program_partition));
        return 1;
    }
    uint32_t n;
    memcpy(&n, (const uint8_t *)p + 12, 4);   /* blob_size, IMAGE-ARTIFACTS.md §2 */
    /* An erased partition reads 0xFFFFFFFF here. Answer 0 so the loader refuses
       it by name ("image too small") instead of reading past the partition. */
    *base = p;
    *size = n <= FIXED_PARTITION_SIZE(program_partition) ? n : 0;
    return 0;
}
#else
extern const uint8_t oevm_blob_start[], oevm_blob_end[];   /* zephyr/blob.S */
#endif
#endif
```

At the load site (~1866-1874), replace the `if (oimg_load_blob(oevm_blob_start, … ) != 0) { … }` statement with:

```c
#if FIXED_PARTITION_EXISTS(program_partition)
    const uint8_t *prog = NULL;
    size_t prog_size = 0;
    if (program_map(&prog, &prog_size) != 0 || oimg_load_blob(prog, prog_size) != 0) {
#else
    if (oimg_load_blob(oevm_blob_start,
                       (size_t)(oevm_blob_end - oevm_blob_start)) != 0) {
#endif
        /* Loud: never boot a bad blob silently. The specific check that failed
           was just printed to stderr by the loader. */
        printk("oevm: oimg_load_blob failed\n");
        return 0;
    }
#if FIXED_PARTITION_EXISTS(program_partition)
    {
        /* rete#310 / IMAGE-ARTIFACTS.md decision 2: the node names the program it
           runs. Printed for `custos load` on the cable; noted for the ota seam, so
           the kernel's publishApps writes obs/apps/main. The cost is printed
           rather than guessed. */
        int64_t t0 = k_uptime_get();
        uint8_t h[32];
        ota_sha256(prog, (long)prog_size, h);
        ota_note_running_hash(h);
        printk("oevm: program sha256 %s (%u ms)\n", ota_env("OEVM_IMAGE_HASH"),
               (unsigned)(k_uptime_get() - t0));
    }
#endif
```

Check that the `#else` (Model A) branch above is untouched. Model A never reaches this code: it is inside `#ifdef OEVM_MODEL_B`.

- [ ] **Step 4: Teach `run.sh` the partition.** In the Model B check (the `if [ "$MODEL" = B ]; then` block that sets `ACTUAL_BLOB`), wrap the existing body as the `else` branch:

```bash
  if [ "$MODEL" = B ]; then
    # rete#310: a board whose devicetree has program_partition links no program,
    # so there is no OEVM_BLOB_FILE to check. Instead, the flash below writes $OIMG
    # at the partition's offset, read from the build's own devicetree (the overlay
    # is the writer; this is a reader, not a copy).
    PROG_REG=$(awk '/program_partition: partition@/{f=1} f && /reg = </{gsub(/[<>;]/,""); print $3, $4; exit}' \
                 "$ZDIR/$BUILD_DIR/zephyr/zephyr.dts" 2>/dev/null || true)
    if [ -n "$PROG_REG" ]; then
      read -r PROG_OFF PROG_SIZE <<< "$PROG_REG"
      OIMG_SIZE=$(stat -c %s "$OIMG")
      if [ "$OIMG_SIZE" -gt $((PROG_SIZE)) ]; then
        echo "[build] FATAL: $(basename "$OIMG") is $OIMG_SIZE B; program_partition is $((PROG_SIZE)) B." >&2
        exit 3
      fi
      echo "[build] model B, program_partition $PROG_OFF ($((PROG_SIZE)) B): $(basename "$OIMG") is written there, not linked"
    else
      # … the existing ACTUAL_BLOB block, unchanged …
    fi
  else
```

Then make the esptool call write both images when `PROG_OFF` is set:

```bash
  PROG_ARGS=()
  [ -n "${PROG_OFF:-}" ] && PROG_ARGS=("$PROG_OFF" "$OIMG")
  "$ESPTOOL" --chip "$ESPTOOL_CHIP" --port "$ESPTOOL_PORT" --baud 921600 write_flash \
    --flash_mode dio --flash_freq 40m --flash_size "$ESPTOOL_FLASH_SIZE" \
    "$ESPTOOL_OFFSET" "$ZDIR/$BUILD_DIR/zephyr/zephyr.bin" "${PROG_ARGS[@]}"
```

Initialise `PROG_OFF=""` next to `BLOB_ARG=()` (~line 497), so `set -u` holds on boards without the partition.

- [ ] **Step 5: Build and flash `c6spare` through the harness, then read the boot.** This also returns `c6spare` from the spike's MCUboot to simple boot, because esptool writes the simple-boot image at `0x0`.

Run: `cd nodus/oevm/tests/target && ./run.sh bridge-health`
Then: `grep -a "build-id\|program sha256\|oimg_load\|PORTA NODE READY\|program_partition" ~/.local/state/rete/console/console-c6spare-$(date +%F).log | tail -6` and `sha256sum nodus/oevm/zephyr/PortaBridgeNodeC6.oimg`
Expected: the run.sh output contains `[build] model B, program_partition 0x320000 (524288 B)`. The boot has `oevm: program sha256 <hex> (N ms)`, where `<hex>` equals `sha256sum`'s, followed by `PORTA NODE READY` and the scenario's own verdict. Record N (the hash cost) for the #310 comment.

- [ ] **Step 6: Falsify the hash line.** On the board, the hash and `sha256sum` share no cause, so a mismatch is the failure the line exists to show. Write a different program at the partition by hand, with the collector stopped (`systemctl --user stop rete-console@c6spare`, then `~/.espressif/python_env/idf5.4_py3.12_env/bin/esptool.py --chip esp32c6 -p <c6spare by-id port> write_flash 0x320000 nodus/oevm/zephyr/HanoiNode.oimg`, then `systemctl --user start rete-console@c6spare`). If HanoiNode's registry is stale, its loader refusal is the outcome instead, which is also a pass for this step.
Expected: the boot's `program sha256` changes to `HanoiNode.oimg`'s hash, or the loader prints its refusal and `oevm: oimg_load_blob failed`, with no `program sha256` line. Either way, the build-id line is unchanged.

- [ ] **Step 7: Commit.**

```bash
git add nodus/oevm/zephyr/boards/esp32c6_devkitc_esp32c6_hpcore.overlay nodus/oevm/zephyr/CMakeLists.txt \
  nodus/oevm/zephyr/src/main.c nodus/oevm/zephyr/run.sh
git commit -m "c6: the program in program_partition, mapped and hashed at boot; run.sh writes it, refs #310"
```

---

### Task 3: custos reads the `.oimg` prefix, and the contract names the program region

**Files:**
- Create: `custos/internal/oimg/oimg.go`
- Create: `custos/internal/oimg/oimg_test.go`
- Modify: `protocol/IMAGE-ARTIFACTS.md` (new §2a; the §2 hash paragraph; decision 2)

**Interfaces:**
- Produces: `oimg.Read(path string) (*oimg.Image, error)` with `Image{FormatVersion uint16; Flags uint16; RegistryVersion uint32; BlobSize uint32; Size int64; SHA256 string}` (SHA256 is lowercase hex of the whole file).

- [ ] **Step 1: Write the failing tests.** Create `custos/internal/oimg/oimg_test.go`:

```go
// Copyright (c) 2026 Ekorau LLC

package oimg

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/davidg238/rete/custos/internal/imagemap"
)

const fixture = "../../../protocol/fixtures/image/Fixture"

func TestTheFixtureReadsAndHashesAsItsMapSays(t *testing.T) {
	img, err := Read(fixture + ".oimg")
	if err != nil {
		t.Fatal(err)
	}
	m, err := imagemap.Load(fixture + ".oimg.map.json")
	if err != nil {
		t.Fatal(err)
	}
	if img.SHA256 != m.SHA256 || img.Size != m.Size || int64(img.BlobSize) != img.Size {
		t.Fatalf("image sha %s size %d blob %d; map sha %s size %d", img.SHA256, img.Size, img.BlobSize, m.SHA256, m.Size)
	}
}

func writeVariant(t *testing.T, edit func([]byte) []byte) string {
	t.Helper()
	b, err := os.ReadFile(fixture + ".oimg")
	if err != nil {
		t.Fatal(err)
	}
	p := filepath.Join(t.TempDir(), "x.oimg")
	if err := os.WriteFile(p, edit(append([]byte(nil), b...)), 0o644); err != nil {
		t.Fatal(err)
	}
	return p
}

func TestReadRefuses(t *testing.T) {
	cases := map[string]struct {
		edit func([]byte) []byte
		want string
	}{
		"bad magic":   {func(b []byte) []byte { b[0] = 'X'; return b }, "magic"},
		"truncated":   {func(b []byte) []byte { return b[:19] }, "20-byte prefix"},
		"size differs": {func(b []byte) []byte { return append(b, 0) }, "blob_size"},
	}
	for name, c := range cases {
		t.Run(name, func(t *testing.T) {
			_, err := Read(writeVariant(t, c.edit))
			if err == nil || !strings.Contains(err.Error(), c.want) {
				t.Fatalf("err %v, want it to mention %q", err, c.want)
			}
		})
	}
}
```

- [ ] **Step 2: Run them and watch them fail.**
Run: `cd custos && go test ./internal/oimg/ -count=1`
Expected: a build failure (`undefined: Read`).

- [ ] **Step 3: Implement.** Create `custos/internal/oimg/oimg.go`:

```go
// Copyright (c) 2026 Ekorau LLC

// Package oimg reads the part of an .oimg custos reads: the 20-byte prefix and
// the hash of the whole file (protocol/IMAGE-ARTIFACTS.md §2). Everything after
// the prefix belongs to the VM.
package oimg

import (
	"bytes"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"fmt"
	"os"
)

// Image is an .oimg as custos sees it.
type Image struct {
	FormatVersion   uint16
	Flags           uint16
	RegistryVersion uint32
	BlobSize        uint32
	Size            int64
	SHA256          string // lowercase hex of the whole file: the program's identity (decision 2)
}

// Read answers the image at path, or refuses it: a wrong magic, a file shorter
// than the prefix, or a blob_size other than the file's length.
func Read(path string) (*Image, error) {
	b, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	if len(b) < 20 {
		return nil, fmt.Errorf("%s is %d bytes, shorter than the 20-byte prefix (IMAGE-ARTIFACTS.md §2)", path, len(b))
	}
	if !bytes.Equal(b[:4], []byte("OIMG")) {
		return nil, fmt.Errorf("%s is not an .oimg (magic %q)", path, b[:4])
	}
	le := binary.LittleEndian
	img := &Image{
		FormatVersion:   le.Uint16(b[4:]),
		Flags:           le.Uint16(b[6:]),
		RegistryVersion: le.Uint32(b[8:]),
		BlobSize:        le.Uint32(b[12:]),
		Size:            int64(len(b)),
	}
	if int64(img.BlobSize) != img.Size {
		return nil, fmt.Errorf("%s: blob_size %d is not the file's length %d", path, img.BlobSize, img.Size)
	}
	sum := sha256.Sum256(b)
	img.SHA256 = hex.EncodeToString(sum[:])
	return img, nil
}
```

- [ ] **Step 4: Run the tests and watch them pass.**
Run: `cd custos && go test ./internal/oimg/ -count=1`
Expected: `ok`.

- [ ] **Step 5: Write the program region into the contract.** In `protocol/IMAGE-ARTIFACTS.md`, insert after §2's hash paragraph:

```markdown
## 2a. The program region (rete#310)

A board that has a program region runs its program from there. The firmware links
none, so writing a new program never touches the firmware's build-id.

| Board | Partition label | Offset | Size | Writer | Readers |
|---|---|---|---|---|---|
| ESP32-C6 devkit | `program_partition` | `0x320000` | 512K | `nodus/oevm/zephyr/boards/esp32c6_devkitc_esp32c6_hpcore.overlay` | `zephyr/run.sh` (from the build's `zephyr.dts`); `custos/internal/load` (a copy that cites this row) |

`blob_size` (§2) is the number of bytes the region holds. An erased region reads
`0xFFFFFFFF` there, and the firmware's loader refuses it by name. After a load, the
firmware prints `oevm: program sha256 <hex> (<N> ms)` and reports the same hash as
`obs/apps/main`'s `#image`.
```

In §2, change "Once #310 is done, it is also how custos checks which program a board is running (decision 2)." to "It is also how custos checks which program a board is running: the node prints and reports the hash of its program region (§2a, decision 2)." In §6 decision 2, change "With #310 the node will report the hash of its program region" to "Since #310 the node reports the hash of its program region".

- [ ] **Step 6: Commit.**

```bash
git add custos/internal/oimg/oimg.go custos/internal/oimg/oimg_test.go protocol/IMAGE-ARTIFACTS.md
git commit -m "custos: read the .oimg prefix; IMAGE-ARTIFACTS §2a names the program region, refs #310"
```

---

### Task 4: `custos load`

**Files:**
- Create: `custos/internal/load/load.go` (the boot report, the loader, the region table)
- Create: `custos/internal/load/load_test.go`
- Create: `custos/internal/load/board.go` (the serial boot reader; verified on the board in Task 5)
- Create: `custos/internal/load/cli.go` (`Run`)
- Modify: `custos/cmd/custos/main.go` (dispatch `load`; the comment listing the verbs)

**Interfaces:**
- Consumes: `oimg.Read` (Task 3); the boot lines from Task 2.
- Produces: `load.Run(args []string, stdout, stderr io.Writer) int`. Usage: `custos load -port <tty> [-esptool esptool.py] [-baud 115200] <name>.oimg`.

- [ ] **Step 1: Write the failing tests.** Create `custos/internal/load/load_test.go`:

```go
// Copyright (c) 2026 Ekorau LLC

package load

import (
	"bytes"
	"errors"
	"strings"
	"testing"

	"github.com/davidg238/rete/custos/internal/oimg"
)

const fixture = "../../../protocol/fixtures/image/Fixture.oimg"

func TestNoteKeepsTheLatestBoot(t *testing.T) {
	var r Report
	for _, l := range []string{
		"oevm: build-id OLD",
		"oevm: program sha256 " + strings.Repeat("a", 64) + " (40 ms)",
		"oevm: build-id NEW", // a second boot: start over
	} {
		if r.Note(l) {
			t.Fatalf("complete too early at %q", l)
		}
	}
	if !r.Note("2026-10-05T14:00:52.394-07:00 oevm: program sha256 " + strings.Repeat("b", 64) + " (41 ms)") {
		t.Fatal("build-id + program should complete the boot")
	}
	if r.BuildID != "NEW" || r.ProgramSHA != strings.Repeat("b", 64) {
		t.Fatalf("%+v", r)
	}
}

func TestNoteCompletesOnTheLoadersRefusal(t *testing.T) {
	var r Report
	r.Note("oevm: build-id B1")
	if !r.Note("oimg_load_blob: image too small (0 bytes)") || r.Refused != "image too small (0 bytes)" {
		t.Fatalf("%+v", r)
	}
}

type fakeBoard struct {
	boots []Report
	errs  []error
	n     int
}

func (b *fakeBoard) Boot() (Report, error) {
	i := b.n
	b.n++
	return b.boots[i], b.errs[i]
}

func fixtureSHA(t *testing.T) string {
	img, err := oimg.Read(fixture)
	if err != nil {
		t.Fatal(err)
	}
	return img.SHA256
}

func run(t *testing.T, board *fakeBoard, region Region) (argv []string, out string, err error) {
	var buf bytes.Buffer
	l := Loader{
		Region:  region,
		Esptool: "esptool.py",
		Port:    "/dev/ttyX",
		Board:   board,
		Flash:   func(a []string) error { argv = a; return nil },
		Out:     &buf,
	}
	err = l.Load(fixture)
	return argv, buf.String(), err
}

var big = Region{Chip: "esp32c6", Offset: 0x320000, Size: 1 << 20}

func TestLoadWritesAtTheRegionAndProvesTheFirmwareUnchanged(t *testing.T) {
	sha := fixtureSHA(t)
	b := &fakeBoard{boots: []Report{{BuildID: "B1", ProgramSHA: strings.Repeat("0", 64)}, {BuildID: "B1", ProgramSHA: sha}}, errs: []error{nil, nil}}
	argv, out, err := run(t, b, big)
	if err != nil {
		t.Fatal(err)
	}
	want := "esptool.py --chip esp32c6 --port /dev/ttyX --baud 921600 write_flash 0x320000 " + fixture
	if strings.Join(argv, " ") != want {
		t.Fatalf("argv %q\nwant %q", strings.Join(argv, " "), want)
	}
	if !strings.Contains(out, "build-id B1 unchanged") || !strings.Contains(out, sha[:12]) {
		t.Fatalf("out %q", out)
	}
}

func TestLoadIntoAnEmptyPartition(t *testing.T) {
	b := &fakeBoard{boots: []Report{{BuildID: "B1", Refused: "image too small (0 bytes)"}, {BuildID: "B1", ProgramSHA: fixtureSHA(t)}}, errs: []error{nil, nil}}
	if _, _, err := run(t, b, big); err != nil {
		t.Fatal(err)
	}
}

func TestLoadFailsWhenTheFirmwareChanged(t *testing.T) {
	b := &fakeBoard{boots: []Report{{BuildID: "B1"}, {BuildID: "B2", ProgramSHA: fixtureSHA(t)}}, errs: []error{nil, nil}}
	if _, _, err := run(t, b, big); err == nil || !strings.Contains(err.Error(), "build-id") {
		t.Fatalf("err %v", err)
	}
}

func TestLoadReportsTheLoadersRefusal(t *testing.T) {
	b := &fakeBoard{boots: []Report{{BuildID: "B1"}, {BuildID: "B1", Refused: "registry_version 7 != firmware 8"}}, errs: []error{nil, nil}}
	if _, _, err := run(t, b, big); err == nil || !strings.Contains(err.Error(), "registry_version 7 != firmware 8") {
		t.Fatalf("err %v", err)
	}
}

func TestLoadFailsWhenTheBoardRunsAnotherProgram(t *testing.T) {
	b := &fakeBoard{boots: []Report{{BuildID: "B1"}, {BuildID: "B1", ProgramSHA: strings.Repeat("f", 64)}}, errs: []error{nil, nil}}
	if _, _, err := run(t, b, big); err == nil || !strings.Contains(err.Error(), "sha256") {
		t.Fatalf("err %v", err)
	}
}

func TestLoadRefusesAnImageLargerThanTheRegion(t *testing.T) {
	b := &fakeBoard{boots: []Report{{BuildID: "B1"}}, errs: []error{nil}}
	argv, _, err := run(t, b, Region{Chip: "esp32c6", Offset: 0x320000, Size: 16})
	if err == nil || argv != nil || b.n != 0 {
		t.Fatalf("err %v argv %v boots %d: must refuse before touching the board", err, argv, b.n)
	}
}

func TestLoadStopsWhenTheBoardDoesNotBoot(t *testing.T) {
	b := &fakeBoard{boots: []Report{{}}, errs: []error{errors.New("no complete boot on /dev/ttyX within 15s — is a collector holding the port?")}}
	argv, _, err := run(t, b, big)
	if err == nil || argv != nil || !strings.Contains(err.Error(), "collector") {
		t.Fatalf("err %v argv %v", err, argv)
	}
}
```

- [ ] **Step 2: Run them and watch them fail.**
Run: `cd custos && go test ./internal/load/ -count=1`
Expected: a build failure (`undefined: Report`, `Loader`, `Region`).

- [ ] **Step 3: Implement the report, the region and the loader.** Create `custos/internal/load/load.go`:

```go
// Copyright (c) 2026 Ekorau LLC

// Package load is `custos load` (rete#310): it writes an .oimg into a board's
// program region over the cable, then proves from the boot that the firmware is
// the same build and the program is the one written.
package load

import (
	"fmt"
	"io"
	"regexp"

	"github.com/davidg238/rete/custos/internal/oimg"
)

// Region is a board's program region: protocol/IMAGE-ARTIFACTS.md §2a.
type Region struct {
	Chip   string // esptool's --chip
	Offset int64
	Size   int64
}

// ESP32C6 copies IMAGE-ARTIFACTS.md §2a's row; the writer is
// nodus/oevm/zephyr/boards/esp32c6_devkitc_esp32c6_hpcore.overlay.
var ESP32C6 = Region{Chip: "esp32c6", Offset: 0x320000, Size: 512 * 1024}

// Report is what one boot said about itself.
type Report struct {
	BuildID    string
	ProgramSHA string // empty when no program loaded
	Refused    string // the loader's own reason, when it refused the region
}

var (
	buildRe   = regexp.MustCompile(`oevm: build-id (\S+)`)
	programRe = regexp.MustCompile(`oevm: program sha256 ([0-9a-f]{64})`)
	refusedRe = regexp.MustCompile(`oimg_load_blob: (.+)`)
)

// Note folds one console line into r and answers whether the boot is complete:
// a build-id, then the program's hash or the loader's refusal. A second
// build-id is a second boot, and starts the report over.
func (r *Report) Note(line string) bool {
	if m := buildRe.FindStringSubmatch(line); m != nil {
		*r = Report{BuildID: m[1]}
		return false
	}
	if r.BuildID == "" {
		return false
	}
	if m := programRe.FindStringSubmatch(line); m != nil {
		r.ProgramSHA = m[1]
		return true
	}
	if m := refusedRe.FindStringSubmatch(line); m != nil && m[1] != "" {
		r.Refused = m[1]
		return true
	}
	return false
}

// Board resets a board and answers its boot.
type Board interface {
	Boot() (Report, error)
}

// Loader is one `custos load`.
type Loader struct {
	Region  Region
	Esptool string
	Port    string
	Board   Board
	Flash   func(argv []string) error
	Out     io.Writer
}

// Load writes the image at path into the region, and answers an error unless
// the board then boots the same firmware running exactly that program.
func (l Loader) Load(path string) error {
	img, err := oimg.Read(path)
	if err != nil {
		return err
	}
	if img.Size > l.Region.Size {
		return fmt.Errorf("%s is %d B; the %s program region holds %d B", path, img.Size, l.Region.Chip, l.Region.Size)
	}
	before, err := l.Board.Boot()
	if err != nil {
		return err
	}
	argv := []string{l.Esptool, "--chip", l.Region.Chip, "--port", l.Port, "--baud", "921600",
		"write_flash", fmt.Sprintf("0x%x", l.Region.Offset), path}
	if err := l.Flash(argv); err != nil {
		return fmt.Errorf("esptool: %w", err)
	}
	after, err := l.Board.Boot()
	if err != nil {
		return err
	}
	if after.BuildID != before.BuildID {
		return fmt.Errorf("the firmware changed across the load: build-id %s, then %s", before.BuildID, after.BuildID)
	}
	if after.Refused != "" {
		return fmt.Errorf("the board refused the program: oimg_load_blob: %s", after.Refused)
	}
	if after.ProgramSHA != img.SHA256 {
		return fmt.Errorf("the board runs sha256 %.12s, not %s's %.12s", after.ProgramSHA, path, img.SHA256)
	}
	fmt.Fprintf(l.Out, "wrote %s (%d B) at 0x%x\nfirmware build-id %s unchanged\nprogram sha256 %.12s running\n",
		path, img.Size, l.Region.Offset, after.BuildID, img.SHA256)
	return nil
}
```

- [ ] **Step 4: Run the tests and watch them pass.**
Run: `cd custos && go test ./internal/load/ -count=1`
Expected: `ok`.

- [ ] **Step 5: Write the serial board and the CLI.** Create `custos/internal/load/board.go`:

```go
// Copyright (c) 2026 Ekorau LLC

package load

import (
	"bytes"
	"fmt"
	"strings"
	"time"

	"go.bug.st/serial"
)

// SerialBoard boots a CP2102N devkit by its reset line and reads the boot. It
// pulses the reset itself rather than relying on whether an open reboots the
// board (rete#319 says yes, #355 says not from Go).
type SerialBoard struct {
	Port    string
	Baud    int
	Timeout time.Duration
}

func (b SerialBoard) Boot() (Report, error) {
	var r Report
	p, err := serial.Open(b.Port, &serial.Mode{
		BaudRate:          b.Baud,
		InitialStatusBits: &serial.ModemOutputBits{DTR: false, RTS: false},
	})
	if err != nil {
		return r, err
	}
	defer p.Close()
	if err := p.SetReadTimeout(200 * time.Millisecond); err != nil {
		return r, err
	}
	// RTS is EN and DTR is BOOT (rete#311): EN low with BOOT high, then release,
	// is a run-mode reset — target::reset_board's sequence.
	if err := p.SetRTS(true); err != nil {
		return r, err
	}
	time.Sleep(150 * time.Millisecond)
	if err := p.SetRTS(false); err != nil {
		return r, err
	}
	var buf []byte
	chunk := make([]byte, 256)
	for deadline := time.Now().Add(b.Timeout); time.Now().Before(deadline); {
		n, err := p.Read(chunk)
		if err != nil {
			return r, err
		}
		buf = append(buf, chunk[:n]...)
		for {
			i := bytes.IndexByte(buf, '\n')
			if i < 0 {
				break
			}
			line := strings.TrimSuffix(string(buf[:i]), "\r")
			buf = buf[i+1:]
			if r.Note(line) {
				return r, nil
			}
		}
	}
	return r, fmt.Errorf("no complete boot on %s within %s (build-id %q) — is a collector holding the port?", b.Port, b.Timeout, r.BuildID)
}
```

Create `custos/internal/load/cli.go`:

```go
// Copyright (c) 2026 Ekorau LLC

package load

import (
	"flag"
	"fmt"
	"io"
	"os/exec"
	"time"
)

// Run is `custos load`. args excludes the verb; it answers the exit status.
//
//	custos load -port /dev/serial/by-id/usb-Silicon_Labs_CP2102N_... PortaBridgeNodeC6.oimg
//
// It needs the port to itself: stop the board's collector first
// (systemctl --user stop rete-console@<board>).
func Run(args []string, stdout, stderr io.Writer) int {
	fs := flag.NewFlagSet("custos load", flag.ContinueOnError)
	fs.SetOutput(stderr)
	port := fs.String("port", "", "the board's console cable (required)")
	esptool := fs.String("esptool", "esptool.py", "the esptool to run")
	baud := fs.Int("baud", 115200, "the console's baud rate")
	fs.Usage = func() {
		fmt.Fprintln(stderr, "usage: custos load -port <tty> [-esptool esptool.py] [-baud N] <name>.oimg")
		fs.PrintDefaults()
	}
	if err := fs.Parse(args); err != nil {
		return 2
	}
	if *port == "" || fs.NArg() != 1 {
		fs.Usage()
		return 2
	}
	l := Loader{
		Region:  ESP32C6,
		Esptool: *esptool,
		Port:    *port,
		Board:   SerialBoard{Port: *port, Baud: *baud, Timeout: 15 * time.Second},
		Flash: func(argv []string) error {
			cmd := exec.Command(argv[0], argv[1:]...)
			cmd.Stdout, cmd.Stderr = stderr, stderr
			return cmd.Run()
		},
		Out: stdout,
	}
	if err := l.Load(fs.Arg(0)); err != nil {
		fmt.Fprintf(stderr, "custos load: %v\n", err)
		return 1
	}
	return 0
}
```

In `custos/cmd/custos/main.go`, add the dispatch after `profile` and the import `"github.com/davidg238/rete/custos/internal/load"`:

```go
	if len(os.Args) > 1 && os.Args[1] == "load" {
		os.Exit(load.Run(os.Args[2:], os.Stdout, os.Stderr))
	}
```

Then extend the comment above to "… profile (a profile dump ranked by name), load (an .oimg into a board's program region, rete#310), and sql, …".

- [ ] **Step 6: Run every Go gate.**
Run: `for m in protocol porta custos; do (cd $m && go vet ./... && go test -count=1 ./...); done`
Expected: every package `ok`, and `go vet` silent.

- [ ] **Step 7: Commit.**

```bash
git add custos/internal/load/load.go custos/internal/load/load_test.go custos/internal/load/board.go \
  custos/internal/load/cli.go custos/cmd/custos/main.go
git commit -m "custos load: write an .oimg into the program region, prove the firmware unchanged, refs #310"
```

---

### Task 5: Acceptance on `c6spare`, the gates, and the close-out

**Files:**
- Modify: `nodus/oevm/docs/vm.md` (the Model B section)
- Modify: `docs/overview.md` (first hour, step 5)
- Modify: `FOCUS.md` (at `/conclude`)

- [ ] **Step 1: Run the host gates, one at a time.**
Run: `cd nodus/nsl && python3 -u tools/run_tests_oevm.py > /tmp/claude-1000/nsl-310.log 2>&1` in the background, then read the `Passed:` line and the aux line. After it finishes, run `cd nodus/oevm && bash tests/porta_sync_host.sh`.
Expected: `Passed: 289` or more, `Failed: 0` (the suite gains `ota_zephyr_test`; the count drifts up), a green aux line with no SKIP, and `PORTA SYNC OK`.

- [ ] **Step 2: Regression on the board.** Task 2 step 5 already ran `bridge-health` on the new layout. Read its verdict and the observation window: TAPs pass, `dropped 0` throughout, and no `WEDGE` line. The window is the check that the new `obs/apps/main` row didn't crowd the first push.

- [ ] **Step 3: Read `obs/apps/main` at porta.** Find c6spare's node id in the boot log (`grep -a "node" …console-c6spare-*.log | tail -3`). On gw85224-01, read porta's running command line first (`ps -o args= -C porta`), then `./porta get <node-id> obs/apps` from `~/porta-v2` with that store flag.
Expected: `obs/apps/main` has `#image` with `#hash`, equal to `sha256sum PortaBridgeNodeC6.oimg`, and lifecycle `#running`.

- [ ] **Step 4: `custos load` end to end, starting from an erased partition.** With the collector stopped (`systemctl --user stop rete-console@c6spare`):
  1. `~/.espressif/python_env/idf5.4_py3.12_env/bin/esptool.py --chip esp32c6 -p <port> erase_region 0x320000 0x80000`
  2. Bake a changed program: copy `PortaBridgeNodeC6.nsl` to `nodus/oevm/zephyr/Load310B.nsl` with its `'PORTA NODE READY'` line changed to `'LOAD310 B READY'`. Bake it with `run.sh --node Load310B.nsl --board esp32c6 --conf-extra "prj_esp32c6_thread.conf;prj_esp32c6_thread_ftd.conf" --build-dir build-310-bake`, without `--flash`.
  3. `cd custos && go run ./cmd/custos load -esptool ~/.espressif/python_env/idf5.4_py3.12_env/bin/esptool.py -port <port> ../nodus/oevm/zephyr/Load310B.oimg`
  4. The same command with `../nodus/oevm/zephyr/PortaBridgeNodeC6.oimg`.

Expected: both loads exit 0, printing `firmware build-id <id> unchanged` with the id from Task 2 step 5, and the program's sha. The first load's "before" boot was the erased partition's refusal. This is #310's "done when".

- [ ] **Step 5: Falsify `custos load`'s check.** The board now runs `PortaBridgeNodeC6.oimg`. Load `Load310B.oimg` with `-esptool /bin/true`, which skips the write.
Expected: exit 1, naming the sha256 mismatch. If this passes, the check shares a cause with the failure it guards against. Then `systemctl --user start rete-console@c6spare` and delete `nodus/oevm/zephyr/Load310B.*`.

- [ ] **Step 6: Promote to the mainline docs.** In `nodus/oevm/docs/vm.md` § Images and deployment, after "The VM is flashed once; images are pushed.", add: "On the C6 the program lives in its own flash partition (`protocol/IMAGE-ARTIFACTS.md` §2a): `run.sh --flash` writes firmware and program, `custos load` replaces the program alone, and the node prints and reports the program's SHA-256 (rete#310)." In `docs/overview.md` first-hour row 5, replace "Spike: today the program is linked into the firmware, so every change is a full flash" with "Done on the C6 (#310): `custos load` writes the program into its own partition; the firmware's build-id is unchanged". Commit:

```bash
git add nodus/oevm/docs/vm.md docs/overview.md
git commit -m "docs: #310 promoted — vm.md Model B, overview first hour step 5, refs #310"
```

- [ ] **Step 7: Record on #310 and hand back.** Write an issue comment (≤ 5 lines): the hash cost N ms from Task 2 step 5, both loads' build-id, the `obs/apps/main` reading, and the falsification. The mainline docs that absorbed the knowledge are `protocol/IMAGE-ARTIFACTS.md` §2a, `nodus/oevm/docs/vm.md` and `docs/overview.md`. The ladder entry in `review/program-sequence.md` is David's. Leave #310 open until David merges.
