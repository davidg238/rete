# #443 Outbox Journal Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every captured stream row is written to a flash journal at capture and replayed after a reboot, so a link or power gap delays rows instead of losing them.

**Architecture:** A new C seam (`journal_backend.h`) stores opaque byte records in a ring: Zephyr FCB on target, a file on the host. PortaBridge appends each captured row (CBOR) at `capture:value:`, writes an ack marker when an ack retires journal rows, and on boot re-stamps the unacked rows with this boot's seqs. Push entries change from `[seq, path, kind, value, ageMs]` to `[seq, path, kind, value, bid, monoMs]` with the node's `mono` on the envelope; porta derives time per boot.

**Tech Stack:** C (oevm VM, Zephyr FCB/flash_map/spi_nor), nsl (`nodus/nsl/lib/kernel.nsl`), Go (porta).

**Spec:** `docs/design/2026-11-14-443-outbox-journal-design.md` (approved 2026-11-14). Read it before any task.

## Global Constraints

- Journal geometry on target: **255 sectors × 4 KB = 1020 KB**, partition label `outbox`; `CONFIG_SPI_NOR_FLASH_LAYOUT_PAGE_SIZE=4096` on MX25R boards.
- A board (or host run) with no journal behaves exactly as today: RAM `pending`, `maxPending` 64, oldest evicted to `sdr`.
- No back-compat anywhere: no versioned fields, no dual-read. Both ends reflash together.
- A journal whose header version mismatches or fails CRC is erased and started clean; the lost rows are counted.
- ttgm and ocm are proprietary: overlays/conf go in `~/workspace/ttgm` and `~/workspace/ocm`, never copied into rete.
- Gates (`CLAUDE.md`): `cd nodus/nsl && python3 tools/run_tests_oevm.py` (read the aux line); `for m in protocol porta custos; do (cd $m && go test ./...); done`; `cd nodus/oevm && bash tests/porta_sync_host.sh`. Never run the suite under `timeout` or through `tail`.
- Adding a `.c` file edits several enumerated source lists (no manifest, #63): find them with `rg -l "ota_posix" --glob '!*.md' --glob '!docs/**' --glob '!**/invarlint/out/**' .` and edit every hit.

## Review Focus

1. **Reboot mid-push:** porta committed rows, ack never arrived, node reboots → rows replay once with the same origin; porta stores the duplicate, never errors. Test in Task 5.
2. **Journal full during an outage:** rotation drops the oldest sector; `sdr` grows by exactly that sector's unacked rows; the newest rows survive. Test in Task 1 (C) and Task 3 (nsl).
3. **Replayed row from a boot porta never saw:** stored with null time, ordered by `(bid, monoMs)`; `porta` readers print it, never crash. Test in Task 5.
4. **Corrupt/foreign journal after a reflash:** header mismatch → erased, node boots and pushes normally. Test in Task 1.
5. **A board with no `outbox` partition:** builds, boots, and behaves as today (the existing `05_pending_bound.nsl` stays green unchanged). Covered by Task 3's gate run.

---

### Task 1: The journal seam, host backend

**Files:**
- Create: `nodus/oevm/src/journal_backend.h`, `nodus/oevm/src/backend/posix/journal_posix.c`
- Modify: every source list found by the Global Constraints command
- Test: `nodus/oevm/tests/test_journal_posix.c` (wired into the C-test runner beside the other posix backend tests)

**Interfaces:**
- Produces (C, `journal_backend.h`):
  - `int journal_open(void)` → 1 when a journal exists, 0 when the board has none (posix: `OEVM_JOURNAL` env names the file; unset → 0). Erases and answers 1 on a bad header; sets the erased-row count.
  - `int journal_append(const uint8_t *buf, int len, uint32_t *pos)` → rows dropped by a rotation this call (≥0), or <0 on error; `*pos` = the record's position (monotone within the journal).
  - `int journal_retire(uint32_t pos)` → 0 ok; appends a marker meaning "every record ≤ pos is acked".
  - `int journal_walk(uint32_t from, int (*cb)(uint32_t pos, const uint8_t *buf, int len, void *ctx), void *ctx)` → walks data records after `from` in order, skipping markers; `journal_last_retired(void)` answers the newest marker's pos (0 if none).
  - `int journal_lost_at_open(void)` → rows erased by a bad-header reset this boot.
- Posix ring: a file of `OEVM_JOURNAL_SECTORS` (default 4) × 4096-byte sectors, so wrap is testable with a few dozen rows.

- [ ] **Step 1:** Write `test_journal_posix.c` with: `append_walk_in_order` (append 3, walk answers them in order with their bytes), `retire_hides` (retire pos of #2, walk from `journal_last_retired()` answers only #3), `reopen_survives` (close, `journal_open` again, same walk), `wrap_drops_oldest` (fill 4 sectors + 1 record: append answers dropped = rows in sector 0, first walked record is sector 1's first), `bad_header_erases` (scribble the header, open answers 1, walk empty, `journal_lost_at_open() > 0`).
- [ ] **Step 2:** Run it; expect a link failure (no `journal_*`).
- [ ] **Step 3:** Implement `journal_posix.c`: header `{magic 'OJRN', version 1}`, each record `{u16 len, u8 type (0 data, 1 marker), payload, u8 crc8}`; a sector that cannot hold the next record is closed and the next one erased (rotation). Track per-sector unacked counts in RAM, rebuilt by the open walk.
- [ ] **Step 4:** Run it; all five pass.
- [ ] **Step 5:** Commit `journal: the seam and its host backend; refs #443`.

### Task 2: Prims over the seam

**Files:**
- Modify: `nodus/oevm/src/prim_ids.h` (ids 169–174), `nodus/oevm/src/platform.c`, `nodus/nsl/lib/kernel.nsl` (Platform accessors)
- Test: `nodus/tests/22_spacesync/39_journal_prims.nsl`

**Interfaces:**
- Consumes: Task 1's C functions.
- Produces (nsl, on `platform`): `journalOpen` → Boolean; `journalAppend: aBytes` → `List with: pos with: dropped`; `journalRetire: pos` → self; `journalReplayFrom: pos do: aBlock` (block takes `pos` and `Bytes`); `journalLastRetired` → Integer; `journalLostAtOpen` → Integer. Prims answer nil (never raise) when no journal exists, like the sensor seam.

- [ ] **Step 1:** Write `39_journal_prims.nsl` (`env: OEVM_JOURNAL=/tmp/oevm_journal_39` and an `rm` of it first via the test's setup convention): open → true, append two Bytes, replay answers both, retire the first, replay from `journalLastRetired` answers one. Expect block at the top per the file convention.
- [ ] **Step 2:** `python3 tools/run-one.py ../tests/22_spacesync/39_journal_prims.nsl B` — fails `not understood`.
- [ ] **Step 3:** Add the prims (pattern: the `otaStage*` cases at `platform.c:1553–1700`) and the kernel accessors.
- [ ] **Step 4:** Run it; passes.
- [ ] **Step 5:** Commit `journal: prims 169-174 and the platform accessors; refs #443`.

### Task 3: PortaBridge writes through the journal

**Files:**
- Modify: `nodus/nsl/lib/kernel.nsl` — PortaBridge `capture:value:` (~2232), `applyAck:` (~2677), the constructor (~2076, where `bootId` is read), `collectOutbound` (~2626)
- Test: `nodus/tests/22_spacesync/40_journal_replay.nsl`, `41_journal_wrap.nsl`; update `31_row_age.nsl` for the new row shape

**Interfaces:**
- Consumes: Task 2's accessors.
- Produces: a pending row is `List with: seq with: path with: kind with: value with: bid with: monoMs with: pos` (pos nil when no journal). The journaled record is the CBOR of `List with: path with: kind with: value with: bid with: monoMs`. `collectOutbound` emits `[seq, path, kind, value, bid, monoMs]`; cells emit their own boot's `bid` and the scan's `monoMs`. New public `journalHighWater` (max unacked rows seen).

- [ ] **Step 1:** Write `40_journal_replay.nsl`: bridge A captures 3 rows on `tel/log`, acks the first (fed `applyAck:` bytes, as `16_refused_node.nsl` does); bridge B built over the same journal file (stands in for the reboot) has `pendingSize` 2, its rows carry seqs 1 and 2 of B's space and A's `bid`/`monoMs`, in capture order, ahead of any cell. Write `41_journal_wrap.nsl` with `env: OEVM_JOURNAL_SECTORS=2`: capture until a rotation, assert `dropped` equals the rows lost and the newest row is still pending.
- [ ] **Step 2:** Run both; fail.
- [ ] **Step 3:** Implement: the constructor calls `journalOpen`, then replays from `journalLastRetired`, re-stamping each with `space stamp`; `capture:` appends and adds `dropped`; `applyAck:` retires the highest `pos` among rows it drops; `maxPending` eviction runs only when `journalOpen` was false; `journalLostAtOpen` is added to `dropped` at boot. `sys/panic` handling unchanged.
- [ ] **Step 4:** Run both, then `31_row_age.nsl` (updated: assert `bid` and `monoMs` instead of age) and `05_pending_bound.nsl` (unchanged, no journal) — all pass.
- [ ] **Step 5:** Commit `PortaBridge: write-through journal, replay at boot; refs #443`.

### Task 4: Health carries the journal high-water

**Files:**
- Modify: `nodus/nsl/lib/kernel.nsl` (the `Health` publish), `protocol/PROTOCOL-NSL-RECORDS.md` (the `obs/health` row, its size), `protocol/` manifest/CDDL that declares `Health`, `nodus/tests/22_spacesync/09_batch_size.nsl` (saturated size)
- Test: `09_batch_size.nsl`, `13_boot_id.nsl`

**Interfaces:**
- Produces: `Health { up, bid, wak, pto, sdr, ref, rej, jhw }` — `jhw` = `journalHighWater` (0 with no journal).

- [ ] **Step 1:** Update `09_batch_size.nsl`'s pinned worst-case `obs/health` size to include `jhw` at its saturated value; run it — fails.
- [ ] **Step 2:** Add `jhw` to the publish and to the record's declaration (mind `MANIFEST.cddl` key order = wire order).
- [ ] **Step 3:** Run `09`, `13`, and `go test ./...` in `protocol` — pass. Re-measure and write the new size in RECORDS.
- [ ] **Step 4:** Commit `obs/health: jhw, the journal high-water; refs #443`.

### Task 5: porta reads the new row shape and keeps per-boot anchors

**Files:**
- Modify: `porta/internal/wire/message.go` (`pushEntries`, `EncodePush`, the push envelope gains `mono`), `porta/internal/space/store.go` (`Entry`: replace `AgeMs` with `BootID`, `MonoMs`; `Ingest` time rule), `porta/internal/space/schema.go` (`boot_anchors(node_id, boot_id, anchor_ms)`; `events.ts_ms` nullable), readers that print `ts_ms`
- Test: `porta/internal/wire/wire_test.go`, `porta/internal/space/store_test.go`

**Interfaces:**
- Consumes: Task 3's wire shape.
- Produces: `Ingest` stamps `at` by the spec's rule 5 — same boot as the envelope: `arrival − (mono − monoMs)`; earlier boot with an anchor: `anchor + monoMs`; else NULL. Every push refreshes `anchor = arrival − mono` for the envelope's boot.

- [ ] **Step 1:** Write store tests `TestIngestSameBootTime`, `TestIngestEarlierBootAnchored`, `TestIngestUnseenBootNullTime`, `TestIngestReplayDuplicateStored` (same origin twice in two boots → two events, no error), and a wire test that a 5-element entry is refused.
- [ ] **Step 2:** `cd porta && go test ./internal/...` — fails.
- [ ] **Step 3:** Implement; every reader of `ts_ms` (find with `rg -n "ts_ms" porta custos`) prints NULL as `-`.
- [ ] **Step 4:** All three Go modules' `go test ./...` and `porta_sync_host.sh` pass (`PORTA SYNC OK`).
- [ ] **Step 5:** Commit `porta: rows carry their origin; per-boot anchors; refs #443`.

### Task 6: Zephyr backend and boards

**Files:**
- Create: `nodus/oevm/src/backend/zephyr/journal_zephyr.c`
- Modify: `nodus/oevm/zephyr/CMakeLists.txt` (link it when `CONFIG_FCB`), `nodus/oevm/zephyr/boards/esp32c6_devkitc_esp32c6_hpcore.overlay` (an `outbox` partition that moves nothing at or after `0x3A0000`; if the declared flash cannot fit 1020 KB, the largest that fits, recorded in the overlay comment), the C6 prj conf (`CONFIG_FLASH_MAP`, `CONFIG_FLASH_PAGE_LAYOUT`, `CONFIG_FCB`)
- Modify (ttgm): `firmware/boards/acc01.overlay` — SPI2 + the MX25R node copied from `~/workspace/ocm/firmware/boards/ocm1.overlay:87-100`, plus `outbox` partition; `firmware/conf/acc01.conf` — `CONFIG_SPI_NOR`, deep power-down support, `CONFIG_SPI_NOR_FLASH_LAYOUT_PAGE_SIZE=4096`, the FCB set
- Modify (ocm): `firmware/boards/ocm1.overlay` — `outbox` partition; the matching conf lines

**Interfaces:**
- Consumes: `journal_backend.h`. FCB over `FIXED_PARTITION_ID(outbox)`; `fcb_append`/`fcb_append_finish`, `fcb_rotate` when full, `fcb_walk`. Header record first in the FCB; FCB's own CRC covers records.

- [ ] **Step 1:** Implement `journal_zephyr.c` to the same contract; `journal_open` answers 0 when `!FIXED_PARTITION_EXISTS(outbox)`.
- [ ] **Step 2:** Build the C6 (`zephyr/run.sh`, no flash) and the OCM.1 / ACC01 images (with `--extra-module` per FOCUS); all link. Record each image's flash/RAM delta in the commit body.
- [ ] **Step 3:** Commit in rete `journal: Zephyr FCB backend, C6 outbox; refs #443`; commit the ttgm and ocm board changes in their repos, citing `rete#443`.

### Task 7: Board gate and close-out

- [ ] **Step 1:** On OCM.1 (or ACC01 with David's go — both are bench nodes in use): read the NOR's JEDEC id at boot (`0xc2 0x28 0x15`) — proves the flash node.
- [ ] **Step 2:** With porta unreachable, let ≥ 50 rows capture; power-cycle the node; restore porta. Every row arrives once (or twice in the duplicate window), in order; rows from the pre-cycle boot carry that boot's `bid`; their times are set (that boot was seen) — `porta` event listing shows it. Record append latency and rows/hour from the run.
- [ ] **Step 3:** Promote: `protocol/PROTOCOL-NSL-RECORDS.md` §3 (row shape, time rules, `jhw`), `porta/docs/ARCHITECTURE.md` (anchors), `nodus/oevm/docs/vm.md` (journal seam). Comment on #443 (≤ 5 lines) with the gate output, then close.
