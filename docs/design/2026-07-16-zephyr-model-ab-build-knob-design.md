# Zephyr Model A/B build knob + staged .oimg bring-up (design)

**Date:** 2026-07-16
**Status:** designed, not implemented
**Owning rung:** `review/program-sequence.md` Phase 2 rung 2 (Zephyr bring-up of the
backend seam) — still open.
**Feeds:** `review/reliability-gaps.md` **G1** (OTA: does the `.oimg` swap independently
of the firmware?). Stage 2 below is the experiment that answers it.

---

## 1. Why — the measured problem

A pristine ESP32 build takes **~50 minutes**, and essentially all of it is one file.
Measured live on 2026-07-16 (`esp32_devkitc/esp32/procpu`, `-Os`):

| Fact | Value |
|---|---|
| `src/solidified_image.c` (generated) | 946 KB of C |
| the `.s` gcc hands to the assembler | **941.7 MB** |
| lines of assembly | **70,991,752** |
| time in `as` alone | ~35–45 min, pinned at 1 core, single-threaded |
| ninja targets before it | 823, all done in the first ~14 min |

The blowup is **Xtensa literal pools** and is architecture-specific. Xtensa cannot encode
a 32-bit constant inline, so every symbol reference emits its own pool entry:

```asm
	.literal_position
	.literal .LC0, Symbol_value
	.literal .LC1, StaticSymbol_value
	.literal .LC2, VtableVtable
```

`solidified_image.c` is one enormous table of cross-referencing pointers — precisely the
worst case. **~1000× expansion, C → asm.**

This corrects a banked belief: the "~8 min" figure in prior notes is not the ESP32
number. It is plausibly the nRF/ARM number, where literal pools are far cheaper. On
Xtensa the same file is ~6× worse.

**Model B removes `solidified_image.c` from the link entirely**, so this cost does not
merely shrink — it disappears. That is the point of this work.

Same root cause, different symptom, already banked: compiling that file at `-O0` moves
its `const` tables from flash `.rodata` into DRAM `.data` and overflows `dram0_0_seg` by
~17 MB. Both symptoms are the file; only Model B removes the file.

## 2. What is actually true (corrections to prior belief)

Three things were believed and are wrong or imprecise. All three were checked against the
tree on 2026-07-16.

1. **The clash is narrower than `zephyr/CMakeLists.txt` claims.** Its comment says
   `oimg_loader.c` *and* `kernel_boot.c` both define `Vtable_*`/kernel statics that clash
   with `solidified_image.c`. Only **`kernel_boot.c`** actually does. The host's
   `run_c_unit_tests()` (`nsl-tuvm/tools/run_tests_oevm.py`) links
   `oimg_loader.c` + `solidified_image.c` **together** and its own docstring says
   "NOT kernel_boot.c, whose static `Vtable_*` defs would clash". `oimg_loader.c`'s real
   conflict on Zephyr is duller: it defines the same four globals that
   `zephyr/src/main.c:113-116` hard-stubs (`oimg_blob_base`, `oimg_blob_end`,
   `builtin_registry`, `oimg_singleton`). That is an `#ifdef`, not a design fork.

2. **The embedded seam already exists and was built for exactly this.** `src/oimg.h`:

   ```c
   /* S0 (R6): no-POSIX byte-array loader for embedded targets. Validates and
      sets oimg_blob_base/end + the header refs from an in-memory image; does not
      own `base`. oimg_load(path) is a mmap wrapper over this (desktop only). */
   int oimg_load_blob(const uint8_t *base, size_t size);
   ```

   The POSIX path (`open`/`mmap`) is already fenced behind `#ifndef OEVM_NO_FILE_IO`.

3. **32-bit Model B is already proven on host.** `tests/test_portability_32.sh` (Task 14)
   serializes, loads and runs a `.oimg` under `-m32` with `oop_size == 4`.

So Model B is **not "unwired"**. It is wired on the host and never connected on the
Zephyr side. The remaining work is genuinely small.

## 3. Design

### 3.1 The knob — `TUVM_MODEL=A|B`

`zephyr/CMakeLists.txt` gains one option, default `A` (today's behaviour, so nothing
regresses):

| | Model A | Model B |
|---|---|---|
| image source | `src/solidified_image.c` | `zephyr/blob.S` (`.incbin` of the `.oimg`) |
| loader | — | `src/oimg_loader.c` |
| kernel | (baked into the image) | `src/kernel_boot.c` |
| defines | — | `-DTUVM_MODEL_B -DOEVM_NO_FILE_IO` |
| build dir | `build-esp32-A` | `build-esp32-B` |

The two link sets are **mutually exclusive by construction** — `solidified_image.c` XOR
`kernel_boot.c`. That is the whole of the clash, and selecting one set is the whole of
the fix. Separate build dirs so A and B never clobber each other's CMake cache.

### 3.2 The boot fork — `zephyr/src/main.c`

