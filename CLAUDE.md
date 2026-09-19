# rete — project instructions

`rete` is one repository holding both halves of a two-sided embedded fleet system and the
wire contract between them. It was consolidated on 2026-08-02 from four repos — `tuvm`,
`nsl-tuvm`, `nsl-tests` and `porta` — by restructuring `tuvm` in place, which is what
preserved the issue numbers and the `#NN` references the program navigates by
(#79; design `docs/design/2026-08-02-rete-repo-consolidation-design.md`).

## Current focus

- **Next session starts with:** branch `281-nrf-porta-bridge`, **#281 — the nRF node still does not attach.** First concrete action: read #281's last comment and the `nrf4ec0` `obs/build` row on the gateway (it pushed at least once on 08-30, so the question is what differs between that boot and every one since); `7adf` stays on `SoakNode` as the control; `92c2` is the cheap second node once this works. **#273 is CLOSED (2026-09-18) and so is #285** — D1+D2 built (`1315e1e`…`888500f`), promoted (`1aa8eec`), landed on the running system and falsified physically (Task 10, 2026-09-17 23:01 → 2026-09-18 18:19: porta `2488c2e` on gw85224-01 with manifest `844ba659…` persisted, rollback `porta.pre273`; `c6bench` reflashed FROM THE GATEWAY with `PortaBridgeNodeC6` + Thread overlay, `obs/liveness {misses: 16, telCad: 2000}`; unplug → `#silent` in 41 s with `21.1 missed check-ins of 2000 ms, 16 tolerated`; replug → boot 4, `#live` at the next sweep). Close-out is §7 of `docs/design/2026-09-14-liveness-two-cadences-design.md`. **One thing the plan did not predict, and it is D2.1 working:** the first sweep after a manifest-shape restart moves every un-reflashed node to `#unknown` ("declared no cadence") until it is reflashed — `nrf4ec0` reads that today. **The liveness thread's next cheap move is #272**: change `DoSweep` in `Liveness.tla` to require Δwak at the declared rate and re-run `bench.cfg`/`slow.cfg` before any Go; D3 `#stalled` is a plan of its own after that. **Bench truth (2026-09-18):** `c6bench` on gw85224-01 runs `PortaBridgeNodeC6` `g2488c2e` (boot 4, `#live`); the gateway flashes it itself via `~/esptool-venv` and its collector is HAND-STARTED (recipe in the `porta-gateway-operational-state` memory). `c6spare` has both cables, a live collector, and is `TARGET_DEFAULT_BOARD`. `c6-bell` is on an 08-22 image with neither the #269 nor the #274 fix and no manifest pin. **Found-during #273, filed P3: #300** — custos is location-free by the snapshot-file interface except for the `live` binding; porta's snapshot-over-HTTP is the seam for many custos to one porta (noted on #198). **#253's burn list minus #273/#285:** #118/#242, #293, #292, #200 (b), #133/#244/#245/#246; the enumeration doc #253 asks for is still unwritten. **Filed 2026-09-17 from the claude-cabot exchange, P2/P3, not scheduled:** **#297** (P2) `Cbor>>encode:` as a native prim — cabot's 50 Hz tick makes the codec the budget (~8.5 of 20 ms); **#298** profile-first; **#299** an overwrite-in-place C-side cell write; **#296** blob-in-SRAM on the C6. The measured answer to cabot's "10–40× slower than MicroPython" prior: per activation nsl is at or AHEAD of CircuitPython on the same ESP32 (`~/workspaceToit/stic/README.md`, hanoi 1.8× faster); the gap is collection/dictionary/byte-pushing mechanics in `kernel.nsl`, and fixed-point SMI (Q14) is the numerics answer — no float design pending.
- **Filed 2026-09-14/16 as asides, P3, not scheduled:** **#292** federation — a porta is a node (same pull/push one level up, no porta-to-porta API); every transport is a loop around `HandleFrom`; Meshtastic is TIGHTER than 6LoWPAN (233 B, no fragmentation); cellular has no perimeter; Wi‑Fi is the first outside deployment (`docs/design/2026-09-14-porta-federation-and-transports-design.md`). **#291** G5 auth finally has a number — porta's perimeter is the transport's, `ensureNode` creates a node on first mention. **#293** no time rides the push: event time is arrival time TODAY on Thread (every entry in a check-in gets one stamp; a deferred entry is a cadence late) — David: "this seems very wrong"; fix is node-monotonic `at` per entry + envelope `now`, no wall clock.
- **Relayed from the `cabot` session 2026-09-17, AWAITING DAVID here, nothing recorded in rete:** (1) a CLAUDE.md rule for the sibling-repos bullet — *cabot may depend on rete; rete never depends on cabot; prims in rete, apps in cabot*; (2) five candidate P3 issues — a native CBOR encode prim (our own `docs/results/2026-08-09-cbor-encode-shape.md` is the justification), an SPI-slave transport seam in C + a Pi master loop around `HandleFrom` (512 B fixed frames), L1 peripheral prims (LEDC/PCNT/SPI-IMU/ADC) with the 1 kHz loop in C under the VM, a porta-role store that keeps up at 50 Hz × 2 nodes (a different porta — #292's territory), and three robot-free measurements; (3) David answered in the cabot session (their DECISIONS.md M20) that cabot's wire rows go into rete's SINGLE `MANIFEST.cddl` as a GENERATED block — riding #253's landing or the next; confirm here before any of it touches rete's docs, and the manifest editing rules memory wants a line about the generated block when it lands. **Updated 2026-09-17 evening from cabot `1f7ecba` (David: work it through):** the source is the `[wire.<record>]` tables *in* cabot's `body.toml`; the marker carries that file's **content hash** (sha256, 12 hex), not a commit; cabot pins rete by commit (`rete.pin`, at `1aa8eec`) and its gate reads `MANIFEST.cddl` and imports `manifestlib`/`manifest_lint`/`zcbor_env` **at that commit via `git show`**, never this working tree. `python -m tools.gen_manifest_rows ostrich --splice protocol/MANIFEST.cddl` writes the block. **Measured against `1aa8eec` and reverted (comment on #253):** lint clean, `protocol/tools` 21 passed, Go parser ok, hash moves `844ba659…` → `7a76880b…`; the ONE red gate is `TestDescriptorsCoverManifest` — 8 `records.yaml` entries wanted, and the `unit` vocabulary (`gate/descriptors.go:39`) has no physical units (mm, mrad/s, mA, mV, a 0–255 membership). Extending it is rete's; cabot's generator then emits the descriptor rows from the same source. cabot #6 is gated on #295 (comment there); #294 is cabot's Q30; #299 serves cabot #7.
- **Decided 2026-09-14 at the desk, D1+D2 BUILT 2026-09-17 (Tasks 1–9; Task 10 the gateway landing is with David) — do not re-litigate: #273 is ANSWERED.** H1 — the node's declaration is the source of truth and porta contributes no constant to the verdict; H2 rejected on principle (a gateway constant deciding the deadline is the defect #273 names). **The fork rested on a conflation:** `cad` is both the telemetry cadence AND, through #231 D1's kernel base tick, the sole source of the app-liveness deadline `observer_core.h:131` infers. Split into two declared numbers — **`appCad`** (app liveness: `platform liveness app:`/`feed`, **fed from the kernel's event dispatch — amended 2026-09-17**, so it proves ONE sentence, "nsl and the VM are alive", and claims nothing about application progress; judged by the observer, which already owns the hw watchdog, so the dump / `RebootNote` / `#watchdog` are kept; clamped; **never crosses the wire**) and **`telCad`** (published as `Liveness { misses, telCad }`; porta derives `bud = telCad × misses`, which makes #231 D3 structural and deletes the snap in `adoptCad:bud:` plus ten hand-written `cadence * 8`). porta writes `#unknown` naming all three numbers where `telCad × misses < SweepTick` rather than capping silently; the miss ladder is a **reason string, not a new verdict**. `OBS_WEDGE_SLACK` leaves §3.2 and stays in §3.1. **`misses: 8` at EVERY period** — a `× 4` long / `× 8` short split was put and rejected: the multiplier is set by loss probability not period (at `c6-bell`'s measured ~50 %, `misses: 4` false-trips ~6 % of windows vs 0.4 % at 8), and a deep-sleep node's attach failures are CORRELATED, so a split would need *more* slack at long periods, not less. **The lever for faster detection is `p`, not `misses`**: two check-in attempts inside a 10 s wake drop per-wake loss to `p²`, and then `misses: 2` beats today's 8 honestly — that is how the mailbox node earns a 2 h budget. **Also decided (D3): `#stalled` is a FIFTH VERDICT** meaning *arriving but behind the declared cadence* (`Δwak` against `elapsed / telCad` — an age, not a delta). It absorbs **#271** and corrects that issue's premise: `wak` is NOT independent progress evidence, because the check-in runs from the app's own tick reaction (`PortaBridgeNodeC6.nsl:156`, `:186`), so arrival implies `wak` advanced — "did `wak` move" is vacuous, and per-exchange it false-trips on every push leg. What `wak` does answer is the reboot loop (it goes backwards) and the declared *rate*. Verdict not reason because it corrects a FALSE GREEN; needs `liveness-stalled` to prove the state fires before any green is trusted. Record: `docs/design/2026-09-14-liveness-two-cadences-design.md`. Gaps David is comfortable with: a node that advances but is **wrong** (#224's negative clock — it would have kept feeding), a node whose **VM never runs** (#201, the ROM downloader — #202's capacitor), and **#290 filed** (`gw/liveness` is recorded, never actioned: the only readers are porta's tests and `porta get`, so today this makes quiet death *visible*, not *guarded*). **Sequence with #285** — the key rename moves `MANIFEST.cddl`'s SHA-256, which is the `obs/build` pin.
- **Decided 2026-09-11 by phone, recorded, NOT built — do not re-litigate:** **#118/#242** node id = the factory link address (sticker value: C6 eFuse base MAC, nRF FICR `DEVICEADDR`), `OEVM_NODE_ID` deleted, board name → `obs/build`, LAN id only (`docs/design/2026-09-11-node-identity-hardware-rooted-design.md`; sequence with #285 — it moves the manifest hash). **#202/#201** the actuator on-time bound is a series capacitor in the drive, hardware-enforced (shipsBellsV3 design §22.6); still blocked until a coil returns. **#288 REFUTED** — the mesh runs on the gitignored `*_local.conf` dataset, not the stock set; a build without the overlay now REFUSES (`c327616`). **#289 filed**: no onboarding process past the house — the image is the credential. **#224 CLOSED** (`0e2cc2a`): `monotonicMillis` boxes past the SMI band; gate `tests/17_overflow/04_monotonic_millis_band.nsl`; no wire change; carried by each node's next flash.
- **#281, 2026-08-30 — item 2 CLOSED, the node still does not attach.** `layout-nrf52840.json` is captured from a board (`698c8d8`), so **nRF builds Model B** and the ~4.9 GB / ~50 min Model A `cc1` bake — which OOM-killed the workstation again this session — is off this board family. `151643f` fixed the build silently making the WRONG image: under NCS sysbuild a bare `-D` after `--` never reaches the image, so `--node PortaBridgeNodeNrf.nsl` was building **TimerNode with no node id** while printing every correct line. `OEVM_MODEL`/`OEVM_BLOB`/`OEVM_NODE_ID` now travel by env (the `-Dzephyr_OEVM_BLOB=` image-scoped form was tried and does NOT work), and the build asserts model, **blob path**, node id and settings-partition address. `4ec0` runs `PortaBridgeNodeNrf` — boots, ticks, `acked 0`, `thread: attach timed out`, absent from the gateway's child/neighbor tables. Refuted at one board cycle each: persisted NVS dataset (`SETTINGS_RAM` verified applied), credentials, gateway address. **`7adf` stays on `SoakNode`** — 48.6 days unbroken is unrepeatable and it is the control that separates a firmware fault from a mesh fault; `92c2` is the cheap second porta node once this works.
- **#272 is now fully decided and buildable, and nothing in the liveness thread is waiting on David.** #273 gave it its number and D3 gave it its verdict. Its next move is the cheap one its own body names: change `DoSweep` in `Liveness.tla` to require `Δwak` **at the declared rate** (not merely non-zero — the non-zero form passes vacuously, which is the whole correction) and re-run `bench.cfg` / `slow.cfg` before any Go is written. **#271** is absorbed for detection and its direction 1 (publish the delivered cadence) is REFUTED — it would make the declaration true by construction and destroy the verdict's evidence; its direction 2 (the `awaitReply` spin) stays live as the cause fix. **#274 is CLOSED** (`336b12b` `d45136e`). **#279 is CLOSED** and #79's docs rungs with it; **#79 itself stays OPEN** as the go-to-market umbrella.
- **The liveness record, now that #273 D1+D2 ARE BUILT (2026-09-17):** `~/workspace/rete/docs/design/2026-09-14-liveness-two-cadences-design.md` is the decision (D1 `appCad`, D2 `telCad`+`misses`, §4 the falsification plan — **DISCHARGED 2026-09-17**: `liveness-fastpoll` false-rebooted on the pre-fix VM sources (`deadline=800ms`, archive `…20260917T181643`) and not on the new build; `liveness-runonce` formed `deadline=4000ms` with `pending=0` and no timer; `liveness-heartbeat`/`liveness-noradio` stayed green at `deadline=16000ms`, now `appCad=16000ms`; §5 the #285 sequencing — done in Task 5; §6 promotion — done in Task 9: RECORDS §4.2 (`obs/liveness` 43 B, `gw/liveness` row), PROTOCOL-NSL §4/§8, ARCHITECTURE §3.5, CLAIMS two rows). `~/workspace/rete/docs/design/2026-08-31-liveness-timers.html` was the map of the system **as it was before the build**; the constants that moved are struck through in place (§3.2, §7, §8) and its footer says the rest is unverified against the post-build tree until the next full re-read. The four diagrams and the TLA+ model are in `docs/spikes/2026-08-27-liveness-*`, browsable at **http://localhost:8088/** (filter `liveness-`).
- **Done 2026-08-29, do not redo — #274.** `336b12b` moved the base tick upstream of the socket bind (bridge built with `socket: nil`, `liveness:` + `startTicking:` unconditional in `TimerlessNodeC6` and `ShipsBellAlarmNodeC6`, both `bridge == nil` guards deleted, `observer_core.h:92-96` rewritten). `d45136e` added **`liveness-noradio`**, the regression gate: same node, NO `CONF_EXTRA`, GREEN on c6spare (`deadline=16000ms`, 18.03 s of silence, `pass=2 fail=0`). The finding worth more than the fix: correcting `liveness-heartbeat.env`'s header showed why THAT row needs a radio — its claim is that nothing speaks between wedge and reboot, which is **vacuous** on a node that cannot check in at all. Both headers now state what makes their own evidence real.
- **Done 2026-08-29, do not redo:** docs passes 1–5. Pass 5 (#279, `86ad461` `8bbc22c` `adbd04c` `6b3b0ef`) rewrote **`porta/docs/CLI.md`, `nodus/nsl/docs/compiler.md`, `porta/docs/ARCHITECTURE.md`, `porta/docs/GETTING-STARTED.md`**, each with a different way of staying true: CLI.md is **gated** (`porta-cli` walks the binary's `--help`; falsified five ways plus two self-canaries), compiler.md **deleted** its wrong opcode table rather than fixing it (two authorities, no third copy), ARCHITECTURE.md was **restructured** so corrections replace text instead of accreting, and GETTING-STARTED says it is prose and **not** gated. New: `nodus/nsl/docs/README.md`. Also `5419d9a` closed **#277** (review/ bannered with the exact `nodus/tuvm/`→`nodus/oevm/` mapping — 56 of 62 paths rewrite straight across, six do not; the ubsan runner moved to `-fno-sanitize-recover=all`, measured a no-op first) and `3f87494` put the Thread soak into `review/tests/CLAIMS.md`, marked **FIELD EVIDENCE — NOT A GATE**.
- **`gatekeeper` is now `custos`** (`f6edd32`, David's call — Latin, to sit with *porta*). Package `porta/internal/custos`, binary `porta/cmd/custos`, **deployed and serving on gw85224-01:6971** (`/dev/status`, `/health`, `/manifest` all 200). The three dated design records keep the old name in their bodies **and filenames** under a rename banner, so `#NN` citations still resolve.
- **Corrected 2026-08-29, do not re-inherit:** **#118 is NOT overtaken by #186.** `platform nodeId` reads the build-time define `OEVM_NODE_ID` (`platform.c:1508`); #186 settled *slot-vs-board* only, two boards flashed from one image still share one identity, and a node id is a name and not a credential. Both the memory and the status page said otherwise; both are fixed, and #118's own body is stale in the same direction.
- **Bench state:** `c6spare` is **BACK on the workstation with both cables** (2026-08-29 evening) and its collector is live — so `class=disruptive` scenarios run again, and #274's board verification is DONE. Its cabling follows #181's bench note: **CP2102N direct to the motherboard** (`1-3`, off the root hub) and the harness-unused **JTAG on the hub** (`1-1.4.4`), because the CP2102N carries flash AND console (`ESPTOOL_PORT` comes from `RETE_CONSOLE_PORT`) and is the adapter that has failed on this bench before (#178). Through a two-tier hub it had not enumerated at all, silently. `c6bench` on gw85224-01 has ONE cable (the CP2102N — flash AND console) and carries `PortaBridgeNodeC6` `g2488c2e` since 2026-09-17. The three nRFs are labelled `7adf` / `4ec0` / `92c2`; **`4ec0` left the echo soak permanently on 2026-08-30** (it now carries `PortaBridgeNodeNrf`), `7adf` and `92c2` are still echoing and `7adf`'s run is past 48.6 days unbroken. **A dongle is flashed by DFU only** — hold RESET while inserting, then `run.sh --flash`; `west flash --runner nrfutil` cannot do it.
- **Awaiting David:** the three `cabot` items in the bullet above (nothing touched in rete until confirmed); whether **#297** goes before or after #281 on the bench; whether **#300** (remote custos) is ever wanted. Nothing in the liveness thread is waiting on David.
- **Open finds not worked on (weekly review):** #280, #282, #283, #284, #285, **#290** (P3, `found-during #273` — `gw/liveness` is recorded but never actioned; deferred deliberately, with the three conditions that would earn a mechanism written into the body), **#286** (P3, `found-during #281` — the dongle console DISCARDS bytes under burst, and the fix can wedge the VM; the discard turns out to be load-bearing), **#287** (P3, an ASIDE David raised off-shift — cooperating agents over the space; the document landed in `cabot`, not here, and its close-out comment records four claims the `cabot`/`vsa` measurements refuted).
- **Not before 2026-09-01:** David is on NEMT until then.

## Where truth lives (do not duplicate across channels)

| Kind | Home | Never in |
|---|---|---|
| Open work / status / bugs | GitHub issues, bands `P0`–`P4`, pinned tracker **#26** | memory, chat |
| What is top of mind right now | `## Current focus` above + `docs/status/dev-status.html` (live at `http://gw85224-01:6971/dev/status/dev-status.html`; refresh = edit + scp, recipe in `.claude/commands/conclude.md`) | memory |
| Direction | `docs/next-steps-overview.md`, `review/program-sequence.md` | dated design docs |
| Design decisions | `docs/design/` (porta: `porta/docs/design/`) | memory, chat |
| Session-to-session state | git + GitHub + this block; sessions end with `/conclude` | memory |

Memory store = David's working preferences and gotchas ONLY. "Save this for next session" means: update the issue, the doc, or the block above. **Session shape:** one focus issue per session; anything else found is filed P3 `found-during #N` and left for the weekly review.

## Tree map

```
porta/       the orchestrator (Go) — the northbound half
nodus/       the device end
  oevm/      the VM (C) + the Zephyr board ports
  nsl/       the dialect, its compiler and the kernel
  tests/     the nsl suite
protocol/    the wire contract both sides answer to (v1 frozen under protocol/v1/)
review/      David's review-agent tree: the program ladder and the claims ledger
docs/        the VM's own documents, the design records and the execution plans
tools/       repo-wide scripts (docmap.sh)
```

**Being in one repo does not couple the two halves.** porta and the nodes are coupled
**only** over the wire; they never import each other's code, and `protocol/` is the whole
of the contract. A change that makes one side compile against the other is a bug, not a
convenience.

## Standing rules

- **Never create a GitHub repo named `tuvm`.** The repo was renamed `tuvm` → `rete`, and
  GitHub serves the old name as a redirect. Creating anything called `tuvm` again silently
  takes that redirect down, and with it every old issue URL and every clone URL in the
  historical record. There is no warning when this happens.
- **`review/` is David's review-agent tree — never sweep, move, tidy, reorder or delete
  files there.** Path fixes forced by a restructure are the one exception, and only with
  explicit permission (granted 2026-08-02 for #79).
- **Cite absolute paths** when referencing documents in conversation
  (e.g. `~/workspace/rete/review/program-sequence.md`, never a bare `review/`) — the
  program still spans two workspace roots, because `~/workspaceToit/stic` (the cross-VM
  benchmark harness) stays outside `rete` on purpose.
- **Two sibling repos are David's, adjacent, and NOT part of `rete`:**
  `~/workspace/vsa` (does hyperdimensional computing actually work? — Phases 0/1a/2a/2b/2c
  complete, and the answer so far is a series of clean negatives) and `~/workspace/cabot`
  (the wheeled house-pet robot: CAD, floor survey, the E-0..E-10 falsification plan, and
  VSA measurements at house-pet scale). Both are **private, Python, and worked on the
  z170** — a local clone here goes stale silently, so `git fetch` before citing any number
  from either. They are read as evidence, never imported; nothing in `rete` depends on them.
- Cross-repo document map: `review/DOCMAP.md` (regenerate with `tools/docmap.sh`).
- Content search across the program:

  ```
  rg -i "<term>" ~/workspace/rete ~/workspaceToit/stic
  ```

  Add `~/workspaceToit/nodus` to reach the frozen Toit v1 implementation (the GitHub repo
  is now `nodus-v1`; the local directory keeps its old name).

## Design posture (David, 2026-08-04)

How to make decisions here. These override the instinct to be thorough.

- **This is an evolving story, so expect the documents to contradict each other.** When
  two documents of record disagree, that is a finding, not an error to route around —
  resolve it and make them agree. Do not preserve both readings, and do not implement to
  whichever one you happened to read.
- **Resolve for simplicity, robustness and expressiveness, with a dash of elegance.**
  In that order. See also the standing preference for **mechanism minimization**: fewer
  distinct mechanisms and API symmetry beat convenience or low blast radius, and
  call-site churn is an acceptable price.
- **There is ZERO need to maintain backward compatibility, and no "fleet" to protect.**
  Both ends of the wire are reflashed together. A shape change is an edit to the
  documents and a rebuild — never a versioned field, a dual-read, or a deprecation
  window. This is what makes corrections possible instead of accretions: the usual way a
  record grows wrong is that each field was added *beside* what was there rather than
  *instead of* it.
- **Prefer progress, spikes and data over complex decisions made early.** Limit scope and
  get something working. **Do not invent tunables before there is data to choose them
  from** — if a design needs N invented constants before it can run once, ship the
  degenerate case whose cost can be measured, and let the measurement earn the knobs
  back. Record what would earn them, so the deferral is falsifiable rather than vague.
- **Remember the lineage: `st/berry` → `ns/berry` → `ns/oevm` → `nsl/tuvm` (now `rete`, the VM at `nodus/oevm`).** This system
  has been rewritten wholesale four times to get here. Nothing is built to last; it is
  built to teach. Design accordingly.

## Docs convention (2026-07-14)

- **Design specs are decision records** and live in `docs/design/` (moved from
  `docs/superpowers/specs/` on 2026-07-14). When a superpowers skill (e.g. brainstorming)
  says to save a design to `docs/superpowers/specs/`, save it to `docs/design/` instead —
  this instruction overrides the skill's default path. porta's own designs live in
  `porta/docs/design/` under the same rule.
- **Implementation plans** stay in `docs/superpowers/plans/` (porta's in
  `porta/docs/superpowers/plans/`) — they are execution scaffolding, historical once the
  work closes.
- **A document leaves the mainline by move + banner, never by deletion and never by a bare
  move** (2026-08-29, #79). Superseded documents go to the `archive/` of the subsystem that
  owns them (`nodus/oevm/docs/archive/`, `porta/docs/archive/`) with a
  `> **HISTORICAL — archived DATE.**` first line naming what replaced them and what was
  deleted since; then rewrite the in-tree citations to the new path (`review/` excepted —
  list those for David) and rerun `tools/docmap.sh`. Kept rather than deleted so that `#NN`
  issues and commits still resolve by path; the banner is what stops an archived spec being
  read as current, which is how `spec.md` described NLR for seven weeks after Milestone A
  deleted it.
- **Reference docs live with their subsystem; dated records live at the program level**
  (the 2026-08-02 consolidation's rule, executed 2026-08-29): `nodus/oevm/docs/` (start at
  `vm.md`), `nodus/nsl/docs/`, `porta/docs/`, each with a `README.md` map; the root `docs/`
  holds tutorials, status, `next-steps-overview.md`, and the dated `design/`, `results/`,
  `spikes/`, `superpowers/plans/` — which stay under their date because that is where the
  issue citations point. `docs/README.md` is the map of the whole.
- **Promotion rule — part of every close-out.** An issue / rung / milestone is not closed
  until its durable knowledge is promoted to a mainline document of record: the ladder
  (`review/program-sequence.md`), `review/tests/CLAIMS.md`, the owning doc under
  `review/lib/`, or — for porta — `protocol/PROTOCOL-NSL*.md` and
  `porta/docs/ARCHITECTURE.md`. The dated design doc stays behind as the historical "why".
  Every plan's close-out task must name which mainline doc absorbed the knowledge.

## Gates

```
cd nodus/nsl && python3 tools/run_tests_oevm.py     # 280 passed, 0 failed (2026-09-17)
cd porta      && go test ./...
cd nodus/oevm && bash tests/porta_sync_host.sh      # PORTA SYNC OK
```

The nsl suite prints `Passed: N / Failed: 0` **and** a line of aux gates — read the aux
line, because it can be red while the count reads zero failures.
**`c-units` leads that aux line and covers ALL 43 C unit tests** (#275, 2026-08-29):
`nodus/oevm/tools/run_c_tests.sh` is DELETED — it was a second aggregate runner nothing
invoked, so it sat red (four aborts) without anyone learning it, which is #261's shape.
Every C test is now registered individually in `nodus/nsl/tools/run_tests_oevm.py`'s
`C_UNIT_TESTS` table, which also owns the `ENG_A`/`ENG_B` link sets and the
`OEVM_BUILD=asan|ubsan` knob. Adding a C test is one row in one file; a test registered in
two places is one that gets forgotten in one of them. The registry runs every test and
reports every failure rather than stopping at the first.
**`tutorial-host`, `tutorial-porta` and `tutorial-board` close that aux line** (#79, 2026-08-29):
`docs/tutorials/gate.sh` extracts every `bash gate` block from a page and runs them in a
fresh directory, canary first. A tutorial is the one document nobody who knows the system
re-reads, so it is executed instead — edit the page and the gate follows; a new tutorial is
one more call to `run_tutorial_gate(page, label)`. **`tutorial-porta` (03-porta.md, ~12 s)
runs both halves of the repo against each other**: a real `porta serve` and a real
`PortaBridgeNode` image on loopback. It therefore binds **udp 16969/16970** — the node's
`gwPort` is a source constant — so it must never run beside
`nodus/oevm/tests/porta_sync_host.sh`, which claims the same pair (that gate is standalone,
so the suite does not collide; two concurrent suites would). It is the ONE gate here that
needs a Go toolchain, and `needs=("go",)` makes an absent `go` a named SKIP rather than a
hard failure, because a missing Go must not couple the nsl suite to the gateway half.
**`tutorial-board` is the odd one and deliberately so**: `docs/tutorials/02-board.md` is PROSE
— its walkthrough needs a devkit, a toolchain and a mesh, so it carries no `bash gate` blocks
and its banner says why. The gate checks the ONE claim on it that is a fact about this repo
rather than about a board: §7's scenario NAMES against `nodus/oevm/tests/target/*.env`. Names
only — a missing scenario is a bug, a stale description is a judgement call, and a gate that
scores prose is one that gets disabled. An unparseable table yields the empty set and FAILS,
because vacancy must not read as agreement.
**`porta-cli` closes it after them** (#279, 2026-08-29): `porta/docs/cli-gate.sh` builds
porta, walks its `--help` tree, and fails if `porta/docs/CLI.md` names a command or flag the
binary does not have, or omits one it does. The tutorial answer — execute the page — is not
available to a REFERENCE, whose claims nobody runs; cobra's own account of itself is. Names
only, both directions, transcripts included; it falsifies itself on an empty page and on an
invented flag before it clears anything. Needs Go, so `needs`-style SKIP applies for
tutorial-porta's reason.
**`footprint: SKIP` is NO LONGER expected, and it never should have been** (#177, fixed
2026-08-22): arm-zephyr-eabi-gcc ships inside `~/zephyr-sdk-0.17.4` and always did. What was
broken was the gate's own un-SKIP instruction, which named a path the #79 consolidation
deleted — so the SKIP was unfollowable, and this file recorded it as normal. `footprint.sh`
now finds the SDK itself. **A SKIP line here is a finding now, not a fixture.** Never run
two nsl suites concurrently; they share `/tmp` paths. **The count above drifts upward as
tests land — treat a mismatch as staleness in this file, never as a missing test.** The
suite also prints `target gates: NOT RUN BY THIS SUITE`: the posix timer backend bypasses
the pulse mailbox, so coalescing, shedding and stale handles need `nodus/oevm/tests/target/run.sh`
and a board.

**What `tests/target/run.sh` actually runs — fourteen scenarios, and a full pass is ~70–100 min
of board time** (`liveness-heartbeat` alone budgets a 300 s timeout plus 180 s of observation;
`cad-rearm` was added 2026-08-26 for #240.2, `porta-refuse` the same day for #232,
`porta-silent` on 2026-08-27 for #269, `liveness-noradio` on 2026-08-29 for #274, and
`liveness-fastpoll` + `liveness-runonce` on 2026-09-17 for #273 — and
**both porta-* scenarios need their injector porta started by hand**, so a full sweep is not
unattended; see their envs). The runner globs
`*.env`, so a new manifest joins the sweep the moment it lands. Budget the time rather than
discovering it. `CLASS=disruptive` means the scenario
deliberately wedges, burns or reboots the node: **a disposable board only** (`c6spare`, which
is `TARGET_DEFAULT_BOARD` — resolve boards through the collector env files, never a table).

**A scenario whose node opens a socket needs `CONF_EXTRA` naming the radio, and only two nodes
do.** `prj.conf` alone has `NETWORKING/IPV6/UDP/SOCKETS` but **no L2**, so a C6 built without
`prj_esp32c6_thread.conf` has no interface and `net udpBindPort:` answers nil. The wedge probes
never notice — `WedgeProbeC6`, `SlowTimerNodeC6`, `PulseCoalesceProbeC6` and `StaleHandleProbeC6`
arm `timers every:` directly and open nothing. `PortaBridgeNodeC6` and `TimerlessNodeC6` bind. **The bind is
no longer load-bearing for `TimerlessNodeC6`** (#274, 2026-08-29): the bridge is now constructed
with `socket: nil` and `startTicking:` runs unconditionally, so a failed bind costs the node its
reporting and not its base tick, its wedge or its deadline. `liveness-noradio` is the gate that
holds that — the same node with no `CONF_EXTRA` at all, verified on a board 2026-08-29
(`deadline=16000ms`, 18.03 s of silence, `pass=2 fail=0`). What a radio still buys is EVIDENCE:
`liveness-heartbeat` keeps its `CONF_EXTRA` because its claim is that nothing speaks between the
wedge and the reboot, and on a node that cannot check in at all that claim is vacuous. The "no
`CONF_EXTRA`" convention is about `wedge-note`'s synthetic-trigger Kconfig split and **was never
about the radio** — conflating the two cost a board cycle on 2026-08-22 (#236, and
`zephyr/run.sh`'s #165 comment records the same shape from before).

| scenario | class | what it gates |
|---|---|---|
| `bridge-health` | observe | the node half of the porta gate — obs acked, `goal/name` converged, nothing evicted (#streamDropped 0) |
| `cad-rearm` | observe | an adopted cadence is a KEPT cadence (#240.2) — the re-arm from inside the old tick's own reaction kills the old registration over the real pulse mailbox, keeps ONE registration, and holds the new period. No radio: `PortaBridge socket:nil` |
| `bridge-forensics` | observe | the same workload under the forensics overlay (#222) |
| `porta-refuse` | disruptive | **a refused node survives past the 157 s death record** (#232, gating #199) — a C6 against a second porta instance answering `#err` to everything (`--test-refuse-all`, gw85224-01:16971, started by hand per the env), watched 240 s past the TAPs. `ref` climbing is the PASS; silence is the FAIL. The host cannot answer this: it never reproduced the death |
| `porta-silent` | disruptive | **a node whose UPLINK is gone survives it** (#269) — porta-refuse's mirror, and the gate porta-refuse could not be. A C6 against a second porta instance that DROPS every datagram and answers nothing (`--test-drop-all`, gw85224-01:16973, started by hand per the env — and it must be `./porta-269`, since the long-running `./porta` there predates the flag), watched 300 s past the TAPs. **`misses` climbing with the node ALIVE is the PASS**; the node going quiet is the FAIL — and TAP 1 is a conjunction (`misses >= 30` AND `ref = 0`), so a run that reaches #232's refusing injector by mistake fails loudly instead of scoring. **This gate caught something it was not built for**: it reproduced a node wedging at 111.7 s under a silent uplink, and the node was HEALTHY — `observer.c` judged the pulse mailbox's depth at a sampling instant, which aliases against a 4.007 s tick sampled every 1001 ms, so the detector was rebooting a node that was still draining and still printing ticks. Fixed by judging PROGRESS (`pulse_drained()`) not depth; GREEN since, 357 s under the condition that had killed it, with `wedge-detect` still firing. The archive was then swept (#270, 2026-08-27) and **the false trips are confined to that one session**: of the 58 archived `deadline=16000ms` events, 49 are REAL wedges on 08-19/20 — boot, ticks 1-6, then 18.2 s of silence against a 2 s cadence, and 35 of those also read `reactor=NOT-ADVANCING` with vm_sp frozen, which is the #222 prim-cache spin and does not depend on depth at all — 2 are `liveness-heartbeat`'s deliberate ones (#231 D1), and 7 are this session's. No attribution was overturned |
| `pulse-coalesce` | disruptive | the coalescer does not destroy logical timer events (#151) |
| `stale-handle` | disruptive | a timer event survives a later unrelated registration (#151) |
| `wedge-note` | disruptive | the `RebootNote` survives a watchdog reset (#215, row 1) |
| `wedge-detect` | disruptive | the detector fires on a real wedge, dumps the condition, reboots, reports `#watchdog` (#215, row 3). **Since #273 (re-verified 2026-09-17): the probe has no bridge, so it declares `appLiveness: 8000` itself and the dump reads `deadline=8000ms appCad=8000ms cond=app-stale`** |
| `wedge-quiet` | disruptive | **no false trip on a legitimately slow node** — one 10-minute timer, and the 600 s observation is why this run is long (#215, row 5) |
| `liveness-heartbeat` | disruptive | **the detector can see a node with NO app timer** — the kernel owns the base tick, so the deadline exists and nothing speaks between the wedge and the reboot (#231 D1). Needs the Thread `CONF_EXTRA`: its node binds. **GREEN ON A BOARD 2026-08-22** — `deadline=16000ms` (the kernel's 2000 ms tick × slack 8, the only timer in the image), 18.03 s of total console silence between the wedge and the reset, `pass=2 fail=0` on boot 2. **Re-verified 2026-09-17 under #273, unchanged: `deadline=16000ms appCad=16000ms`** — the bridge's declared default, no longer an inference from the shortest timer |
| `liveness-noradio` | disruptive | **#274's regression gate: a deadline exists even when the bind fails.** The same node as above with NO `CONF_EXTRA` — the exact condition that gave 300 s of total silence on 2026-08-22. GREEN ON A BOARD 2026-08-29: bind fails, `timerless tick` climbs at 2 s (there were none before the fix), wedge at tick 15, `deadline=16000ms` from the kernel base tick alone, 18.03 s of silence, reboot, `pass=2 fail=0`, then 106 ticks with no second wedge. The red it is shaped to catch is SILENCE, which is what made the defect invisible. **Re-verified 2026-09-17 under #273: `deadline=16000ms appCad=16000ms cond=app-stale`** — the number is unchanged, it is now the bridge's declared default (cad × 8) rather than an inference |
| `liveness-fastpoll` | disruptive | **#273 D1's reproduce-first gate: a 100 ms timer beside a 3 s blocking reaction every 4 s.** RED on the pre-#273 build (`deadline=800ms cond=pulse-undrained`, a false reboot 2 s into the first slow tick — archived 2026-09-17 `…20260917T181643`), GREEN the same day with the declared `appCad 6000`: 50 slow ticks, zero WEDGE lines. A 1.5 s block did NOT reproduce the red — the old rule sampled at 1 s and needed two in-block samples — so the block is 3 s |
| `liveness-runonce` | disruptive | **a node with NO repeating timer is judged at its declared number** (#273 D1). `deadline=4000ms appCad=4000ms pending=0` from the declaration alone (GREEN 2026-09-17); the vacuous outcome on the old rule is TIMEOUT, so this row asserts a deadline was FORMED |

A scenario passing its TAPs is **not** the verdict: every one carries an observation window
that watches past it, because a gate that scores at tick 2 against a node that dies at tick 6
is a false green (#216).

---

# porta — the gateway half

`porta` (Latin "gate/door") is the **gateway project**. It is the northbound half of a
two-sided system whose unit of coupling is a **space**: every node owns a tree of paths,
and porta holds a mirror of that tree. Operators write intent under `goal/**`; the node
writes what it sees under `obs/**`, `tel/**` and `sys/**`. Convergence is not a mechanism —
`goal/X` versus `obs/X` is a tree diff, so the desired-vs-observed view and the self-heal
loop are the same query.

The two sides exchange the mirror over **plain UDP carrying CBOR**, in two node-initiated
idempotent messages: a `pull` that fetches writes porta has for the node, and a `push` that
delivers the node's entries. Both are bounded by a 512 B batch and a 256 B value, which are
radio constraints and not preferences (6LoWPAN fragments above ~102 B and a datagram
survives only if every fragment does).

There are **no verbs on the wire and none in the store.** The operator vocabulary is CLI
sugar over path writes and tree reads: `porta put` is a write to a `goal/` path, `porta get`
is a tree read, `porta log` is a query over the append-only event log.

> **The porta auto-memory namespace (`~/.claude/projects/-home-david-workspace-porta/memory/`)
> is v1-era and has NOT been reviewed against the rewrite.** It holds memories written
> against the command queue, TFTP delivery and the report schema — all deleted. Treat
> anything from it as a claim about a system that no longer exists until you verify it
> against the code. The memories this work depends on live in the **tuvm** namespace
> (`nsl-cbor-wire-shapes`, `rewrite-may-not-delete-a-capability`), which is the namespace
> `~/workspace/rete` sessions get.

## Status

v1 was deleted in one commit (design D7: demolition precedes construction, so no decision
is made with v1 still in the room). The last v1 tree is tagged **`porta-v1-final`**.

What v1 was — a command vocabulary and report schema, a command queue, container image
delivery over TFTP, and telemetry ingest — is gone, along with nine per-feature sqlite
tables. The **capability audit in
`porta/docs/design/2026-08-01-porta-space-sync-rewrite-design.md` §4.2 gives every v1 verb,
route and MCP tool a fate**, because a rewrite may delete a mechanism but may not silently
delete a capability. Three of v1's fourteen `nodes` columns turned out to have no successor
until they were enumerated; all three are closed.

The core is built and green: `cval`, `space` and `wire` all landed
(`porta/docs/superpowers/plans/2026-08-01-porta-space-sync-core.md`, DONE), the node side
landed (`docs/superpowers/plans/2026-08-02-porta-space-sync-node.md`, DONE), and the C6
hardware gate closed the rung 5/5. See `docs/next-steps-overview.md` Part 2 for what is
next.

## Layout

- `porta/cmd/porta/` + `porta/internal/portacli/` — the cobra CLI (`serve`, `get`, `put`,
  `log`). The operator verbs are sugar over path writes and tree reads.
- `porta/internal/cval/` — the nsl message algebra's CBOR codec, **hand-rolled**. Not
  layered on a CBOR library, and this is not a preference: the algebra's map keys are
  themselves tagged values (Symbols, tag 39), a Record's *name* is a Symbol rather than a
  text string, and decoding must be **closed** so a mis-versioned peer cannot smuggle a
  value through. The node's `Cbor` class in `nodus/nsl/lib/kernel.nsl` is the authority,
  not the RFC; the encoder is golden-tested against the node's own output.
- `porta/internal/space/` — the store: `nodes`, `cells` (latest value per path), `events`
  (append-only, both directions, with `actor`/`reason` provenance), `exchanges` (one row
  per request — two per check-in, a `pull` and a `push`. **There is no `leg` column**: it
  was dropped with the cadence inference rete#231 D4 deleted, which was its only reader
  (and which had to count pulls alone or infer a period half the node's real one, #217
  C1). What the table is for now — the replay index and the `last_seen` heartbeat — wants
  every request rather than one leg, and **both** legs stamp `nodes.last_seen_ms` via
  `RecordExchange`). Four tables where v1 had nine, because the unit of
  extension is now a path: a new feature adds rows, not tables.
- `porta/internal/wire/` — pull/push framing as a pure `Handle([]byte) []byte`, wrapped by
  a thin UDP loop.
- `porta/internal/httpsrv/`, `porta/internal/serverstat/` — survived the demolition
  untouched. `httpsrv` now depends on a one-method `NodeCounter` interface rather than a
  concrete store, which is how it outlived v1's store and should outlive the next one.
- `porta/devsdk/` — the public dev-SDK (`exec`, `provision`; `apiclient` was deleted with
  v1's HTTP surface). All language/hardware toolchain lives on the node side, which imports
  `devsdk`; porta itself stays language-neutral.
- `porta/deploy/` — the deb build and systemd unit.

## Documents of record

- `protocol/PROTOCOL-NSL.md` — the current wire protocol: the space model, path namespaces,
  cells vs streams, the exchange. **§§1–4 are the conformance target.**
- `protocol/PROTOCOL-NSL-RECORDS.md` — the record catalogue: what shape rides at each pinned
  path, plus the size law (§2.1/§2.2).
- `protocol/MANIFEST.cddl` — **the machine-normative half of the catalogue** (#243,
  2026-08-25): path → kind + class + dir + shape, enforced by the nsl suite's
  `nsl-tools-py` aux gate (formerly `manifest-py`), porta's store (all three write doors), and the seam gate.
  Editing it has hard zcbor rules — see the `manifest-cddl-editing-rules` memory or the
  file's own header.
- `porta/docs/design/2026-08-01-porta-space-sync-rewrite-design.md` — the rewrite's decision
  record (D1–D8), the store schema, the wire shapes, and the §4.2 capability audit.
- `porta/docs/ARCHITECTURE.md` — system architecture. **Rewritten for v2 2026-08-29**
  (#279) and current: the three planes, both UDP transports, modes and app lifecycle as
  paths, the manifest gate, the store's two open doors, one-repo topology. §7 records the
  Toit lineage as history.
- `protocol/v1/PROTOCOL.md` — **v1. Deleted, historical only.** It describes the command
  verbs, JSON args and CRC32 payload delivery that no longer exist. Do not implement from
  it; read it only to understand what `porta-v1-final` did, and it is what the frozen Toit
  nodes still speak.
- `porta/docs/CLI.md` — **the v2 command reference and current** (rewritten 2026-08-29,
  #279). Gated: `porta/docs/cli-gate.sh` walks the binary's own `--help` and fails if a
  command or flag named on the page does not exist, or if one that exists is unnamed.
  Names only — it does not score the prose, and the transcripts on it are dated evidence.
- `porta/docs/GETTING-STARTED.md` — **rewritten end to end 2026-08-29** (#279) and
  current: install, confirm, point a node at it, first operations, and `custos`. Prose
  and NOT gated — it says so itself and defers to `CLI.md` and tutorial 3 on any
  disagreement.
- `porta/docs/DEVSDK.md` — **stale in parts**, written against v1's verb set. `exec` and
  `provision` survive; `apiclient` went with v1's HTTP surface, and `provision`'s
  `firmware.config["porta"]` shape is Toit-era.

## Key decisions & constraints (don't re-litigate)

- **Verbs live only at the CLI.** The store and the wire have none (D2). Adding a feature
  adds a path, not a table, a verb or an endpoint.
- **All values on the wire and in the store are raw deterministic CBOR** (D4.1); decode only
  for display. All timestamps are **milliseconds** (D4.2). All paths are **node-relative**
  (D4.5) — the `node/<id>/**` prefix is a view, never storage, so a captured session can be
  re-pointed at a different node.
- **One monotonic seq per space per side** (D5). Kind is a property of the leaf, set by the
  first write (D6); `KindStream = 1`, `KindCell = 2`, matching `SPACE_KIND_*` in
  `nodus/oevm/src/space.h` — shared with the VM, do not renumber.
- **The integer band is ±(2^64−1)**, which exceeds `int64`. Represent out-of-range values
  exactly; never silently clamp.
- **There is no tombstone.** Stopped state is written explicitly as a value; genuine removal
  is an action on a `do/` stream.
- **A rewrite may delete a mechanism, but it may not silently delete a capability** (D8).
  Anything the old system could answer that the new one cannot is a regression wearing the
  costume of a simplification. This is why demolition was preceded by an audit.
- **Replay-from-0 is designed behaviour, and since 2026-08-26 it is safe per kind.** Cold
  boot = cursor 0 replays cells as latest-value (idempotent); streams are guarded — porta
  never re-serves a row the node has acked as applied (`nodes.cursor_hw`, bounded by
  `served_hw`), so `goal/do/*` cannot re-execute across reboots. See
  `porta/docs/design/2026-08-26-space-sync-durability-trio-design.md` and
  `PROTOCOL-NSL.md` §3.
- Bulk transfer (images, profiler blobs) is **deliberately off the space** and deferred to a
  CoAP blockwise plane; auth (G5) is out of this rung.
- Go 1.26, sqlite via `github.com/mattn/go-sqlite3`, cobra. Copyright header
  `// Copyright (c) 2026 Ekorau LLC` on every new Go file. `gofmt -l .` must print nothing
  before every commit.
