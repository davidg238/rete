# #443 — the outbox journal: unacked stream rows survive a reboot

_2026-11-14. Issue rete#443 (P1). Decisions are David's unless marked **(Claude's call)**._

## Why

Rows captured while the link is down live only in PortaBridge's RAM `pending`
(`nodus/nsl/lib/kernel.nsl`, `capture:value:`, cap `maxPending` 64, oldest evicted to `sdr`).
A reboot loses them: the ACC01's overnight captures died at its 10:59 reboot on 2026-10-24
while the Pixel's Tailscale was off. A link or power gap should **delay** rows, never lose them.

## Goals / non-goals / anti-goals

- **Goal:** every captured stream row reaches porta, in order, across link outages and reboots,
  until the journal itself wraps.
- **Goal:** `obs/health` reports the backlog high-water, so link quality can be judged (David, 2026-10-24).
- **Non-goal:** cells. A cell is re-read at check-in; only stream rows (which the reactor kills on
  delivery) need storing.
- **Non-goal:** inventing capture times. A row's time is only as good as what porta saw of its boot.
- **Anti-goal:** a second queue. RAM is a window onto the journal, never a store of its own.

## Approaches (David picked B, 2026-11-14: "simplicity")

| | A. Two stages | **B. Write-through (picked)** |
|---|---|---|
| How | 16-row RAM ring spills to flash when full | every captured row is appended to flash at capture |
| Unplanned reboot | loses ≤ 15 RAM rows | loses nothing |
| Moving parts | two queues + the hand-off | one queue |

Energy did not separate them: a ~50 B row write is ~20–30 µJ and a 4 KB erase ~6 µJ per row,
~0.03 J/day at ~800 rows, while a chip left in standby (~5–8 µA) costs ~1.5–2 J/day. **The
energy rule is therefore deep power-down between uses, required in either approach.** (Figures
from memory of the MX25R1635F datasheet; the builder confirms them against the datasheet.)

## Decided

1. **Write-through journal.** `capture:value:` appends the row to a flash journal before it
   joins the push window. A board with no journal partition behaves exactly as today (RAM, 64).
2. **Storage = Zephyr FCB** (`subsys/fs/fcb`, present in both `~/ncs` and `~/zephyr-esp32`) over a
   `fixed-partitions` node labelled `outbox`. **255 sectors × 4 KB = 1020 KB** (FCB's
   `f_sector_cnt` is a `uint8_t`). A linear ring: when full, FCB rotates and the oldest 4 KB sector
   is dropped; its unacked rows are counted into `sdr` (David: capacity 1 MB, drop oldest first).
