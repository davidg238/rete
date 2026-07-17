# Perfstex on ESP32 — first on-target benchmark (Model B `.oimg`)

The classic Smalltalk SpeedTester, ported to OEVM/Newspeak
(`nsl-tests/00_perf/perfstex.ns`, `public main: platform args:` — a node entry, so it
boots unmodified), run on the **ESP32-D0WDQ6 / WROOM-32 devkit @ 240 MHz** via the
**Model B `.oimg`** path: a generic VM plus the program as a blob `.incbin`'d into
flash `.rodata`, no `src/solidified_image.c` in the link.

Commit: tuvm `b7f4a54` · date 2026-07-16 · Zephyr 4.2.0 (`~/zephyr-esp32`, upstream)

Config — **all seven** knobs (the float pair is not optional; see §3):

```
OEVM_HEAP_SIZE=65536 METHOD_CACHE_SIZE=256 PRIM_CACHE_SIZE=256
VM_STACK_INIT_WORDS=4096 OEVM_MARK_STACK_SIZE=512
OEVM_FLOAT_ARENA_SLAB_CELLS=512 OEVM_FLOAT_ARENA_SLAB_BUDGET=4
```

Reference for the whole exercise: `ns-zephyr/docs/results.md` **M3** (the same
benchmark on nRF52840-DK, PASS 2026-06-28) and `ns-zephyr/docs/ZEPHYR_NODE_GUIDE.md` §3.

## 1. Result — six sub-tests + `done`

```
integerMath: 30      floatMath: 62       arrayWrite: 1431
arrayIterate: 1026   stringCompare: 376  hanoi18: 4248      done
```

## 2. ESP32 vs nRF52840 — same VM, same iteration counts, different silicon

The honest comparison is OEVM-on-ESP32 against OEVM-on-nRF52840: identical VM and
identical iteration counts, so this measures **the chip**, not the implementation.
Clock ratio **240 MHz ÷ 64 MHz = 3.75×** is the number to beat.

| Sub-test | Iterations | OEVM/nRF52840 (ms) | **OEVM/ESP32 (ms)** | speedup |
|---------------|-----------:|-----------:|----------:|--------:|
| integerMath   |      1,000 |         95 |    **30** |   3.17× |
| floatMath     |      1,000 |        284 |    **62** | **4.58×** |
| arrayWrite    |     10,000 |      4,751 | **1,431** |   3.32× |
| arrayIterate  |      1,000 |      3,407 | **1,026** |   3.32× |
| stringCompare |     10,000 |    1,163 † |   **376** |   3.09× |
| hanoi(18)     |    262,143 |     14,719 | **4,248** |   3.46× |

† the **post-`oevm#17`** nRF number (identity fast path in `String=`, `oevm cee55d4`),
not the 1,609 in M3's table. tuvm carries that fix, so comparing against the pre-fix
figure would flatter the ESP32 by ~40%.

**Reading it.** Five of six land at **3.09–3.46×**, consistently *just under* the
3.75× clock ratio — expected for a part that executes XIP out of external flash
through a cache, against the nRF's tightly-coupled internal flash. That the spread is
this tight is itself the signal: nothing here is pathological, and the VM scales with
the clock.

**The outlier is `floatMath` at 4.58× — above the clock ratio**, i.e. the ESP32 is
faster than clock speed alone explains. Not investigated. Candidate causes worth
separating before anyone quotes this number: both parts emulate `double` in software
(the ESP32 LX6 FPU and the Cortex-M4F FPU are both single-precision only), so this is
a soft-float codegen and/or memory-system difference, not "the ESP32 has an FPU".
`floatMath` is also the one sub-test whose working set is the float arena, which is
configured differently here than the M4 default path — a confound to rule out.

**Berry is deliberately absent.** M3 ran a Berry head-to-head, but those numbers are
nRF52840 silicon. Putting them beside an ESP32 column would measure the chip and
report it as a VM difference. If a Berry comparison is wanted on this part, Berry has
to run on this part.

## 3. Footprint

| Region | Bytes | % of ESP32 |
| ------ | ----- | ---------- |
| FLASH  | 294,684 | 7.03 % (of 4 MB) |
| dram0  | 135,024 | 68.68 % (of 192 KB) |

For contrast, M3's nRF52840 build was 216,584 B / 256 KB (**82.6%**) RAM.

The blob itself is **112,832 bytes** of `.oimg`, living in DROM (flash-mapped
`.rodata`, XIP'd in place — never copied to RAM). That is the point of Model B: the
program costs flash, not SRAM.

## 4. The float arena — a bug this benchmark found twice

`floatMath` aborts on its **first boxed float** unless the arena is shrunk:

```
VM Float Arena Error: Out of Memory
abort()
```

`gc.c`'s slab is `OEVM_FLOAT_ARENA_SLAB_CELLS` cells, default **4096**. On Xtensa
`struct Float` is **16 B** (8-byte `double` alignment — the same ABI fact
`zephyr/layout-esp32.json` records), so one `struct FloatSlab` is
`4 + 4096*16 + 512` = **66,052 B ≈ 64.5 KB** against this build's **~60 KB libc
heap**. The first `malloc` cannot succeed — it is not a shortage, it is arithmetic.

This is the *same bug* `ns-zephyr/docs/results.md` M3 records finding on nRF52840 in
June 2026: *"OEVM allocated boxed floats in 4096-cell (~64 KB) slabs — larger than
free RAM, so `floatMath` aborted on the first box."* `zephyr/CMakeLists.txt` already
forwards both knobs; only `run.sh`'s export list was short (fixed — it exports seven
now). **The lesson worth keeping: "the five load-bearing knobs" was never true — it
was true only for nodes that never box a float**, and TimerNode never does.

## 5. Reproduce

`perfstex.ns` lives outside `zephyr/`, and `run.sh` currently resolves `--node`
relative to `zephyr/` only — so it cannot drive this yet (a papercut worth closing;
every other node is in-tree). Until then:

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

## 6. Caveats

- **Single run, no repeats.** No median-of-N, no variance. `millis` is `k_uptime_get()`
  (1 ms tick), so `integerMath: 30` carries ~3% quantisation on its own. Treat the
  small sub-tests as indicative; `hanoi18`/`arrayWrite` are the trustworthy ones.
- The ESP32 numbers were taken with the **float arena at 512/4**; the nRF M3 numbers
  used the same 512/4, so that axis is matched.
- No claim is made that this is the ESP32's ceiling — nothing was tuned for it.
