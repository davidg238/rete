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

## Known stale docs (do not treat as current)

`tuvm/docs/ROADMAP.md` (oevm-era backlog; prime directive still valid, task list superseded),
`fiber_*.md`, `2026-06-29-r7-fibers-design.md` and the derecursify plans (historical record of
the dead pillar), `topics.md`/`optimizations.md` (pre-pivot snapshots). `ECOSYSTEM.md` and
`deployment-model.md` remain valid for the fleet/keeper layers until phase 3 revisits them.
