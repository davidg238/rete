# nsl-tuvm — program sequence (the overarching order of work)

**Status:** drafted 2026-07-04 (mid-Milestone-B). This is the ONE document that sequences
the whole program. It is an *index with ordering*: every line points at the document that
owns the detail — decisions live in `nsl-tuvm/docs/superpowers/specs/`, execution state in
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

Owner doc: `nsl-tuvm/docs/superpowers/specs/2026-07-03-nsl-tuvm-first-step-design.md` §4
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
   `nsl-tuvm/docs/superpowers/specs|plans/2026-07-10-rung5-udp-transport-l1.*` (Layer 1) and
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
     (`nsl-tuvm/docs/superpowers/specs/2026-07-11-rung8-ontarget-timer-design.md`): a real Zephyr
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

0. **Rung 9 — cells.** `nsl-tuvm/docs/superpowers/specs|plans/2026-07-13-rung9-cells.*`. A value
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

## Known stale docs (do not treat as current)

`tuvm/docs/ROADMAP.md` (oevm-era backlog; prime directive still valid, task list superseded),
`fiber_*.md`, `2026-06-29-r7-fibers-design.md` and the derecursify plans (historical record of
the dead pillar), `topics.md`/`optimizations.md` (pre-pivot snapshots). `ECOSYSTEM.md` and
`deployment-model.md` remain valid for the fleet/keeper layers until phase 3 revisits them.