3. **Flash geometry:** 1-byte writes (the `jedec,spi-nor` driver's own `write_block_size`, `spi_nor.c:293`),
   4 KB erases via `CONFIG_SPI_NOR_FLASH_LAYOUT_PAGE_SIZE=4096` (driver default 64 KB). The
   `erase-block-size`/`write-block-size` properties in ttgm's old `mezrit_acc01.dts` are ignored by
   current Zephyr and that file is not built; it is left alone.
4. **Row identity:** the journal stores each row with its origin `(bid, monoMs)` (rete#444's pair,
   which totally orders a node's observations across reboots without a clock). A row replayed in a
   later boot gets a **fresh seq in the new boot** at replay; its origin travels with it. The
   restart-rewind rule (`protocol/PROTOCOL-NSL.md`, "A RESTART is declared…") is untouched.
5. **Time is honest** (David: "be honest about what we can do … maintain sequence"):
   - row from the push's own boot → arrival − age, exactly as today (error = one-way transit);
   - row from an earlier boot porta saw at least once → that boot's clock anchor + `monoMs`;
   - row from a boot porta never saw → **no time**; it is ordered by `(bid, monoMs)` and porta
     stores it as "between the neighbouring timed rows".
6. **Boards (the parts we have):**

   | Board | Flash | Change |
   |---|---|---|
   | ACC01 | MX25R1635F, SPI2, CS P1.06 | add the NOR node (copy `~/workspace/ocm/firmware/boards/ocm1.overlay:94`, same chip and pins) + `outbox` partition to ttgm `firmware/boards/acc01.overlay`; layout page size in `firmware/conf/acc01.conf` |
   | OCM.1 | MX25R1635F (node exists) | add the `outbox` partition in ocm's overlay |
   | C6 | internal | an `outbox` partition that moves nothing at or after `0x3A0000` (the Thread dataset in `storage`) |

   Drivers: `CONFIG_FLASH`, `CONFIG_SPI_NOR` (+ its deep-power-down support) on the MX25R boards,
   `CONFIG_FLASH_MAP`, `CONFIG_FLASH_PAGE_LAYOUT`, `CONFIG_FCB` everywhere a journal exists.

7. **A journal the firmware cannot read is erased** (David, 2026-11-14). A journal header carries a
   format version; a mismatch after a reflash, or a failed CRC, erases the partition and starts clean,
   and the rows lost are counted. Unsent rows at a format-changing reflash are an accepted loss
   (no back-compat).

## Claude's calls (correct any)

- **(Claude's call) Retirement by ack marker.** FCB cannot delete one entry, so an advancing ack
  appends a small marker naming the last retired journal entry. Boot walks the journal; entries after
  the last marker are unacked and replay. A marker is written only when an ack retires journal rows
  (≤ one per pushed batch), never per check-in.
- **(Claude's call) One row shape on the wire.** A push entry becomes
  `[seq, path, kind, value, bid, monoMs]` and the push envelope carries the node's `monoMs` now.
  porta computes the age itself for the current boot and keeps one clock anchor per `(node, bid)`,
  `arrival − monoMs-now`, refreshed on every push of that boot. This adds per-boot state to porta,
  which rete#444 avoided — the price of rule 5. No back-compat: both ends reflash together.
- **(Claude's call) Health:** `obs/health` gains one field, the journal backlog high-water in rows.
  Under B, RAM holds only the push window, so a separate RAM high-water would measure `maxRows`.
- **(Claude's call) The C seam is opaque bytes:** `append(bytes) → dropped-count`, walk-from-marker,
  `retire(position)`. nsl owns the row encoding (CBOR); C owns flash and FCB.
- **(Claude's call) A host backend** (`backend/posix/`) implements the same seam over a file, so the
  nsl suite can gate replay without a board.

- **(Claude's call) Boot order.** Before the bridge first pushes: walk the journal from the last
  marker and re-stamp every unacked row with this boot's seqs, in journal order, so replayed rows
  take the lowest seqs of the boot, ahead of any cell write. The new `bid` triggers porta's existing
  restart rewind; replayed rows are then ordinary rows of this boot, so the frame stays ascending
  and ack/high-water logic is unchanged.
- **(Claude's call) At-least-once.** A reboot between porta committing a row and the node writing
  its marker replays that row once. porta does not dedupe; a duplicate is possible only in that
  window and carries the same origin `(bid, monoMs)`, so a reader can spot it.
- **(Claude's call) `pending` becomes the push window** over the journal when one exists;
  `maxPending` and its eviction apply only to a board without a journal. `sys/panic`'s re-offer
  (`panicSeq`) is unchanged and its row is journaled like any other.
- **(Claude's call) Wrap accounting.** The boot walk keeps an unacked-row count per sector in RAM;
  a rotation adds that sector's count to `sdr` (a since-boot counter, as today). Dropped rows leave
  a gap in origin `(bid, monoMs)`, never in seq, because seqs are assigned at replay.
- **(Claude's call) An untimed row** is stored with a null time; porta's store and readers must accept
  null and show it as unknown.
- **(Claude's call) No row-rate target.** Append is ~1 ms of SPI in the capture path; the board
  gate measures the ACC01's real rate and capture latency, and that measurement is what would earn
  any batching.

## Gates (agent first: host test, then real VM)

- Host: a suite test captures rows, "reboots" the host VM over the same journal file, and asserts
  they are pushed again in order with their origin; a wrap test asserts oldest-first drop and `sdr`.
- porta: Go tests for the three time rules in decision 5.
- Board: OCM.1 or ACC01 with porta unreachable, a power cycle mid-outage, then reconnect — every row
  arrives, in order, timed where its boot was seen. Read the chip's JEDEC id first (proves the NOR node).

## Left to the builder

The C seam's exact prim names; the marker's encoding; the C6 partition's offset and whether the C6's
flash fits 1020 KB (else the largest that fits, recorded); where `capture:` calls the append.

## Promotion on close

`protocol/PROTOCOL-NSL-RECORDS.md` §3 (row shape, time rules, health field),
`porta/docs/ARCHITECTURE.md` (per-boot anchor), `nodus/oevm/docs/vm.md` (the journal seam).

## Addendum 2026-11-21 — board gate, measured on OCM.1

PASS on OCM.1's MX25R1635F (master `cfa3b099`, ocm `b1a0343`). The JEDEC id check is the
open itself: `journal: outbox 255 x 4096 B` prints only after Zephyr's spi-nor driver has
accepted `jedec-id [c2 28 15]`. With porta blocked, 6 rows were captured. A J-Link reset
followed (a reset clears RAM `pending` as a power cycle does). After the unblock, gw stored
boot 3's seq 1–6 once each and in order, timed at capture (12:27–13:47), then boot 4's first
row. Rate with the room empty: 1 row per 16 min. Capture latency: not measured, so batching
is still unearned. The plan's "≥ 50 rows" became ≥ 5 (David): 50 < `maxPending` 64 exercised
no refill, and refill and wrap are covered by host tests 40 and 41. Detail: #443.
