# #232 Board Session Implementation Plan — porta-refuse scenario, #114 must-deliver, two riders

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement
> this plan task-by-task, inline in the controlling session. Subagents are ruled out for this
> plan: it holds a board, long builds and a live-gateway deploy, all of which the
> `feedback-subagent-long-builds` and `subagents-ignore-the-worktree` memories forbid
> delegating. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close #232 (and with it #199) by landing the `porta-refuse` target scenario, and
land the node-side remainder of the durability trio: #114's must-deliver re-offer plus the
two riders from the 2026-08-26 durability review.

**Architecture:** Four small changes meet at one board run. (1) porta grows a supported
refuse-all fault-injection knob beside `SetDropAckEvery`; (2) the kernel's PortaBridge takes
over sys/panic publication as a re-offer-until-acked loop driven by porta's ACK covering the
entry's seq (design D4), deleting the app-side publish-then-ack-on-hope copies; (3) `obs/health`
gains `rej` (inbound counted drops) beside `ref`, which is rider 2's "surface the drop";
(4) a new disruptive target scenario points a C6 at a refusing porta and watches past the
157 s death record. The live gateway is upgraded in the same session — the pending
durability-trio binary and the new manifest ship together at this deliberate moment.

**Tech Stack:** Go 1.26 (porta), nsl (kernel + probe node), C (one host-only test seam in
`platform.c`), bash env manifest (target harness), zcbor-validated CDDL (manifest).

**Spec:** `porta/docs/design/2026-08-26-space-sync-durability-trio-design.md` (D1 riders, D4)
+ issue #232's body (the scenario spec) + issue #114's DECIDED comment (2026-08-26).

## Global Constraints

- Copyright header `// Copyright (c) 2026 Ekorau LLC` on every new Go file; `gofmt -l .`
  prints nothing before every commit.
- nsl tests: inline `(* expect: *)` at the top, class with `main:platform args:`, run via
  Path B only (`run-one.py <file> B` — the bare form bakes Path A over the tracked fixture).
- Never run two nsl suites concurrently (shared /tmp).
- `MANIFEST.cddl` edits follow the `manifest-cddl-editing-rules` memory (zcbor 0.9.1: named
  sym keys only, KEY ORDER IS WIRE ORDER, no quantified tagged list element) — read that
  memory in full before Task 5's edit.
- The gateway memory `porta-gateway-operational-state` must be read in full before Task 8
  (the pkill self-kill has struck five times; `porta get` without `--db` creates an empty
  store).
- Boards are resolved through collector env files, never a table. CLASS=disruptive runs on
  the disposable board only (`c6spare`, the default).
- All snippets below are UNRUN until their task executes (plan-embedded code is unrun code);
  the executing task must let the compiler/test correct them, not trust them.

---

### Task 1: porta — `SetRefuseAll` + `--test-refuse-all`

**Files:**
- Modify: `porta/internal/wire/server.go` (fields + setter + refusal in `HandleFrom`)
- Modify: `porta/internal/portacli/serve.go` (flag beside the existing `--test-drop-ack`)
- Test: `porta/internal/wire/refuse_test.go` (new)

**Interfaces:**
- Produces: `(*wire.Server).SetRefuseAll(bool)`; CLI flag `--test-refuse-all` on `porta serve`.
- Consumed by: Task 9 (the refusing porta instance on gw85224-01:16971).

- [x] **Step 1: Write the failing test.** Shape it on `drop_test.go`'s use of the store +
  `Handle`. Three properties: (a) with the knob on, a well-formed pull AND a well-formed push
  each get an `#err` reply (decode it: tag `err`, `msg` mentions refuse), and the store shows
  no ingested rows and no exchange row — the refusal happens BEFORE `handle`, because #199's
  witnessed refusal was a reject-at-the-door, not an ingest-then-nack; (b) with the knob off
  (zero value), the same frames are answered normally; (c) the refusal never answers silence
  — the reply is non-nil (a node that gets no reply cannot distinguish rejection from loss).

- [x] **Step 2: Run it.** `cd porta && go test ./internal/wire/ -run Refuse -v` — expect FAIL
  (SetRefuseAll undefined).

