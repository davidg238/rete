# tuvm on-device results — ESP32

On-silicon milestones for the ESP32 port. Shape deliberately mirrors
`ns-zephyr/docs/results.md` (the nRF52840 predecessor's ledger) so the two read
side by side; `E` numbers here, `M`/`R` numbers there.

Board: **ESP32-D0WDQ6 / WROOM-32 devkit, rev v1.0 @ 240 MHz**, `/dev/ttyUSB0` (CP210x),
`esp32_devkitc/esp32/procpu`, Zephyr **4.2.0** upstream via the `~/zephyr-esp32`
workspace (not NCS — ESP32 is absent from NCS's manifest). Config `prj_esp32.conf`.

Status/progress lives in the ladder (`tuvm/review/program-sequence.md`, Phase 2 rung 2),
not here. This file is measurements + the evidence behind them.

| Milestone | Program | Heap cap | Expected | Result | RAM (build report) |
|-----------|---------|---------:|----------|--------|--------------------|
| E1 | `zephyr/TimerNode.ns` (**Model A**, solidified) | 64 KB | `TIMER OK`, then alive | PASS (2026-07-16) | not captured † |
| E2 | `zephyr/TimerNode.ns` (**Model B**, `.oimg`) | 64 KB | `TIMER OK`, then alive | PASS (2026-07-16) | 135024 B / 192 KB (68.68%) |
| E3 | `nsl-tests/00_perf/perfstex.ns` (6 sub-tests, **Model B**) | 64 KB | six timings + `done` | PASS (2026-07-16) | 135024 B / 192 KB (68.68%) |

† E1's green run predates the Model A/B knob and its build was never re-run under
plain `prj_esp32.conf` — the only surviving Model A elf carries the `coredump.conf`
overlay, so quoting its RAM would misreport the shipping config. The **program**
evidence is the captured log, not a build report. Re-running Model A costs ~50 min and
~4.9 GB (see §4.2); it has not been judged worth it, since Model B supersedes it.

Green logs for all three: `tuvm/zephyr/esp32-debug-artifacts/`.

---

## E1 — TimerNode on ESP32 (Model A): rung-2 gate 1

The real on-target `k_timer` ISR path (`timer_zephyr.c`), no radio, no GPIO. Green after
**two stacked bugs** — the ROM `fflush` fault (`596b208`) and, once that was unmasked, an
uncancelled `every: 200` repeater filling the 16-deep pulse mailbox (`883cdc5`). The full
three-state timeline is on the ladder; the short version is
`dead 0.891s → dead 4.290s → ALIVE at 20s`.
Log: `esp32-debug-artifacts/tuvm-esp32-GREEN-both-fixes.log`.

## E2 — TimerNode on ESP32 (Model B `.oimg`): the build knob's gate

Same node, same output, via the **Model B** path: a generic VM (`oimg_loader.c` +
`kernel_boot.c`) plus the program as a `.oimg` blob `.incbin`'d into flash `.rodata`
(`zephyr/blob.S`), with **no `src/solidified_image.c` in the link**. This is the
`ENG_A`/`ENG_B` split `tools/run_c_tests.sh:28-29` has always run on the host, ported to
Zephyr behind `-DTUVM_MODEL=A|B`.

Verified in the ELF: `tuvm_blob_start=0x3f400884`, `tuvm_blob_end=0x3f41b31c`, span
**109208 B** — exactly the `.oimg` size — both in **DROM** (flash-mapped `.rodata`,
XIP'd in place, never copied to RAM), 4-aligned per `blob.S`'s `.balign 4`.
`run_solidified_entry_point`/`init_solidified_vtables` are absent from the image.

**Why this milestone exists — the loop, not the node:**

| | Model A | Model B |
|---|---:|---:|
| whole pristine loop (bake→build→flash→capture) | ~50 min, **OOM** | **44.0 s** |
| pristine build alone | ~46 min | **11.6 s** |
| `.incbin` of the blob (real Xtensa `as`) | — | **0.007 s** |

Log: `esp32-debug-artifacts/tuvm-esp32-GREEN-modelB-oimg.log`.
Recipe: `zephyr/run.sh --model B --node TimerNode.ns --flash --capture 20`.

**A and B are not byte-identical on the console — B is cleaner.** Both print `TIMER OK`
and sit alive, but Model A also emits 48 lines of the non-fatal ROM-`setjmp`
`<err> os: ** SYSCALL` register-window spill dumps (PC `0x4005626f`); **Model B emits
zero**. B never enters the ROM `setjmp` path. That is new information about the ladder's
own "setjmp soundness on Xtensa unverified" loose thread — the noise is Model-A-specific,
not intrinsic to the VM on this part. Not investigated; do not assume benign.

## E3 — Perfstex on ESP32 (Model B): first on-target benchmark

The classic Smalltalk SpeedTester, ported to OEVM/Newspeak
(`nsl-tests/00_perf/perfstex.ns` — already a node, `public main: platform args:`, so it
boots unmodified). Six sub-tests + `done`:

```
integerMath: 30      floatMath: 62       arrayWrite: 1431
arrayIterate: 1026   stringCompare: 376  hanoi18: 4248      done
```

### ESP32 vs nRF52840 — same VM, different silicon

The honest comparison is OEVM-on-ESP32 against OEVM-on-nRF52840 (`ns-zephyr` **M3**,
PASS 2026-06-28): identical VM, identical iteration counts, so this measures **the chip**,
not the implementation. Clock ratio **240 MHz ÷ 64 MHz = 3.75×** is the number to beat.

| Sub-test | Iterations | OEVM/nRF52840 (ms) | **OEVM/ESP32 (ms)** | speedup |
|---------------|-----------:|-----------:|----------:|--------:|
| integerMath   |      1,000 |         95 |    **30** |   3.17× |
| floatMath     |      1,000 |        284 |    **62** | **4.58×** |
| arrayWrite    |     10,000 |      4,751 | **1,431** |   3.32× |
| arrayIterate  |      1,000 |      3,407 | **1,026** |   3.32× |
| stringCompare |     10,000 |    1,163 † |   **376** |   3.09× |
| hanoi(18)     |    262,143 |     14,719 | **4,248** |   3.46× |

† the **post-`oevm#17`** nRF figure (identity fast path in `String=`, `oevm cee55d4`),
not the 1,609 in M3's own table. tuvm carries that fix, so quoting M3's pre-fix number
would flatter the ESP32 by ~40%.

**Reading it.** Five of six land at **3.09–3.46×**, consistently *just under* the 3.75×
clock ratio — expected for a part executing XIP out of external flash through a cache,
against the nRF's tightly-coupled internal flash. The tightness of that spread is itself
the signal: nothing is pathological, and the VM scales with the clock.

**`floatMath`'s 4.58× is above the clock ratio and is NOT explained — do not quote it**
until someone separates the candidates. It is *not* "the ESP32 has an FPU": the LX6 and
the Cortex-M4F FPUs are both single-precision, so **both parts emulate `double` in
software**. That leaves soft-float codegen and/or the memory system — and `floatMath` is
also the one sub-test whose working set is the float arena (§4.1), a confound to rule out
first.

**Berry is deliberately absent.** M3 ran a Berry head-to-head, but those numbers are
nRF52840 silicon. Placing them beside an ESP32 column would measure the chip and report
it as a VM difference. For a Berry comparison on this part, Berry must run on this part.

### Footprint

| Region | Bytes | % of ESP32 |
| ------ | ----- | ---------- |
| FLASH  | 294,684 | 7.03 % (of 4 MB) |
| dram0  | 135,024 | 68.68 % (of 192 KB) |

M3's nRF52840 build was 216,584 B / 256 KB (**82.6%**) for comparison. The blob is
**112,832 B** of `.oimg` in DROM — XIP'd from flash, never copied to SRAM. That is the
point of Model B: **the program costs flash, not RAM.**

---

## 4. Two embedded gotchas these milestones surfaced

### 4.1 The float arena — a bug found twice, on two chips

`floatMath` aborts on its **first boxed float** unless the arena is shrunk:

```
VM Float Arena Error: Out of Memory
abort()
```

Arithmetic, not shortage. `gc.c`'s slab is `OEVM_FLOAT_ARENA_SLAB_CELLS` cells, default
**4096**. On Xtensa `struct Float` is **16 B** (8-byte `double` alignment — the same ABI
fact `zephyr/layout-esp32.json` records), so one `struct FloatSlab` is
`4 + 4096*16 + 512` = **66,052 B ≈ 64.5 KB** against this build's **~60 KB libc heap**.
The first `malloc` can never succeed.

`ns-zephyr` hit and fixed this *identical* bug on nRF52840 in June 2026 — `results.md`
M3: *"OEVM allocated boxed floats in 4096-cell (~64 KB) slabs — larger than free RAM, so
`floatMath` aborted on the first box"*; `ZEPHYR_NODE_GUIDE.md` §3 pins the values.
`zephyr/CMakeLists.txt` already forwarded both knobs; only `run.sh`'s export list was
short.

**So "the five load-bearing knobs" — repeated by the ladder, the plans and `run.sh` —
was never true. It is seven.** Five is correct only for nodes that never box a float,
and TimerNode never does:

```
OEVM_HEAP_SIZE=65536 METHOD_CACHE_SIZE=256 PRIM_CACHE_SIZE=256
VM_STACK_INIT_WORDS=4096 OEVM_MARK_STACK_SIZE=512
OEVM_FLOAT_ARENA_SLAB_CELLS=512 OEVM_FLOAT_ARENA_SLAB_BUDGET=4    # <- not optional
```

`run.sh` now exports all seven. Values are ns-zephyr's, proven on nRF52840.

### 4.2 Model A's build is a memory bug, not just a slow one

`cc1` on `src/solidified_image.c` reaches **~4.9 GB RSS** and has been **OOM-killed by
the kernel**, taking the terminal's cgroup with it (`oom-kill: task=cc1`,
`anon-rss:4865120kB`; 15 GB box, ~9 GB desktop, 4 GB swap full). Model A's build cannot
be casually scheduled — free RAM first, or use Model B. Note the cost is **`cc1`**,
*before* `as` ever runs: docs blaming the ~50 min on `as` chewing 941.7 MB of generated
asm are at best incomplete.

---

## 5. Reproduce

E2 (and any in-tree node):

```sh
zephyr/run.sh --model B --node TimerNode.ns --flash --capture 20
```

E3 — **`run.sh` cannot drive this yet**: `perfstex.ns` lives in `nsl-tests/00_perf/` and
`run.sh` resolves `--node` relative to `zephyr/` only (a papercut worth closing; every
other node is in-tree). Until then:

```sh
source ~/zephyr-esp32/.venv/bin/activate
export ZEPHYR_SDK_INSTALL_DIR=~/zephyr-sdk-0.17.4
source ~/zephyr-esp32/zephyr/zephyr-env.sh
export OEVM_HEAP_SIZE=65536 METHOD_CACHE_SIZE=256 PRIM_CACHE_SIZE=256 \
       VM_STACK_INIT_WORDS=4096 OEVM_MARK_STACK_SIZE=512 \
       OEVM_FLOAT_ARENA_SLAB_CELLS=512 OEVM_FLOAT_ARENA_SLAB_BUDGET=4

python3 ~/workspace/nsl-tuvm/tools/ns_compile_oevm.py \
  ~/workspace/nsl-tests/00_perf/perfstex.ns /tmp/perfstex.oimg \
  --format=oimg --layout=zephyr/layout-esp32.json

cd zephyr && west build -b esp32_devkitc/esp32/procpu -d build-esp32-perf . -- \
  -DCONF_FILE=prj_esp32.conf -DTUVM_MODEL=B -DTUVM_BLOB=/tmp/perfstex.oimg
~/.espressif/python_env/idf5.4_py3.12_env/bin/esptool.py --chip esp32 --baud 921600 \
  write_flash --flash_mode dio --flash_freq 40m --flash_size 4MB 0x1000 \
  build-esp32-perf/zephyr/zephyr.bin
python3 capture_coredump.py 60        # hanoi18 alone is ~4.2 s; 60 s is generous
```

The console here emits **~520 B/s** (~20× under the 115200 line rate), so short capture
windows truncate output silently and mid-token. Budget generously.

## 6. Caveats

- **E3 is a single run, no repeats** — no median-of-N, no variance. `millis` is
  `k_uptime_get()` (1 ms tick), so `integerMath: 30` carries ~3% quantisation on its own.
  Treat the small sub-tests as indicative; `hanoi18`/`arrayWrite` are the trustworthy rows.
- Both ESP32 and nRF M3 numbers used the float arena at **512/4**, so that axis is matched.
- Nothing was tuned for the ESP32; no claim is made that these are its ceiling.
- E1's RAM is not captured (see the ledger's † note).
