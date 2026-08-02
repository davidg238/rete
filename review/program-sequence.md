# nsl-tuvm — program sequence (the overarching order of work)

**Status:** drafted 2026-07-04 (mid-Milestone-B). This is the ONE document that sequences
the whole program. It is an *index with ordering*: every line points at the document that
owns the detail — decisions live in `nsl-tuvm/docs/design/`, execution state in
the SDD ledger, open items in GitHub issues on `davidg238/{tuvm,nsl-tuvm}`. Update this file
at milestone boundaries, not per task.

**North star** (unchanged from the 2026-06-25 node roadmap): a modern embedded scripting
**node** — object-capability security, OTA/in-image debug, resident VM under a nodus-style
keeper on the porta fleet plane — nRF52840-class (~256 KB) + OpenThread. What the 2026-07-03
pivot changed is the concurrency pillar: **no fibers, no NLR** — a reactive, run-to-completion
model (channels + reactor + supervisor) replaces them. The Zephyr gate remains **memory, not
speed** (old `tuvm/docs/ROADMAP.md` prime directive; still true).

---

## Phase 0 — heritage: what the pivot kept (done, pre-2026-07-03)

The pre-pivot ladder (`nsl-tuvm/docs/superpowers/plans/2026-06-25-ns-zephyr-capability-node-roadmap.md`)
ran R0–R7 on the fiber-era VM. Its **infrastructure rungs survive the pivot** (language-neutral
or VM-shape-independent); its **concurrency rungs are dead**:

| Rung | Status post-pivot |
|---|---|
| R0 `.oimg` Model-B loader (primary path) | KEPT — carried into tuvm, format now v8 |
| R1 ns-posix keeper + real porta end-to-end | KEPT as asset — re-validation needed (phase 3) |
| R2 telemetry / R3 porta-debug bridge | designs exist (posix-keeper R2/R3 specs) — re-validate in phase 3 |
| R4 GPIO capability blueprint | KEPT as design (`2026-06-26-gpio-capability-deferred-blueprint.md`) — phase 3 |
| R5 net/Thread capability (r6 design) | re-validate against no-fiber VM — phase 3 |
| R6 ns-zephyr port | superseded in detail, right idea — phase 2 |
| R7 fibers + de-recursify | **DEAD as a feature** — the flat interpreter it built is kept; fibers/NLR deleted by Milestone A |

Frozen oracles: `~/workspace/oevm`, `ns-oevm`, `ns-tests` (never touched); pre-fiber shape
reference = oevm `cee55d4` / ns-oevm `a327d60` (`review/2026-07-04-pre-fiber-baseline.md`).

## Phase 1 — the first step: prove the model on the host (Milestones A–D)

Owner doc: `nsl-tuvm/docs/design/2026-07-03-nsl-tuvm-first-step-design.md` §4
(+ `review/2026-07-03-first-step-design-addendum.md` A1–A3). Bar: end-to-end on the desktop
with the balloon gone.

- **A — Seed + delete. DONE 2026-07-04.** Merged to master all 3 repos; 88/0/12 both paths;
  text −10.02% banked; deletion machine-proven (07_no_nlr + diff oracle). `tuvm/docs/MILESTONE_A.md`.
- **B — Immutable message algebra. DONE.** Merged to master all 3 repos (12 tasks landed,
  reviewed, gates green). Standing decisions absorbed: L7 signed-64-bit integers,
  `->` as compile-time rewrite (no builtin→kernel-Object delegation exists — Task 2 finding).
  Close-out synced into `CLAIMS.md`/`review/tests/README.md`.
- **C — Capability-scoped channels + reactor. DONE 2026-07-06.** Merged to master all 3 repos
  (`nsl-tuvm f4ef42c` / `tuvm 6930afc` / `nsl-tests 7e94bfa`). Landed the demo-node
  feed-forwards: capabilities-as-constructor-args, the degenerate (exact-match) tuple space,
  FIFO run-to-completion `Space>>drain`. Close-out synced into `CLAIMS.md`/`review/tests/README.md`.