- [x] **Step 3: Implement.** In `server.go`: a `refuseAll bool` field beside `dropAckEvery`,
  same TEST-ONLY comment posture; `func (s *Server) SetRefuseAll(on bool) { s.refuseAll = on }`;
  at the top of `HandleFrom`:

  ```go
  if s.refuseAll {
      s.logf("porta: TEST-REFUSE: refusing datagram from %s (%d bytes)", peer, len(req))
      return encodeErr("refused by TEST-REFUSE (--test-refuse-all)")
  }
  ```

  In `serve.go`, mirror the `--test-drop-ack` flag wiring exactly (find it at
  `portacli/serve.go:129`) for `--test-refuse-all`.

- [x] **Step 4: Run the test and the package.** `go test ./...` from `porta/` — all green,
  `gofmt -l .` prints nothing.

- [x] **Step 5: Commit.** `feat(#232): porta grows --test-refuse-all — the supported form of the spike's env-gated early return`

---

### Task 2: Rider 1 — the PortaBridge header stops lying about the cursor

**Files:**
- Modify: `nodus/nsl/lib/kernel.nsl:1611-1614` (comment only)

- [x] **Step 1: Edit the two stale claims.** The `cursor` line currently reads "the last seq
  this node has APPLIED. RAM only in this slice; cold boot = cursor 0 = full replay is the
  DESIGNED behaviour…". Rewrite to carry the 2026-08-26 design's three facts: RAM is a
  DECISION (D3 — persistence would buy a stranding mode, and the porta-side guard is what
  makes cursor 0 safe); the receipt attests DELIVERED-AND-OFFERED, not executed (counted
  drops and shed entries advance past entries that never ran — see `rej` in obs/health);
  replay-from-0 is per kind (cells latest-value, streams guarded by porta's `cursor_hw`).
  Cite `porta/docs/design/2026-08-26-space-sync-durability-trio-design.md` D1/D3.

- [x] **Step 2: Verify the suite still passes** (comment-only change, but the suite is
  cheap insurance against a stray delimiter): `cd nodus/nsl && python3 tools/run_tests_oevm.py`
  — count matches, aux line green.

- [x] **Step 3: Commit.** `docs(#232): PortaBridge header — cursor is RAM by decision (D3), receipt is delivered-and-offered`

---

### Task 3: Host seam — a plantable reboot note (`OEVM_TEST_PANIC_NOTE`)

**Files:**
- Modify: `porta`-side none; `nodus/oevm/src/platform.c` (host block around line 516-553)
- Consumed by: Task 4's host test.

The host has `reboot_note_write()` but no way for a test to plant a note — the writers are
fatal handlers. Task 4's re-offer test needs a note at boot.

- [x] **Step 1: Add the env seam.** In the HOST `reboot_note_init()` (platform.c:524), after
  the validity check, follow the `OEVM_NO_DISPLAY` shape (platform.c:282):

  ```c
  /* TEST SEAM (rete#232, the OEVM_NO_DISPLAY shape): plant a note so the
     re-offer loop is testable on the host, where no fatal handler ever runs.
     Value = RN_REASON_* code. Host-only: the target's init is in zephyr/. */
  const char *tp = getenv("OEVM_TEST_PANIC_NOTE");
  if (tp != NULL) reboot_note_write((uint32_t)atoi(tp), 0, 0, 0);
  ```

  Check `reboot_note_write`'s real arity first (platform.c:532 shows 4+ args — mirror it).

- [x] **Step 2: Verify per-test env delivery exists.** Read how `run_tests_oevm.py` passes a
  per-test `env:` (the `nsl-test-env-seam` memory: per-test env in the runner; a global
  export leaks into every test). Confirm the mechanism's exact spelling before Task 4 uses it.

- [x] **Step 3: Build the host VM and confirm a planted note surfaces.** Quick probe: run any
  existing test with `OEVM_TEST_PANIC_NOTE=1` set and a one-off snippet calling
  `platform panic` — expect a `#Panic` record with reason `#watchdog`. (Or fold this proof
  into Task 4's test and skip the probe.)

- [x] **Step 4: Commit** (may ride Task 4's commit if the proof was folded in).

---

### Task 4: #114 — the bridge re-offers sys/panic until porta's ACK covers it

**Files:**
- Modify: `nodus/nsl/lib/kernel.nsl` (PortaBridge: one slot, one method, one checkIn line)
- Modify: `nodus/oevm/zephyr/PortaBridgeNodeC6.nsl` (delete `reportPanicOnce`, `panicAcked`,
  the `tick` call site, and `missesAtTickStart` if nothing else uses it)
- Modify: `nodus/oevm/zephyr/ReplayProbeC6.nsl` (same deletion — it carries a copy)
- Inspect only: `nodus/oevm/zephyr/TimerlessNodeC6.nsl`, `WedgeProbeC6.nsl` — their
  `panicNoteAck: 0` calls are scenario semantics (clearing without reporting); LEAVE them.
- Test: `nodus/tests/22_spacesync/24_panic_reoffer.nsl` (new; next free number after 23)

**Interfaces:**
- Consumes: `outer panic` (kernel builds the `#Panic` record or nil), `outer panicNoteAck:`
  (kernel forwarder), `space seq` (reads the space counter), the capture subscription
  (bridge's `whenever:` stamps stream writes into `pending` synchronously — verify against
  `05_pending_bound.nsl` before relying on it).
- Produces: automatic must-deliver sys/panic for every bridged app; apps no longer touch
  the panic note when they hold a bridge.

- [x] **Step 1: Write the failing host test** `24_panic_reoffer.nsl`, with per-test env
  `OEVM_TEST_PANIC_NOTE=1` (watchdog). Shape it on `16_refused_node.nsl` (bridge with
  `socket: nil`, replies fed to `applyAck:` directly). Properties, each printed for the
  `expect:` block:
  1. After one `checkIn`-equivalent offer pass (call the new method directly — with
     `socket: nil`, `checkIn` early-returns), `pending` holds a sys/panic row and the note
     is still present (`platform panic` non-nil): offering is not acking.
  2. A second offer pass publishes AGAIN (re-offer per check-in, duplicates acceptable):
     pending gains a second sys/panic row (or the count climbs after an eviction).
  3. Feed an `#ack` whose `hw` is BELOW the panic seq → note still present.
  4. Feed an `#ack` whose `hw` covers the panic seq → next offer pass calls
     `panicNoteAck:`; `platform panic` now answers nil and no further row is published.

- [x] **Step 2: Run it, expect FAIL.**
  `cd nodus/nsl && python3 tools/run-one.py ../tests/22_spacesync/24_panic_reoffer.nsl B`
  (mind the runner's real path conventions; a CRASH discards printed lines — read the whole
  output).

- [x] **Step 3: Implement in PortaBridge.** One slot `panicSeq ::= 0.` beside `fitRows`.
  One method, called from `checkIn` between `publishApps` and `applyLivenessGoal` (ahead of
  the per-check-in rows for the same tail-trim reason `publishApps` documents — sys/panic
  is must-deliver, so it takes the LOWER seq):

  ```
  offerPanic = (
    | p |
    (panicSeq > 0 and: [ acked >= panicSeq ]) ifTrue: [
      outer panicNoteAck: bootId.
      panicSeq:: 0.
      ^ self ].
    p:: outer panic.
    (p == nil) ifTrue: [ ^ self ].
    space out: 'sys/panic' value: p.
    panicSeq:: space seq.
    ^ self )
  ```

  Notes for the implementer: `== nil`, never `isNil`, on `p`'s upstream (`platform panicNote`
  answers a prim-built indexable — the kernel's `panic` already guards this, keep it there).
  `space seq` after the `out:` is the captured row's stamp ONLY IF the capture reaction runs
  synchronously — `05_pending_bound.nsl` asserting `pendingSize` right after `out:` is the
  evidence; if it proves async, take the seq from `pending`'s last row instead. Carry a
  comment with the D4 rationale: re-offer-per-check-in is what survives `maxPending`
  eviction; the ACK covering the seq is what replaces `reportPanicOnce`'s
  clean-check-in guess; duplicates in an append-only log are acceptable, loss is not.

- [x] **Step 4: Run the new test — PASS.** Then delete the app-side copies
  (PortaBridgeNodeC6 `reportPanicOnce` + `panicAcked` + call site; ReplayProbeC6's copy),
  leaving a one-line comment pointing at PortaBridge>>offerPanic (#114).

- [x] **Step 5: Full host gates.** `python3 tools/run_tests_oevm.py` (count +1, aux green),
  `cd nodus/oevm && bash tests/porta_sync_host.sh` (PORTA SYNC OK).

- [x] **Step 6: Commit.** `feat(#114): sys/panic is re-offered each check-in until porta's ACK covers it — the bridge owns must-deliver now`

---

### Task 5: Rider 2 — `rej` reaches obs/health (and the manifest learns it)

**Files:**
- Modify: `nodus/oevm/zephyr/PortaBridgeNodeC6.nsl` (`publishHealth`: `#rej -> bridge rejected`)
- Modify: every other `publishHealth`/Health-record producer — find them:
  `rg -ln "#Health" nodus/` (expect a host twin `PortaBridgeNode.nsl` and the manifest
  goldens fixture)
- Modify: `protocol/MANIFEST.cddl` (Health shape — READ the `manifest-cddl-editing-rules`
  memory first; KEY ORDER IS WIRE ORDER, keys sort by ENCODED bytes)
- Modify: `protocol/PROTOCOL-NSL-RECORDS.md` (the Health row: `rej` beside `ref`, with the
  one-line reason: counted inbound drops advance the cursor past entries that never
  executed, so the loss must be visible or it is silent forever)
- Modify: `nodus/nsl/tools/fixtures/manifest_goldens.nsl` (regenerate/re-derive the
  obs/health golden per that file's own procedure — read its header)

- [x] **Step 1: Make the edits, goldens last.** Follow the golden fixture's own
  regeneration instructions; never hand-patch golden hex.

- [x] **Step 2: Run the gates that own the manifest.**
  `cd nodus/nsl && python3 tools/run_tests_oevm.py` — the `nsl-tools-py` aux gate validates
  the CDDL edit; `cd porta && go test ./...` — the seam gate checks porta's copy. Expect the
  porta seam gate to name any drift between the file and the store-loaded manifest —
  resolve per its message, do not improvise.

- [x] **Step 3: Commit.** `feat(#232): obs/health carries rej — a counted inbound drop is permanent under the replay guard, so it must be visible`

---

### Task 6: The `porta-refuse` scenario — probe node + manifest

**Files:**
- Create: `nodus/oevm/zephyr/RefusedProbeC6.nsl`
- Create: `nodus/oevm/tests/target/porta-refuse.env`

**Interfaces:**
- Consumes: Task 1's refusing porta (port 16971 on the gateway host); the TAP-lite block
  (copy it verbatim from PortaBridgeNodeC6 — three copies exist by design, this is the
  fourth); `bridge refused` / `bridge misses`.
- Produces: the ninth-plus scenario; TAP name `porta-refuse`.

- [x] **Step 1: Write the probe node.** Start from PortaBridgeNodeC6 and CUT: no goal/name
  convergence check, no dbg/req reaction, no spans requirement beyond construction. Keep:
  Thread attach wait, bridge construction — same `gwAddr`, but port **16971** (read the real
  constant name — `gwPort` — and override only the number, with a comment naming the
  refusing instance), 2 s cadence, per-tick report line **extended with `ref`**:
  `tick N misses M ref R cursor C acked A dropped D` (the observe regex keys on
  `tick N misses`, unchanged). TAP plan (3), scored at tick 15 (~30 s of refusal):
  1. `refused-climbing`: `bridge refused >= 10` — the injector provably refused us (an
     injector that silently does nothing must not score a survived run).
  2. `not-a-timeout`: `bridge misses` ≤ 2 — refusal is answered, not dropped; separates
     "refusing porta" from "no porta".
  3. `still-alive`: trivially true at scoring time (the real liveness verdict is the
     observation window; this TAP just anchors the count).
  Then tick forever — the window does the judging.

- [x] **Step 2: Write the manifest.** Model on `bridge-health.env` + `wedge-quiet.env`:

  ```
  SCENARIO=porta-refuse
  NODE=RefusedProbeC6.nsl
  BOARD_ID=esp32c6
  CONF_EXTRA="prj_esp32c6_thread.conf;prj_esp32c6_thread_ftd.conf"
  CLASS=disruptive
  TIMEOUT_S=300
  OBSERVE_S=240
  OBSERVE_PROGRESS='^.*tick [0-9]+ misses'
  ```

  The header comment must carry: (a) #232's rationale — the host never reproduced the
  death, so only this window can answer it; (b) why 240 s: the death record is 157 s from
  FIRST CHECK-IN, the window starts at TAP-pass (~30 s after first check-in), so 240 s
  watches past 270 s of refusal — comfortably beyond the record, and re-open the number if
  a death ever outlives it; (c) `ref` climbing is the PASS, silence is the FAIL; (d) the
  PRECONDITION: a refusing porta must be listening — the exact start/stop commands from
  Task 9, spelled verbatim (the harness never reaches into the gateway; starting the
  injector is the operator's act, same posture as bridge-health's goal/name write);
  (e) CLASS=disruptive because the scenario is designed to kill the node if #199's fix is
  inadequate.

- [x] **Step 3: Run the harness's own checks.** `python3 -m pytest tests/target/test_observe.py`
  from `nodus/oevm` (it asserts over every key any manifest sets), plus a no-board parse
  check if the runner offers one (`./run.sh` with no board attached should die at the
  collector check, not at the manifest).

- [x] **Step 4: Commit.** `feat(#232): porta-refuse target scenario — a refused C6 watched past the 157 s death record`

---

### Task 7: All host gates green before any board time

- [x] **Step 1:** `cd nodus/nsl && python3 tools/run_tests_oevm.py` — expected count (276 + new
  tests), `Failed: 0`, EVERY aux gate green (read the aux line; footprint SKIP is a finding).
- [x] **Step 2:** `cd porta && go test ./...` green, `gofmt -l .` silent.
- [x] **Step 3:** `cd nodus/oevm && bash tests/porta_sync_host.sh` — `PORTA SYNC OK`.
- [x] **Step 4:** Commit anything the gates forced. Push.

---

### Task 8: The deliberate gateway deploy (durability trio binary + new manifest)

The pending deploy note comes due here: the live porta must speak the new Health shape
before a re-flashed node reports to it, and the durability-trio binary (cursor_hw
migration) has been waiting for "a deliberate moment" — this is it, one restart for both.

- [x] **Step 1: READ `porta-gateway-operational-state` memory in full.** Non-negotiable —
  the pkill self-kill recipe, the hand-start location (`~/porta-v2`), which db is being
  served, how the systemd/hand-start situation actually stands.
- [x] **Step 2: Build for the gateway's architecture.** `ssh gw85224-01 uname -m` first;
  cross-build `porta` accordingly (CGO — mattn/go-sqlite3 — so build ON the gateway if the
  toolchain answer is unclear; `git pull` there and `go build ./cmd/porta` in place is the
  low-risk path).
- [x] **Step 3: Stop/start the live porta per the memory's OWN recipe** (never a bare
  `pkill porta`). Store migration runs on first open. Load/store the new manifest by
  whatever mechanism armed it on 2026-08-25 (the memory + `porta/cmd` will name it — likely
  a `porta manifest` verb; verify, don't guess).
- [x] **Step 4: Verify recovery.** `/health` via gatekeeper (`curl http://gw85224-01:6971/health`),
  then watch one check-in cycle land from c6bench/bell (`porta log` with the RIGHT `--db`,
  or the exchanges view) — nodes re-appear within their cadence. The expected one-time
  event: a node that reboots after this upgrade replays old streams ONCE before the guard
  latches (recorded, not a bug).
- [x] **Step 5:** Note deploy time + binary commit in the session log (for the close-out).

---

### Task 9: The refusing porta instance on the gateway host

- [x] **Step 1: Start it** (throwaway db in a scratch dir, refusing, port 16971 — exact
  flag names from Task 1; run it under `setsid`/`nohup` with a log file):

  ```
  ssh gw85224-01 'cd ~/porta-v2 && nohup ./porta serve --addr :16971 \
      --db /tmp/porta-refuse-232.db --test-refuse-all \
      > /tmp/porta-refuse-232.log 2>&1 & echo started'
  ```

  (Adjust to the real serve flags — `--addr`'s actual spelling, whatever the HTTP listener
  needs to NOT collide with :6970; read `porta serve --help` output first.)
- [x] **Step 2: Prove it refuses** from the workstation side: send any datagram to
  `gw85224-01:16971` — hex-decode the reply, expect the `#err` with the TEST-REFUSE text in
  the log. An injector that silently does nothing must be caught HERE, not by a dead board.
- [x] **Step 3:** Record the stop command for close-out (`kill` by the pid echoed at start,
  never pkill-by-pattern).

---

### Task 10: Board time — c6spare

- [x] **Step 0: Bench check.** The board changes between sessions: confirm c6spare is
  present via the collector env files + `ls /dev/serial/by-id/`, collector running
  (`target::collector_ok` will also gate this). ONE port handle rule throughout.
- [x] **Step 1: Run the new scenario first** (it's the session's headline):
  `cd nodus/oevm/tests/target && ./run.sh porta-refuse` (~10 min incl. flash + 240 s window).
  This is also **.oimg format v10's first on-hardware boot** — if the node fails to boot at
  all, suspect v10 before suspecting the scenario, and say so in the log.
- [x] **Step 2: Regression scenarios** for the kernel.nsl edits (host is blind to this seam):
  `./run.sh bridge-health wedge-detect` — bridge-health proves the bridge still converges
  against the LIVE (upgraded) porta with the new Health shape; wedge-detect proves the
  MOVED panic publication end-to-end (wedge → note → reboot → bridge re-offers → porta row).
  Read wedge-detect's checker before running so its pass criteria are known, not assumed.
- [x] **Step 3: A scenario's TAPs are not the verdict** — read each observation-window
  outcome. On any FAIL: stop, diagnose with the systematic-debugging skill, do not re-run
  hoping.
- [x] **Step 4: Kill the refusing porta** (Task 9's recorded pid). Verify :16971 is closed.

---

### Task 11: Close-out (the promotion rule)

- [x] **Step 1: Issues.** #232: close with the verification detail (scenario table row,
  build ids, window outcomes). #114: close citing the landed re-offer + wedge-detect green.
  #199: close citing porta-refuse green past 157 s (its body's "blocks closing" is
  released). #266: note its gate was already released; nothing further here.
- [x] **Step 2: Documents of record.** CLAUDE.md's scenario table gains the `porta-refuse`
  row (and the count in the prose). `protocol/PROTOCOL-NSL.md` §3's honest-receipt wording
  gains the pointer to `rej`. The durability design doc gets a one-paragraph addendum:
  D4 implemented (where), riders landed (how), scenario green (when, build id).
- [x] **Step 3: Status surfaces.** `docs/next-steps-overview.md` session-close note;
  `docs/status/dev-status.html` re-synthesized + re-scp to gw85224-01:porta-v2/docs/status/.
- [x] **Step 4: Memories.** Update `porta-gateway-operational-state` (new binary deployed,
  manifest version), `two-c6-devkits-on-the-bench` if the bench moved, and a new memory
  only if something non-obvious survived the session (e.g. the refuse-injector recipe).
- [x] **Step 5: Push everything.** `git push`; confirm clean tree.

---

## Self-review notes

- Spec coverage: #232 scenario (Tasks 1, 6, 9, 10), its two side-questions are *observed*,
  not built-for (no new instrumentation — the 4104 question only answers on a death, and
  the forced-rebuild tunable stays unearned per the design posture); #114 (Tasks 3, 4, 10
  step 2); rider 1 (Task 2); rider 2 (Task 5); deploy note (Task 8); .oimg v10 first boot
  (Task 10 step 1); promotion rule (Task 11).
- Deliberate deviation from bite-size: Tasks 8-10 are operational, not code, and their
  steps are checklists over live systems; their "test" is the system's own gates.
- Type consistency: `SetRefuseAll` (Task 1) ↔ Task 9's flag; `offerPanic`/`panicSeq`
  (Task 4) are self-contained in PortaBridge; `rej` key (Task 5) ↔ Task 6's report line
  prints `bridge refused`, which is `ref`'s source and needs no new accessor.

---

## Execution deviations (2026-08-26, all tasks complete)

- **offerPanic's state machine is richer than Task 4's sketch**, forced by two facts the
  plan flagged as risks: capture is DEFERRED (`out:` enqueues on the space ring; the stamp
  is minted at `space drain`), so the seq is latched from `pending` a tick later; and an
  evicted offer must not be mistaken for a delivered one, so `capture:` clears `panicSeq`
  on evicting a sys/panic row. "Re-offer per check-in" became "re-offer whenever no offer
  is standing and none was receipted" — same durability, fewer duplicates.
- **Found while moving the publication: NOTHING subscribed sys/panic** — every publication
  since #215 died undelivered at drain. The bridge now subscribes at construction, and
  `porta_sync_host.sh` gained leg 9 (not in the plan) asserting a planted note ARRIVES at
  porta as `sys/panic #watchdog` over the real wire.
- **wedge-detect was dropped from Task 10** — its probe has no bridge (it acks the note
  itself), so it cannot validate the moved publication; leg 9 covers the delivery seam and
  bridge-health covers the kernel.nsl edits on hardware.
- Task 3's seam went into `reboot_note_init()` as planned, but the host mains never called
  it — both (`oevm_main.c`, `oevm_image_main.c`) now do, mirroring zephyr's boot order.
- Board results: `porta-refuse` PASS 2/2 + 240 s window (tick 150, `misses 0 ref 150`,
  300 s of refusal, no fault; injector counted 313 refusals); `bridge-health` PASS 3/3 +
  180 s window against the live upgraded porta (`#rej` stored). Builds
  `…-g92439fc[-dirty]-20260826T19…`, c6spare.
