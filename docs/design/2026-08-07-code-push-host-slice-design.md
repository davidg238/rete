# Code-push, the host-only vertical slice (#25) — design

2026-08-07. Decision record for the #25 rung: `.oimg` swap over UDP, simulated A/B with
a deploy watchdog, the signing seat, and the seed of the G10 fleet-sim — all host-side,
localhost UDP, no hardware. Spans both halves, so it lives in the root `docs/design/`.

Refs: #25 (issue text + the 2026-07-22 revenue-gate comment),
`review/reliability-gaps.md` G1/G5/G10/G11, `protocol/PROTOCOL-NSL.md` §1 (non-goals),
§4.3, §5.3, §7; `protocol/PROTOCOL-NSL-RECORDS.md` §2.3 (BlobRef), §4 (App records).

## 0. What this rung is for

The deploy path is simultaneously the least-built, the most decision-foreclosing, and —
per the revenue-gate comment — the entire product of the only path that compounds.
Every risky G1/G5/G10 decision can be exercised on the desktop, so the flash map at
Zephyr time becomes a transcription of proven host behaviour, not a guess. The state
machine (stage → verify → activate → trial → confirm | revert) is the deliverable;
the transport is deliberately the degenerate case.

## 1. Decisions

- **D1 — Transfer is a hand-rolled, CoAP-shaped block pull over UDP.** Two messages:
  node asks `[hash, blockNo]`, porta answers `[blockNo, more, bytes]`. Stop-and-wait,
  client-initiated, resumable by block number — the exact shape of a CoAP blockwise
  GET, so the later swap to real CoAP (ratified direction, PROTOCOL-NSL §5.3) is
  framing only. Zero new dependencies on either side. **What earns the CoAP swap** is
  already pinned: the DTLS footprint measurement on the most constrained target
  (PROTOCOL-NSL §7 q7); on Thread, blockwise + ETag + DTLS arrive together with the
  framing change. Until then this is the bulk plane.
- **D2 — The node self-confirms, MCUboot-shaped.** The runner boots the trial slot
  with a trial flag; the app itself, after its **first successful porta check-in**,
  calls a confirm primitive. No confirm within N seconds → revert. This is G1's
  "contact porta" criterion expressed node-locally: the runner never talks to porta,
  and every piece transcribes 1:1 to MCUboot later (trial flag → swap-test boot,
  confirm prim → `boot_write_img_confirmed()`, runner → boot code). The state machine
  is deliberately MCUboot's, not an invention — it is field-proven, and if the
  independent-swap decision ever reversed, the same machine lands on fused images
  unchanged.
- **D3 — Real ed25519 from day one.** porta signs the image's SHA-256 at push (Go
  stdlib); the runner holds the public key and verifies the staged image **before
  activation** and the active slot **on every exec** (as MCUboot verifies every
  boot). ed25519 is on MCUboot's supported menu, so the scheme can survive to target.
  The seat is the point (G5: auth before the push path exists, never after); the key
  story stays deliberately dumb — one keypair, two files, `porta key gen` once,
  missing key is an error, never "unsigned". The private key never leaves porta.
- **D4 — Verification has exactly one seat: the runner.** The C seam checks the
  content hash (transfer integrity); it never holds a key. The thing that judges an
  image lives outside the image it judges.
- **D5 — The unit of deployment is the whole `.oimg`, and `<name>` is `main`.**
  tuvm has one image, one heap, one world; the runtime unit of multiplicity is the
  job, and modules are compile-time. `goal/apps/<name>` deliberately keeps the
  per-name (Toit-container-heritage) shape — multi-app later adds per-name slots and
  per-name checks, rows not mechanisms — but the genuinely hard part (separately
  compiled images sharing one VM, per-app isolation) is a VM feature for its own
  rung, if a fleet ever earns it. This slice reads and writes exactly
  `goal/apps/main` / `obs/apps/main`.
