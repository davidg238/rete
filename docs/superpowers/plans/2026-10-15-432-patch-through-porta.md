# Plan: patch through porta (rete#432)

Design: `docs/design/2026-10-15-432-patch-through-porta-design.md` (Q1–Q5 decided). Branch `432-patch-porta`. Written
2026-10-15. Tasks 1–8 are host work; Task 9 is the bench and waits for David's hands where a board needs a cable.

## Found while planning (read from the code; the design's "Corrections from planning" carries them)

- **P1. A patch cannot land before `main:`.** The bridge, and with it the cell, exists only inside the app's `main:`
  (the MagTag checks in after its setup, the bell strikes before it checks in). `acceptGoal:` executes nothing: it
  queues `dbg:` lines that `commandLoop` runs at the first stop after the check-in (`kernel.nsl:1654–1680`,
  `debugger wake` `:2534`). Patches land there, like breaks, and ahead of them. A patch to code that runs before the
  check-in (the bell's strike) takes effect from the next wake.
- **P2. The set's bound is 16 methods, not 64.** `METHOD_OVERRIDES_MAX 16` (`oevm.h:562`) binds before
  `METHOD_PATCHES_MAX 64`. A set holds at most one patch per method (custos replaces), so 16 lines.
- **P3. There is no reboot primitive.** Q2's reboot needs one. On Zephyr a software reset keeps RTC memory
  (`main.c:297`) and the MagTag already reads it as a reboot wake (no strike, `tryCatchUp`), as after an OTA trial.
  On the host the process exits and the gate relaunches it.
- **P4. The host leg belongs in `sleepy_debug_host.sh`, not `porta_sync_host.sh`.** The sync gate runs a standard
  image; the sleepy gate is already the debug-through-porta gate (debug image, `OEVM_IMAGE_HASH`, the sha refusal,
  re-arm across killed processes). It is not in CLAUDE.md's gates today; it joins them.
- **P5. `after=<seq>` is ambiguous.** A node's row seq rewinds every boot. The cursor is porta's event id, and
  `Store.Events` (`space/store.go:774`) gains a path filter and an after-id.
- **P6. custos cannot rebuild the set from porta without reading a blob.** porta has no HTTP read of a blob:
  `GET /blob/{hash}` is added (general, language-neutral).
- **P7. The patch reference is a `BlobRef`, not a bare bstr** (RECORDS §2.3, "the one indirection"): its `size` lets
  the node refuse before it fetches. The field is optional; absent means no patches.
- **P8. `patch_method.py` refuses a patch over 511 bytes** (`:40`, `:91`), the cable's line. That cap stays on the
  cable; the porta path (a blob) does not have it.
- **P9. custos's debug session is synchronous** (`answerWait` 10 s, `session.go:19`). A sleepy node answers at its
  next check-in, so on a porta node every verb answers `Pending` at once, as profile does.
- **P10. Replies come from `dbg/resp`, not the tee.** The tee echoes them too (`Debugger>>say:`), but `dbg/resp` is
  exactly-once through porta and needs no `--advertise`. #415's profile used the tee because its rows are bulk.

## Tasks

1. **protocol: `patches` in `Debug`.** `? sym-patches => blobref` in the `debug` rule (`MANIFEST.cddl:184–191`; the
   key after `breaks`, wire order), `sym-patches` in the key list, `records.yaml:767–790`, RECORDS §4.1 (`:369`: the
   blob is the patch set for `sha`, one VM patch text per line, applied in order; unsigned, Q3), PROTOCOL-NSL `:475`.
   A golden with `patches` beside the existing one (`manifest_goldens.nsl`, `test_manifest_gate.py`). gw's
   `MANIFEST-415.cddl` is replaced by a copy of the new manifest at the bench (Task 9).
2. **porta: two reads.** `GET /blob/{hash}` answers the blob's bytes (404 if absent). `GET /node/{id}/log?path=P&after=N`
   answers the node's stream rows on path `P` with event id > `N`, oldest first, each with its id (P5). No patch code
   in porta (D1–D2). Unit tests in `ops_test.go`; `Store.Events` grows the filter, not a second query.
3. **VM: fetch a blob, reboot.**
   - `ota.c`: `ota_fetch_blob(host, port, hash, size, buf)`, non-static, over `fetch_block`, checking the SHA-256
     and the size. posix and Zephyr through the existing UDP seams.
   - Prim 165 `fetchBlob:size:host:port:` answers a ByteArray, or nil on a failed fetch or hash (it logs why).
   - Prim 166 `reboot`: Zephyr `sys_reboot(SYS_REBOOT_WARM)` (RTC kept); posix exits with code 75, which a gate
     reads as "relaunch me". Footprint re-banked if the ratchet moves.
4. **kernel: the set in `DebugLink>>acceptGoal:`.** The link keeps the lines it applied this boot.
   - No `patches`, nothing applied: as today. The sha check covers both (a mismatch refuses patches and breaks).
   - Fetch the blob (porta's host, exchange port + 1, as OTA). If the applied lines are a prefix of the blob's, queue
     `dbg:patch` for the rest, ahead of the `dbg:clear`/`dbg:break` lines. Otherwise, with lines applied, print
     `debug: patch set does not extend the applied one — rebooting` and reboot (Q2). Nothing applied and a set:
     apply it all (the boot replay).
   - A failed fetch answers `dbg:error patches fetch` and is retried at the next check-in; breaks still arm.
   - Standard images ignore it, as today (`dbgLink` is nil, `kernel.nsl:1861`).
   - Tests: an nsl case for the prefix rule (apply, extend, shrink → reboot requested), against a stub fetch.
5. **`patch_method.py --blob`:** writes the patch text with no line cap (P8); the cable path keeps its 511.
6. **custos: the debug verbs on a porta node** (Q5).
   - A porta `debug.Line`: `Send` is `PUT dbg/req`; a poller on `GET /node/{id}/log?path=dbg/resp&after=` feeds
     `Session.OnLine`. Every verb answers `{Pending, Seq}` (P9); `GET /board/<n>/debug/state` shows the replies.
   - `break`/`clear` rewrite the cell's `breaks` (read the cell, edit, `PUT`), keeping `patches`.
   - `POST /board/<n>/debug/patch?method=…`: `nsl.Patch` with `--blob`; the set is read back from the cell and
     `GET /blob` (custos keeps no state, P6), the method's old line replaced, a 17th method refused; `POST /blob`,
     then the cell. `DELETE …/debug/patch?method=…` removes one (the node reboots into the smaller set).
   - custos's porta `load` rewrites the cell to `Debug{sha: <new>, breaks: []}` after the push.
   - Go units against a fake porta: the cell round trip, the set merge, the log cursor.
7. **Gate: `sleepy_debug_host.sh` leg P.** custos serves the sleepy node as a porta node (`porta key gen` first:
   `POST /blob` signs). An agent patches a method whose output the leg reads; the output changes. The node process
   is killed and relaunched: the patched output from its first stop. A second patch extends the set with no exit
   75. A `DELETE` exits 75, the relaunch runs the smaller set. **Falsify first:** with the boot replay removed
   (Task 4's "nothing applied: apply it all"), the relaunch part goes red. The gate joins CLAUDE.md's list.
8. **Docs.** `docs/limits.md` (`:42` hot patch row: unsigned through porta, 16 methods, a shrink reboots); the
   design's "As built"; custos docs for the porta debug verbs; FOCUS at `/conclude`.
9. **Bench (with David).** gw's porta on the new manifest (an explicit "restart porta"). The wall bell over Thread
   and the MagTag over WiFi, each patched through custos, the patch still in force after a wake. Q4: the MagTag's
   fetch cost per wake (blocks, ms), before any RTC copy is considered.

## Close-out (promotion)

RECORDS §4.1 (the `patches` field), the peer design's 0.1 addendum (custos's porta debug verbs), paper E §9's
lifecycle table (patch through porta), the ladder, CLAIMS (a patch-through-porta row on leg P), `docs/limits.md`,
#26's 0.1 table.
