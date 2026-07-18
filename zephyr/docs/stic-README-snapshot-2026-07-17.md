# STIC Smalltalk benchmark

A translation to Toit, of a Smalltalk benchmark written by the Smalltalk Industry Council (STIC) in the late 90's.  
It is only published for comparison to the original Smalltalk benchmark and to enable results to be reproduced.  
See also the [original post](https://ekorau.com/some-numbers/).

### Update 2026-07-17

The Toit code has had minor updates since the original post.  
A comparison of several language implementations on an embedded target.

STIC benchmark

| Test | Toit | uLisp  | CircuitPython | tuvm/nsl † |
| --: | --: | --: | --: | --: |
| alloc | 16.0 | 21.6 | 11.4 | 24.1 |
| array write | 19.4 | 98.1 | 63.3 | 138.9 |
| dictionary write | 5.7 | 1.4 | 0.9 | 43.3 |
| float math | 41.1 | 124.3 | 20.9 | 35.1 |
| integer math | 9.4 | 122.5 | 14.4 | 15.3 |
| collection iterate | 12.7 | 42.5 | 29.2 | 532.6 |
| collection write | 9.5 | 6.7 | 15.4 | 97.0 |
| string compare | 1.5 | 12.5 | 2.9 | 5.2 |
| hanoi @ blocks:  8 | 0.0000 | 0.01 | 0.01 | 0.003 |
| 16 | 0.2 | 3.2 | 1.4 | 0.7 |
|   20 | 3.0 | 54.2 | 21.6 | 12.0 |
|  21 | ut | ut | 43.1 | 24.0 |
|  22 | ut | ut | se | ut |
| 24 | 49.2 | 924.1 | se | 191.7 |

Notes:  

1) ut: un-tested,  se: stack exhausted
2) Hardware:
   - Espressif ESP32 DevKitc V4 WROOM-32E  
3) Software:  
    - Toit v2.0.0-alpha.180
    - uLisp v4.7
    - Adafruit CircuitPython 9.2.7

† tuvm/nsl column (2026-07-16, preliminary — not yet a peer of the other
columns): a **single run**, not a mean of 3; on **Zephyr 4.2.0**, not
ESP-IDF/FreeRTOS; 96 KB VM heap. Port: `performance.nsl` (nsl), semantics and counts from
`performance.toit`. Measurement provenance and caveats:
`~/workspace/tuvm/zephyr/docs/results.md` (E4; VM-side trial branches exist
that halve collection iterate — E6 — but are unmerged and not quoted here).

## tuvm native-AOT — the OEOM lever (2026-07-17)

The `tuvm/nsl` Hanoi rows above are the **interpreter**. Because tuvm's object model
(OEOM) makes every call a `send` and every method just a `Binding → Closure`, "native vs
interpreted" is **one field** on the closure — so a hot method can be replaced by
hand-written C *dropped straight into the vtable*, with **callers, interpreter, and
`.oimg` image format all unchanged** (no JIT, no recompile, no format break). Same board,
same binary, A/B:

| Hanoi n | interpreted (`tuvm/nsl`) | native-in-vtable | speedup |
| --: | --: | --: | --: |
| 16 | 0.7 s   | 0.0067 s | **~116×** |
| 20 | 12.0 s  | 0.103 s  | **~119×** |

~11.75 µs/move → ~0.10 µs/move; native is linear and allocation-free (no GC). This is the
*fully-inlined* AOT **ceiling** (the recursion collapses to raw C), so it sits ~10× past
Toit's compiled column on this workload — a real bytecode→native compiler preserving
send/GC semantics would land between the interpreter and this bound. The point is the
**lever, not the number**: OEOM's single dispatch choke point lets you ship the
interpreter, learn from months of fleet usage which one selector is actually hot, then
push a surgical bespoke-AOT patch for exactly that method. Method + caveats:
`~/workspace/tuvm/docs/design/2026-07-17-oeom-native-substitution-hanoi-spike-design.md`.

## Program-shaped benchmarks: Richards and DeltaBlue

The STIC rows above are single-mechanism spin loops. Richards (the classic
OS-task-dispatcher simulation, Martin Richards / V8 lineage) and DeltaBlue
(the incremental constraint solver, Freeman-Benson & Maloney) are the
complementary *program-shaped* pair: polymorphic dispatch, linked structures,
branchy control flow, no single hot loop — and both **self-verify** (Richards
by its canonical scheduling counts 23246/9297; DeltaBlue by checking every
plan execution).