- **D6 — The update check lives in `checkIn`, not in a `whenever:`.** The tail of
  `PortaBridge>>checkIn` compares the goal record's image hash against the running
  image's hash. The bridge already wakes every cadence tick, so **every failure
  retries for free** at the next tick, and no reaction machinery (or #133 exposure)
  is added.
- **D7 — Blocking fetch, day one.** `otaStage:sig:` fetches synchronously; invisible
  on localhost. **What earns async** (a background thread + pulse-mailbox completion):
  a measured check-in stall on a target-class transfer. Recorded, falsifiable.
- **D8 — Blob store is content-addressed files, not a table.** `<datadir>/blobs/<hex-sha256>`,
  written stage-temp-then-rename. Idempotent by construction, streams, and keeps the
  bulk plane out of the space store (four tables stay four).

## 2. Topology

```
porta side                              node side (per node, host)
──────────                              ───────────────────────────
blobs/<hash>            ◄── porta blob put app.oimg   (stores, signs, prints hash)
goal/apps/main record   ◄── porta put …/goal/apps/main '{image:{hash:…},sig:…}'
                                        runner (Python)
UDP :P   exchange   ◄─── pull/push ───► │ execs → oevm VM (slot A image)
UDP :P+1 blocks     ◄─── {hash,blk#} ─► │   checkIn sees goal hash ≠ running hash
                     ──► {blk#,more,bytes}  C ota seam fetches blocks → inactive slot
                                        │   VM exits STAGE_READY (42)
                                        │ runner: verify ed25519 → trial-boot B
                                        │   new VM checks in → otaConfirm
                                        │ confirm in time → B active; else revert to A
```

Two planes on two ports, matching the protocol's control/data split. The block port is
**exchange port + 1** — a convention, not a knob; the runner needs no transport
knowledge. The block server is a pure `Handle([]byte) []byte` wrapped by the same thin
UDP loop idiom as `wire`. Block data payload is 256 B (the value-cap number;
radio-honest). Unknown hash answers an error frame. The server is stateless — resume is
the client re-asking from any block number.

**Amendment 2026-08-07 (planning): the block frames are fixed binary, not cval CBOR.**
Request `0x01 ‖ hash[32] ‖ blockNo u32 BE`; response `0x02 ‖ blockNo ‖ more ‖ data`;
error `0x7F ‖ text`. The fetch loop lives in the C seam (G3), which cannot reach the
nsl `Cbor` class — a C CBOR encoder for one fixed shape would be more mechanism than a
37-byte frame, and CoAP, the ratified successor, is itself binary-framed. CBOR remains
the law of the space planes; this is the data plane's one binary surface.

## 3. Porta side

- `porta/internal/blob/` — content-addressed file store (D8).
- `porta/internal/blockserv/` — the block responder, **fixed binary frames** per the §2
  amendment above (not `cval` CBOR: the fetching half is the node's C seam).
- CLI: `porta key gen` (once), `porta blob put <file.oimg>` → prints `{hash, sig,
  size}`. The record is written with the existing `porta put`. A `porta deploy` sugar
  verb (blob put + record put in one) is **not** built this rung; it can come later if
  the two-step grates.
- Record: `goal/apps/<name>` = `App { image: BlobRef, sig: bytes, … }` — one protocol
  edit adding `sig` to the App record in RECORDS §4 (an edit, not a versioned field;
  no back-compat by posture).

## 4. Node side

A C `ota` seam — the data plane stays off the algebra, and staging buffers live outside
the VM heap by rule (G3). New `nodus/tuvm/src/ota.c` + `ota_backend.h` +
`backend/posix/ota_posix.c`, following the existing backend pattern (the nine
enumerated source lists are the plan's problem). Three prims, name-resolved, appended:

- **`otaStage: hash sig: sig`** — blocking block-fetch loop (D7) writing **directly
  into the inactive slot file** (the flash analog — no rename), verifying the full
  SHA-256 at the end. Resume is the staging file's length: a killed node restarts and
  asks for block `filesize/256`; no cursor state invented. Idempotent: the seam first
  hashes any existing staged file and answers success immediately on a match, so a
  crash after staging costs one re-hash, not a refetch. Per-block retry is bounded
  (default 5 × 1 s, constants in the seam).
- **`otaActivate`** — writes `stage/ready.json` `{hash, sig}` and exits the process
  with code 42, the host analog of "reboot into trial slot".
- **`otaConfirm`** — touches the confirm marker; becomes
  `boot_write_img_confirmed()` on target.

Policy lives in the kernel bridge, once, for every app:

- `checkIn` tail (D6): if the applied `goal/apps/main` image hash ≠ `OEVM_IMAGE_HASH`
  → stage on this tick, `tel/log` "staged" — and **activate on the *next* tick, after
  its exchange completes**. `otaActivate` exits the process, and a log line written on
  the same tick as the exit would die in the in-RAM pending queue (#114's family); the
  one-cadence-tick delay is what makes the staging observable at porta before the
  reboot.
- First successful exchange while `OEVM_OTA_TRIAL=1` → `otaConfirm`, exactly once.
- The bridge publishes `obs/apps/main` (running image hash, lifecycle), so
  `goal/apps/main` vs `obs/apps/main` **is** the rollout dashboard — desired-vs-observed
  tree diff, no new mechanism. A reverted node's divergence persisting is the
  failed-rollout signal; rollback needs no reporting path of its own.

Runner→VM env: `OEVM_IMAGE_HASH`, `OEVM_OTA_TRIAL`, stage/confirm paths, porta address.

## 5. The runner

`nodus/tuvm/tools/oevm_runner.py`. It never talks to porta and never parses CBOR — it
reads exit codes, markers and its manifest. Per-node directory:

```
<nodedir>/
  manifest.json      { active: "a", prev: "b", trial: false }
  slots/a.oimg       slots/b.oimg
  stage/ready.json   { hash, sig }        ← written by otaActivate
  confirm            (marker)             ← written by otaConfirm
  pub.key
```

The loop:

1. **VERIFY** — ed25519-check the active slot on every exec. Fail → refuse to run it;
   revert to `prev` if one exists.
2. **RUN** — exec the VM on the active slot with the env above. Entering trial deletes
   any stale confirm marker first, so a confirm can only come from the trial boot.
3. **On exit** —
   - **42 (STAGE_READY):** staged slot's SHA-256 must match `ready.json.hash` and the
     signature must verify over it. Pass → `prev=active, active=other, trial=true`,
     re-exec. Fail → log, truncate the staged slot, re-exec the old image.
   - **any other exit while trial and unconfirmed** → **REVERT**: `active=prev,
     trial=false`, re-exec. A crashing trial image never gets a second chance
     (`prev` is never itself trial).
   - **exit while confirmed** → re-exec (G11's reboot-as-recovery).
4. **TRIAL WATCHDOG** — while trial: marker appears → `trial=false`, permanent. No
   marker within **N seconds (default 30, a flag)** → kill, REVERT.

ed25519 verify on the runner side is a vendored, public-domain pure-Python reference
implementation (~100 lines, milliseconds on host — fine for a sim; the target verify
is C and out of scope).

## 6. Failure table

| Failure | Behaviour |
|---|---|
| Block timeout / porta down mid-fetch | Bounded stop-and-wait retries (default 5 × 1 s), then abort the stage; `tel/log` once; next cadence tick resumes from the staged length |
| Unknown hash at porta | Error frame → abort, log, retry next tick (record pushed before blob — visible, self-heals when the blob lands) |
| Staged hash mismatch | C seam truncates the slot, logs; refetch next tick |
| Signature rejected at runner | Truncate staged slot, log, keep running the old image. The node refetches each cadence — deliberate: the loop is operator-visible (goal ≠ obs) and the fix is pushing a good record. A rejected-hash memo is deferred until the noise is measured |
| Crash between `otaActivate` write and exit | `ready.json` does not match the staged file → same path as sig-reject |
| Trial image crashes or never confirms | Revert, exactly once |
| Retarget arrives during an unconfirmed trial | Staging deferred until confirm or revert — a trial node never stages, so the rollback slot is never destroyed while unproven. Enforced in `PortaBridge>>otaCheck`, not in the runner: §5 step 3 handles exit-42 before the trial revert, so a trial node that staged would leave `active` = a never-confirmed image with `prev` cleared, breaking "`prev` is never itself trial"; and reordering the runner is worse, because by exit-42 the staging has already overwritten the rollback slot, so a revert-first runner verify-fails on a cleared `prev` and exits 3 — a brick. Cost of deferring: at most one tick of latency, either way the trial ends |
| Stale confirm marker | Deleted on trial entry |

**A retarget that lands on a partial stage costs one wasted fetch, and heals itself.**
The resume offset is the slot's own length, so a new hash arriving over a half-staged
different image resumes *into a foreign prefix* whenever the new image is the same length
or longer at that offset; the bytes are wrong rather than incomplete, the whole-image
digest catches it, the seam truncates and the next cadence tick refetches clean. (The
shorter case is caught earlier: the resume block is past the new image's end, porta
answers an error frame, and a resumed fetch treats that as a stale slot and truncates.)
Not worth a fix — a length-preserving truncate or a stored per-hash cursor would be a
second mechanism for a case that already self-heals at the price of one transfer.

**Amendment 2026-08-08 (as built): a rejected signature is LOGGED, not counted.** Both
this table and §5 step 3 said "count + log", and no counter was ever built — the runner
prints `runner: staged-reject <why>` and nothing accumulates. Struck rather than
implemented, on the same reasoning that defers the rejected-hash memo: a count nobody
reads is a mechanism invented ahead of the question it would answer, and the refetch loop
is already operator-visible as `goal` ≠ `obs`. If the memo is ever earned, it and its
counter arrive together.

## 7. Testing

- **Go unit tests:** blob store (idempotence, hash addressing), `blockserv.Handle` as
  a pure function (golden frames via cval), sign-at-push.
- **C test** (`run_c_tests.sh` family): resume-offset arithmetic + final-hash verify
  against a stub.
- **The gate — `nodus/tuvm/tests/host_ota/run.sh`**, TAP-lite with a declared plan, in
  the `porta_sync_host.sh` idiom: porta + blockserv + **3 runner-wrapped VMs**, four
  scenarios:
  1. **Clean deploy** — push once; all three `obs/apps/main` converge to the new hash.
  2. **Kill mid-transfer** — SIGKILL one node mid-fetch; it resumes from staged length
     and converges.
  3. **Bad signature** — runner refuses activation; the old image keeps running and
     reporting.
  4. **No-check-in revert** — a valid, signed image that boots but never bridges; the
     watchdog fires, the node reverts, the old hash reappears in `obs/apps/main`.
- Standing gates stay green: nsl suite, `go test ./...`, `porta_sync_host.sh`.

This gate is the **seed of G10**, not a framework: scenarios are code in one script,
generalised only when a later scenario earns it (the target-harness lesson: generalise
from worked examples).

## 8. Out of scope, stated

- Real MCUboot, Thread transport, delta images, commissioning — at their scheduled
  steps (#25's own scope line).
- An image that checks in but is otherwise sick — health *is* the first successful
  exchange (G1's criterion).
- Boot-loop containment / safe mode (G11) — its own decision at Zephyr bring-up.
- Multi-app (D5). One future note worth keeping: with capabilities being rooted paths
  (attenuation only narrows), a node-side supervisor supplying each app's
  `usingPlatform:` argument at **load** time is the natural seam for separately
  delivered apps composing on one heap — a binding question, not a heap question, and
  it preserves the single-heap immutable-record message-passing decision. (David,
  2026-08-07, aside during this design.)
- `porta deploy` sugar verb.

## 9. Document edits carried by this rung (promotion rule)

- `protocol/PROTOCOL-NSL-RECORDS.md` — `sig` added to the App record; the
  single-app (`main`) scope note.
- `protocol/PROTOCOL-NSL.md` §5.3 — the host block plane (CoAP-shaped, exchange
  port + 1) recorded as the day-one bulk transport; TFTP → CoAP direction unchanged.
- `porta/docs/ARCHITECTURE.md` — blob store + block server.
- Close-out must name which mainline doc absorbed each durable finding.

## 10. The target half — proposed 2026-10-10 (rete#233, for David)

*Status: DECIDED (David, 2026-10-10: yes to T1–T8, with T8 amended below; T9 added after S1). S1, T9, S2 and S3 are done on `233-ota`: all four scenarios proven on c6spare, the reset mid-fetch refetching from block 0 as T7 decided, ~60 s on Thread (rete#233, 2026-10-10; the retry loop's mesh cost is on rete#175). Program OTA only (peer design D4; firmware OTA stays
deferred), for the two 0.1 demo boards: the wall C6 on Thread (#380) and the MagTag on
WiFi (#379). The host gate (`nodus/oevm/tests/host_ota/run.sh`) was re-run today: `plan 4`,
four `ok`, `HOST OTA OK`. Everything above this section is unchanged by it: the kernel's
`otaCheck`, `ota.c`, porta's blob store and block plane all run on target as they are;
only `backend/zephyr/ota_zephyr.c` stops being inert, and `main.c` gains the runner.*

- **T1 — The firmware is the runner.** `main.c` transcribes §5 step for step: manifest,
  verify, run, trial, revert. D4 holds on target — the seat that judges a program is the
  firmware, outside the program it judges — which is #310 Q2's "receiver in the firmware".
- **T2 — Two equal program slots, run in place.** `program_partition` (512K at `0x320000`)
  becomes `program_a` and `program_b`, 768K each at `0x220000` and `0x2E0000`; `image-0`
  and `image-1` shrink 1536K → 1024K (firmware 587 KB, 488 KB under MCUboot). Nothing
  after `0x3A0000` moves, so the Thread dataset survives again. Same carve on the S2.
  `program_map` takes the slot's offset, so the VM runs from either without a copy.
  Edits: both overlays, `protocol/IMAGE-ARTIFACTS.md` §2a, `custos/internal/load`'s copy,
  `run.sh`'s offset read. A firmware over 1024K or a program over 768K earns a resize.
- **T3 — The manifest lives in `settings` (NVS, already mounted).** `ota/active`,
  `ota/prev`, `ota/trial`, `ota/ready` (the staged slot's hash and sig), and each slot's
  hash and sig. `ota_touch_confirm` = `ota/trial := 0`, at most one write per boot (D2).
- **T4 — ed25519 verify is MCUboot's own.** `ED25519_verify` from
  `bootloader/mcuboot/ext/fiat/curve25519.c` plus `tinycrypt-sha512`, compiled into the
  firmware: the transcription D2 promised, no new dependency. The public key is compiled
  in from porta's pub file (`OEVM_OTA_PUBKEY=<hex>` to `run.sh`); a build without one has
  no OTA and the seam stays inert, which is today. Verified at ready-time for the staged
  slot and at every boot for the active one (§5 step 1). The cost is printed beside the
  existing `program sha256 … (N ms)` line, not guessed.
- **T5 — Cable and air share the slots by one rule: a slot the manifest does not
  describe was written by the cable, and is trusted as the cable is.** `custos load`
  writes the active slot (the boot banner names it; an erased manifest means `a`). At boot
  the firmware already hashes the active slot; a hash that differs from the manifest's
  entry means the cable rewrote it, and the firmware adopts it (hash recorded, no sig,
  trial off). This is safe because a stage only ever targets the inactive slot, so a torn
  stage can never sit in the active one; the inactive slot boots only through `ota/ready`
  after a verified signature.
  *As built (S2, 2026-10-10):* the cable always writes `program_a` (`custos load`, `run.sh`
  unchanged), and the runner hashes slot a at every boot: a hash the manifest has not seen
  makes a active, unsigned, no trial. **Blind spot, measured the same day:** an IDENTICAL
  cable write is invisible — loading the very image the manifest already records for a
  while b is active changes nothing, and the node keeps running b. The cable's answer is
  to load something else, or the push path's. Recorded rather than fixed: the only fix is
  a stamp the cable would have to write beside the image, a second mechanism for a case
  the person can see on the console (`runner: exec b …`).
- **T6 — A trial image gets one boot.** Any reset before confirm reverts (MCUboot's
  swap-test semantics, and what makes the rule hold through the MagTag's deep sleep, which
  is a reset); a trial that has not confirmed within a fixed 150 s is reset by a k_timer,
  five of the bell's 30 s cadences, the ratio the host gate uses (10 s against 2 s). The
  existing task watchdog already resets a wedged node, which then reverts.
- **T7 — Degenerate cases shipped, each with what earns its knob.**
  - The staged length is in RAM: a reboot mid-fetch refetches from block 0, because a
    flash slot has no length of its own. A stored cursor is earned by a measured mid-fetch
    reboot rate on the bell. §4's resume arithmetic stays proven on the host.
  - The fetch stays blocking, 256 B stop-and-wait (three 6LoWPAN fragments a block). The
    first number this rung produces is the bell image's fetch time over Thread (199,304 B
    today) and the check-in stall it costs; that number decides async and block size (D7).
  - #175's refetch-forever is unchanged until it is measured on the mesh; `otaCheck`'s
    trial-unconfirmed guard bounds the worst case to one fetch per cadence.
- **T8 — The push front for 0.1 is porta's CLI on the gateway**, `porta blob put` then
  `porta put --raw <node> goal/apps/main`, exactly as the debug line was driven on
  2026-10-10. `custos load` with a porta target waits on porta's HTTP interface (peer design
  F1, still `/health` and `/manifest` only) and is filed, not built here.
  *Amendment 2026-10-10 (David):* the HTTP front IS in 0.1, for consistency with debug: rete#398
  (porta HTTP gains a blob upload and a goal write; `custos load` gains a porta target), built
  after S1–S3 prove the board. Paper E Appendix D, the agent HTTP surface, is finalized against it.

- **T9 (David, 2026-10-10, after S1) — the bulk plane is off the message THREAD as well
  as off the wire.** S1 measured the first target fetch at ~3.3 KB/s over Thread (174 KB
  in ~55 s) and the observer's app-stale rule (8 × cadence = 16 s) rebooted the node
  mid-fetch, three times, then halted it: D7's blocking fetch cannot finish on a board,
  and the knob D7 said a measurement would earn is earned. Two fixes were weighed: a
  time-sliced stage inside the check-in (bounded stall, no thread) and a fetch thread.
  The thread was chosen because the side channel was meant to keep bulk off the message
  plane, and a slice still stalls that plane for its length (two check-ins on the bell,
  every tick late on a 2 s node) exactly while a push is being watched. As built:
  `ota_stage` starts the fetch on a thread the backend gives it (`ota_thread_start`: a
  `k_thread` with a 3 KB static stack on zephyr, a detached pthread on posix) and answers
  1 staged / 0 in progress with the bytes so far / −1 failed; `otaStage:` answers true,
  the Integer, or false; the kernel asks again at each check-in and logs `ota staging N`.
  No mailbox: the check-in cadence is the poll. The slot's length stays the cursor, so
  the host gate's kill-mid-transfer scenario is unchanged. RAM: the thread exists only
  where a program slot exists; the C6 has 172 KB free after its 128 KB arena, the S2's
  DRAM is 222 KB total (a 28 KB arena node today), and the WROOM — which has overflowed
  DRAM by 4.7 KB once, with WiFi — has no slot and gains nothing. One mechanism; a
  slot board whose build cannot fit the stack is what would earn a sliced, on-thread
  fallback, and the WROOM decides that the day it gets a slot. The stack is sized by
  the high-water line the thread prints (`CONFIG_INIT_STACKS`), not by the 3 KB guess.

**Order, on `c6spare` and never the bell until proven:** (S1) `ota_zephyr.c` real against
slot `b`, staged over Thread from the gateway's porta — the fetch time; (S2) the runner in
`main.c` with T3–T6 and the verify cost; (S3) the host gate's four scenarios by hand on the
board: clean, reset mid-fetch, flipped sig byte, an image that never checks in; then the
MagTag, the bell, and paper E §9's OTA line.