- **D — Supervisor + let-it-crash + host job. DONE 2026-07-07.** All 10 tasks landed on
  `milestone-d` (crash boundary `vm_protected_fire` unwinding to the one marked reactor
  frame, per `review/lib/exceptions.md` constraint 4; `Supervisor`/`SupervisedJob`
  restart+quarantine; owner-tag identity sweep; host virtual-clock timers; the soak/leak
  test D.5; the demo-node-shaped integration test D.7; carry-forwards nsl-tuvm#5 and
  #9.1/#9.2/#9.3 all closed). `tuvm/docs/MILESTONE_D.md`.

**Phase 1 (host proof) is now COMPLETE — Milestones A through D all landed and green.**
Phase 2 (below) is entered next.

Library landing order rides these milestones — `review/lib/README.md` table is authoritative
(combinators ✓ post-A; messages = B; bytes after B; fsm = C; timers = D).

## Phase 2 — make it real: exceptions + Zephyr (the "second step")

Order chosen so each rung de-risks the next; entry criterion for the phase = Milestone D
green — **met 2026-07-07** (see phase-1 entry above); phase 2 starts now.

1. **Full `on:do:`/`ensure:`** (first-step §8 deferral). Owner: `review/lib/exceptions.md`
   (the 5 pinned constraints; deferring was safe *because* of them). Must compose with D's
   setjmp crash boundary. Retires the td-crash/04 'policy TBD' placeholder.
   **DONE 2026-07-07 (Milestone E).** `vm_on_do`/`vm_ensure` built beside D's `vm_protected_fire`
   (prims 90-93: `raise`/`error:tag:`/`on:do:`/`ensure:`); Record-valued raise; `^`-in-handler
   rejected; C5 no-cross-dispatch pinned; `nsl-tests/12_exceptions/01..14`. Whole-milestone gate
   green both paths; whole-branch review Ready (F1 interned-`#error` fix landed). `tuvm/docs/MILESTONE_E.md`.
2. **Zephyr bring-up** of the backend seam (`backend/<target>/`). Precursors that can land
   during phase 1: FOOTPRINT_CONFIG=mcu true-target RAM line (tuvm#5 — ideally before B Task 12
   banks RAM numbers); OEVM_BUILD=asan|ubsan harness knob. First work order: the **ISR→channel
   pulse primitive** (demo-node finding — the Zephyr seam's shape). On-device memory work rides
   the G3 decision (`2026-07-04-G3-gc-strategy-decision.md`: non-moving mark-sweep + M1–M6
   mitigations); soft watchdog via the debugger safe-point hook (kept in A for this reason).

   In-rung finding 2026-07-16 — **the ESP32 ROM silently outranks the libc Kconfig.** The VM
   boots and runs on a real ESP32-WROOM-32 (`esp32_devkitc/esp32/procpu`, `tuvm/zephyr/prj_esp32.conf`),
   prints `TIMER OK`, then faulted EXCCAUSE 28 (load prohibited, VADDR 0). Root cause is NOT
   the `^0` return path and NOT the uncancelled `every: 200` k_timer — both hypotheses were
   wrong, and both cost a session. It is the **print itself**: `tuvm/src/dispatch.c`'s `out`
   prim called `fflush(stdout)` → ROM `fflush` 0x40059394 → ROM `__getreent` 0x4000be8c →
   NULL load. `esp32.rom.newlib-funcs.ld` assigns all 62 of its newlib symbols by **plain
   assignment, not PROVIDE** — i.e. STRONG symbols, deliberately, per that file's own header
   ("so that ROM functions are always used instead of the ones provided by libc.a"). **The
   consequence generalizes past this bug: on ESP32 the libc choice is not authoritative.**
   Picolibc-vs-newlib is decided per-symbol by a vendor linker script, which is why the
   newlib→picolibc switch changed nothing and why the host ASan repro stayed green (glibc's
   fflush is fine) — a class of bug the host harness structurally cannot see. Most of the 62
   are pure and harmless (`memcpy`/`strlen`/`abs`); the biting ones are anything touching
   `_reent` — `fflush`, `fclose`, `fputwc`, `ungetc`, `wcrtomb` — since ROM `__getreent`
   wants an ESP-IDF/FreeRTOS TCB that Zephyr never creates. **Diagnostic:** `nm zephyr.elf`
   — `A` (absolute) = ROM-bound, `T` at 0x400f____ = picolibc; reach for it first when a
   Zephyr/ESP32 libc call faults implausibly.

   The rule this rung takes from it: **core must not reach around `backend/<target>/`.**
   `dispatch.c` calling `fflush(stdout)` directly at two sites *was* the defect — the seam
   (`tuvm/src/platform_backend.h`, `platform_console_flush`) already existed and was bypassed,
   which is exactly how a host-only assumption reached the SoC. Fixed in
   `tuvm/src/backend/zephyr/platform_zephyr.c` (flush is a no-op: `main()` sets stdout
   `_IONBF`, so nothing is ever buffered) + `dispatch.c` now calling the seam; 25/25 C tests
   pass, host ASan+UBSan clean.

   **DEVICE-CONFIRMED 2026-07-16 — and it unmasked a second, stacked bug.** Reflashed: the
   fflush fault is gone, but the node then died at **4.29s** with `pulse mailbox full` →
   `k_oops()`. Cause: `tuvm/zephyr/TimerNode.ns` started an `every: 200` repeater and
   **returned from `main:`** — so it stopped draining while the k_timer kept firing into the
   16-deep pulse mailbox. Arithmetic pins it: `OEVM_PULSE_MAILBOX_DEPTH` = 16; 0.891s → 4.290s
   = 3.399s = **17.0 fires**; 16 fill it, the 17th aborts. The long-standing "uncancelled
   k_timer" hypothesis was right about *this* crash and wrong about the fflush one — it was
   masked, because the node used to die at 0.891s before the mailbox could fill. **Two bugs
   were stacked on one symptom, and only the device could show the second.** Fixed
   (`883cdc5`): the node holds the token from `every:` and `cancel:`s it before `^0` — the
   API exists for exactly this ("a timer token is a small capability: holding it is the right
   to cancel it"), and it is the same shape as tuvm#23's supervisor sweep, where a dead
   instance's repeater outlives it. **Node bug, not platform**: every production node blocks
   forever draining, so only a *returning* node can leak a live repeater.

   The measured device timeline, all three states on real hardware:

   | state | result |
   |---|---|
   | before any fix | TIMER OK 0.891s → EXCCAUSE 28 → **dead 0.891s** |
   | after fix #1 (fflush) | TIMER OK 0.891s → pulse mailbox full → k_oops → **dead 4.290s** |
   | after fix #2 (cancel) | TIMER OK 0.891s → nothing → **ALIVE at 20s**, log ends 0.891s |

   **Rung-2 gate 1 (TimerNode, the real on-target timer backend) is now GREEN on ESP32** —
   no fault, no oops, no coredump. Green log: `tuvm/zephyr/esp32-debug-artifacts/`.

   **Residue this leaves on the ladder — the pulse mailbox still kills the node.** tuvm#20
   (`1857c26`) replaced the *event ring's* `abort()` with the G2 overflow policy — "the ring
   never kills the node" — but `pulse_post_isr` (`tuvm/src/backend/zephyr/pulse_zephyr.c:19`)
   still `k_oops()`es on a full mailbox, by design (`tuvm/src/pulse_backend.h:17`). #20 fixed
   the ring and left the ISR-side mailbox behind. That is a **G2 consistency gap**, not a
   TimerNode issue, and it outlives this node — a real ISR burst would kill a production node
   the same way. Deliberately NOT fixed with #2 (that would have papered over a genuine node
   bug). Decide whether G2's "never kills the node" extends to the ISR seam.

   Loose thread: `setjmp`/`longjmp` are ROM-bound too and `tuvm/src/vm.c` builds the
   raise/ensure crash boundary on them; the non-fatal `<err> os: ** SYSCALL` dumps (still
   present in the green log, expected) are ROM `setjmp` (0x40056268) spilling register
   windows — noisy, execution continues, soundness on Xtensa unverified.

   **In-rung 2026-07-16 — the Zephyr Model A/B build knob. Model B is GREEN on ESP32.**
   The rung stays OPEN: stage 2 is unbuilt (see the open item at the end). Design
   `tuvm/docs/design/2026-07-16-zephyr-model-ab-build-knob-design.md`, plan
   `tuvm/docs/superpowers/plans/2026-07-16-zephyr-model-ab-build-knob.md`.

   `zephyr/run.sh --model A|B` now owns the loop (bake → build → flash → capture) and
   `TUVM_MODEL` selects the link set: **A** = `src/solidified_image.c` (today's build,
   byte-for-byte); **B** = `oimg_loader.c` + `kernel_boot.c` + a `.oimg` `.incbin`'d into
   flash `.rodata` via `zephyr/blob.S`, with no `solidified_image.c` at all.

   | | measured |
   |---|---|
   | Model B, whole pristine loop (bake+build+flash+15s capture) | **44.0 s** |
   | Model B, pristine build alone | **11.6 s** |
   | `.incbin` of a 946 KB blob (real Xtensa `as`) | **0.007 s** |
   | Model A, build | **~50 min — and did not finish; OOM** |

   **The ~50 min was never really `as` — it is `cc1`, and it is a memory bug, not just a
   slow one.** The design blamed `as` on 941.7 MB / 71M lines of generated Xtensa asm. On
   the device-run attempt, **`cc1` (the C compiler, before `as` ever runs) reached 4.86 GB
   RSS and was OOM-killed by the kernel**, taking the terminal's cgroup with it
   (`oom-kill: task=cc1`; `anon-rss:4865120kB`; 15 GB box, ~9 GB held by the desktop, 4 GB
   swap full). So Model A's build cannot be scheduled around — it can take the developer's
   machine down. This reprices the "just re-run Model A to check" move that the ladder and
   plans have been assuming is merely slow.

   **A == B on program output; B is CLEANER.** Both print `TIMER OK` and sit alive.
   But Model A also emits 48 lines of the non-fatal ROM-`setjmp` `<err> os: ** SYSCALL`
   spill dumps (PC 0x4005626f) described in the loose thread just above — and **Model B
   emits zero of them**. B never enters the ROM `setjmp` path. That is new information
   about that loose thread: the noise is Model-A-specific, not intrinsic to the VM on
   Xtensa. Not investigated here; do not assume it benign.

   **Corrections to the record, both from reading the source rather than the comments:**
   - `zephyr/CMakeLists.txt`'s clash comment blamed `oimg_loader.c` *and* `kernel_boot.c`.
     Only **`kernel_boot.c`** clashes, and precisely **13 `Vtable_*` symbols** do.
     `oimg_loader.c` shares **no** symbol with `solidified_image.c` and links under both
     models.
   - That comment (and the plan, which inherited it) cited *"`run_c_unit_tests()` in
     `tools/run_tests_oevm.py`"*. **No such function exists.** The real evidence is
     `tools/run_c_tests.sh:28-29`, which has run this exact split on the host all along:
     `ENG_A` = `oimg_loader.c` + `solidified_image.c`, `ENG_B` = `oimg_loader.c` +
     `kernel_boot.c`. The knob is that ENG_A/ENG_B split ported to Zephyr — not a new idea.
   - `oimg_load_blob()` already existed for exactly this (no-POSIX byte-array loader, S0/R6);
     Model B needed no new loader, only `OEVM_NO_FILE_IO` to drop its `mmap` sibling.

   **Stage 0 (`zephyr/layout-esp32.json`, banked) — the `-m32` proxy really was unsafe, for a
   broader reason than the design gave.** The design rejected the host proxy because i386
   aligns `double` to 4 and Xtensa to 8, with `CONFIG_FPU=y` keeping floats live. The
   on-target diff confirms rejection but widens it: `struct Float` moved (12→16, `value`
   4→8, the predicted `double` case) **and so did `struct LargeInteger` (20→24, `sign` 4→8,
   `mag` 12→16) — which contains no float at all.** Xtensa 8-aligns *every* 64-bit scalar,
   `int64_t`/`uint64_t` included, so disabling FP would **not** have rescued the proxy. A
   wrong layout yields a blob that loads and then misbehaves — worse than one that fails
   loudly. Regenerate only when a struct or the toolchain changes.

   Practical notes banked in the tooling: the ESP32 console here emits **~520 B/s** (~20×
   below the 115200 line rate), so short capture windows silently truncate — the plan's 8 s
   cut the layout dump off mid-token. And `oimg_load_blob()` reports every validation failure
   (magic / `format_version` 8 / `registry_version` vs firmware `BUILTIN_REGISTRY_VERSION`
   = 5 / size) via `fprintf(stderr)`, which nothing on Zephyr unbuffers — `main()` now sets
   `stderr` `_IONBF` so a bad blob cannot fail silently.

   **And then the first cross-VM column — E4, STIC on ESP32 (2026-07-16).** With the loop
   at 44 s, the STIC benchmark was ported to nsl (`nsl-tests/00_perf/performance.ns`,
   `0e0715a`; design `tuvm/docs/design/2026-07-16-performance-nsl-stic-column-design.md`)
   and run against the published Toit / uLisp / CircuitPython numbers for this class of
   chip. Nine tests, 14.9 min, clean. Full table + caveats: `tuvm/zephyr/docs/results.md`
   **E4**.

   **tuvm is last on 5 of 9 and wins nothing** — that is the honest headline, and the
   ladder should carry it rather than the flattering subset. The shape is diagnostic
   though: **competitive on raw compute** (float math 0.85× Toit, integer math 1.63×,
   both while *boxing* every result) and **badly behind on collections/blocks**
   (collectionIterate **41.9×**, collectionWrite 10.2×, dictionaryWrite 7.6×).

   The number worth carrying: `collectionIterate` = **26.6 µs per block activation**,
   ~5× the cost of a complete four-op arithmetic chain (5.1 µs). `kernel.ns:132`'s
   `do: aBlock = (... aBlock value: ...)` cannot use oevm's literal-block inlining —
   `aBlock` is a *parameter*, so the inlining that took fib 30× → 2.4× never fires. That
   taxes every `do:`/`collect:`/`select:`, i.e. all idiomatic nsl collection code.
   **Filed as tuvm#37, queued as first work next session.** `Dictionary`/
   `OrderedCollection` growth policy is a separate, cheaper library-side suspect.

   **E4 is NOT publishable and NOT a controlled comparison**: a single run against
   reference columns that are means-of-three, tuvm on Zephyr vs their ESP-IDF, and a
   board revision not yet matched (README says WROOM-32E → rev v3.0; the attached board
   is D0WDQ6 **rev v1.0**). Plan is to rerun Toit *and* nsl on a definitely-identical
   board. Orientation only — but the gap it names is concrete.

   **No `CLAIMS.md` row for this.** `review/tests/CLAIMS.md`'s rule is that a criterion
   without a green *named test* is unproven, and the Model B gate is a manual device run
   (`zephyr/run.sh --model B ... --flash --capture`), not an automated test. The evidence is
   the banked green log (`tuvm/zephyr/esp32-debug-artifacts/tuvm-esp32-GREEN-modelB-oimg.log`)
   and this entry. Automating an on-target A/B equivalence gate would earn a row — the
   `.oimg` path makes that affordable for the first time (44 s, no 4.9 GB `cc1`), so it is
   now worth considering rather than dismissing.

   **The knob paid for itself the same day: `perfstex` now runs on ESP32.** With the loop at
   44 s, the classic SpeedTester (`nsl-tests/00_perf/perfstex.ns` — already a node,
   `main: platform args:`, so it boots unmodified) was baked, flashed and run on real
   silicon in about a minute. All six sub-tests plus `done`. Full write-up +
   reproduction: **`tuvm/zephyr/docs/results.md`** — the ESP32 on-device ledger (E1
   TimerNode/Model A, E2 TimerNode/Model B, E3 perfstex), deliberately shaped like
   `ns-zephyr/docs/results.md` so the two ports read side by side.

   | sub-test | OEVM/nRF52840 (M3) | **OEVM/ESP32** | speedup |
   |---|---:|---:|---:|
   | integerMath | 95 | **30** | 3.17× |
   | floatMath | 284 | **62** | **4.58×** |
   | arrayWrite | 4,751 | **1,431** | 3.32× |
   | arrayIterate | 3,407 | **1,026** | 3.32× |
   | stringCompare | 1,163 † | **376** | 3.09× |
   | hanoi(18) | 14,719 | **4,248** | 3.46× |

   † post-`oevm#17` nRF figure, not M3's pre-fix 1,609 — tuvm carries that fix.

   Same VM, same iteration counts, different silicon, so this measures **the chip**: clock
   ratio is 240 MHz ÷ 64 MHz = **3.75×**, and five of six land at 3.09–3.46× — just under
   it, as XIP-out-of-flash should. **`floatMath`'s 4.58× is above the clock ratio and is
   not explained**; don't quote it until someone separates soft-float codegen from the
   arena-config confound (both parts emulate `double` — the LX6 and M4F FPUs are
   single-precision only, so it is *not* "the ESP32 has an FPU"). Berry is deliberately
   absent: M3's Berry column is nRF silicon, and putting it beside an ESP32 column would
   report a chip difference as a VM difference. Single run, no repeats, 1 ms `millis`
   quantisation — `hanoi18`/`arrayWrite` are the trustworthy rows.

   This also validates stage 0 the hard way: `integerMath` overflows into `LargeInteger`
   and `floatMath` boxes `Float` — **the exact two structs the `-m32` proxy got wrong** —
   and both produce sane timings on the banked layout.

   **`perfstex` also caught a real defect, and it was a known one.** `floatMath` aborted on
   its first boxed float (`VM Float Arena Error: Out of Memory`). Cause is arithmetic, not
   shortage: `gc.c`'s slab is 4096 cells, `struct Float` is 16 B on Xtensa, so one
   `FloatSlab` is ~64.5 KB against a ~60 KB libc heap — the first `malloc` cannot succeed.
   **`ns-zephyr` found and fixed this identical bug on nRF52840 in June** (`results.md` M3:
   *"slabs larger than free RAM, so floatMath aborted on the first box"*;
   `ZEPHYR_NODE_GUIDE.md` §3 pins the fix at `OEVM_FLOAT_ARENA_SLAB_CELLS=512` /
   `_SLAB_BUDGET=4`). `zephyr/CMakeLists.txt` already forwarded both knobs — only
   `run.sh`'s export list was short. **So "the five load-bearing knobs", repeated by this
   rung and every plan, was never true — it was true only for nodes that never box a
   float, and TimerNode never does. It is seven** (`run.sh` fixed accordingly). The wider
   lesson: `ns-zephyr/docs/{results.md,ZEPHYR_NODE_GUIDE.md}` is prior art this port keeps
   rediscovering the hard way — check it before debugging an embedded limit.

   **Open item — stage 2, which is what answers G1:** flash partition + blob-only reflash +
   independent `.oimg` OTA. Today Model B still relinks and reflashes the whole image; the
   44 s makes that cheap enough to defer, but G1 wants the blob to move on its own. Its own
   rung.

   **Open papercut:** `run.sh` resolves `--node` relative to `zephyr/` only, so it cannot
   drive nodes that live outside the tree — `perfstex.ns` (in `nsl-tests/00_perf/`) has to
   be built by hand. Every in-tree node is fine; the perf suite is not in-tree.
3. **Wire seam: CBOR + Int64. DONE 2026-07-10.** Owners: `review/lib/cbor.md` (CBOR over custom
   wire format, tags 27/39) and `review/lib/integers.md` (L7: boxed Int64 on 32-bit — decided,
   built with B). G4 says the `.oimg` line-table provision must not be foreclosed meanwhile (it
   wasn't — v8 audit).

   Rung-close: landed as an nsl `Cbor` class (encode/decode tree-walk) in kernel.ns, backed by
   7 C leaf prims (ids 97-103, append-only after `pumpEvents`=96) that do the byte-level,
   non-recursive work — architecture (B), per the frozen wire-seam spec. Encoding is
   deterministic per RFC 8949 §4.2: shortest-form ints/floats, definite lengths only, map keys
   sorted bytewise over the encoded key bytes; decode is total and closed via Milestone-E
   `on:do:`/`ifInvalid:` — every byte sequence yields exactly one message or exactly one of
   `not a value` / `message depth exceeded` / `invalid utf-8` / `integer out of range`. The
   integer contract is the ratified L7 range **±(2^64−1)** (both SMI and LargeInteger, shortest
   form); only `−2^64` and the CBOR bignum tags (2/3) overflow it, both routing to
   `integer out of range`. Both-path (Path A solidified / Path B `.oimg`) verified A==B; tests
   in `nsl-tests/14_cbor/` (01_roundtrip, 02_golden_hex, 03_map_order, 04_reject, 05_int_band,
   06_float_roundtrip). Co-landed a `.oimg` large-SMI capacity fix (Task 1) needed to represent
   the widened integer range.

   (Infrastructure rung between 3 and 4, not its own conceptual step: **on-target measurement.
   DONE 2026-07-10** — prim 104 `heapStats` + `HeapStats` class, the MCU footprint ratchet as a
   5th named gate, DK capture. `tuvm/docs`/SDD ledger.)
4. **Transport: UDP/OpenThread — a three-layer arc.** The transport that the wire seam (item 3)
   and the reactive channel model (Milestone C) were built to feed. Design/plan:
   `nsl-tuvm/docs/design|superpowers/plans/2026-07-10-rung5-udp-transport-l1.*` (Layer 1) and
   `.../2026-07-10-rung6-udp-bridge-l2.*` (Layer 2). Sequenced
   host-proof-first, each layer de-risking the next:
   - **Layer 1 — the confined `net` UDP capability. DONE 2026-07-10 (rung 5).** The host POSIX
     capability (`Net`/`Socket`/`Datagram` over `udp.c` + `udp_posix.c`, both-path boot grant,
     receiver-identity dispatch) was already complete and green (`nsl-tests` `42_udp`/`43_udp_confined`,
     suite 179/0/12). Rung 5 proved the **Zephyr `udp_zephyr.c` backend functionally on
     `native_sim`** (previously link-only): a `net`-granting Model-A node (`tuvm/zephyr/NetNode.ns`)
     round-trips a datagram over Zephyr NSOS loopback (`UDP OK`). No VM/facade/compiler change;
     footprint unchanged. Payload stays `String` this layer.
   - **Layer 2 — the reactive channel↔UDP bridge. DONE 2026-07-10 (rung 6).** A pure-`.ns`
     `NetBridge` (in `kernel.ns`) over the L1 `Socket` send/receive prims + the `Cbor` codec —
     no new C. Outbound: an `out whenever:'' do:` drain-time reaction CBOR-encodes each post and
     `sendTo:`s the peer. Inbound: an explicit `pumpInbound` (receive-until-nil → CBOR-decode →
     post onto `in`), following the reactor's pump-then-`drain` idiom; a malformed datagram is
     **dropped, never crashes the node** (untrusted-input boundary). Lands the immutable-`Bytes`
     payload deferred from L1 (a module-internal `Bytes>>rawLeaf` send seam; `Datagram>>data` is
     now a `Bytes`). Proven **bidirectionally on host AND native_sim** (`nsl-tests` `45_udp_bridge`
     A↔B + garbage-drop; `tuvm/zephyr/NetBridgeNode.ns` → `BRIDGE OK` over NSOS loopback). Makes
     the node network-transparent (demo-node north star: node posts `node7/reading`; porta bridges
     it to the LAN, CBOR wire). Ingress stays an explicit `pumpInbound` call (rule-of-three — a
     unified `pumpEvents` registry waits for a second data source; serial-rx paper-sketch in the
     design §7 flags stream-framing as its own future concern). VM finding: a reified `whenever:do:`
     block cannot inline a cross-class call needing outer/`platform` resolution — routed through a
     `self`-send helper (established house style; candidate tuvm issue).
   - **Layer 3 — real OpenThread on the nRF52840-DK. DONE 2026-07-11 (rung 7).** Brings the
     native_sim-validated `udp_zephyr.c` up on silicon over an OpenThread mesh. The Rung-6
     `NetBridge` is **unchanged** — no VM/bridge/CBOR/facade edit — so this rung is radio/mesh
     bring-up only. A Model-A MTD child (`tuvm/zephyr/ThreadEchoNodeDK.ns`) auto-joins the porta
     OTBR mesh with a baked (gitignored) dataset; a BUTTON1 press posts a `Record` whose outbound
     reaction CBOR-encodes and sends it over 802.15.4 to the gateway's `udp-echo6` (:7777), which
     reflects it verbatim; the node receives, decodes value-equal, and prints **`BRIDGE OK`**
     (bench-verified — live silicon + live gateway + radio, not a `west build` CI gate). The
     three-layer UDP arc is complete. Retires the skipped `nsl-tests` `43_udp_mesh`/`44_udp_thread`
     (both superseded by this real bench gate; suite skips 12 → 10).
     Two plan premises broke and were fixed within the allowed surface (node `.ns`, board conf,
     `src/main.c` node glue): (a) the MTD child never SLAAC-configures an **OMR** address, so it has
     no route/source for the OMR prefix and `zsock_sendto` to the gateway's OMR silently failed —
     the target is the gateway's **ML-EID** (the mesh-local /64 the node itself lives in); (b) NCS
     v3.2.4's `openthread_run()` enables OT (attach + `ot-ctl ping` work — OT-internal ICMP) but
     never `net_if_up()`s the Zephyr socket iface, and with `NET_CONFIG_SETTINGS=n` nothing else
     does, so **`zsock_sendto` returned `ENETDOWN` (errno 115) even while attached** — fixed by
     `CONFIG_OPENTHREAD_MANUAL_START=y` + a `thread_bringup()` in `src/main.c` that calls
     `openthread_run()` **plus `net_if_up()`** and waits for attach. The design's OT-thread-starvation
     risk proved a non-issue (net RX preempts the non-blocking poll).
     Also ported to the **nRF52840 dongle**: it attaches to the mesh as an MTD child but is
     **headless** — the SoC USB-CDC console does not survive the radio coming up (Issue A; picolibc
     did not beat it). Its cadence + headless proof are rung 8
     (`nsl-tuvm/docs/design/2026-07-11-rung8-ontarget-timer-design.md`): a real Zephyr
     `k_timer` backend for the nsl `Timers` seam drives a 15 s autonomous send, and a **self-gating
     train** (the next send is conditional on the previous reflection decoding value-equal) makes the
     gateway's outbound-only echo log witness round-trip closure with no console.
   - **Rung 8 — the on-target timer seam + the autonomous node. DONE 2026-07-11** (dongle DFU
     flash pending — needs hands). The `Timers` seam (prims 85–88), until now a **virtual clock**
     advanced only by `advanceMillis:`, gained a real backend: `src/timer_backend.h` +
     `backend/posix/timer_posix.c` (the virtual clock, lifted VERBATIM — the host floor is the
     gate, and stayed at 180/0/10 + 5 gates) + `backend/zephyr/timer_zephyr.c` (a `k_timer` whose
     expiry ISR posts through the **existing** pulse mailbox onto `sys/timer` — the path
     `pulse_backend.h` already sanctioned: "A GPIO/timer ISR calls `pulse_post_isr`"). No VM,
     `Timers`-class, `NetBridge`, or CBOR change. `monotonicMillis` is now real on target;
     `pumpEvents` idles the CPU between ticks and yields to OpenThread.
     Gates: **`TIMER OK`** on native_sim (the backend proven with zero hardware), then
     **`BRIDGE OK` every 15 s on the DK** — five prints in **exact lockstep, to the second**, with
     five gateway echo lines, and **no button pressed**. That lockstep *calibrates* the
     **self-gating train**, the headless proof: the node re-sends only after the previous
     reflection decodes value-equal, so the gateway's outbound-only log witnesses full round-trip
     closure (N lines = N−1 proven round-trips) and a broken inbound leg stops the train dead.
     Issue A (dongle USB-CDC dies under radio) is thereby **routed around rather than solved** —
     the dongle needs neither a button nor a console. Footprint: +1.5 KB flash, no RAM.

## Phase 3 — rejoin the node ladder (keeper, fleet, capabilities)

Entry: phase 2 rungs 1–2 green on hardware. These re-validate proven pre-pivot assets against
the no-fiber reactive VM rather than building from scratch.

**Gate on the whole phase, now cleared: tuvm#32 — DECIDED 2026-07-13, option A.** Porta's
protocol needs a name that *has a value*; the node's space only had queues. The decision
went to David and came back reframed as a **mailroom**: everything on the wire is a *letter*
(the bridge reads the **address**, never the contents; the addressee's job opens it), and the
thing that differs is the **pigeonhole** — a **stream** you empty by reading, a **cell** that
holds the latest letter. What forced a cell is that a supervised job's crash is *doctrine*
here, and three verified `space.c` facts (every name is a queue · an event with no reaction
registered is **drained and dropped**, `space.c:262` · there is **no request/reply**) together
make a restarted job's setpoint **unrecoverable** without one. Two arguments of mine were
retracted on the way (the etcd/consensus framing; a bogus "notices vs letters" distinction),
and David's industrial-control point — *"there is no clear; there is a start button and a
separate stop button, and stop always wins"* — **deleted the tombstone**: a stopped loop is
`auto = false`, not an absent `auto`, so a cell is **`value`, full stop**. Decks:
`review/cells-and-streams-v3.html` (supersedes v1/v2); code: `review/cells-sketch.ns` (the same
PID scenario under option A and under option C — the diff is the argument).

0. **Rung 9 — cells. DONE 2026-07-13** (tuvm 0cadc2c · nsl-tuvm aa21b71 · nsl-tests
   8c9536c; suite 189/0/10 both paths; tuvm#32 closed with the ruling; footprint
   re-banked +605 B text / +256 B BSS with the reason).
   *(Post-close, 2026-07-14: a P0 sweep rode the seam while it was hot — tuvm#23
   crash-timer-sweep and tuvm#20 G2 overflow policy both landed, gated, and closed;
   suite floor now 193/0/10; ISR-side residue filed as tuvm#34. The kind field this
   rung added is what let #20's overflow policy need zero new API.)*
   `nsl-tuvm/docs/design|superpowers/plans/2026-07-13-rung9-cells.*`. A value
   slot on `SpaceChannel`, three prims (`spaceKind:` / `spacePeek:` / `spacePut:value:`), two
   `Channel` methods (`put:value:`, `peek:ifAbsent:`). `put` posts onto the **existing** ring, so
   a `whenever:` on a cell fires through the unchanged reactor — the only difference from a post
   is that the cell **also kept a copy**. The verb declares the kind; the wrong verb on an existing
   name raises (`EISDIR`, and nobody calls that a design flaw), enforced in **nsl**, because
   `space.c` is storage and not policy. Gate — host only, no radio, no bridge, no porta:
   **write a setpoint · crash the job · let the supervisor restart it · peek the correct setpoint
   back.** Passes under A; cannot be written under C. `seq`/`changedSince:` are deferred to the
   bridge rung that needs them, and **option B (served prefixes, Plan 9's move) stays unbuilt and
   unblocked** — the radio is just another `dev/` driver, and if B ever lands, A's cell becomes its
   first server with no job-code change.
1. **ns-posix keeper + porta rejoin** (R1/R2 assets; keeper core was VM-neutral by design).
2. **Remote debug over porta** (R3 design; debugger survived Milestone A untouched).
3. **Capability breadth**: GPIO (R4 blueprint) → i2c → **OpenThread/UDP**. The UDP/net capability
   is no longer pending re-validation — it landed as Phase-2 item 4 (Layer 1 done: host both-path +
   native_sim; fiber-free entry points reviewed, GC temp-rooting confirmed). What remains here is
   Layer 3 (real OpenThread on the DK) plus GPIO/i2c. Capabilities are granted as **constructor
   args** per the Milestone-C rule.

## Phase 4 — generalize (only when real workloads justify)

First-step §6 guarantees these are additive on the same VM shape: wildcard/associative matching,
persistent state-tuples (TTL/lifecycle), large-buffer ownership-transfer (zero-copy), multiple
event sources, power/deep-sleep. No owner docs yet — write them when a workload forces the issue.

---

## Cross-cutting ledgers (constraints, not phases)

- **Reliability gaps** `review/reliability-gaps.md` (G1–G11): G3 RESOLVED (GC decision doc);
  G4 protected through B (v8 audit clean); the rest are watch-items assigned above where scheduled.
- **Metrics** `review/metrics-spec.md`: every phase banks its footprint/latency lines; the RAM
  line becomes target-config (mcu) at phase 2 rung 2 (tuvm#5).
- **Test suites** `review/test-suites-spec.md` + `review/tests/CLAIMS.md`: T-A, T-B, T-C, T-D
  all landed with their milestones (`nsl-tests/07_no_nlr`, `08_messages`, `09_channels`,
  `11_supervision` + `10_fsm/02`); `CLAIMS.md` is current as of Milestone D's close.
- **Target user program**: `review/demo-node.ns` — readme-driven; every phase should shrink the
  gap between it and what actually runs.

## Addendum 2026-07-11 — direction decisions binding on phases 2–3

Decided in the 2026-07-11 architecture review (David's calls); owner docs written the same day:

- **Driver exposure & example ladder** (`review/zephyr-driver-exposure.md`): devices surface as
  granted channel prefixes (`dev/<n>/sample·event·ctl`); three tiers (uniform-API-family binding
  → C-side driver job → custom prims). Ladder for phase-2 rung 2+: **BME280 → AMG8833 Grid-EYE
  (native Zephyr driver, same Tier-1 binding) → character-grid UI on a 128×128 TFT. LVGL is
  dropped, not deferred.**
- **Porta wire** (`porta/docs/PROTOCOL-NSL.md`, porta#23): porta protocol v1 is frozen for Toit
  nodes; **nsl nodes speak a space-sync protocol** (goal/obs retained trees + event channels,
  cursor sync; `take:` never crosses the wire; payload bulk + auth off-space). This refines
  phase 3 item 1 (keeper/porta rejoin) and the CBOR wire rung: pin per-prefix record schemas
  (`PROTOCOL-NSL-RECORDS.md`) before first implementation; first proving ground is the G10
  host fleet-sim (also the tuvm#25 vertical slice).
- **Distribution law** (`WHY.md` §"Paths extend across the network"): location transparency must
  not become failure transparency (Waldo TR-94-29) — remoteness lives in the grant, with
  per-prefix delivery contracts enforced at the bridge job (G2 overflow, G5 auth, G8 skew:
  one door).
- **Issue discipline**: P0–P3 labels + pinned tracker tuvm#26 define the release gates
  (unattended node = P0 clear; production fleet = P0+P1 clear). Flight target ≈ 2 months
  (2026-09); `review/slides.html` is the target artifact and tracks milestone truth.

## Addendum 2026-07-12 — phase 2 closes; phase 3 is blocked on one decision

- **Phase 2 is essentially COMPLETE.** Items 1–4 are all done (exceptions/Milestone E;
  Zephyr bring-up; CBOR+Int64 rung 3 + on-target measurement rung 4; the three-layer
  UDP/OpenThread arc rungs 5–7 + the on-target timer rung 8). The dongle closed 2026-07-12.
  **What remains in phase 2 is the driver ladder** from the 2026-07-11 addendum —
  BME280 → AMG8833 Grid-EYE → character-grid TFT (`review/zephyr-driver-exposure.md`).
  That ladder is also the forcing function for L2 (tuvm#28): BME280's calibration registers
  are exactly the mixed-width u16/s16/u32 reads the binary-data library does not yet have.

- **L1 landed** (nsl-tuvm `661883f`, nsl-tests `1600c47`): the full combinator set +
  `Interval`. Suite floor moves **180 → 183/0/10**, five gates green. Costs zero MCU text.
  One deviation, now tuvm#30: **the spec's `1 to: n` sugar is not landable in nsl.** Integers
  are VM builtins with no nsl-reachable vtable (`IntegerVtable` is static C; `1 to: 5` compiles
  to a real send and dies DNU), so ranges are built as `Interval from:to:[by:]`. The general fix
  — giving builtins a protocol — is the *same* mechanism that would let L2's `bitField:` land as
  methods rather than prims, so **#30 and #28 should be decided together**.

- **`review/language-and-library-spec.md` is a PROPOSAL, not truth.** It now carries a
  build-status table; trust that and the code over its prose. Two items had already been
  ratified *differently* and the doc had not caught up (this cost real time to rediscover):
  L6's codec shipped **in nsl over C leaf prims**, the inverse of the doc's "codec in C"; and
  L7's ratified range is **±(2^64−1)**, not the signed-64/trap-at-2^63 the doc proposed.

- **PHASE 3 IS BLOCKED on tuvm#32.** `PROTOCOL-NSL.md` is built on **retained** channels, and
  the node does not have them — the space is a pure event queue (`space.c`: post/take/register;
  no latest-value slot, no per-path seq, no tombstone, no changed-since-cursor). The doc
  contradicts itself on where they live: §2 makes retained a channel *kind* "declared per path
  prefix" (→ the space, i.e. C), while §3.1 promises the bridge needs "**No VM changes**" (→ the
  bridge's private Map, i.e. nsl). **The deciding question is not cost but *who besides the
  bridge needs a retained path*** — §3.2 (the supervisor reacting to `goal/apps/**` "exactly as
  it reacts to any channel") and §3.3 ("the protocol IS the space, filtered by grant") are only
  true if the space owns it. Nothing downstream — bridge job, porta store, fleet-sim — is safe
  to build first. Decision material: `review/retained-channels-v2.html` (the current deck; v1
  kept alongside for contrast) and `porta/docs/PROTOCOL-NSL-RECORDS.md`.

  **Reframed 2026-07-12 (David's read):** a channel is a **name**, not a pointer, so the real
  question is what the name resolves to and *who can answer it*. Consequences: (a) **"retained"
  is MQTT jargon** for a broker delivery trick and should be dropped — a name resolves to a
  **cell** (has a value) or a **stream** (delivers occurrences); (b) an earlier argument of mine
  — that the two kinds need different verb sets and this makes the problem deep — is
  **RETRACTED**: mixed namespaces are normal (`read()` a directory → `EISDIR`), and sending the
  wrong verb is simply an error; (c) **the actual crux is that the space has no request/reply.**
  A job cannot *ask* another job for a value, so a cell held in the bridge's private `Map` is
  unreachable by any second reader except via a direct object reference (authority by reference,
  not by name — what capabilities exist to prevent) or hand-rolled per-consumer cache coherence
  (which reintroduces precisely the multiplying special cases this protocol exists to kill).

  Prior art now folded in: **Plan 9** (the namespace *is* the interface; `fid` ≈ our `Channel`;
  *don't add a kind, add a server*), **Linda** (it always had `rd`, the non-destructive read we
  never built), **etcd/ZooKeeper** (we are building *their* thing — `revision`/watch-from/
  tombstone/compaction — not 9P, because a sleeping lossy node cannot mount), **LOCUS/Waldo**
  (transparency you pay for later). Three options enumerated on tuvm#32: **A** cell as a second
  kind in `space.c`; **B** served prefixes (Plan 9's move); **C** nsl only. A and B are
  *indistinguishable from the job's side*. Current recommendation: **A now, B when it has a
  second customer, not C.** New hazard found and filed: **porta#24** — a node whose cursor falls
  behind porta's tombstone retention is **silently stranded** (etcd's compaction-vs-watch bug).

- **The records precondition is met.** `porta/docs/PROTOCOL-NSL-RECORDS.md` (porta `921e8a4`)
  pins the per-prefix schemas the 2026-07-11 addendum required *before* implementation, and adds
  the **size law**: the binding constraint on message size is the **radio, not the heap** —
  6LoWPAN fragments above ~102 B and a datagram survives only if every fragment does, so at the
  bench fleet's measured worst-node loss (4.6%) a 1 KB batch arrives only ~54% of the time.
  Caps: 256 B/value, 512 B/batch, enforced at the bridge; anything larger is a content-addressed
  `BlobRef` fetched out of band.

## Addendum 2026-07-17 — OEOM native-substitution spike (Hanoi AOT ceiling)

A throwaway probe (behind `OEVM_HANOI_NATIVE`, off by default) proved OEOM's
native-substitution mechanism end-to-end: a hot user selector
(`hanoiMove:from:to:temp:`) serviced by a C `PrimitiveClosure` stored as a vtable
`Binding`, dispatched transparently through the existing fork (`tuvm src/dispatch.c:1673`)
with no change to callers or the interpreter. Wiring cost: one new `src/hanoi_native.c`
+ one `strcmp` gate; `.oimg` format untouched; symbol identity solved by scanning the
bench class's own vtable for the canonical interned selector (no intern API, no
solidifier change).

**ESP32 (WROOM-32E, Model B, IRAM-pinned, means-of-3):** n=20 bytecode 12,316 ms vs
native 103 ms — **~119×** (n=16 ~116×). Native ~100 ns/move, linear/GC-free (zero
alloc); `moves == 2ⁿ−1` confirms the PrimitiveClosure path fired, not a DNU. This is the
*fully-inlined* AOT ceiling (recursion collapsed to raw C), ~10× past Toit's compiled
engine on this workload — so a real bytecode→native compiler preserving send/GC
semantics lands between the interpreter and this bound. Hanoi is all-heat-in-one-method;
spread-heat workloads (DeltaBlue) dilute it — this is a ceiling, not a whole-program
number. Go-signal for the "collect months of usage → push a bespoke-AOT patch for a
proven hot method" path filed under Phase 4 (generalize). Why + full method:
`tuvm/docs/design/2026-07-17-oeom-native-substitution-hanoi-spike-design.md`; numbers:
`tuvm/docs/superpowers/plans/2026-07-17-hanoi-ab-results.md`.

## Addendum 2026-07-17 — the next arc: demo → connect/observe → harden → optimize

Sequence decided with David 2026-07-17 for the batch that carries the phase-2 tail into
phase 3. Four arcs, ordered so each makes the previous one *more* than it was. This does
**not** renumber the phases — it orders the near-term work across the phase-2/3 boundary.

**Phase 3 is UNBLOCKED — this supersedes the 2026-07-12 "PHASE 3 IS BLOCKED on tuvm#32"
line above.** That block was cleared the next day: tuvm#32 DECIDED option A (2026-07-13)
and rung 9 (cells) landed the same day (see the phase-3 entries). Porta rejoin + remote
debug are ready to enter.

1. **Arc 1 — a demonstrable node (the driver ladder).** The remaining phase-2 tail.
   Promote `review/zephyr-driver-exposure.md` from sketch to a rung-2 design/spec:
   BME280 (Tier 1) → AMG8833 Grid-EYE (Tier 1, same binding) → character-grid TFT
   (Tier 2). Forcing functions ride here: the L2
   binary-data library (tuvm#28 — BME280's mixed-width calibration registers) and the
   builtins-protocol decision (tuvm#30 — decided together with #28). First spec of the
   arc (the `.ns`→`.nsl` precursor closed 2026-07-18; the spec is the next work item).
   *(An FTD-repeater "network map" example was considered and dropped 2026-07-18 —
   per-router neighbor/link-quality reporting to the OTBR is a built-in OpenThread feature,
   not a custom nsl example worth building.)*

2. **Arc 2 — connect + observe (porta + debug + profiling).** Phase 3 items 1–2
   (ns-posix keeper + porta rejoin; remote debug over porta, R3 design) **plus a new
   profiling rung**. Profiling moves *here*, not dead-last: a real, complete workload
   (Arc 1's node, connected via Arc 2) is what you want to profile, and profiling is the
   **forcing function for Arc 4** — you cannot pick a method to AOT-substitute without it.
   Splits cleanly: the profiling *hook/tooling* lands here; *acting* on it is Arc 4.

3. **Arc 3 — P0 hardening (toward the unattended-node flight gate).** tuvm#34 (the
   `pulse_post_isr` full-mailbox `k_oops()` — the G2 "never kills the node" rule not yet
   extended to the ISR seam); Model-B stage 2 (flash partition + blob-only OTA reflash —
   answers G1); an automated on-target A/B equivalence gate (now affordable at 44 s, earns
   a `CLAIMS.md` row). P0-clear = unattended node (issue discipline, 2026-07-11 addendum).

4. **Arc 4 — optimize (act on the profile).** The perf/AOT story: native substitution of
   the hot method the Arc-2 profiler names — the Phase-4 payoff the 2026-07-17 OEOM spike
   proved a ceiling for — plus tuvm#37 (collection/block perf; `do:`/`collect:`/`select:`
   cannot inline a *parameter* block). Deliberately last: optimize a whole, real workload,
   not a toy.

Owner docs as each arc opens: Arc 1 → new driver-ladder design under `docs/design/`
(this session); Arcs 2–4 → their rung designs when they start. Update this ladder at each
arc boundary, per the file's own rule.

**FILED 2026-07-19 — the manifest / module-injection rung (no arc yet; Arc 1 is its
forcing function).** Reusable service modules (an `HttpServer`, a per-device driver facade)
have **no capability-pure home in separate files today**, and that will bind harder with
every driver-ladder rung. Spike result (`docs/design/2026-07-19-amg8833-thermal-sensor-rung-design.md`
§4a): `ns_compile_oevm.py` bakes ONE app file plus `lib/kernel.nsl` + `lib/supervisor.nsl`
appended as top-level siblings; appending a **third** library file compiles it in but leaves
it **unreachable** — a node calling `HelloService usingPlatform: platform` died on both paths
with `Message 'HelloService' not understood by receiver <platform>`. Cause: `resolve_name`
routes an unbound capitalised name to `platform Name`, and `Kernel` reaches its own classes
only by **nesting them with an explicit accessor each** (`public NetBridge = ( ^ NetBridge )`).
This is deliberate — the compiler comments pin the routing as "never a VM global
(capability-pure)" — so direct naming of sibling modules would reintroduce **ambient
globals**. The correct fix is **injection**: build out the `packageUsing: manifest`
parameter, which **every node already declares and no node body uses** (vestigial today), so
boot hands the app a manifest exposing its library modules (`manifest HttpServer`). Sized as
a genuine feature (boot-path + toolchain + manifest construction), NOT a micro-change — the
"~10 lines" first estimate was wrong. Until it lands, a module's own classes **nest** in its
boot module (Newspeak idiom: `DemoNode`/`TempNode`), and promotion costs nothing later
because composition is already by injection — only where a class lives and how it is obtained
changes, never the `main:` wiring.

**Arc-1 precursor — DONE 2026-07-18: `.ns` → `.nsl` source-extension migration.** Hard cut
across the live repos (nsl-tests 209 files, tuvm/zephyr 13 nodes, nsl-tuvm `kernel`/`supervisor`);
all discovery globs + hardcoded paths repointed; discriminating comment sweep + active-zephyr-docs
sweep. Deliberately kept `.ns`: the frozen oracles (`ns-tests`/`oevm`/`ns-oevm`), the primordialsoup
Newspeak std lib, `review/`, `docs/spikes/` archives, historical docs (dated design records,
`MILESTONE_*`/`ROADMAP`/`fiber_*`), and generated `.stmap` fixtures (refresh on rebake). Suite floor
unchanged — 195/0/14 both paths + named gates — so no `CLAIMS.md` row moves. Landed on branch
`chore/ns-to-nsl` in all four repos. Design: `docs/design/2026-07-17-ns-to-nsl-extension-migration-design.md`;
plan: `docs/superpowers/plans/2026-07-17-ns-to-nsl-extension-migration.md`. Driver-ladder spec (Arc 1) is next.

**Coordination-model spike #1 — DONE 2026-07-18: webserver channel-device (request/reply over channels).**
Verdict **clean** — request/reply decomposed into pulse ingress + NetBridge egress + an opaque correlation
id, no channel reification, no VM-core change; G2 shed proven under real concurrent curl load. Idiom sharp-edges
surfaced (`\\` modulo is not a selector; channel payloads must be algebra **values**, not mutable Arrays) plus an
egress-datapath lesson (stream large bodies, don't stage them in a bounded C buffer). Findings of record:
`~/workspace/tuvm/review/model-capability-findings.md`; design/plan dated `2026-07-18-webserver-channel-device-*`.

**On-device last mile (#46) — DONE 2026-07-18: HttpServerNode serves `/thermal` (8×8 SVG heat-grid) live over
WiFi from an ESP32 WROOM-32.** posix → device was **backend-only, zero nsl change** (seam portability proven on
silicon). The `pumpEvents` single-source assumption bit exactly as flagged — zephyr `pulse_pump()`'s `K_FOREVER`
sleep starved a second source; fix = `pulse_pump_nb()` gated on `http_is_listening()` (the recorded round-robin
design point, now load-bearing code). Also fixed: ROM-newlib forced `setjmp`/`ungetc` faults (patch script +
hand-rolled parse; zero-SDK-touch tracked in #48) and on-device egress capacity (chunked send + net-buf pool).
RAM is the ceiling (VM heap + WiFi + net-bufs barely coexist on a no-PSRAM WROOM-32). Findings: the `#46` section
in `~/workspace/tuvm/review/model-capability-findings.md`; design/plan dated `2026-07-18-esp32-wifi-http-last-mile-*`.
(Superseded 2026-07-20 by the AMG8833 rung below: `HttpServerNode`/`HttpNode` are retired and
`/thermal` is now served by `ThermalHttpNode` from a **real** sensor. The entry above stands as the
record of what was true on 2026-07-18 — the frame it served was hard-coded.)

**AMG8833 Grid-EYE thermal rung (Arc 1) — phase 1 landed 2026-07-20, NOT closed: unstable under
sustained HTTP load (#54/#55).** `/thermal` now serves a real Panasonic AMG8833 read through Zephyr's
uniform `sensor` API — proven on silicon (`max 30.0C min 25.2C hot (7,2)`, 12 distinct fill colours
vs the synthetic frame's 2; three successive fetches moved 29.5/30.0/29.7 °C with a stable hot cell,
so the cadence genuinely re-samples). Architecture as designed: a Tier-1 C seam (`src/sensor_backend.h`,
posix-synthetic + zephyr-real backends) behind one prim `sensorFrame` (116) answering 64 centi-°C SMIs;
`zephyr/ThermalHttpNode.nsl` nests `ThermalNode` (ctl-cadence sampler) / `HttpServer` (generic
path→block route table) / `ThermalView` (frame→SVG), which never reference each other — producer and
consumer meet only at `dev/thermal0/sample`. `HttpServerNode.nsl` retired; `HttpNode.nsl` kept (it is
the `httpInject:` twin of the `14_http` suite, not a superseded listener — the retirement plan had
conflated them). Suite floor **202/0/14** (was 200), no `CLAIMS.md` row moves — and deliberately so:
a capability claim would be premature while #54 stands.

**The durable lessons (this is the promotion; the dated design/plan docs keep the narrative):**
1. **The host timer backend is a VIRTUAL clock and nothing else moves it.** `advanceMillis:` is the
   only driver, so any host binary running a real reactor loop has permanently frozen timers — silently:
   no error, just a cadence that never ticks. New opt-in `OEVM_TIMER_WALLCLOCK=1` (an **eighth** env
   knob) plus seam call `timer_backend_poll()` drained from `pumpEvents`. Default stays virtual so
   suite determinism is untouched. #51.
2. **There are TWO independent millisecond clocks.** `0 millis` → `oevm_millis` is REAL (and is what
   the `00_perf` benchmarks time with — the published ESP32 tables are unaffected by anything in the
   virtual-clock path); `platform monotonicMillis` → `timer_backend_millis` is the VIRTUAL one. On
   target they converge, which is what makes the split easy to miss. `ThreadTimerNode`/`SoakNode` spin
   on a `monotonicMillis` deadline and would hang forever if run on host. #52.
3. **A raw indexable prim result cannot cross a channel.** `put:value:` runs the value algebra;
   `platform valueRank:` answers −1 for a vtable-less prim result and the fallback `valueRank` send
   DNUs. Copy to a `List` (rank 8) at the publish boundary — `basicAt:` reads the prim, `at:` reads
   everything downstream of the cell.
4. **The `sys/timer` → `Timers>>dispatch:` fan-out lives in the CALLER**, by design (kernel.nsl Timers
   header). Omit it and timer fires reach `sys/timer` and stop.
5. **A devicetree overlay filename is derived from the board qualifiers** (`esp32_devkitc/esp32/procpu`
   → `boards/esp32_devkitc_esp32_procpu.overlay`), and **CMake caches overlay discovery** — a newly
   added overlay needs `--pristine`. Both failure modes are silent: green build, no node, backend
   compiles its `DT_NODE_EXISTS` `#else` branch, nil frames, exit 0.
6. **DRAM is the ceiling at 99.58%** (195792 of 196608 B — ~816 B spare). Pre-existing; the sensor cost
   ~200 B. Anything device-resident now competes with it. #53.
7. **A periodic timer + a blocking HTTP render overflows the 16-slot ISR pulse mailbox, and a full
   mailbox is a hard `k_oops()`.** Node dies after ~25 s of light load. Idle for 70 s is fine, so load
   is required to trigger it. This is pre-existing infrastructure fragility, not a sensor defect:
   `ThermalHttpNode` is the **first node to run a periodic timer and an HTTP listener at once**
   (`HttpServerNode` published one static frame and armed no timer). #54; fix scoped in #55, which must
   land before Task 9 (the phase-2 threshold trigger adds another ISR producer to the same mailbox).

**The meta-lesson: every bug this rung presented as SUCCESS.** Four separate silent failures (missing
`sys/timer` wiring, the frozen virtual clock, the wrong overlay filename, the incremental build ignoring
a new overlay) produced green builds, passing gates and exit 0 while doing nothing. Every one was caught
by running the thing and inspecting real output, never by reading code. Two verification steps in the
plan were themselves worthless for the same reason — `run-one.py` reports SKIP *before* it compiles
(`run_tests_oevm.py:395`/`:483`), so a syntax error and a clean pass are indistinguishable. Plans should
assert on observable behaviour, not on "it built". This is the argument behind #56 (agent-watchable
on-device test/debug/profiling): diagnosing #54 took ~a dozen hand-built hardware round-trips, the panic
backtrace was unsymbolized and CORRUPTED, and **the mailbox overflow was never observed — it was inferred
from source**, so #54 is a well-supported hypothesis rather than a measurement.

Design: `docs/design/2026-07-19-amg8833-thermal-sensor-rung-design.md`; plan:
`docs/superpowers/plans/2026-07-19-amg8833-thermal-sensor-rung.md` (Tasks 1–8 ticked; Task 9 gated on
#55, Task 10 is this entry). Open: #51, #52, #53, #54, #55, #56.

**ESP32-C6 second Zephyr target (Arc 1 spike) — measured 2026-07-20, NOT a primary-target
decision.** Evaluated per #57 (David has 3 boards on hand) as a candidate fix for #53's
DRAM ceiling. All four `max-linkable OEVM_HEAP_SIZE` cells are now measured and reviewed:

| board    | radio            | ceiling  |
|----------|------------------|----------|
| WROOM-32 | WiFi             | 53248 (52 KB) |
| WROOM-32 | none             | 106496 (104 KB) |
| C6       | none             | 319488 (312 KB) — links at 100.00% region, 8 B spare |
| C6       | OpenThread/IPv6  | 262144 (256 KB) |

**The honest headline is 262144 vs 53248 = 4.9× more VM heap on a board with a working
radio.** OpenThread costs 57344 B vs WiFi's 53248 B, so the two radios are near-equally
expensive — the C6's entire advantage is its unified memory map, not a cheaper radio. Do
**not** headline 319488: it links with 8 bytes spare (the exact condition #53 already
calls a hard ceiling, just on a bigger part), was measured under a near-idle node, and
the GC is collect-when-full so heap high-water always pegs to the arena ceiling regardless
of live-set size. The wall moved; it should not be leaned on.

**The durable lessons (this is the promotion; the dated design/results docs keep the
narrative):**
1. **Board target `esp32c6_devkitc/esp32c6/hpcore`.** Overlay filename derives from the
   board qualifiers (`boards/esp32c6_devkitc_esp32c6_hpcore.overlay`) — **a wrong name is
   silently ignored, not an error**, and CMake caches overlay discovery, so a newly-added
   overlay needs `-p always`/`--pristine` on the first build after adding it (same failure
   shape as the AMG8833 rung's lesson 5 above, now confirmed on a second board).
2. **No FPU** (`rv32imac_zicsr`) — tuvm runs soft-float on the C6. The Xtensa float-arena
   knobs (`OEVM_FLOAT_ARENA_SLAB_CELLS=512`, `OEVM_FLOAT_ARENA_SLAB_BUDGET=4`) transfer
   unchanged and were not re-tuned; `struct Float` is 16 B on both targets (see 5).
3. **Flash offset `0x0`, not `0x1000`.** At `0x1000` the C6 boot-loops silently — no
   error, no console output.
4. **Console on the UART bridge, flashing on native USB** — the non-obvious assignment.
   The native port re-enumerates on target reset (breaks a continuous reader); the bridge
   chip never reboots, so it carries the always-on log. This is what makes an
   agent-watchable serial log possible (the #56 debug leg).
5. **`layout-*.json` must be generated per target because you cannot know it matches
   until you look — not because it necessarily differs.** The measured RV32 ilp32 layout
   turned out byte-identical to Xtensa (every struct size and field offset), verified
   three independent ways (chip probe, compiler flags, and the `oevm.h` field types). A
   target with genuinely different alignment rules would differ; this one did not — Model
   B's bake path still treats each target's JSON as authoritative rather than assuming
   reuse.
6. **`OEVM_HEAP_SIZE=H` really costs `H + H/32`.** `src/gc.c:33-34` declares
   `heap_bitmap[OEVM_HEAP_SIZE/sizeof(oop)/8+1]` — the GC alloc bitmap — as a *separate
   static array outside the arena*, exactly `H/32` on any 32-bit target (board-independent).
   This yields a ceiling cross-check: `used(H) − H − H/32` is constant per config
   (108032 for C6 no-radio, 166160 for C6+Thread), and each constant reproduces its own
   cell's measured ceiling exactly.

   **Be precise about what that does and does not prove.** Neither constant was a forward
   prediction of an unmeasured value — both were fit from their own cell's failing probes
   after that bisect had converged. What transfers is the *model form* (`H + H/32 + const`),
   established on cell (c) and then applied unchanged to cell (d), where it again yielded a
   constant stable to the byte across three probes. So this is cross-validation of the model
   and strong evidence the bisect measured capacity and only capacity — not an independent
   derivation of either ceiling. An earlier revision of this entry called the C6+Thread
   figure "a genuine forward prediction"; that overstated it. See
   `docs/results/2026-07-20-c6-heap-ceiling.md`'s cell (c) section for the derivation.
7. **The VM cannot link without `CONFIG_NET_SOCKETS` on any target.** `src/udp.c` and the
   Zephyr UDP/HTTP backends are in `target_sources` unconditionally because
   `dispatch.c`'s `send()` fast path probes `udp_object()` on every send. "No radio" never
   means "no sockets" (echoes the AMG8833/Framing note above, now load-bearing on a
   second board's Kconfig too).
8. **No forced-ROM-newlib fault on the C6.** The WROOM-32's `tools/patch-esp32-rom-ld.sh`
   problem (ROM binding `fflush`/`setjmp` as strong symbols reaching for a nonexistent
   FreeRTOS TCB) does not reproduce; the patch is correctly gated to the `esp32` profile
   only (`ROM_PATCH=0` in `run.sh`'s `esp32c6` profile).
9. **OpenThread builds for RISC-V in Zephyr 4.2 with no porting work.** The board yaml
   declares `netif:openthread` and does not declare `wifi` — WiFi is out of scope on this
   board by hardware/BSP decision, not a tuvm gap.
10. **The table is reproducible with `tools/heap-ceiling.sh`.** `bash tools/heap-ceiling.sh
    --board esp32|esp32c6 [--conf-extra prj_esp32c6_thread.conf]` bisects the max-linkable
    `OEVM_HEAP_SIZE` to a 4096 B grain against the linker's own region-overflow diagnostic
    — link-only, no hardware needed. The C6's linkable region is `sram0_0_seg` = 437512 B,
    not the DTS node's 512 KB.
11. **`--pristine` is load-bearing for the heap bake itself, not just overlay caching.**
    `OEVM_HEAP_SIZE` reaches the compiler via `target_compile_definitions(...
    $ENV{OEVM_HEAP_SIZE})` at CMake *configure* time, so an incremental build reuses the
    cached define and every probe after the first would silently re-measure the first
    probe's heap size — a plausible-looking wrong ceiling. `heap-ceiling.sh` forces
    `-p always` on every probe for exactly this reason; same mechanism as lesson 1's
    overlay-caching trap, one layer lower.
12. **A bare `west build` never exports `OEVM_HEAP_SIZE`.** Only `run.sh` sets the seven
    env exports; skip it and build the app directly with `west build`, and `gc.c`'s 16 MiB
    host default applies, overflowing the link by ~17 MB — which reads as "it doesn't fit"
    rather than "you forgot the exports." `zephyr/CMakeLists.txt:176-190` has a Kconfig
    safety net for exactly this, but it is gated on `CONFIG_SOC_NRF52840` — no C6 (or
    WROOM-32) equivalent exists, so this trap is live on both ESP32 targets.
13. **`run.sh --conf-extra FILE.conf`**, undocumented anywhere in mainline before this
    entry, forwards `-DEXTRA_CONF_FILE=FILE.conf` to `west build` (stacking with the
    WiFi-overlay auto-detection if both apply); `heap-ceiling.sh --conf-extra` forwards
    straight through. This is how cell (d)'s config (`prj_esp32c6_thread.conf` layered on
    `prj_esp32c6.conf`) gets built and bisected at all — without it a ladder-only reader
    cannot reproduce the Thread cell.

**Not decided by this entry.** Whether the C6 becomes the primary dev-loop target (#57),
whether #53 should stay open given the C6 path (recommended: yes — the WROOM-32 remains a
supported target and its ceiling is still real for anyone on Xtensa), and whether Task 9's
trigger thread should be scheduled on the C6 (#58 — clearly affordable there given the
headroom) are all David's calls. Posted as data/recommendations on the three issues, not
closed.

Design: `docs/design/2026-07-20-esp32c6-second-target-design.md`; plan:
`docs/superpowers/plans/2026-07-20-esp32c6-second-target.md`; measurement detail:
`docs/results/2026-07-20-c6-heap-ceiling.md`. Suite floor unchanged (this rung touched no
nsl source) — no `CLAIMS.md` row moves. Open: #53, #57, #58.

**ESP32-C6 end-to-end Thread ping — rung CLOSED 2026-07-20 (#59, relates #57).** The C6
joins the live porta mesh and round-trips application data: `ThreadPingNode.nsl` sends five
CBOR `#reading` Records (`#seq` 1..5) to the gateway's ML-EID echo (`udp_echo6.py` on
`:7777`, systemd unit `udp-echo6`) and prints `UDP OK` **only** after every one is reflected
and decodes value-equal. Two witnesses: that console line, and the gateway's journal showing
five datagrams from `:54360`. **Deterministic: 20/20 boots across two soaks.** This is the
first tuvm data path over 802.15.4 on non-Nordic silicon.

1. **ONE shared `zephyr/src/thread_bringup.c`, not a board split.** The design's premise —
   "the C6 can't compile the nRF bring-up" — was FALSE, from an incomplete header probe.
   Both NCS and upstream 4.2 expose the SAME no-arg OT API (`openthread_init`/`_run`/
   `_mutex_lock`/`_get_default_instance`), declared in the OT **module** header
   `<openthread.h>` (Zephyr 4.2 moved OT to `modules/openthread/`), **not** in
   `<zephyr/net/openthread.h>` — which is where the design-time probe looked. The seam
   (`thread_bringup.h`) is kept as cheap insurance, but there is one implementation.
   `net_if_up()` after `openthread_run()` remains load-bearing: OT attaches without it, but
   `zsock_sendto` returns ENETDOWN (115) forever.

2. **Zephyr 4.2.0's ESP32 802.15.4 driver cannot run Thread — this is the rung's most
   expensive finding, and it is an ENVIRONMENT fact invisible from this repo.** Stock
   v4.2.0 gets you a node that never attaches, and then a node that attaches but drops
   datagrams. Four fixes are required, and `~/zephyr-esp32/zephyr` carries them on branch
   **`tuvm-c6-radio-fix`** (v4.2.0 + 4 commits). **On a plain v4.2.0 checkout the C6 rung
   does not reproduce.**
   - `d66a39f7b5b` (upstream 4.3.x) — `stop()` called `esp_ieee802154_disable()`, a full
     radio teardown, where OpenThread only expects *sleep*. Symptom: role stuck DETACHED
     forever with a provably correct dataset. Its commit message names it: nodes "not
     forming a network".
   - `a9eb90a20e9` + a 3-line extract from `18dbda57d81` — the missing
     `select ESP_MAC_ADDR_UNIVERSE_IEEE802154`. **The select alone is INERT on 4.2.0**: it
     targets a symbol `soc/espressif/Kconfig` doesn't declare there (only hal_espressif's
     IDF-side Kconfig does, which Zephyr never sources), so it fails SILENTLY. Any Kconfig
     cherry-pick must be proven by grepping the generated `.config`.
   - **`03c903fa836` (ours, no upstream fix exists — the bug is live in v4.3.1 too):**
     `esp_ieee802154_transmit_failed()` received the TX error and **discarded** it, giving
     the semaphore unconditionally, so `esp32_tx()` returned 0 for a frame that lost channel
     access or was never acknowledged. OpenThread was told the frame went out and never
     retransmitted it. Measured effect: the 5-datagram gate failed **~40-60% of boots**;
     0/20 after propagating `-EBUSY` (CCA_BUSY/COEXIST → `OT_ERROR_CHANNEL_ACCESS_FAILURE`)
     and `-ENOMSG` (NO_ACK/INVALID_ACK → `OT_ERROR_NO_ACK`) per `ieee802154_radio.h`. The
     same patch also stops `handle_ack()` releasing the previous TX's already-released
     frame. **Upstreamed as zephyr#114163** — see finding 6 for the form that fix
     finally took, which is NOT the one first written here.

3. **Method, twice over.** (a) *"Config is provably correct on-device yet it still doesn't
   work" does NOT imply hardware.* A whole session was spent concluding the C6 radio was
   dead — power ruled out, dataset proven byte-correct on-device — when the answer was a
   one-command `git log v4.2.0..v4.3.1 -- <driver>`. Diff the driver against the newest
   upstream release before blaming silicon. (b) *Instrument the component boundary you do
   not own.* The flake was only diagnosable because the gateway journals every datagram:
   PASS runs showed five, FAIL runs showed zero, which killed "bad reply path", "CBOR
   mismatch" and "timeout too short" in one measurement. Three fixes aimed at the node
   failed before that evidence existed; none were needed after.

4. **Gate design pinned.** The node bounds each wait with a real `monotonicMillis` budget
   (the `ThreadTimerNode`/`SoakNode` idiom), never an iteration count — a spin bound races
   the mesh RTT with no time basis. It distinguishes `echo timeout` from `echo mismatch`;
   the previous single `no echo seq` message read as a mismatch when it was a timeout and
   misdirected the investigation. The gate stays intolerant of loss *by design*: with the
   driver correct, loss is now a real regression signal, not mesh weather.

5. **Dataset overlay ordering.** `prj_esp32c6_thread.local.conf` (gitignored, real porta
   credentials) is auto-layered by `run.sh` **after** `--conf-extra`, because Zephyr merges
   `EXTRA_CONF_FILE` in order and LATER WINS — placed earlier, the tracked placeholders
   silently beat the real dataset and the node joins a fake network.

6. **Upstream review moved the stale-pointer fix, and the reviewer was right (2026-07-21).**
   zephyr#114163 was approved by a maintainer within a day; Espressif's `sylvioalves` then
   asked for the `ack_frame`/`ack_frame_info` clearing to sit at the **top of `esp32_tx()`**
   rather than inside `esp_ieee802154_transmit_failed()`, and for `handle_ack()` to clear
   `ack_frame_info` alongside `ack_frame`. Both were accepted: clearing per-transmission
   also covers the **TX-timeout** path, which returns `-EIO` without ever reaching
   `handle_ack()` and so left the previous transmission's pointers live — a case the
   failure-callback placement could not reach — and it takes two stores out of an IRAM ISR
   callback. Local branch is now `cbc06d38482`; the gate is **20/20 on the revised patch**
   with all 100 expected datagrams in the gateway journal
   (`tuvm-esp32c6-threadping-GREEN-20of20-ackframe-review.log`). *Method note: the two
   trees have diverged — `~/esp/zephyr-upstream-patch` is rebased on Zephyr **main**, which
   already carries an RX-msgq refactor of this driver, while `~/zephyr-esp32/zephyr` is
   v4.2.0 + fixes. The patches are no longer textually interchangeable and must be applied
   to each by hand.*
   Prior evidence for the **pre-review** form, kept because it is the only long-duration
   run: `SoakNode.nsl` overnight, **2390/2399 round-trips over ~10 h, 9 misses, one boot,
   no reset or coredump** (`tuvm-esp32c6-SOAK-overnight-0720-2304.log`) — the run that was
   meant to surface a double-release and did not.

Design: `docs/design/2026-07-20-esp32c6-thread-ping-rung-design.md` (its §1 board-split
claim is amended by finding 1, and its flagged "OpenThread-on-C6 unproven" hazard was real —
just in the driver, not the silicon); plan:
`docs/superpowers/plans/2026-07-20-esp32c6-thread-ping.md`. No `CLAIMS.md` row moves — those
track spec §7 criteria and this is a device gate. Open: zephyr#114163 is in review, not
merged — until it lands the C6 still depends on the local branch; whether to bump the
workspace to Zephyr 4.3.1 wholesale (mbedtls Kconfig renames + OT deprecated-callback
removals sit in that range, so it needs its own compat pass).

**Job-latency instrumentation — rung CLOSED 2026-07-21 (#61, relates #53, #63, #65).**
`ThermalHttpNode` on the WROOM-32 now measures and publishes segmented end-to-end latency
for both job shapes it runs — timer→thermal→cell and request→format→response — as a live
table on the node's own `/stats` page. Seven spans, all past the n ≥ 200 gate, measured on
hardware over 310 thermal jobs and 296 paced HTTP requests:

| span | n | mean µs | min µs | max µs | max/min | p50 | p95 |
|---|---|---|---|---|---|---|---|
| `httpQueue` (C-owned) | 297 | 61 | 41 | 118 | 2.9× | <100us | <100us |
| `egress` (C-owned) | 3567 | 1319 | 98 | 71288 | **727×** | <1ms | <100ms |
| `jobThermal` | 310 | 25229 | 24408 | 28338 | 1.16× | <100ms | <100ms |
| `acquire` | 310 | 12798 | 12491 | 15533 | 1.24× | <100ms | <100ms |
| `vmCompute` | 310 | 4101 | 3961 | 4804 | 1.21× | <10ms | <10ms |
| `publish` | 310 | 8225 | 7848 | 9045 | 1.15× | <10ms | <10ms |
| `render` | 296 | 39837 | 37401 | 98484 | 2.6× | <100ms | <100ms |

Derived (**not measured** — subtractions, labelled as such): `render` = 15894 µs egress +
**23942 µs compute** (12.05 chunks/render); client RTT p50 159071 µs − node service 39898 µs
= **119173 µs network, 75% of what the client feels**. Client side, 296 paced requests:
p50 159 ms, p95 179 ms, max 289 ms. DRAM cost **384 B** measured (`dram0` 195,936 →
196,320 B; a further 16 B for the per-request HTTP stamp brings the shipped node to
196,336 B / 99.86%). Encoded record sizes finally **measured**, not estimated:
`#LatencySpan` **140 B**, `#Metrics` **195 B**.

1. **The segments account for 99.6% of their parent** — `acquire` + `vmCompute` +
   `publish` = 25,124 µs against `jobThermal`'s 25,229 µs. The 105 µs gap across four
   begin/end pairs is the instrumentation's entire cost: **~13 µs per bracket**, including
   the nsl send. Spans are effectively free at this granularity, which is the precondition
   for the whole approach.

2. **The VM is not the bottleneck of the sensor job; nothing is.** The I2C read is 12.8 ms,
   everything the VM does is 12.3 ms — parity. But **two-thirds of the VM's share is
   copying the same 64 numbers around**: `vmCompute` builds an OrderedCollection (4.1 ms),
   and `publish` (8.2 ms) contains a *second* 64-element copy (`List fromCollection:`) plus
   the channel's value algebra. Inferred split ~4.1 ms copy + ~4.1 ms `checkValue:`/
   `rankOf:`. A single 25 ms job number would have said none of this.

3. **Everything the VM touches is predictable; everything the network touches is wild.**
   max/min is 1.15–1.24× across every VM segment and **727×** for `egress`, which is also
   the only span whose p95 sits two decades above its p50. This is the argument for
   percentiles-over-means in one column: a mean of 1319 µs describes neither the 98 µs
   common case nor the 71 ms tail.

4. **A collect-when-full GC on a saturated heap produced no latency outliers.** 1523
   collections during the run, heap high-water pegged at 53,228/53,248 — and `vmCompute`'s
   max is 1.21× its min. GC is frequent, cheap, and inside the mean rather than in the
   tail. This contradicts the usual intuition and is worth citing.

5. **The u32-difference wrap argument is only valid in the units the subtraction happens —
   and the device run is what caught it.** The seam offered one `span_now()` documented as
   free-running microseconds; the Zephyr backend converted cycles→µs *before* returning, so
   its value domain wrapped at 17,895,697 rather than 2^32. `/stats` showed `jobThermal`
   max = **−17872490** with a phantom sample in the ≥1 s bucket; the arithmetic pins it
   exactly (4,277,094,806 = 23,207 − 17,895,697 + 2^32 — an ordinary 23 ms job). **Silent
   on the host**, where the backend unit really is the microsecond. Fixed by moving the
   subtraction into the backend (`span_stamp()` + `span_elapsed_us()`) and renaming the
   stamp so nobody reads microseconds into it again.

6. **The node cannot survive an unpaced request burst — filed as #65 (P1).** 500
   back-to-back requests took it off the network entirely: `EHOSTUNREACH`, `ip neigh`
   INCOMPLETE, **no panic and nothing on the console**, recovered only by hardware reset.
   At 10/s half the connections are refused; at 4/s it is 297/300. `pulseCoalesced` and
   `pulseDrops` were **0** throughout, so this is *not* the #54 blocking-render-starves-
   the-pump pattern. The published HTTP figures are therefore from a **paced** run and are
   labelled so. Pre-existing; #61's spans are what made it characterisable.

7. **Two more source lists, and the count was never the point (#63).** `src/span.c` had to
   reach *nine* enumerated lists across two repos, not the seven #63 recorded.
   `tests/http_roundtrip_host.sh` and `tests/hanoi_ab_host.sh` had been failing to link
   since the prims landed, and the suite's `0 failed` never saw them — neither is invoked by
   `run_tests_oevm.py`. `hanoi_ab_host.sh` was additionally missing `sensor_posix.c` from an
   *earlier* rung entirely.

**Deliberately not built:** the `tickQueue` span (plan Task 12), dropped on David's call —
spending the last of the DRAM on the one span that touches the ISR mailbox, a seam #34/#55
had just stabilised and which #62 notes has no automated Zephyr coverage, is a bad trade.
The published table is complete without it.

Design: `docs/design/2026-07-21-job-latency-instrumentation-design.md` (with two
implementation addenda); plan: `docs/superpowers/plans/2026-07-21-job-latency-instrumentation.md`.
`CLAIMS.md` gains the latency rows; `metrics-spec.md` gains the latency group and has its
*"histograms — nodes can't afford the slots"* line removed as now-false. Open: #65 (burst
fragility), and the derived columns above are subtractions rather than measurements —
closing that would need a span inside `httpChunk:` on the nsl side of the write.

**ADDENDUM 2026-07-22 — #65 is root-caused and fixed, and it partly invalidates the HTTP
half of the table above.** Two corrections to point 6, which recorded #65 as it was
understood on the day. Full record:
`~/workspace/tuvm/review/2026-07-21-issue-65-http-load-wedge/README.md`; branch
`fix/65-http-load-wedge`.

- **The rate table in point 6 is ONE ceiling, not two regimes.** `latency_harness --delay`
  sleeps *between* requests, so nominal "4/s" was really 1/(0.25+0.11) = 2.78/s and nominal
  "10/s" was really 4.8/s. The node's actual ceiling was 4 net_contexts ÷ 1.5 s TIME_WAIT =
  **2.67 req/s** — the node is the active closer (HTTP/1.0 `Connection: close`), so every
  reply parked a context. "297/300 at 4/s" was simply the offered rate landing on the
  ceiling. Fixed by `CONFIG_NET_TCP_TIME_WAIT_DELAY=250`.
- **`EHOSTUNREACH` was RX packet-slab starvation, not a WiFi fault.** The node stayed
  associated and addressed while unable to answer ARP: the 4-deep listen backlog filled
  with idle ESTABLISHED connections which pinned all 8 RX `net_pkt`s, after which
  `zsock_accept()` could never succeed again — and with `CONFIG_NET_TCP_KEEPALIVE` unset an
  idle ESTABLISHED connection has *no timer of any kind*, so nothing reaped them. Freeing
  RX required receiving. Fixed by keep-alive on the listening socket (the reaper), backlog
  4 → 2, `NET_PKT_RX_COUNT` 8 → 16, and `http_poll()` no longer treating every accept
  failure as "nothing pending". Verified by a 130-cycle / 60-minute concurrent soak.

**Consequence for the table: the `egress` and `render` figures, and the client-side p50/p95,
were taken on a build whose RX slab was a bottleneck, and should be re-taken before they are
quoted again.** Widening the slab alone dropped client-side timeouts from 1716 to 183 over a
comparable run — most of the "slow" tail was requests stalling on RX descriptors rather than
on anything the VM did. The `egress` span's 727× max/min spread (point 3) is the figure most
likely to move, and point 3's *argument* — percentiles over means — survives regardless,
because it does not depend on the magnitude. The **thermal** job half of the table
(`jobThermal`/`acquire`/`vmCompute`/`publish`) never touches the network path and is
unaffected, as are points 1, 2, 4 and 5.

Also of note for #53: the fix was funded from the VM arena rather than the packet pools
(`run.sh` esp32 default 53248 → 49152, against measured live bytes of 21,860), which left
`dram0` at **98.19% / 3568 B spare** — more headroom than before this rung, not less.

**Performance deck (deck 2) — BUILT 2026-07-22 (#70, relates #61, #66–#69, #78).**
`review/performance_slides.html` — one self-contained file, no build step, inheriting
`review/slides.html`'s styling so the two decks read as one family. Thesis: *optimize the
right thing*. Preamble + five acts (reality first · what 256 KB costs · the OEOM mechanism ·
the unbenchmarked axes · agent-first as premise), 34 slides in a pool, **four playlists**
over it — record 33 / peer 26 / investor 15 / talk 20 — plus per-element `data-aud`
framing and one genuine variant slide (`stic-investor`). URL carries the audience:
`#peer/9` deep-links a deck and a slide.

The mechanism that matters for the program, not just for the deck:

1. **A figure cannot render without a status.** Every number lives once in a `DATA` block
   with a `src` naming its witness, and carries one of `measured` / `prov` / `der` / `rec` /
   `ind` / `tbd`. A `tbd` is a first-class value that renders as a loud badge, so **an
   unmeasured claim cannot quietly look measured** — the failure `PROTOCOL-NSL-RECORDS.md`
   §4.4 committed against itself with `~185–230 B`. The three tables (spans, STIC, Hanoi,
   surface) are *rendered from data* rather than hand-authored, and the last slide is a
   witness appendix generated from that data at load. That appendix is the surface #69's
   checker reads; hand-authored HTML would have made it theatre.
2. **The #65 addendum above is carried into the artifact, not silently absorbed.** Every
   HTTP-path figure (`egress`, `render`, client p50/p95, and the 7.7% VM share derived from
   them) renders with a `provisional` badge and a slide-level note saying the rows were
   taken on a build whose RX slab was the bottleneck and are pending a re-take. The paced-run
   asterisk stays *as well* — it is a separate defect, and collapsing the two would lose one.
3. **Act I has two independent witnesses now.** The #61 `/stats` table, and the 2026-07-22
   profiler-slice capture of the same thermal job (#72) — the second one *rendered from the
   porta-lite store rather than transcribed*, which is #69's story working before #69 exists.
4. **Surface figures were re-measured at build time rather than transcribed**, and had
   already drifted: `kernel.nsl` 123,308 → **131,423 chars** in one day (Toit's counts
   unchanged; C substrate 422,315 chars ≈ 106k tokens over 36 files). One day of drift in
   the deck's own numbers is the argument for the appendix in point 1.

**Decisions this build made that the design left open.** The §8 open question —
a normalised STIC column — is **decided NO**, not deferred: normalisation is a rhetorical
act, and an option added "just in case" is how it reaches the investor slide. Act IV's
`legibility` slide was rewritten because the design's "a silent failure we cannot yet
explain" is no longer true; the counterexample gets *stronger* — the fix is reported and
the indictment kept, because it was invisible for a day, which is precisely what the
never-silent design law exists to prevent.

**A finding the deck produced by being written: #78.** Tracing Act II's "the collection
weakness is in the shipping job" down to the mechanism showed that `List setItems:` — and
`Map setPairs:`/`Record` with it — computes every element's rank **twice**, once via
`checkValue:` and again via `depthOf:`, each going out to the `platform valueRank:` prim.
`publish`'s 8225 µs is a second 64-element copy plus that doubled walk; `put:value:` itself
is O(1) and touches no element. Estimated ~20% off `publish` from one fused walk, to be
**A/B'd on the device against the very spans that found it** rather than argued — which is
the Act III strategy slide applied to itself.

Verified in-browser (all four playlists walked, per-audience framing swaps confirmed, deep
links, zero console output, no slide overflowing a 680–720 px-tall viewport). Design:
`docs/design/2026-07-21-performance-deck-design.md`; plan:
`docs/superpowers/plans/2026-07-22-performance-deck-build.md`. `CLAIMS.md` gains no row —
the deck asserts nothing new, it cites. Open: #66/#67/#68 are the three badges still
showing, and #69 is the checker that keeps the appendix honest against reality.

**Adafruit MagTag (ESP32-S2) third Zephyr target, and the button seam going plural —
2026-07-23.** Branch `feat/magtag-esp32s2-port` (5 commits, pushed, no PR). The VM boots on
internal SRAM (`TIMER OK`), four buttons deliver one event per press, and chording works.
A capacity/display platform, **not** a perf or mesh one — see the caveats below before any
number from this board goes anywhere.

1. **No board port was written.** `esp32s2_saola.dts:9` already includes
   `<espressif/esp32s2/esp32s2_wrover_n4r2.dtsi>`, which *is* the MagTag's module
   (ESP32-S2-WROVER, 4 MB flash, 2 MB PSRAM), so the board target `esp32s2_saola/esp32s2`
   carries the boot gate and only board IO needed an overlay. Check for an existing board
   target on the same module before porting one.
2. **No FPU.** `soc/espressif/esp32/Kconfig:12` and `esp32s3/Kconfig:11` both
   `select CPU_HAS_FPU`; `esp32s2` does not. tuvm runs soft-float here, like the C6. **No
   float figure from this board belongs in the STIC table without "soft-float" on the row.**
3. **Flash offset `0x1000`, not the C6's `0x0`** — Xtensa keeps the ESP32's offset.
   Confirmed from the board rather than assumed: `esp32s2_saola.dts:13` includes
   `partitions_0x1000_default.dtsi`.
4. **The ROM `setjmp`/`longjmp` patch applies, with a DIFFERENT symbol set.**
   `esp32s2.rom.newlib-funcs.ld` forces `fflush`/`longjmp`/`setjmp` but **not** `ungetc`, so
   `tools/patch-esp32-rom-ld.sh` grew a `--chip` flag and per-chip lists. `TIMER OK` is the
   proof it works — `layoutdump` never calls `setjmp`, so only a fired nsl reaction tests it.
   A `west update` now breaks two boards, not one (#48).
5. **`layout-esp32s2.json` was generated on device**, per the C6 entry's rule above, and came
   out byte-identical to both existing layouts. Generated, not assumed — that is the point.
6. **Flashing is manual and cannot be automated.** The soldered UART Debug header breaks out
   TX/RX only (no DTR/RTS) and the native USB port belongs to the ROM, so esptool has no
   reset line either way: hold BOOT0, click RESET. Worse, **once Zephyr is flashed the native
   USB stops enumerating at all** (no USB stack in this config), so the soldered header is
   not a convenience — it is the board's only console. In download mode the port is
   `303a:0002` / `usb-Espressif_ESP32-S2_0-if00`; in application mode it is a different
   string (`239a:80e6`, Adafruit), which is why a `*MagTag*` glob would be exactly wrong.
7. **`EDGE_TO_ACTIVE` did not mean press-only, and a time-lockout debounce cannot fix it.**
   Two designs were disproved on hardware before the third worked; the failure *masqueraded
   as contact bounce* because the lockout swallowed the release of any short press. The
   working design is a debounced press/release state machine over `GPIO_INT_EDGE_BOTH`.
   Full measurements in #60 and `docs/design/2026-07-23-magtag-buttons-leds-design.md` D3.
   **Method note worth carrying:** the first harness polled the console at 200 ms and every
   measured gap came out a multiple of 200 ms — it was measuring its own loop. A quantised
   histogram means suspect the instrument.
8. **Chording needed no new prim.** A logical pin is only a key into `platform.c`'s
   `host_gpio_register` table, so a button owns a second key for its release (`GPIO + 100`).
   Unregistered release pins are a silent no-op in `host_gpio_fire()`, so the nRF boards are
   untouched. A `gpio level:` read remains the better long-term answer (zero mailbox traffic).
9. **PSRAM is NOT in play.** 2 MB exists but `CONFIG_ESP_SPIRAM` is off, and enabling it
   would not enlarge the VM arena anyway: `oevm_heap_store` is a static array, whereas
   `ESP_SPIRAM` selects `SHARED_MULTI_HEAP` and hands PSRAM out as a separate runtime region.
   Using it means changing how the arena is *obtained*. `OEVM_HEAP_SIZE=65536` is
   **provisional** (dram0 at 81.8% of 217 KB), not a measured ceiling — run
   `tools/heap-ceiling.sh` before quoting it.
10. **Out-of-tree and invisible to git:** the Zephyr SDK needs
    `xtensa-espressif_esp32s2_zephyr-elf` installed
    (`~/zephyr-sdk-0.17.4/setup.sh -t xtensa-espressif_esp32s2_zephyr-elf`); only `riscv64`
    and `xtensa-espressif_esp32` were present. A fresh clone cannot build this target
    without it.

Design: `docs/design/2026-07-23-magtag-buttons-leds-design.md` (8 decisions; the sleep half
D5/D7/D8 is designed, not built). `CLAIMS.md` gains no row — nothing here is a performance
claim. Open: #60 (nsl debounce API + counter API), #80 (nRF DK/dongle unverified on the
`EDGE_BOTH` change — rungs 7/8 depend on that seam), #81 (`timer_zephyr.c:71` kills the node
on a load condition), #82 (LEDs — next work item), #83 (deep sleep / ext1).

**The gpio seam gains an outbound direction — 2026-07-23, #82 done, proven on the MagTag.**
Until now the gpio seam was inbound-only: an edge became a channel post, and nothing could
drive a pin out or ask it its state. Two primitives (`gpioSet:to:` / `gpioLevel:`, ids
123/124) close that, appended to the counted-not-computed table with the parity guard advanced.

1. **Pins are silkscreen GPIO numbers; polarity lives in the overlay.** The device leaves drive
   through `gpio_pin_set_dt`/`get_dt`, so `gpio set: 21 to: true` means NeoPixels *powered*
   though the gate is `GPIO_ACTIVE_LOW`, and `gpio level: 15` true means BUTTON_A *pressed*.
   A raw-pin API would have pushed polarity into every caller and let a typo reach an e-paper
   SPI line on the same controller. See `docs/design/2026-07-23-gpio-output-level-seam-design.md`.
2. **An undeclared pin raises, and the raise is written in nsl.** `gpioLevel:` on a pin the
   overlay never declared answers `nil` from C; `GpioPulse>>level:`/`set:to:` turn that into
   `error: … tag: #gpioUndeclared` — the same division `Vector>>at:` uses over `basicAt:`,
   and the only one available since `build_and_raise` is static to `dispatch.c`. An undeclared
   pin is a wrong literal, not a load condition, so a silent `false` would be a value that is
   simply wrong. **Host divergence (design Risk 4):** on the host `set:to:` creates a slot and
   so cannot raise — the raise has host coverage only through `level:`.
3. **The level read is chording for zero mailbox traffic.** `gpio_target_level` searches the
   *input* table too, so a node reads a button's held state directly. Proven on hardware:
   holding A+C and tapping B printed `held=ABC` (B still down at read time) — three concurrent
   pin reads, no release events registered. This is the better answer the release-event path
   (a6656d3) stood in for; the migration was deferred to a follow-up (deliberately not done in
   the rung that introduces its replacement) and landed in #84 — see the close-out below.
4. **`gpio level:` matches on the LOGICAL pin, not `spec.pin`.** A final-review catch: a node
   names the pin it registered with `gpio pin: N`, which on single-button boards is decoupled
   from the physical pin (`PULSE_LOGICAL_PIN 13` vs `P0.11`). Matching on `spec.pin` would make
   `level: 13` on the nRF DK raise for a registered button and disagree with the host. Fixed to
   match on `pulse_logical_of()`; identity on the MagTag, correct everywhere.
5. **Output-readback is unusable on this driver (design Risk 1, now measured).** Reading GPIO13
   back after driving it returned `false` regardless of the driven level — the esp32 driver does
   not leave the input path enabled on a `GPIO_OUTPUT` pin. The LED node toggles from its own
   state and only *prints* the readback, never steering on it, so the gate (the physical D13 LED
   following BUTTON_A — confirmed) is unaffected. Do not use `level:` on an output as a fault
   detector.
6. **No new `.c` file.** The host/device split rides `#ifdef __ZEPHYR__` (precedent in `gc.c`,
   `vm.c`), so the nine-source-list tax (#63) is avoided for two functions. Firmware links at
   dram0 81.8% of 217 KB — fits, tight; `OEVM_HEAP_SIZE` stays provisional.

Gates (`zephyr/MagTagLedNode.nsl`, over the soldered UART console): gate 1 — D13 red LED
follows BUTTON_A (on→press→dark); gate 2 — `held=ABC` via `level:` with no release pins
registered. Host: `nsl-tests/13_pulse/02_gpio_set_level.nsl` (drive/read/nil) and
`03_gpio_undeclared_raises.nsl` (the tag-asserting raise gate). Full suite 214/0. `CLAIMS.md`
gains a row (the raise is a behavioral guarantee). Open: NeoPixel *data* (ws2812/RMT driver)
is a separate, larger issue — this rung only made the GPIO21 power gate drivable. (The
chording-migration follow-up landed as #84, below.)

**Chording migrated onto `level:`; release events retired — 2026-07-23, #84 done, proven on
the MagTag.** The #82 rung introduced `gpio level:` but left the older release-event chording
path (a6656d3) in place, deliberately not replacing a proven seam in the rung that first ships
its replacement. #84 completes the swap.

1. **`MagTagChordNode.nsl` now reads, it does not track.** It registers only the four press
   pins and, on any press, reads the held set via `gpio level:` (the same `heldOf:` shape as
   `MagTagLedNode`). The four release-pin registrations and four held-flags are gone. Held
   state is now *pulled* when a button fires, not *pushed* on every press and release.
2. **`OEVM_BUTTON_RELEASE_BASE` and the release-fire branch are removed from `main.c`.** The
   `else` branch that detects the settled release **stays** — it re-arms the debounce state
   machine (`in->pressed = false`), which is why `GPIO_INT_EDGE_BOTH` is still requested. Only
   the `gpio_target_fire(logical + 100)` call went; the release edge is now internal, firing
   no event a node can see. `MagTagChordNode` was the sole registrant of a release pin, so
   nothing else was touched.
3. **The `up  held=` lines are gone by construction.** A release produces no event now, so the
   node observes state only on a press — the trade the design chose (zero mailbox traffic for
   held-state, at the cost of not seeing a release until the next press).
4. **Hardware gate (MagTag ESP32-S2, 2026-07-23, soldered UART console):** hold A + tap B →
   `held=AB`; hold A+C + tap B → `held=ABC`; tap B alone → `held=B`. The regression is covered
   in the same trace: `B` fired **twice** (`held=ABC` then `held=B`), i.e. press → settled
   release (silent internal re-arm) → press again — proving the release-fire removal did not
   disturb the re-arm. No separate regression flash needed on the S2.

Bench helper added: `zephyr/flash-magtag.sh` folds the S2's manual sequence run.sh cannot
automate (wait for ROM download mode → flash → prompt for RESET → open the PL2303 console,
which is a *different* port from the native-USB flash port). `CLAIMS.md`'s #82 row extended
with the migration outcome. Open: #80 (nRF `EDGE_BOTH` still unverified on hardware — the
re-arm this rung now relies on exclusively runs there too), #81 (`timer_zephyr.c:71`).

**WiFi proven on the MagTag (ESP32-S2) — spike, 2026-07-23 (#85).** WiFi needed *no new
code*: `CONFIG_WIFI_ESP32` builds for esp32s2 on Zephyr 4.2, and the `backend/zephyr` net
stack + board-agnostic `wifi_bringup()` + #72's `NET_IPV4_MAPPING_TO_IPV6` carried the whole
chain. On hardware: `CONNECT_RESULT status=0` → DHCPv4 `ip=192.168.0.157` → `TIMER OK`. **The
cost is dram, and only dram:** WiFi's ~40 KB system heap + net bufs overflow `dram0` by
32,704 B at the default 64 KB arena; **halving the arena to 32 KB fits at dram0 99.51%**
(1,088 B spare — the WROOM-32 #53 knife-edge again). So a WiFi node on this no-PSRAM part
trades half its VM heap: fine for a small/net/e-paper node (the e-paper framebuffer is a 5 KB
C arena per G3, not VM heap), but it cannot also hold a large working set (deltablue 112 KB)
or a colour TFT framebuffer (RGB565 150 KB) while WiFi is up. The only reclaim lever the S2
has that the WROOM-32 lacks — its 2 MB PSRAM for the WiFi/LwIP heap — is **unproven and not a
one-liner** (S2 PSRAM has WiFi-DMA constraints; Zephyr wires it as a separate
`SHARED_MULTI_HEAP`, not the static `oevm_heap_store`). Spike only: overlay in scratch, heap an
env override, no tracked-file change. Also reconciled #36 (native-ESP-IDF WiFi backend) as
superseded — WiFi shipped via Zephyr under #46. Open: #85 (make S2 WiFi a tracked config +
decide the arena/PSRAM tradeoff).

**The MagTag makes sound — spike, 2026-07-23 (#86, commit `5d7fdd8`).** The onboard buzzer
works under Zephyr: SPEAKER is GPIO17 (also DAC_1/A0) driven by LEDC channel 0 through a
pinctrl route; SPEAKER_SHUTDOWN is GPIO16, HIGH = un-muted. Proven on hardware as three
ascending tones (E6/G6/C7, 500 ms each). Inert without `CONFIG_PWM` — the plain button build
is unchanged at dram0 81.82% (181,808 B), so the button and WiFi images are untouched.

Four gotchas, each of which cost a build, and one design consequence:

1. **The amp needs ~50 ms to settle after enable.** Driving PWM in the same breath is silent.
   This masqueraded as a pin-routing bug for three builds — the largest time sink of the rung.
2. **A cold first `pwm_set` can return `-EINVAL`** (not *always*, as a first read of it claimed):
   with the slow clock not yet enabled the driver falls back to full 14-bit resolution and the
   prescaler underflows its `0x100` floor. **Low frequencies are what is at risk** — first-call
   500 Hz → 250 and 1047 Hz → 119 rejected, first-call 2000 Hz → 500 passed. Spend the risky
   first call on a throwaway tone.
3. **`rc=0` is not evidence of audio.** 160 ms notes returned success and were inaudible; 500 ms
   is loud. A silent pass reads exactly like a driver failure — the same "a silent port proves
   nothing" trap in a new place (see also #87 on `--capture`).
4. **LEDC has a low-frequency floor** (`SOC_LEDC_TIMER_BIT_WIDTH` = 14 on the S2), so ~1 kHz is
   the practical bottom.

**Consequence: a bell is not a PWM job.** A bell is a low fundamental with decaying partials;
LEDC gives neither, and has no amplitude control. The bell path is the **DAC on the same pin**
(GPIO17 = DAC_1/A0, what the vendor guide drives) streaming unsigned 8-bit PCM — `dac_esp32.c`
and `dac@3f408800` already exist. PWM stays fine for a notification beep. This matters because
the motivating idea — a **ship's-bell clock** (bells every 30 min on the 8-bell watch, struck in
pairs) — converges WiFi/NTP + DAC + e-paper on one board, and #85's arena arithmetic (WiFi costs
half the VM heap on this no-PSRAM part) is the budget that decides whether it fits. Not done:
the chime is boot-time C; a real feature wants an nsl-facing `tone:for:` seam like #82's gpio.

**nsl paints the e-paper — 2026-07-23, #88 done, proven on the MagTag.** An nsl node writes
text to the 2.9" panel and refreshes it, full-panel and single-row, on host and ESP32-S2. Four
blocking prims at 125-128 (`displayClear`, `displayAt:col:put:`, `displayRefresh`,
`displayRefreshRow:`) over Zephyr's CFB; nsl never sees a pixel. Mirrors #82's gpio shape and
rides `#ifdef __ZEPHYR__`, so no new `.c` file and no nine-source-list tax (#63). Gates, console
plus photographs: `DISPLAY OK rows=18 cols=12 font=10x16` / `gate1 painted` (three legible lines,
black on white) / `gate2 row 4 -> 1/2/3` (`presses: 3` clean, one row repainting) / `gate3 caught
displayBounds`. dram0 86.10% (191,328 B). Design:
`docs/design/2026-07-23-magtag-epaper-display-seam-design.md`.

**This rung EXTENDED the Tier-1 catalog claim rather than consuming it.** The bench MagTag is the
pre-July-2025 revision, whose IL0373 panel Zephyr supports *nowhere* — verified absent from both
workspaces. The extension was bounded exactly as predicted (one binding yaml, one quirks struct,
two byte-layout functions) because the IL0373 is a UC81xx in all but name. `review/zephyr-driver-
exposure.md` carries the revised claim.

Five findings, four of which only hardware could produce:

1. **The grid is 12x18, and the number that looked like a font size was not one.** CFB ships no
   8x8 font — exactly three (`font1016`/`font1524`/`font2032`); the `8` is `ppt`, a tiling
   granularity (`cfb.c:724`). 128/10 = 12 cols, 296/16 = 18 rows. Geometry now lives in ONE place
   (`OEVM_DISPLAY_ROWS/COLS` in `platform_backend.h`) and device bring-up **hard-checks** its
   derived values against it and refuses to come up on mismatch — a printed number a human is
   meant to eyeball is not an enforcement mechanism.
2. **Blanking-off is load-bearing.** `uc81xx_write` only issues the refresh command with blanking
   OFF; without `display_blanking_off()` every write loads RAM and returns success against a
   panel that never repaints. `rc=0` against blank glass, again (cf. #86's silent audio).
3. **The panel painted WHITE ON BLACK.** uc81xx advertises `PIXEL_FORMAT_MONO10` (1 = Black) but
   its own init clears both RAM banks to `0xff` to reach white — coherent only if 1 = White. CFB
   inverts only when `(pixel_format == MONO10) == inverted`, so it passed the buffer through
   untouched. Fixed locally with `cfb_framebuffer_invert()` (flag-only, no I/O). The MONO10
   declaration looks wrong for the whole family, but one panel is not evidence about
   uc8175/uc8176, so the rung compensates and records the question.
4. **CFB unions glyphs; it does not overwrite — and the two seams had diverged.** A counter
   refreshed in place rendered 0,1,2,3 *stacked in one cell*. `draw_char_htmono` (`cfb.c:262`)
   only sets bits and never clears the glyph cell; its `draw_bg` parameter is accepted and
   ignored (only the VTILED path honours it). Separately — a second, independent defect that
   is NOT the cause here — `draw_text` passes its `wrap` argument into that `draw_bg` slot,
   conflating two unrelated concepts: `cfb_print` supplies true and `cfb_draw_text` false, so
   on a VTILED panel whether the background is cleared depends on which spelling of "draw a
   string" you called. The
   host seam had no such problem — `platform.c` keeps a char grid and REPLACES characters. **So a
   host-green test could never have caught this**, and the fix was to remove the divergence: the
   device now keeps the same shadow grid and CFB is demoted to a pure renderer over a freshly
   cleared framebuffer. +224 B dram0. The lesson generalises past this rung — the design's "one
   place names the geometry" discipline did not extend to "one place defines the write
   semantics", and that gap is where the bug lived.
5. **A plausible fix was written, flashed, and refuted.** The first hypothesis for #4 blamed
   `uc81xx_write`'s `!auto_copy` back-buffer copy (it targets DTM2 on every real refresh instead
   of DTM1, so the controller's old-frame reference never advances). It made **no difference**,
   because the union happens in RAM before any panel command. Reverted rather than left in the
   shared tree as an unvalidated patch; whether it is a real latent bug is **open, and this
   experiment could not answer it**.

**Deviations from the plan.** Gate 2's row flashes black-then-white on each update — design D2
predicted the OTP waveform and the plan pre-committed to calling that a pass; the LUT follow-up
is now the clock rung's. Portrait shipped deliberately (#89: the uc81xx family has no 90/270
rotation, and 180° turned out to be one free register write, exposed as a `rotate-180` devicetree
boolean). **`rotate-180` is ON for this board, settled on the bench the same day** — with it off
the text started at the wrong end and the board read upside down. Which state is correct is a
fact about how the panel is mounted, in no datasheet, which is exactly why the knob shipped off
and deferred to hardware. dram0 is 86.10% either way, confirming the "one register write" claim.

**Heap ceiling, measured not guessed.** `tools/heap-ceiling.sh --board esp32s2` →
max-linkable `OEVM_HEAP_SIZE=94208` (92 KB); 98304 overflows dram0 by 2,912 B — measured with
driver, CFB and shadow grid linked in. The default stays 65536 because a value that LINKS is not
a value that BOOTS and only 65536 has run on hardware. The ~28 KB between is headroom the
ship's-bell clock can claim explicitly and then boot-verify against #85's WiFi arithmetic.

**Three upstream candidates now.** The IL0373 family member, `cfb_framebuffer_finalize_area`
(CFB cannot express a bounded flush — it grows `finalize_area`), and CFB's OR-only
`draw_char_htmono` / `wrap`-as-`draw_bg` confusion. See
`review/zephyr-upstream-contribution` notes and #114163's process lessons before filing.

## The DAC strikes a bell — 2026-07-23, #94 gates 1-2 done, proven on the MagTag

**The rung was rescoped before it was built, and that is the first result.** #94 was filed as a
*ship's-bell clock*: WiFi/NTP + DAC + e-paper converging, with the arena arithmetic as its gate.
The clock face was dropped on the bench — what is wanted instead is a **notification appliance**
(external alert → LED + a line of text + a button to acknowledge), which needs no timekeeping.
So the whole arithmetic that motivated the issue evaporated for its original reason, and the
rung collapsed to **sound only**.

**#86's prediction is now evidence.** That issue argued a real bell needs the DAC because LEDC
bottoms out near 1 kHz (`SOC_LEDC_TIMER_BIT_WIDTH` = 14 on this part; 500 Hz → `-EINVAL`) and
has no amplitude control. Gate 1 swept 220/440/880/1760 Hz: **all four audible, and 220/440 are
below the LEDC floor entirely** — the DAC reaches a register PWM cannot. Gate 2 struck the
ship's-bell pattern at 440 vs 220: **440 far superior**, and the four-step amplitude ramp
(`DAC_CW_SCALE_1/2/4/8`, −6 dB a step) **reads as a strike rather than a staircase**. Verified at
1, 5 and 8 bells — lone stroke, odd count, full end-of-watch eight.

**Two predictions were wrong, both usefully.** (1) A sine was expected to be *quieter* than a
square wave on a tiny buzzer, since a square carries far more harmonic energy — backwards: the
low end is the good end and 1760 Hz sounded bad. Sweeping rather than playing one note is what
caught it; a single frequency would have produced a confident wrong conclusion in either
direction. (2) The four-step ramp was expected to possibly staircase and force per-sample
additive synthesis — it did not, so **the expensive option is off the table** and the bell costs
**zero RAM and zero CPU** (hardware cosine generator, no buffer, no DMA, no blocked thread).

**Zephyr's DAC API cannot make a tone at all.** `dac_esp32.c` is `channel_setup` + `write_value`
— one 8-bit DC level per call, no buffer, no DMA, no continuous mode. So #86's "stream unsigned
8-bit PCM" is not an API call. The tone comes from the legacy IDF `dac_cw_generator_*`
(130 Hz–55 kHz, four amplitude steps), reachable because `CONFIG_DAC_ESP32` compiles
`dac_common_legacy.c` and its directory is on the global include path.

**A silent failure is not a missing failure — #95.** `dac_esp32_init` fails on this part:
`clock_control_esp32.c` returns `-EALREADY` when a module clock is already on (a success), and
`dac_esp32.c:71` treats any non-zero as fatal. `adc_esp32.c:668` discards that same call's return
for the *same* clock module, so the ADC driver already tolerates what the DAC driver dies on.
Invisible because these builds carry `CONFIG_PRINTK` but not `CONFIG_LOG`, so every driver
`LOG_ERR` compiles to nothing; `CONFIG_LOG_MODE_MINIMAL` (~1 KB) made it speak. Same family as
#86's "rc=0 is not evidence of a physical effect". Routed around by taking the pin enable from
`dac_output_enable()` — which *removes* a dependency rather than adding a workaround. Upstream
candidate, not patched locally because that checkout carries the C6 radio fix.

**Known artifact, diagnosed not fixed** *(fixed in stage 1 below — largely, not wholly)*. An
audible echo on each strike: `bell_strike()` calls `dac_cw_generator_config()` per envelope step,
and that rewrites the frequency register and re-stamps phase mid-tone — a phase discontinuity,
i.e. a transient, three times per strike. Fix is `dac_ll_cw_set_atten()` alone, which touches only
the amplitude field.

**The arena arithmetic came back for WiFi, and #85's cost model needed correcting.** Back-computed
from #85's *measured* overflow rather than its quoted Kconfig number, WiFi's full static cost on
this part is **~73 KB** (the 40,960 B heap add plus ~32 KB of driver static data and net buffers),
against ~31 KB free in a display+DAC build. `ESP_WIFI_HEAP_SPIRAM` reclaims 21,504 B — a
**first-class Kconfig choice**, not the bespoke work #85 assumed, with `ESP_SPIRAM` supported for
this SoC series and the 2 MB deriving straight from `esp32s2_wrover_n4r2.dtsi`. But it closes only
half the gap: **the arena must also come down**, 64 KB → ~32–40 KB. PSRAM links (adds a 1 MB
`ext_ram_seg`, costs 2,544 B of dram0); **boot unproven**.

**The 180 s refresh limit, settled (#89).** Real, and it applies to mono panels — Adafruit's own
`il0373_flexible_2.13_monochrome.py` sleeps 180 s — but it is a convention, not an interlock. A
one-row update does **not** escape it today, because #88's `finalize_area` still drives the OTP
waveform: a partial *area* under a full-refresh waveform is smaller, not gentler. Only a real
partial LUT changes that. Moot here: the display's job is now **stats every 30 minutes** = 48
refreshes/day, comfortably inside the guidance, so no LUT is needed and #94's question 3 stays
closed.

**Still open on this rung:** WiFi + NTP so the bells strike at real times, and stats on the panel
every 30 min. The RGB/NeoPixel flash was dropped — it would have needed `spi3` or I2S, since this
Zephyr tree has no RMT driver and `spi2` is the e-paper's.

## nsl strikes the bell — 2026-07-23, #94 stage 1, on the MagTag

**The seam, not the sound, is the result.** `PRIM_pf_bellStrike` (129) promotes the bell from
boot-time C to something a node decides, closing #86's "not done here". Five sites, the same
recipe #88's display seam established: `prim_ids.h`, the enum + `PRIM_NAMES` + **the parity check**
in `dispatch.c`, the host leaf and dispatch case in `platform.c`, the device leaf in
`zephyr/src/main.c`, and the `Bell` class + accessor in `nsl-tuvm/lib/kernel.nsl`. The parity
check is the load-bearing one: omitting it silently misroutes every platform prim after the
insertion point, and nothing else catches that.

**ONE prim, not a family — and the boundary is the interesting decision.** The ship's-bell
phrasing (pairs, lone final stroke) and the four-step envelope stay in C because they are
hardware-shaped; exposing them separately would let nsl compose a striking the cosine generator
cannot voice. The **1-8 range check went the other way, into nsl**, because it is *not* a hardware
limit — the generator would happily strike twenty — it is the ship's-bell convention, a fact about
the product. Consequence, and the reusable part: **C carries no bounds status code for the bell**,
so this seam has two tags (`#bellUnavailable`, `#bellFault`) where the display has three. The rule
that falls out: a status code in C is warranted only when C is the thing that knows.

**The boot demo was deleted, not kept.** Gates 1-2 rang three times of day at start-up. Left in,
that would be ~20 s of bells before a node got a word in — but the real objection is evidential: a
board that rings on every reset sounds like a working seam **whether or not the seam works**. The
amp mute/un-mute moved from once-at-boot to per-strike with it, which is also what makes the
striking re-entrant.

**Witnessed on the MagTag** (console): `BELL OK 440 Hz` → `striking 5` (nsl) → `bell: 440 Hz x5`
(the C leaf, i.e. the prim reached with the count the node meant) → `struck` → `gate3 caught
bellBounds` → `ready`. Gate 3 firing **on the board** is what proves the range check shipped inside
the `.oimg`, not merely that it passes on the host. Then **BUTTON_A pressed twice → exactly two
`repeat 5` / `bell: 440 Hz x5` / `struck`** — the gate that proves the striking is re-entrant, i.e.
that moving the amp mute/un-mute from once-at-boot to per-strike actually works. **Two presses,
two strikings**: no double-fire from the `EDGE_TO_ACTIVE` question (tuvm#84) and no press lost to
the ~2.5 s blocking strike, which is the mailbox shedding-and-coalescing property (tuvm#34) doing
its job on a prim that blocks far longer than a display refresh does.

**The echo: largely gone, and the residual is the honest part.** Configure once, then step
`dac_ll_cw_set_atten()` alone — no frequency rewrite, no phase re-stamp, no RTC-8M re-measure
mid-tone. Bench verdict was *"largely gone"*, so the diagnosed cause was real but not the only one.
Two mechanisms remain and were never addressed: each -6 dB step is an **instantaneous amplitude
jump**, a waveform discontinuity unless it lands on a zero crossing, and the strike **ends by
disabling the generator at 1/8 scale**, a hard cut from a non-zero amplitude. Both are inherent to
a 4-step hardware envelope; neither is a bug in the fix. A cheap discriminator if it ever matters:
a per-*strike* artifact points at the steps, a per-*striking* one at the amp gate.

**Corrected: esptool resets this board itself.** `run.sh` and `flash-magtag.sh` both claimed a
manual RESET was needed after flashing because the native port belongs to the ROM. Wrong —
`ESP32S2ROM.hard_reset()` detects USB-OTG and arms the RTC watchdog instead of driving a reset
line it does not have (`esptool/targets/esp32s2.py:291-314`). Observed: `Hard resetting with a
watchdog...`, then the app booted untouched. The flash loop needs a human **once**, to enter
download mode, not twice. Also confirmed: reopening the PL2303 console handle does **not** reboot
the board — unlike the CP210x consoles.

**A build-hygiene trap worth one line.** The first stage-1 link overflowed dram0 by 38,848 B and
the bell had nothing to do with it: `build-esp32s2-B/CMakeCache.txt` still carried
`EXTRA_CONF_FILE=prj_esp32s2_wifi.conf` from the stage-4 link probe. `--pristine` cleared it.
**Any RAM number quoted from an incremental Zephyr build is suspect** until the cache is checked —
which matters most at stage 4, where the whole question *is* a RAM number.

## The clock strikes itself — 2026-07-24, #94 stages 2-3, on the MagTag

**The headline is not the feature, it is that the feature was claimed a day early.** Stages 2 and
3 were committed and reported as "witnessed by button" on 2026-07-23: BUTTON_B drove
`advanceAndStrike`, the bell rang, the panel repainted, and every visible thing a working clock
does happened. Then the unattended run: **9h23m, 0 of ~19 expected strikes.** The button and the
timer deliberately share ONE code path — the design's own note calls it "THE ONE PATH THE TIMER
AND BUTTON_B SHARE" — so pressing the button exercised all of it *except the only part in
question*, which is whether the timer ever calls it. **An interactive trigger cannot witness an
autonomous path.** The shared path that made the code clean is exactly what made the test
worthless, and neither the host suite nor a photograph of the panel could have caught it.

**The bug is a platform-accessor freshness bug, and it is the reusable part.** `platform timers`
answers a **fresh** `Timers` instance on every call. The node registered with one
(`timers every: … post: … on: …`) and the drain loop dispatched on another, so the C-side k_timer
fired correctly every 300 s, the event arrived, and `dispatch:` looked it up in an instance whose
nsl-side `pending` registry was empty — and dropped it. Silently: no raise, no status code,
nothing to read. **Fresh-instance accessors are safe only when C owns the state** — `display`,
`gpio`, `bell` all qualify, because the instance is a stateless handle onto hardware. `timers` is
the exception in the seam family, because its registry lives in **nsl**. Fix is one field holding
one instance (`b6a9ead`); the general rule is that any seam keeping nsl-side state must be bound
once, not re-fetched.

**Witnessed unattended 2026-07-24, and the gate is a full watch, not a strike.** Boot 07:17,
cadence 300 s, no button touched at any point:

```
00:30 →1  01:00 →2  01:30 →3  02:00 →4  02:30 →5
03:00 →6  03:30 →7  04:00 →8  04:30 →1   ← the watch rolls
```

Nine ticks, nine strikings, zero misses, zero faults, every repaint landing **after** its strike
as the ordering rule requires. Insisting on a full 8-bell watch rather than stopping at the first
strike is what verifies `bellsForHalfHour:` across its whole range and the roll back to 1 — the
button gates had only ever exercised a hand-picked count.

**The cadence was cut 30 min → 5 min for the bench, and that makes it a 6× demo clock.** One tick
still advances one half-hour of *clock* time, so a full watch costs 40 min of bench time instead
of 4 hours — but the panel now reads `04:30` after 45 real minutes. 300 s was chosen as the
fastest cadence that still clears the e-paper's ~180 s refresh guidance (#89), so the panel is
never stressed. **Stage 4 must reconcile this**: NTP alignment needs either 1800000 for that
build or an explicit tick→clock-time mapping. Flagged in the node, not left to be rediscovered.

**Stage 3's panel is honest about what it does not know**, which is the point of putting stats on
glass at all: `bells` reads `--` until the first strike rather than showing a seeded `lastBells`,
and `ntp`/`24h` read `--` through stages 2-3. A panel that displays a plausible number it has not
earned is worse than one that admits the gap.

**One number came out of the run unresolved, and it is stage 4's problem.** Live bytes climbed
monotonically across all nine ticks — 18 KB at boot, then 21/24/27/30/33/35/38/41/**44** KB — with
`gc 0` throughout. That is the collect-when-full GC behaving exactly as `tuvm-vm-memory-model`
says it does (nothing is reclaimed until the arena is full), so it is **not evidence of a leak** —
but the run ended before the first collection, so it is **not evidence against one either, and no
post-GC live figure was ever measured.** It matters because stage 4 links at a **16 KB arena**,
and this build shows 18 KB live at boot on a 64 KB one: uncollected, that working set does not fit
the arena stage 4 proposes. The cheap discriminator is a host run at `OEVM_HEAP_SIZE=16384` under
simulated time, which costs no hardware.

## The bell rings at the right time — 2026-07-24, #94 stage 4, on the MagTag

**WiFi + SNTP + count-and-phase catchup, witnessed at 08:30:00 Phoenix time.** The chain,
unedited, ~40 s from reset:

```
wifi: up, ip=192.168.0.157
clock ready, 1800000 ms per tick
ntp: ok, local sod=30596            <- 08:29:56, against a wall clock reading 08:29:57
ntp sync -> 08:00, boundary in 4000 ms
08:30 strike 1
| tuvm  08:30+ | heap 19/20K | gc 2 | bells 1 | ntp sync
```

Both halves of the catchup are visible in those two lines. **Count:** 30596 s floors to
half-hour index 16, so the panel reads `08:00+` — the counter is in the 08:00 slot, which is
what `+` has always meant. **Phase:** 1796 s into that slot leaves 4000 ms to the true
boundary, and the strike lands there. A count that is right while the strikes fall at
boot-relative offsets is still a clock that rings at the wrong minute; phase is what makes it
a clock.

**THE PLAN'S 16 KB ARENA COULD NOT HAVE BOOTED, and finding that without the board is the
transferable part.** `VM_STACK_INIT_WORDS` is 4096 words = **16,400 B on a 32-bit target**,
against a **16,384 B** arena — the VM stack alone is larger than the whole heap it must be
allocated from. The failure is structural and lands at boot, before a line of node code runs:
`Out of Memory (requested 16400, used 172 of 16384)`. Every RAM discussion on this rung — the
design's own fallback ladder of *drop DNS → drop SNTP → fix PSRAM* — was aimed at the wrong
quantity, because 16384 was only ever established as a figure that **links**, and linking says
nothing about whether the arena can hold the VM's own fixed structures.

**The method that caught it, which is the reusable recipe.** Build the host image VM
`-m32` with the *device's* knob values (`METHOD_CACHE_SIZE=256 PRIM_CACHE_SIZE=256
VM_STACK_INIT_WORDS=4096 OEVM_MARK_STACK_SIZE=512`) and a candidate `-DOEVM_HEAP_SIZE`, then
run the node's allocation path under simulated time. **The `.oimg` must be recompiled against a
32-bit layout** (`dump_layout` built `-m32`, `oop_size: 4`) or the image is silently wrong and
the VM dies with `Message 'OIMG' not understood` — the 64-bit layout is the default and nothing
warns. Measured, 20 simulated ticks each:

| arena | stack words | result |
|---|---|---|
| 16384 | 4096 (device stock) | **fails at boot** — `requested 16400, used 172 of 16384` |
| 18432 | 4096 | fails mid-run |
| 20480 | 4096 | survives, gc=60, live 19,256 |
| 24576 | 4096 | survives, gc=15, live 21,984 |
| 16384 | 2048 | survives, gc=15, live 13,792 |
| 16384 | 1024 | survives, gc=9, live 6,812 |

**Shipped at `OEVM_HEAP_SIZE=20480` with the stock stack**, which links at **96.64% dram0
(~7.4 KB spare)** and boots at `heap 18/20K`. The alternative — 16384 with a 2048-word stack —
has more link headroom and is tempting for the ADC rung queued next, but the VM stack is
**fixed-capacity and never grows** (`vm.c:398`), and an overflow is a bare `abort()` that no
node can catch. Shrinking it trades a boot failure for a crash on the first deep call, so it
stays on the shelf as a *known* lever rather than a default. The 64-bit host, whose objects are
wider, is the harder case — so a working set that fits there fits the device with room; the
inference runs one way only and the probe file says so.

**Why the pre-flight was worth its hour: the flash budget.** Entering download mode on this
board is a hand operation (BOOT0 + RESET), so an unattended session gets **unlimited flashes
but exactly one run** — esptool's post-flash watchdog reset boots the app, and Zephyr then takes
the native USB down until a human intervenes. Under that constraint the ordering rule is
absolute: everything provable on the host is proved before the reset is spent. Here it converted
a certain wasted run into a table.

**Three defects in the plan's code sketch, each fatal on a one-run budget.** (1) The sketch
re-armed the periodic strike inside `advanceAndStrike` whenever `strikeTok == nil`, reading the
token as a mode flag — but `advanceAndStrike` has a second caller, BUTTON_B, so a press between
a re-phase and its boundary would have left **two strike timers armed forever**. The phase
one-shot now posts to its own channel. (2) `(halfHours - newHalf) * 1800` is unwrapped, so a
local 47 against a true 0 reads as a 47-half-hour correction instead of one, spiking the 24 h
window by 84,600 s once a day. (3) `self corrField: lastCorr , 's'` — binary `,` binds tighter
than a keyword message, so that passes `lastCorr , 's'`, an SMI concatenation, *into* the
formatter. All three are the kind of defect a plan cannot catch by review, only by execution.

**A cadence decision the rung forced.** Stages 2-3 ran a 300000 ms tick — a 6× demo clock — and
stage 4 had to reconcile it. It went back to **1800000**: aligning a count to real time and then
advancing it six times too fast is not a clock, and the alternative (an explicit tick→clock-time
mapping) only formalises being wrong between syncs. Bench speed was the only loss, and BUTTON_B
still fast-forwards.

**Second strike, and the steady state it revealed.** `09:00 strike 2` landed on the boundary
with `ntp +0.0s` / `24h +0.0` / `+0.0` — the honest-coarse readout populated, reporting that the
correction the count needed across the half hour was **zero**, i.e. the crystal held. Heap
steady at `19/20K` with `gc 5`, so the collector is cycling and the working set is flat.

**But the periodic strike timer never fires in the synced steady state, and that was not
designed — it fell out.** The NTP timer is armed ONCE, on the first sync, as a periodic at
1800000, so **every subsequent sync lands at the same offset within the half-hour, frozen at
whatever moment the board first synced.** This board booted at 08:29:56, so its syncs land
~4 s before every boundary; each one re-phases (cancel the periodic, arm a one-shot), the
one-shot strikes and re-arms the periodic, and the next sync cancels it 1796 s later. The clock
is therefore driven **sync → one-shot → sync → one-shot**, and the periodic is a fallback that
only runs if a sync misses. That is benign — arguably better, since every strike is
NTP-aligned, and the fallback is correctly phased because it was armed at a true boundary — but
it means the free-running periodic path is **proven only by stages 2-3 and the host suite, not
by this run**, and the honest reading of the hardware evidence is that the synced path was
witnessed twice and the degraded path zero times.

**The wart underneath it is a simplification made against the design.** The design specified
`ntpServerMidpointMillis` (900000) precisely so syncs land at the inter-strike midpoint; the
implementation dropped it, reasoning that the drain is serialised so a sync/strike collision
costs latency rather than correctness. That reasoning holds — but it left the sync offset
**boot-dependent**, and this boot froze it at the worst available position: 4 s before every
strike, for a call that blocks up to 4 s on timeout. Cost is a bell up to ~4 s late, never a
bell missed. Fix is one line — arm a one-shot at `msToBoundary + 900000`, then the periodic
from there — and it is queued rather than flashed, because entering download mode needs a
human.

**The panel reading `heap 20/20K` is a plateau, not a leak — measured, because the display
cannot tell you which.** The device climbed 18 -> 19 -> 19 -> 20 KB across its first three
strikes, which reads exactly like a slow leak and would be a genuine problem on a bench where
re-flashing needs a person. Settled on the host instead: **96 simulated strikes (48 h) at the
shipped 20480 arena** hold live between **19,084 and 20,384 B with no trend** (tick 1: 20,016;
tick 96: 20,000; final 19,588) and a **constant** GC rate of ~2.9 cycles per strike. Constancy
is the load-bearing half — an *accelerating* GC rate is what a shrinking free pool looks like,
and a plateau at 95-99% of the arena is just collect-when-full doing what
[[tuvm-vm-memory-model]] says it does. The lesson generalises: **at a collect-when-full GC the
live figure pegs near the ceiling by construction, so a rounded panel reading is nearly
uninformative about headroom — the informative series is the GC RATE.**

**Known imprecision, stated rather than hidden:** `up` counts one whole tick for the phase
one-shot's partial interval, so it reads `0d00:30` about 40 s after a boot that syncs, and
`0d01:00` at the second strike. At the row's 30-minute granularity that is inside the rounding
it already admits to, but it is an over-count, not a rounding.

## The clock survives its own death — 2026-07-25, #94 stage 5, on the MagTag

**The rung:** the always-on reactor clock became a run-once deep-sleep appliance. Between
strikes the CPU is off and **the VM does not exist** — each timer wake is a fresh boot that
lives a few seconds, strikes, and dies; state crosses the gap in RTC slow memory through an
indexed-SMI seam (`rtcGet:`/`rtcPut:value:`), and the bell counter is a **monotonic ratchet**
(a sync may advance it, never retract it) so a backward NTP correction cannot double-strike.

**Witnessed 2026-07-25 on the MagTag** (compressed witness build: physical sleeps cut to 15 s,
the clock still advancing a true half-hour per wake): ~15 real deep-sleep cycles
(`deep sleep 15 s` → 15.0 s powered down → `rst:0x5 (DSLEEP)` → cold boot), the ratchet
climbing 1→8 **with no `cold boot` line after the first** — retention across real power-downs,
the event a full bench day of reboot-substitutes never produced. At the first watch-end the
sync **advanced** `true 66572 predicted 12600 -> advance half 36` (silently — no bell storm for
the skipped halves), and the very next wake rang **5 bells as the wall clock read 18:30
Phoenix** — half-hour index 37, watch position 5: right bells, right time, off an anchor the
board fetched itself two minutes earlier. The second watch-end took the **hold** branch:
`true 66681 predicted 72000 -> hold half 40` — NTP said the clock was 89 minutes ahead and the
ratchet **held** rather than stepping back, recording `ntp -5319.0s` (= true − predicted,
exact) and `ppm -979919` (= −5319/5428 s elapsed, exact; the magnitude is the compression
artifact, the arithmetic is the witness). Both watch-end panels painted all rows inside the
12-column grid, including the pathological ppm. The shipping build (1800 s cadence) is flashed
and free-running; a genuine drift figure needs its first real watch-ends.

**Two root causes found on the host, zero flashes, after a bench day of isolation pointed at a
phantom.** The 2026-07-24 session ended with eight flashes "proving" a POWEROFF × config
interaction and a retention failure. Both were something else:

1. **`CONFIG_POWEROFF` died at the boot handoff because of a Zephyr loader bug.**
   `soc/espressif/common/loader.c` `map_rom_segments` classified any segment loading into RTC
   IRAM (0x40070000 — the wake stub the sleep HAL places, one address past `SOC_IRAM_HIGH`) or
   RTC slow memory (0x50000000) as end-of-image, broke its walk early, and never ran the
   IROM/DROM flash-offset fix-up — the MMU then mapped stale linker offsets and the first
   flash-mapped fetch died silently right after `libc heap size`. The dying boots printed
   `Image with 3 segments` while the ROM had loaded five, and their `paddr=00040000h` is
   exactly the stale static. The minimal deep_sleep sample "worked" only because, without a
   135 KB `.oimg` incbin moving the elf2image layout, the stale statics happen to be right.
   Fixed (`bc47c6fab9c`, all three RTC windows classified; upstream candidate) and verified by
   simulating the exact walk over the real failing binary (`docs/spikes/2026-07-25-esp32-
   loader-walk-sim.py`): the fixed walk computes IROM `0x20000`/DROM `0x80000`, byte-identical
   to the known-good boot. **The method lesson: the isolation ladder varied configs; the bug
   was in code that reads the image, so no config delta could localise it. Reading the loader
   against the image's actual segment table found it in one sitting.**

2. **The "retention failure" was the RTC magic overflowing the device SMI.** `0x94BE11C0` has
   int32 magnitude 2^30.7; a 32-bit SMI holds ±2^30, so `rtcPut:` stored garbage, the magic
   never validated, and every wake cold-init — on the 61-bit-SMI host it round-trips, which is
   why every host test passed. Reproduced with a `-m32` host VM + the real `.oimg` (wakes
   1-2-3 all cold), fixed to `0x14BE11C0` = 348000704, re-verified (`wake half 2/3/4`).
   **The false inference is the reusable half:** the prior session concluded `sys_reboot`
   loses `.rtc_noinit` — but a magic that never validates is *observationally identical* to a
   retention failure (garbage ≠ magic every wake, retained or not), so the reboot-substitute
   run proved nothing about retention either way. And the seam rule it hardens: **any constant
   crossing the nsl↔C boundary must fit |v| < 2^30, and the detector is running the harness
   against a `-m32` VM — the 64-bit suite is structurally blind to this class.**

**Two smaller facts the run surfaced.** The e-paper was being wiped on every boot — and the
first fix (`1b96fee`, buffer-only CFB clear) was necessary but NOT sufficient: the remaining
wiper was `display_blanking_off()`, which the uc81xx driver implements as a full refresh of
controller RAM its init just cleared to white. The durable fix (`01aa7fa`) makes bringup LAZY
behind `display_target_available()` — wakes that never paint never touch the panel, and the
watch-end stats persist on the glass across strike wakes (photographed on the 1-min demo). And
**a reflash is always a cold start**: the BOOT0+RESET chord reads as a POWERON-class reset and
RTC slow memory does not survive it, so after any flash the clock free-runs from 00:00 and
converges at its first watch-end sync (≤4 h) — accepted design behaviour, worth knowing before
reading a 1-bell first strike as a bug.

**The evening's 1-min demo pass (same day) hardened three more things** — full detail in the
2026-07-25 evening comment on #94: `elapsedSec` now accrues awake time (without it the drift
row would carry ~+9700 ppm of pure boot overhead — the panel's zero-drift `ntp +0.0s` hold at
`sync true 69132 predicted 69132` is what the fix looks like when it works); the cadence is one
parameterized `slotSec` (1800 shipping / 60 demo — scaling the slot keeps every number honest,
where compressing only the physical sleep repeats stage 4's rejected 6×-clock mistake); and a
cold boot paints a placeholder panel (dashes for never-synced rows) as a display-alive signal
that also wipes any stale reflash image.

**The same evening the cold-boot path was corrected, and the correction is the rung's sharpest
lesson about where a defect hides** (`d2f82ed` tuvm / `6bdf90a` nsl-tuvm; design §8 amendment).
The paragraph above records "after any flash the clock free-runs from 00:00 and converges at its
first watch-end sync (≤4 h) — accepted design behaviour, worth knowing before reading a 1-bell
first strike as a bug." **That was wrong, and it was wrong in the most expensive way: it wrote a
defect down as intended behaviour.** §8 of the design had always called for a sync at cold boot;
stage 4 ran a boot-sync retry every ~30 s precisely "so no wrong bell is heard"; the reactor
rewrite dropped it and nothing replaced it. What the ladder had accepted was up to **4 hours of
wrong bells after every power loss or reflash** — witnessed on the bench when a RESET at 20:00:56
rang 1 bell and then 2/3/4/5 on the half-hours, every one wrong against true ship's time.

The fix is a separate cold-boot path that **never reaches the strike**: sync → `applySync:`
advances the ratchet from the free-run 0 to true time (silently; skipped bells are not rung) →
paint the *synced* panel → sleep to the next *true* boundary, so the first bell heard is that
boundary's with the right count. Device witness: cold boot 22:15:08 → `ntp: ok, local sod=80108`
→ `sync true 80108 predicted 0 -> advance half 44` → `deep sleep 892 s` (a **partial** interval —
the §3 phase alignment that no cold-boot path had ever exercised) → no bell at boot →
`wake half 45 bells 5` at 22:30:14, correct on the wall clock. The §5 placeholder paint moved
with it: it now runs after the sync, so dashes appear only when NTP genuinely fails.

**The rule the stage now leaves, with midnight no longer special** — it is simply one of six
watch-ends: *strike at every boundary; sync + paint at boot and at every 8 bells; radio on only
for those.* The third clause is **not built** — `main.c:1232` still brings WiFi up on all 48
daily wakes when only 6 need it (measured: 6.5 of every 7.5 awake seconds), filed as tuvm#99.

**Two bench traps from the same evening, both of which produce a plausible-looking wrong result.**
`flash-magtag.sh` calls `run.sh` **without `--conf-extra prj_esp32s2_wifi.conf`**, so it silently
builds a no-WiFi image that boots clean and simply never syncs; and the esp32s2 profile's default
`OEVM_HEAP_SIZE=65536` (`run.sh:143`) will not link against the WiFi stack — it overflows
`dram0_0_seg` by 43,536 B, where this node's working arena is 20480. Confirm a flash by the
panel's heap row and the presence of `wifi: up`, not by the build succeeding. Detail in
`[[tuvm-esp32-port]]` item 19.

**Now witnessed on hardware:** the **cold-boot `ntp FAIL` path** — the accidental no-WiFi build
above drove the real device through `ntp miss` ×3 → `sync failed, stale grows` → a panel reading
`ntp FAIL` / `sync 11d/17h`, which is the `markSyncFailed:` + FAIL-row code the harness had only
ever exercised on host.

**THE FIRST UNATTENDED SHIPPING-CADENCE RUN (22:15 → 08:00, 2026-07-24/25) IS THE RUNG'S REAL
RESULT, AND IT IS A NEGATIVE ONE.** Detail on #94 (five defect write-ups + the ordered queue) and
in §11 of the design doc; the durable points:

**The timebase is measured at last: ~10,700 ppm fast (≈1%)** — three watch-end measurements
(10,500 / 10,768 / 10,271) agreeing within ±3% across nine hours. 227× the navigator's 46 ppm
(4 s/day). Bells arrive up to 155 s early at the 4 h sync cadence. **The stability is the useful
part**: a stable rate error is correctable in software by applying the already-persisted
`driftPpm` when arming the sleep (est. residual ~300 ppm — a watch goes from 155 s to ~4 s),
which is why the DS3231M hardware timebase (#100, ±5 ppm) is an upgrade rather than a rescue.

**Five defects, all in `applySync:`, none caught by a green host suite.** The worst — the hold
branch holds the count but re-anchors the clock, so count and clock never re-converge and the gap
grows one slot per watch end — meant the clock **rang the wrong bell count from 04:00 onward**.
The midnight one — a magnitude compare on an index that wraps 47→0 — read a 66 s lead as a 23½ h
lag and struck 8 bells twice.

**The methodological lesson, and it is the one to carry to other rungs:** all five are the same
shape as the cold-boot defect recorded above — **the prose specifies the behaviour correctly and
the code does something else, in a case no test exercises.** Two of them are contradictions of
comments sitting in the same file. The host harness is green and stayed green throughout; the
gap is specific and now queued (a sync landing *across* the wrap; a hold arming a *longer*
sleep). An unattended run at the real cadence found in nine hours what months of host-green
development did not — for a time-domain appliance, **wall-clock soak is not optional
verification, it is the only verification that addresses the product claim.**

**Not witnessed on hardware:** the `ntp FAIL` *watch-end* specifically (the cold-boot FAIL above
shares `markSyncFailed:` and the panel row, not the watch-end entry); the RC-oscillator drift
number the panel exists to show (needs the shipping cadence's real watch-ends — the first honest
sample is the second sync of a run); button wake and power measurement (out of scope, #83 /
design §10, and now queued behind tuvm#99 since WiFi-every-wake dominates the power profile).

## The bells land on the minute — 2026-07-25, #94 stage 5, the correction batch

The queue set from the overnight run, worked in David's priority order (defect 5, defect 1+2,
defect 3, then disciplining). Suite 227/0, all aux gates green; firmware links for
`esp32s2_saola` with `rtc_slow_seg` at 40 B. Design doc §12 carries the detail.

**Defect 5 — the sleep now belongs to the count, not to the clock.** Having struck `lastHalf`,
the next strike is `lastHalf + 1` and it is due at *that half's* boundary, so the sleep runs to
`((lastHalf + 1) * slotSec) - predictedNow` mod 86400. The same number as before on the
free-running path, and the difference that matters after a hold: the sleep lengthens until the
world reaches the count, which is what the class comment always promised. One formula replaced
the branch-specific spelling, so advance / hold / free-run / cold-boot all phase-align the same
way.

**Defect 1 — the ratchet invariant is modular, and is now spelled that way**, as forward distance
round the 48-slot dial rather than `>`. Defect 2 came free: the midnight case now takes the hold
branch, so the real drift is measured instead of discarded. **The split is asymmetric on purpose**
— hold only if the count leads by at most 3 slots, advance otherwise — because modular arithmetic
cannot tell "3 slots ahead" from "45 slots behind", and the two mistakes are not equally
survivable: advancing wrongly rings one wrong count and self-corrects, while holding wrongly goes
*silent* for the length of the hold. A never-synced board (`staleSec >= 999999`) adopts true time
unconditionally, which is the one real case of a large forward distance — a cold boot at 23:30 is
genuinely 47 slots from the free-run 0.

**Defect 3 — a rate needs a baseline.** Whole-second drift over a 68 s window is ±14,700 ppm of
pure quantisation, which is how `+58823` happened. Below `slotSec * 2` (an hour at the shipping
cadence, ±278 ppm) the sync is reported and the rate skipped. Derived from `slotSec` so a 60 s
demo build can still measure and discipline itself.

**Disciplining, and the cumulative-calibration trap.** `driftPpm` became `calPpm`, a cumulative
calibration applied when *arming* the sleep. The trap is that overwriting it with each fresh
measurement destroys the correction the moment it works: a well-disciplined watch measures ~0,
which as a replacement zeroes the calibration and restores the full 1%. It must **accumulate** the
residual, and `elapsedSec` must accrue the *intended* seconds rather than the commanded ones, or
the loop double-corrects. Seeded from the overnight run's mean (−10,513 ppm) so a reflashed board
is disciplined from its first sleep instead of spending a whole watch learning what is already
known — a starting guess that the loop supersedes within one watch, not a constant.

**Two further defects the fix itself exposed, both invisible to per-decision tests.**

1. **The awake window was accrued but never subtracted.** The wake is strike → work → sleep, so
   boot/WiFi/strike seconds fall *between* this strike and the next. Accruing them keeps the rate
   honest and puts every strike one awake window (~15 s) past its boundary, permanently. It has to
   be both subtracted from the sleep and added to the accrual; the invariant that says so is that
   `predictedNow` after arming must land *exactly* on the boundary of the count about to be
   struck. **The same error sits in the measurement path** and is worse there: `predictedNow` is
   the clock at the strike while `trueSec` was read a window later, so differencing them scores
   WiFi association as oscillator drift — a phantom +1041 ppm per sync that feeds the calibration,
   so a *perfect* clock gets corrected every watch end and the loop walks away from the truth.
   Both are fixed by stamping a clock basis at the instant NTP answers: on a synced wake only the
   work *after* the read is subtracted. Getting that half wrong merely moves the error — it turns
   a 15 s late bell into a 15 s early one.
2. **The correction was truncated, not rounded.** The correction is whole seconds; discarding the
   fraction is itself a rate error — up to 0.9 s per 1800 s slot = 500 ppm, the same order as the
   residual the mechanism exists to remove. It walked the bells ~30 s off across a watch in
   simulation. Rounding leaves ±0.5 s per slot, so ±4 s per watch before the next sync re-anchors,
   which is the accuracy target. Sub-second correction would need fractional state across a deep
   sleep — that is what #100's hardware timebase buys instead.

**The test that closes the harness gap, and what it cost to not have it.** All the per-decision
tests were green while defects 5, 6 and the truncation were live, because each pins one decision
and none of them lets a per-cycle error accumulate into an audible one. The new capstone walks the
real node through twelve wakes against a **simulated 1.07 % fast oscillator** — the rate actually
measured on the board — with a device-sized awake window, across midnight and a watch-end sync,
and asserts every count is the one the hour deserves at the minute it deserves it. It found all
three. Against that model the clock now holds **every bell within 0.7 s of its boundary** with the
calibration converging (−10,513 → −10,673 → −10,743); ablating the seed leaves the first watch
57 s off before the first sync catches it, which is the value of seeding measured in bells. It is
a simulation and says nothing about power, radio or temperature — **the wall-clock soak conclusion
above stands unchanged**; this narrows what the soak has to discover, it does not replace it.

**A VM seam fixed on the way.** `0 millis` on host was `CLOCK_MONOTONIC` since the *machine*
booted, so a node reading it as its own awake window got days — and past ~12.4 days of host uptime
the value no longer fits a 31-bit SMI. The hook's contract is `k_uptime_get()`'s, "ms since boot",
and on a host a process *is* the boot; it now answers process-relative. Every delta-taking caller
is unaffected (a constant offset cancels), and `OEVM_MILLIS_FIXED_MS` lets a host test pose a
specific awake window. Whole suite 227/0 across the change.

**WITNESSED ON THE MAGTAG, 2026-07-25 10:00:02 — four bells at ten o'clock, heard.** Flashed
09:59; cold boot acquired the clock (`ntp: ok, local sod=35955` against a wall clock reading
09:59:15) and took the **adopt** branch — `sync true 35955 predicted 6 -> advance half 19` — the
never-synced rule doing exactly the job it was added for. Then
`sleep: target 39 s cal -10513 awake 6 s`: 36000 − 35955 − 6, the count's own boundary less the
awake window, seeded calibration live. Woke 09:59:59, struck **half 20, four bells**. The same
board four hours earlier rang 5 bells at 09:27:56 where 3 was due at 09:30. The state arithmetic
closes exactly — `elapsed = 0 + 39 + 6 = 45`, `predicted = 36000`, dead on the boundary — which is
the invariant the awake fix was built around, now confirmed off-simulation.

**The one honest gap in that witness: the bell sounds ~2 s into the minute, and it is not the
clock.** The *wake* is on time (09:59:59 for a 10:00:00 boundary); `wifi_bringup()` runs on every
boot ahead of the node code, so association sits between the wake and the strike. That is
**tuvm#99** (lazy WiFi), already filed, and it costs the 2 s and most of the power budget both.

**Still not witnessed:** the **hold** branch, and the first *measured* ppm on this hardware — both
need a watch-end (12:00), and that residual is the number that decides whether disciplining
survives contact with a real oscillator rather than a modelled one. Until then the 0.7 s figure
above remains a simulation result, and an unattended night is still what promotes the rest.

**Witnessed by accident, and worth keeping:** a first flash built without
`--conf-extra prj_esp32s2_wifi.conf` linked no WiFi stack at all, which drove the real device
through the **cold-boot NTP-failure path** end to end — three instant misses, `sync failed, stale
grows`, no bell struck, `ntp FAIL` and `sync --` painted from the sentinel, free-run arming a full
slot. The ladder had that path recorded as host-gated only. **The process lesson is the reusable
half:** the bad image *linked cleanly*, and a clean link says nothing about which Kconfig overlays
went in. The tell was in the numbers — `dram0` well under the known-good **98.68%**, `libc heap
102 kB` against 74 kB, panel `heap 18/64K` against `18/20K`. Check a build against the recorded
good figures BEFORE flashing, not after it misbehaves.

## shipsBellsV3 on the C6 — the timebase closes, the striker opens (2026-07-31/08-01)

**The clock half is done and witnessed.** A DS3231 on the C6's i2c0 is read, corrected over
Thread from a chrony on gw85224-01, and read back at **skew 0** — `ThreadNtpNode.nsl`, commits
0fb9a21/7787f88 (tuvm), 8eefae4 (nsl-tuvm), 3cc505b (nsl-tests). #100 has the transcript.
The part was **10h16m48s slow**, and the number that made that legible is that the skew was
*stable across two runs*: a fixed offset is a bad seed, an accumulating one would have been
drift. `WallClockNode`'s own comment had predicted exactly this failure of a constant seed.

**One prim cost SIX edit sites, not the three the seam's shape suggests** — `prim_ids.h`, a
*second parallel enum* inside `dispatch.c`, `PRIM_NAMES`, the parity guard's `== PRIM_pf_…`
comparison, `platform.c` (case + host leaf + extern), and the `kernel.nsl` accessor without
which nsl cannot reach it at all. Two of those were missed on the first build and **both were
caught at compile time by `prim_names_enum_parity_check`**, whose negative-sized-array trick
and whose comment ("ADVANCE THIS to the new last id") did precisely the job they exist for.
This is the counted-not-computed discipline paying for itself; do not soften it.

**A guard that names the wrong dependency fails in both directions.** `ntp_target_sync` was
gated on `CONFIG_WIFI` when what it needs is SNTP. That left the WROOM-32 *unbuildable* (#104:
`CONFIG_WIFI=y`, no `CONFIG_SNTP`, undefined reference to `sntp_simple`) **and** left the C6 —
which has a perfectly good IPv6 network and no WiFi — holding the `-1` stub. Gating on the real
dependency fixed both at once, verified by rebuilding the WROOM-32 from the issue's own repro.
The generalisable half: **gate on the thing you call, not on the reason you thought you'd have
it.**

**FTD fits, and the reason to choose it is not room.** +8 KB SRAM over MTD on the C6, with the
VM's whole 128 KB arena already inside that figure (`oevm_heap_store` is a static array, not a
runtime malloc). The mesh reports `D=1` and promoted the node to **Router**. The decision is a
role decision that splits along the two product versions: FTD for the mains-powered one that
never moves, MTD/SED for §9's battery version where not keeping the radio up is the point.

**The striker half opened, and it bites.** §12's decision gate was reached — the doorbell chime
sounds like a doorbell and is out, replaced by a telephone gong and a uxcell XRN-0530 (#102).
§11.2's ladder was aimed too high: **10 ms throws solidly, 5 ms fails**, so the minimum clean
pulse is *below* the design's floor. Single shots per rung, so a bracket rather than a
threshold.

**The hazard, and the lesson worth carrying past this rung: a declared GPIO output is OFF only
while something is actively driving it that way.** The `gpioout` seam configured the strike pin
inside `main()` — hundreds of milliseconds after reset, and *not at all* during esptool's
download window, which is **seconds**. The coil was energised throughout. Moving configuration
to `SYS_INIT(PRE_KERNEL_1, 50)` shrinks the firmware-side window and **cannot close it**; only a
pull-down at the transistor makes the off-state a property of the circuit. The design's own 12 V
schematic omits that resistor, and §11's instruction to verify a pull-down refers to a
*different* candidate driver that has one — corrected in the design record at §17.3.

**Still not witnessed:** a **~1 s energisation at boot** (#105) whose cause is unknown — the pin
appears to be actively *driven* high, since a 10 kΩ base pull-down is fitted and measures ~0 V
through reset. The localising test (drain node, coil disconnected, watched across a full flash
cycle) has not been run. Deprioritised behind a piezo buzzer, and **blocking before any coil is
reconnected**.

**Process note.** A suite run leaves the tracked `src/solidified_image.c`/`.stmap` fixture dirty
(#106) — 16,301 lines that a `git add -A` would have committed into the fixture. `run.sh`
defends against this with an EXIT trap; the nsl runners do not. Stage file-by-file until they do.

## The node speaks the new protocol — 2026-08-02, porta space-sync plan 2 (host-green)

**Two independently written implementations of one wire agreed on the first protocol
run.** `PortaBridgeNode.nsl` checks in against a real `porta serve` over UDP loopback:
`obs/health` reaches porta's cell tree, `goal/name` converges to `obs/name`, and every
`tel/log` entry arrives exactly once (`tuvm/tests/porta_sync_host.sh`). No wire fixes
were needed, and the reason is that plan 1 golden-tested the Go codec against *this
VM's own CBOR* rather than against the spec — the second implementation was written to
the bytes, not to the prose. **The rung is NOT closed:** the gate is the C6 over a real
Thread mesh, including a deliberate datagram drop, and that is plan 3.

**The VM change is small and the reason it is small is rung 9.** One monotonic counter
per space, one `last_seq` per channel, three prims (138–140), one nsl verb. Cells were
already a value that outlives the job that wrote it; replication needed only a number
on top of them. Design D5 settled ONE counter per space, not one per path, and the
forcing argument is that a **scalar cursor cannot address a per-path sequence** — a
per-path seq makes a cursor a vector over every name in the space.

**The scan answers cells only, and the asymmetry is the interesting part.** A stream's
`seq` advances on every write like a cell's, but `space_dequeue` kills the event on
delivery, so there is nothing left to send by the time a check-in comes round. Cells are
re-readable and therefore *pollable*; streams are not, and must be **captured as they
are written**. That single fact produced three consequences: the bridge subscribes each
outbound stream path with `whenever:`; captures are stamped with a **bump**
(`spaceStamp`) rather than a read, because two captures with no intervening write would
otherwise share a seq and the gateway drops `seq <= high-water` as a duplicate — *silent
loss, not a repeated delivery*; and a stream entry written while the bridge is down is
gone, which is honest for `tel/**` (lossy by policy) and **not** honest for `sys/panic`
(must-deliver). Filed, not papered over.

**Ascending order is a correctness property, not tidiness.** The bridge sends a prefix
of the scan and moves its cursor to the last entry it sent; any other order strands
whatever it skipped. The same rule makes tail-trimming a batch to 512 B safe — what is
dropped rides the next check-in — and it is why the ack watermark is taken from the
gateway's reply and never from what was sent.

**Where authority is enforced is a design decision, and it belongs on the node.** porta
may write `goal/**` and `dbg/req`. An inbound entry anywhere else is counted and
dropped — never applied, never raised — because an accepted write to `obs/**` would let
the network *forge the node's own report of what it observes*, which is the one thing
the whole convergence view rests on. A node that crashes on a malformed datagram is a
node an attacker can switch off, so nothing on this path raises.

**Two old lessons paid out again, unprompted.** `prim_names_enum_parity_check` caught
nothing this time because the six edit sites were followed deliberately — the previous
rung's cost is what made that cheap. And the `platform timers` freshness trap is
restated on the `PortaBridge` accessor: the cursor, the ack watermark and the pending
queue all live on the instance, so a node that re-fetches the accessor silently starts a
second, empty bridge.

**One process note, and it is embarrassing in the useful way.** The interop gate's first
failure was not a protocol bug — it was `porta get` called without `--db`, reading an
empty default database while the real one filled up correctly two feet away. *A green
tool invocation against the wrong store looks exactly like a dead node.* That is the
same family as "rc=0 is not a physical effect": check what the instrument is pointed at
before debugging the thing it is pointed away from.

## The rung closes — 2026-08-02, porta space-sync plan 3 (the C6 hardware gate, green 5/5)

**The rung is CLOSED.** `tests/porta_sync_c6.sh` ran `PortaBridgeNodeC6.nsl` on a real
ESP32-C6 over a real Thread mesh against `porta serve` on gw85224-01, five consecutive
clean runs, each a cold re-flash exercising cold-boot-at-cursor-0 full replay (porta#29).
Every run: leg 3 `rows=13 distinct=13, 1..13`; leg 4 `rows=26 distinct=26, 1..26` across
4 swallowed acks, `misses 0 → 5`. What the radio added over loopback is not recoverable
from the code, so it is recorded here:

- **The `seq <= hw` dedupe fired, but this 5/5 asserted its effect, not the dedupe
  itself — corrected below.** Leg 4's drop made the node re-push rows porta had already
  applied; `assert_exactly_once` then saw no duplicate in the log, which is consistent
  with the dedupe firing but *equally* consistent with the node never having re-pushed at
  all (`Ingest` in `porta/internal/space/store.go` skipped those rows silently — nothing
  observed the skip). The fix-wave that closed the review (2026-08-02, same day) makes
  the skip count a return value of `Ingest`, logs it from `handlePush` only when it is
  nonzero, and leg 4 now takes a per-run delta over that log line
  (`tests/porta_sync_c6.sh`, `gw_skips`) and fails if it never moves — the same shape as
  `gw_drops` for swallowed acks. That change lands `go test ./...` green
  (`TestReplayedPushLogsSkipCount`, `TestIngestDedupesOnHighWater` now asserts the skip
  count too), **and it has now been re-run on hardware: 5/5 clean with the corrected
  gate, every run logging `gateway logged 4 duplicate-skip line(s) this run`.** So the
  dedupe is witnessed rather than inferred — the node re-pushed rows porta had already
  applied, and porta was observed discarding them. The earlier 5/5 (pre-fix-wave) stands
  only for the properties it actually asserted: no duplicates, no gaps, `misses` moved.
- **A 512 B batch is 5–6 6LoWPAN fragments over a 127 B 802.15.4 PHY**, so real loss is
  expected on a mesh in a way it is not on a LAN. The gate asserts *no duplicates and no
  gaps*, never `misses == 0` — a node's `misses` climbing is normal operation, not a fault.
- **A gateway restart mid-stream is indistinguishable from loss to the node**, and the
  high-water cursor survives it — the same idempotent-by-cursor property that removes the
  need for a retransmit layer (design D1) also absorbs a dead gateway coming back.

**N7 was wrong, and hardware is what proved it.** Plan 2's N7 held that "a miss costs
latency and nothing else, which is the whole reason there is no retransmit layer." On the
first C6 boot, with the gateway's :6969 still owned by v1 and every check-in missing, the
node's unacked `PortaBridge` grew its `pending` queue without bound and OOM-panicked at
tick 48 (~96 s) on a 128 KB heap — a miss also costs memory, and costs it unboundedly. No
loopback run could have found this: loopback's gateway always answers, so `pending` is
trimmed every tick and never grows. The fix bounds `pending` at 64 rows with oldest-first
eviction and a counted, published `#streamDropped` (`nsl-tuvm/lib/kernel.nsl`, `79885aa`;
test `nsl-tests/22_spacesync/05_pending_bound.nsl`, `75228d4`; node publish, `8250831`).
The cost is accepted plainly: the bound trades an OOM death for a telemetry gap, and the
gap is counted rather than silent.

**`sys/panic` is not decided, and this rung does not decide it.** "Drop the oldest" is the
wrong answer for a must-deliver path (RECORDS §4.3); that decision, and the adjacent case
of a stream entry written while the bridge is down, belong together at porta#32.

**The gate found two bugs in itself**, both worth keeping as durable operational
knowledge because they are exactly the kind that produces a silently-wrong green:

- A first run failed `leg 2: obs/build missing` with 45 minutes of uptime on a
  "just-flashed" node — the gateway had started on a fresh db *before* flashing, so the
  still-running pre-flash node repopulated it, and `obs/health` (rewritten every tick)
  satisfied the wait while `obs/build` (written once per boot) never arrived. Fixed by
  flashing first, then wiping the db and starting the gateway, and by waiting on
  `obs/build` rather than `obs/health` — only the once-per-boot cell can prove *this* boot
  pushed.
- The swallowed-ack count in leg 4 was read cumulatively from an appended `serve.log`
  (8 → 16 → 24 across runs), so a run could pass leg 4 on a *previous* run's drops even if
  its own injector never fired. Fixed with a per-run delta and a truncated `serve.log`;
  falsified end-to-end with `DROP_EVERY=100000` (`leg 4: gateway swallowed 0 acks this
  run — the drop never fired`, exit 1) with a genuine ordinary-loss `misses 0 → 1` still
  present, proving natural loss cannot masquerade as an injected drop. Commit `59c475b`.

Five clean runs of the corrected gate are the claim (`docs/spikes/2026-08-02-c6-gate-run1.log`,
`79f1c7f`); an earlier 5/5 against the pre-fix (cumulative) assertion is superseded and not
counted, and one run of the definitive sequence was killed mid-leg by the harness and
re-run clean — a flake was not rounded up to a pass.

**Filed, not built:** the orphaned v1 LAN nodes at 192.168.0.240/.241 (porta#33) — no
migration path from v1's JSON wire to v2's CBOR space-sync wire; and it is worse than "no
gateway" (porta#34) — .241 sends a TFTP read request to :6969 once a second, forever, and
porta rejects each one (decoded: opcode `0001`, filename `commands?id=7c9ebdd8f58c`, mode
`octet`); and `PortaBridge>>awaitReply` consuming the first datagram to arrive, whatever
it is, rather than checking the record tag (tuvm#108) — a narrowing loopback could not
surface. All three filed as GitHub issues out of this close-out.

**A whole-branch review fix-wave, same day, before merge.** Beyond the dedupe-evidence
gap above, the review found two more gaps in the gate script itself. All were applied,
verified on the host (`go test ./...` green, `tests/porta_sync_host.sh` still prints
`PORTA SYNC OK`, `nsl-tests/22_spacesync/05_pending_bound.nsl` still passes) **and then
re-run on hardware, 5/5 clean**:
- `MISSES_BEFORE` was sampled *before* the deliberate gateway restart in leg 4, so the
  restart's own 4–6 s outage (against a 2 s cadence) scored a miss regardless of what the
  injector did — the team's own falsification run showed `misses 0 → 1` with zero acks
  swallowed. Now sampled after `gw_serve_start` returns, excluding the restart's own miss
  from the delta.
- Contiguity alone does not pin the start of the tick series: `porta log --limit N`
  truncates from the OLDEST end, and `PortaBridge`'s `maxPending` eviction (also
  oldest-first) produces the identical shape under a long pre-connection outage — both
  leave a contiguous tail that `assert_exactly_once` would have passed. Both scripts now
  assert `first == 1`; the C6 gate's `--limit` is raised to 5000 for headroom, and it now
  also asserts `obs/health` reports `#streamDropped: 0` during legs 3 and 4 — the node-side
  half of the same property.
- Minors: `nsl-tuvm/lib/kernel.nsl`'s `maxPending` comment now states outright that
  oldest-first eviction manufactures a gap at the head of the series once the bound bites
  (roughly 65 unacked ticks, ~130 s, at the 2 s cadence); `05_pending_bound.nsl` tightened
  to the exact invariant `dropped + pendingSize == n`; `PortaBridgeNodeC6.nsl`'s header
  corrected to four differences (the `dropped` console field) and carries the
  `chip`/`sdk`/`image`/`builtAt` provenance comment now, matching the host file; `porta`'s
  `SetDropAckEvery` documents `s.acks` as fleet-wide; a `dropHarness(t, 0)` test covers the
  never-armed default; `porta_c6_console.py --reset` (unused, and not the flashing
  interface on this board) is deleted; `DO_FLASH=0` now skips the db wipe and the
  `PORTA NODE READY` wait, both of which assumed a fresh boot only a flash guarantees.

## Known stale docs (do not treat as current)

`tuvm/docs/ROADMAP.md` (oevm-era backlog; prime directive still valid, task list superseded),
`fiber_*.md`, `2026-06-29-r7-fibers-design.md` and the derecursify plans (historical record of
the dead pillar), `topics.md`/`optimizations.md` (pre-pivot snapshots). `ECOSYSTEM.md` and
`deployment-model.md` remain valid for the fleet/keeper layers until phase 3 revisits them.