Files, one per language, all printing the same format
(`richards: <ms>` / `deltablue: <ms>` per run, 3 runs, then
`verified: <bool>`):

- `richards.toit`, `deltablue.toit` — adapted from the Toit repo's ports
  (classes verbatim; timed harness added).
- `richards.py`, `deltablue.py` — direct translations of the same, for
  CircuitPython (also run on CPython).
- nsl: `richards.nsl` and `deltablue.nsl`
  (SOM-lineage bodies, same timed harness; copies of nsl-tests/00_perf).

Workload definitions: Richards = one canonical run (idle count 10000).
DeltaBlue = `chain_test(50)` + `projection_test(50)`, SOM shape (50
variables / 49 equality constraints, 50 verified plan executions; the
Toit/Python chain tests were matched to this shape so all columns do
identical work). n=50 rather than the canonical 100 because n=100's live
constraint graph overruns the tuvm column's 96 KB device heap.

| Benchmark (seconds/run) | Toit | CircuitPython | tuvm/nsl † |
| --: | --: | --: | --: |
| richards | 1.318 | tbr | 16.07 |
| deltablue | 0.093 | tbr | 1.16 |

tbr: to be run (CircuitPython column pending). The Toit figures are means of 3
runs on the jag/Jaguar node (`richards.toit` 1.318/1.317/1.319 s; `deltablue.toit`
0.097/0.093/0.089 s), self-verified, 2026-07-17. The tuvm/nsl figures are means
of 3 same-binary runs (richards 15.957/16.117/16.145 s at 96 KB VM heap — merged
perf defaults: IRAM hot-set + debug-hooks-off + fast-path opcodes, −8.8% vs the
pre-merge 17.6 s; deltablue 1.160/1.164/1.159 s at 112 KB — DeltaBlue's
constraint graph needs the larger arena, see tuvm's results ledger). The Toit and tuvm/nsl nodes run on two
identical **Espressif ESP32-DevKitC V4 (WROOM-32E)** boards sitting side by side
— same devkit, same silicon — so the ~12× gap is pure VM (Toit's compiled engine
vs tuvm's bytecode interpreter), not hardware. Same software caveats as the †
footnote above.

### Hanoi scaling (tuvm/nsl, ESP32)

The STIC `hanoi @ blocks: 8` row above is a single tiny point. Towers of Hanoi is
a pure-recursion call-throughput probe (moves = 2ⁿ−1; recursion *depth* is only n,
so no VM-stack pressure), which makes it a clean way to check the interpreter's
per-send cost at scale. Measured on the same ESP32-DevKitC V4 (WROOM-32E), Model B
(.oimg), single binary:

| n | time | moves (2ⁿ−1) | µs/move |
| --: | --: | --: | --: |
| 8  | 3 ms          | 255        | ~12   |
| 16 | 749 ms        | 65,535     | 11.43 |
| 20 | 11,984 ms     | 1,048,575  | 11.43 |
| 24 | 191,735 ms (3 min 12 s) | 16,777,215 | 11.43 |

Per-move cost is **constant at 11.43 µs/move** across five orders of magnitude of
work — perfectly linear, no GC cliff (n=24 is exactly 8× n=21's 23,967 ms). No
watchdog is armed in this build (`CONFIG_WATCHDOG`/`CONFIG_TASK_WDT` unset), so the
3-minute n=24 compute runs uninterrupted. Toit/other-VM columns for the larger n
are tbr.

**Native-AOT ceiling:** replacing this interpreted `hanoiMove:` with a native C method
dropped into the vtable runs the same n=20 at **103 ms vs 12,316 ms (~119×)** — see the
*tuvm native-AOT — the OEOM lever* section above for the A/B table and the mechanism.

On an i3-12100 desktop machine, using Zig v0.14.0

| Test                 | Toit   | Zig     |
| -------------------: | -----: | ------: |
| alloc                | 0.0473 | 2.7775  |
| array write          | 0.1373 | 0.0155  |
| dictionary write     | 0.0217 | 0.0250  |
| float math           | 0.0950 | 0.0140  |
| integer math         | 0.0743 | 0.0085  |
| collection iterate   | 0.1407 | 0.0310  |
| collection write     | 0.0440 | 0.0017  |
| string compare       | 0.0100 | 0.0130  |
| hanoi @ blocks: 8   |  0.00000000 | 0.00000147  |
| 16                   | 0.0010     | 0.0003 |
| 20                   | ut     | ut |
| 21                   | ut     | ut |
| 22                   | ut     | ut |
| 24                   | 0.3933 | 0.0671  |

 