The only source fork, and it is small (4 calls vs 3). Host Path B's sequence
(`oevm_image_main.c`) is the reference, including the ordering constraint.

Note the fork is **two separate regions**, not one. The stub globals are at *file scope*
(today `main.c:113-116`) and are what `oimg_loader.c` would duplicate-define; the boot
calls are inside `main()`. Getting this wrong is the most likely first-try link error.

*File scope:*

```c
#ifndef TUVM_MODEL_B
/* Model A XIP stubs — is_in_blob() checks oimg_blob_base first and returns 0 while it
   is NULL, so the XIP accessor paths are never taken. Under Model B these MUST NOT be
   compiled: src/oimg_loader.c owns all four, and defining both duplicate-defines. */
const uint8_t *oimg_blob_base = NULL;
const uint8_t *oimg_blob_end  = NULL;
oop builtin_registry[BUILTIN_COUNT] = {0};
oop oimg_singleton[3] = {0};
#endif

#ifdef TUVM_MODEL_B
extern const uint8_t tuvm_blob_start[], tuvm_blob_end[];   /* from blob.S */
extern void kernel_boot(void);
extern void (*gc_additional_roots_hook)(void);
static void roots_cb(void) {}
#endif
```

*Inside `main()`:*

```c
#ifdef TUVM_MODEL_B
    gc_additional_roots_hook = roots_cb;
    kernel_boot();                       /* ONCE, before load/bootstrap */
    if (oimg_load_blob(tuvm_blob_start, (size_t)(tuvm_blob_end - tuvm_blob_start)) != 0) {
        printk("oimg_load_blob failed\n");   /* loud: a bad blob must not boot silently */
        return;
    }
    oimg_bootstrap(oimg_root());
#else
    init_solidified_vtables();
    init_reflection_symbols();
    run_solidified_entry_point();
#endif
```

### 3.3 `zephyr/run.sh` — the loop

One entry point owning every papercut hit on 2026-07-16:

```
zephyr/run.sh --model A|B --node TimerNode.ns [--flash] [--capture SECS] [--pristine]
```

It owns:

- **cwd** — the app is `zephyr/`, not the repo root (`. doesn't contain a CMakeLists.txt`).
- **the five env exports** — `OEVM_HEAP_SIZE=65536 METHOD_CACHE_SIZE=256
  PRIM_CACHE_SIZE=256 VM_STACK_INIT_WORDS=4096 OEVM_MARK_STACK_SIZE=512`. Load-bearing,
  and `-p always` discards the CMake cache that would otherwise carry them.
- **the compiler path** — `~/workspace/nsl-tuvm/tools/ns_compile_oevm.py`. NOT
  `~/workspace/ns-oevm`, which `bench.sh:8` still hardcodes and which is a *different,
  stale* repo (its HEAD predates the current work by ~2 weeks). Fixing `bench.sh` is out
  of scope here but should be filed.
- **the bake**, per model:
  - A: `ns_compile_oevm.py <node>.ns src/solidified_image.c` (default `--format=c`)
  - B: `ns_compile_oevm.py <node>.ns <node>.oimg --format=oimg --layout=zephyr/layout-esp32.json`
- **build dir** per model; incremental by default, `--pristine` opts into `-p always`.
- **flash** (`--flash`) — esptool directly, NOT `west flash`, which would trigger a
  rebuild: `esptool.py --chip esp32 --baud 921600 write_flash --flash_mode dio
  --flash_freq 40m --flash_size 4MB 0x1000 <build>/zephyr/zephyr.bin`.
- **capture** (`--capture N`) — `capture_coredump.py N`, which resets and logs the console.

It must also **restore `src/solidified_image.c`** on exit (it is tracked, committed as the
`MethodCallTest` fixture; a bake dirties it and must never be committed).

### 3.4 Why `.incbin`, not a C byte array

The obvious way to embed the blob is `xxd -i` into a `const uint8_t[]`. Don't. That
re-enters the assembler through the front door — hundreds of thousands of `.byte`
directives — which is the cost this whole design exists to escape. Instead:

```asm
/* zephyr/blob.S */
    .section .rodata
    .global tuvm_blob_start
tuvm_blob_start:
    .incbin "TimerNode.oimg"
    .global tuvm_blob_end
tuvm_blob_end:
```

`.incbin` is a byte-for-byte file copy at assemble time: no parsing, no literal pools,
effectively free. It also lands the blob in flash `.rodata`, which is what
`is_in_blob()`/`oimg_resolve()` need — the ESP32 maps its DROM segment, so the blob is
directly addressable and XIP works with no mmap call.

This was the load-bearing assumption of stage 1. **VERIFIED 2026-07-16, before any
implementation** — `.incbin` of a 946 KB blob through
`xtensa-espressif_esp32_zephyr-elf-as`:

| | assembler wall time |
|---|---|
| `src/solidified_image.c` | **~46 min** |
| `blob.S` `.incbin`, same byte count | **0.00 s** |

The object is 925 KB and the bytes land in `.rodata` (`size=0xe6f50` = 946,000 exactly),
i.e. flash-resident and directly addressable — exactly what `is_in_blob()` /
`oimg_resolve()` require, with no mmap call. The margin is not close, so stage 1's payoff
is real.

## 4. Staging

### Stage 0 — the layout, on target (blocking)

The `.oimg` is **width- and ABI-specific**. `ns_compile_oevm.py --format=oimg` takes
`--layout=<layout.json>`, produced by `tools/dump_layout.c` — which must **run** on the
target ABI to emit it. `test_portability_32.sh` runs it under `-m32` on x86. **An Xtensa
binary cannot run on the host.**

Rejected: reusing `/tmp/layout32.json` from `-m32` as a proxy. Both are 32-bit with 4-byte
oops, but the i386 SysV ABI aligns `double` to 4 bytes while Xtensa aligns it to 8. With
`CONFIG_FPU=y` and `CONFIG_PICOLIBC_IO_FLOAT=y` in `prj_esp32.conf`, floats are live —
so a struct containing one could silently differ in offset. A wrong layout means a blob
that loads and then misbehaves, which is far worse than one that fails to load.

**Do:** build a minimal Zephyr app that runs `dump_layout`'s logic on the ESP32 and prints
its JSON over the console; capture it and bank it as `zephyr/layout-esp32.json`, committed.
It only changes when a struct or the toolchain changes.

Note `dump_layout.c` currently `printf`s to stdout; on target that is the console, which
is fine — and per the 2026-07-16 `fflush` finding, it must **not** call `fflush`.

### Stage 1 — embedded blob (the build-time win)

- Bake `<node>.ns` → `<node>.oimg` with `layout-esp32.json`.
- `blob.S` `.incbin`s it; `TUVM_MODEL=B` links loader + `kernel_boot`.
- **Gate:** `run.sh --model B --node TimerNode.ns --flash` prints `TIMER OK` on the
  device — identical output to Model A, which is exactly the A==B equivalence the host
  already asserts.
- **Payoff:** firmware becomes fixed; a script change re-bakes a small blob. Still a full
  reflash per change, but a fast one.

### Stage 2 — flash partition (a later rung; feeds G1)

Blob moves to its own flash partition. esptool writes **only the blob** at its offset; the
firmware never rebuilds. This is the concrete form of the G1 question — "does the `.oimg`
swap independently?" — and answering it with a working artifact beats answering it on
paper. Explicitly **out of scope** for this design; stage 1 must be green first.

## 5. Risks

| Risk | Severity | Mitigation |
|---|---|---|
| ~~**`.incbin` assumption wrong**~~ — would have evaporated stage 1's entire payoff | ~~HIGH~~ **RETIRED** | **Verified 2026-07-16 before implementation** (§3.4): 0.00 s vs ~46 min, bytes in `.rodata`. Not close. |
| Layout mismatch host-vs-Xtensa | HIGH | Stage 0. Do not proxy with `-m32`. |
| `.oimg` format/registry version skew | MED | `oimg_load_blob` already validates magic, `format_version == 8`, and `registry_version` against the firmware's `BUILTIN_REGISTRY_VERSION = 5`, and fails loudly. Free. |
| `oimg_loader.c` uses `fprintf(stderr, ...)` on error paths | LOW | picolibc `fprintf` is fine (it resolves to `T`, not ROM `A`). Per 2026-07-16: **never `fflush`**. |
| Model B's `kernel_boot` path never ran on Zephyr | MED | Ordering is pinned by `oevm_image_main.c` (`kernel_boot()` ONCE, before load/bootstrap). Host `-m32` proves the sequence. |
| A/B drift | LOW | One app, one `#ifdef`, shared backends. The alternative (two app dirs) was rejected for exactly this — today's `fflush` fix would have landed in only one. |

## 6. Success criteria

1. `run.sh --model A --node TimerNode.ns --flash` reproduces today's behaviour (`TIMER OK`).
2. `run.sh --model B --node TimerNode.ns --flash` prints the **same** `TIMER OK`.
3. Model B wall-clock is **dramatically** below Model A's ~50 min. Bank both numbers in
   `review/metrics-spec.md` terms — the ladder wants each phase's build/footprint lines.
4. `zephyr/layout-esp32.json` is committed and reproducible.
5. Neither model leaves `src/solidified_image.c` dirty.

## 7. Out of scope

- Stage 2 (flash partition / independent blob OTA) — its own rung.
- Fixing `bench.sh`'s stale `ns-oevm` path — file it.
- The nRF/`prj_dk.conf` side. The knob is written to generalize, but only ESP32 is proven.
- The ROM-bound `setjmp`/`longjmp` thread (`vm.c`'s raise/ensure boundary on Xtensa).
