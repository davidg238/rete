# Profile through porta: the last 0.1 lifecycle row (rete#415)

> Status: **Q1, Q3–Q7 and Q2′ DECIDED (David, 2026-10-12); Q8 open. Host half BUILT on branch `415-profile-porta` (see As built); the bench half waits.** #415 asks to settle whether "profile local or OTA" (#26's
> lifecycle table) is built or open work. **Answer: half built.** The trigger through porta is built and gated; the
> rows' way back to an agent is not, nor is custos's verb for it, nor the sleepy-node case. Decided upstream
> and not reopened: profiling is in the standard image, and one command reader in every image is fed by the
> cable or by `dbg/req` (#312 Q1–Q2, `2026-10-05-custos-profile-over-usb.md`); agent first, with HTTP as the only agent
> surface (peer design D11–D12); porta's operator routes are general twins of its CLI (#398 Q1); a push answers
> at once and is polled (#398 Q2); porta and custos share only `protocol/` (#338).

## Where it stands

| Piece | State | Where |
|---|---|---|
| The trigger: a `profile <ms>` line on `dbg/req` opens a self-limiting window | **built**, every image | `Kernel>>Profiler` (`nodus/nsl/lib/kernel.nsl`, `3c886c27`); gate `porta_sync_host.sh` leg 8b, `04_dbg_req_chain` |
| The counter in the shipped image | **built** on the C6 by default; on the MagTag's debug+profile image | `e76c8095`; `528285b0` (#380/#379) |
| The rows: `mprof[…]` / `mprof:` / `mprofa:` lines from C, on the console | **built**, and *by decision* never on the space | method-profile design D4–D5; RECORDS §4.5 |
| A console off the cable: the UDP tee (`log_udp.c`), a build-time literal IPv6 target | **built**; on the MagTag (→ msi :54330). Not recorded for the wall bell, and FOCUS names no console for it | `nodus/oevm/zephyr/CMakeLists.txt:332` |
| custos reading a tee: `--udp-log <name>=<port>` | **built** (D16) | `custos/internal/console/sink.go` |
| custos ranking a dump: `profile.Parse` / `Rank` / the page | **built**, cable only | `custos/internal/profile/`, `/profile` (#312, #364) |
| porta's write route reaching `dbg/req` | **not built**: `PUT /node/{id}/…` refuses anything outside `goal/` | `porta/internal/portacli/ops.go` (#398 P1) |
| custos's porta node serving `profile` | **not built**: it serves `load` only | `custos/internal/custos/board.go` (#398 P2) |
| A window on a deep-sleep node | **not built**: the window closes on a timer, `deepSleep:` never returns, and the table is RAM, so a wake shorter than the window dumps nothing | #381 (open) |

So the row is open work, and the work is to close the return path, not to build the trigger.

## Findings

**F1. The bulk path is not porta, and that was decided on purpose.** D4 (2026-08-06) rejected the table over
`dbg/resp`: a dump is ~200 rows. At about 20 B per `mprof:` line and 30 B per `mprofa:` line that is roughly 10 KB,
which is about 20 check-ins at the 512 B batch, or ten minutes on the wall bell's 30 s cadence. RECORDS §4.5
states it as a claim of record. "Through porta" therefore means the request goes through porta and the rows come
back by the node's console tee.

**F2. The tee goes to an address baked into the image, and nothing requires the bake.** `OEVM_LOG_UDP6` is a
literal ULA chosen at build time. The rows land on whichever machine the image names (msi for the MagTag). The
reason recorded for it (`CMakeLists.txt:332`) is that the mesh has no DNS, so the address must be a literal. That
argues for a literal, not for a *build-time* one. `log_udp.c` parses the string with `inet_pton` when the socket
opens, so a target supplied at run time costs one prim. (David, 2026-10-12: "why can't porta tell the device which
custos to report to?")

**F3. UDP drops rows without saying so.** The mesh loses about 4.6 % of datagrams (PROTOCOL-NSL §batch sizing). One
console line is one datagram, so a 200-row dump over Thread expects about nine missing rows. The header
`mprof[<why>]: methods=N …` gives the true count and the `end` line marks the close, so custos can **detect** a
short dump. It cannot recover one.

**F4. A sleepy node needs the window to close before sleep.** `dbg/req` is served once per application, ever (#262),
so a request reaches exactly one wake. The MagTag's wake is shorter than any useful window. Unless the window
closes and dumps before `deepSleep:`, the request is spent and no rows come back. That is the second half of
#381's title. Its first half, profiling a cold boot before porta has answered, is a different question.

## Proposal

**P1. porta: the `PUT` route also takes `dbg/req`.** porta's inbound authority is exactly `goal/**` and `dbg/req`
(PROTOCOL-NSL §3). The #398 route admits only the first, so this change aligns the route with the rule rather than
widening it. The value is a bare `tstr` (RECORDS §4.5). This adds no new route. The same change gives debug through
porta its first HTTP path; until now it has been driven by `porta put` from a shell on the gateway.

**P2. custos: a porta node with a console serves `profile`.** A porta node and a UDP sink of the same name are one
board (`--porta-node bell=206ef1195438 --udp-log bell=54330`). `GET /boards` lists `profile` for it only when both
are present. `POST /board/bell/profile?ms=N`:

1. `PUT /node/<id>/dbg/req` with `profile <N>`, through `--porta-http`;
2. answer at once with `{Seq, Pending: true}`. The window opens at the node's next check-in, not at the POST
   (#398 Q2's rule, for the same reason);
3. the board's console store is watched for the next `mprof[<why>]: … end`. `GET /profile/last` answers
   `Pending` until it arrives, then the ranked report the cable path already produces (`profile.NewReport`),
   plus `Rows` against the header's `methods=` (F3) as a warning when they differ.

One answer shape: the cable's `/profile` and the porta board's `/profile/last` return the same `Report`. The
cable's capture blocks because its window is known to open now, and the porta's is polled because it is not.
That is the same split as load.

**P3. The sleepy node: close the window before sleep (#381's second half, taken into this row).** `Kernel>>
deepSleep:` closes an open window first (`profileStop` dumps with `why = sleep`), so a window longer than the wake
is cut at sleep and **says so**, not lost. The ms is still the user's bound and the wake is the node's. No new
field, and no window carried across sleeps: the table is RAM, and carrying it would mean an RTC-retained 2 KB
table, a tunable with no data to choose it from.

**P4. The wall bell gets a tee, delivered by the deploy row.** Rebuild the bell image with `OEVM_LOG_UDP6` set to
the custos host's stable ULA, routed by the Thread border router, and **push it through porta**
(`POST /board/bell/load`). Then the bell profiles with no cable, and its new image arrived with no cable.

**P5. Gates.** Host: `porta_sync_host.sh` or `sleepy_debug_host.sh` gains a leg where custos serves a host VM as
a porta node with a console, and `POST /profile` → `/profile/last` ranks a window (see Q6 for how a host VM's
console reaches the sink). Boards: the wall bell (a window over Thread, rows counted against the header), and
the MagTag (one wake's window, `why = sleep` when it is cut). Unit: the route's `dbg/req` admission, and the
porta board's profile against a fake porta plus a fake sink.

## What is not in this issue

- **Rows through porta** (F1's rejected alternative). This is Q1, but D4 stands unless David reopens it.
- **Cold-boot profile** (#381's first half): profiling the boot before porta answers needs the request to be
  waiting at boot, which is the replay question #262 closed.
- **Recovering lost rows** (F3). What would earn it: a measured loss on the bell that changes a ranking.

## Questions for David

1. **Keep D4: rows by the console tee, request through porta (P1–P2)?** Recommended: yes. The alternative,
   rows on `dbg/resp`, reverses D4 and D5 (nsl would have to read the table, or C would have to write into the space). It
   costs about ten minutes per dump on the bell, and it would put a profiler blob on the plane every other blob
   stays off. In its favour: one path, any agent anywhere (F2), and nothing lost (F3).
2. **Pair a porta node with its console by name (P2), or a new flag** (`--porta-node bell=<id>,udp=54330`)?
   Recommended: by name. Both flags exist, and a board is already "whatever shares the name" (D15).
3. **Answer at once and poll `/profile/last` (P2), as load does?** Recommended: yes, for the reason #398 Q2
   gave. A block cannot be bounded for a sleepy node.
4. **Take #381's close-before-sleep into this row (P3), and leave its cold-boot half on #381?** Recommended: yes.
   Without it the MagTag cannot be profiled at all, and the 0.1 demo names it.
5. **Give the wall bell a tee and push it through porta (P4)?** Recommended: yes. Its alternative is a cable on a
   wall clock. Before building, the bench check is whether the border router routes the tee to opti7040 or msi.
6. **The host gate's console.** Recommended: the posix backend honours `OEVM_LOG_UDP6` as the board does, so the
   host leg exercises the same sink. The alternative is a host-only console source in custos, which is a second
   mechanism and the reason D16 has one store.
7. **Band.** #415 is P3. It is the last 0.1 row, so it should be P1, as #233 was raised.

## Q2′: the console's target as a goal cell (after David's question, 2026-10-12)

**P2′.** Replace the build-time target with a cell: `goal/console` = `Console{addr: tstr, port: uint}`, where an
empty `addr` turns the tee off. It is the same shape of thing as `goal/debug/main`: a whole-value cell, replayed into every
cold boot's first check-in, so a deep-sleep node re-tees on each wake. A porta node in custos gains its console
by custos writing that cell (one `PUT`, the #398 route) with its own reachable ULA (`--advertise <ula>`, since
a dev machine has several interfaces and only the operator knows which one the border router routes to) and a
port custos binds for that board. The pairing flag disappears: a porta node *has* a console because custos
told the node where to send it. Two developers with two custos can profile two nodes on one mesh, each node
teeing to the custos that last wrote its cell. On one node, the last writer wins, which is the same contention
`goal/debug/main` already has.

What it costs: one prim (set the tee target), the kernel applying the cell, a manifest record, and custos's
write. What it gives up: console lines before the first check-in are not teed. The cable still has those, and
the profile window only opens after a check-in anyway. Admission is CIDR only, as for #398 Q3. The same gap now
also lets anyone in range redirect a node's console, which belongs in #253's note.

**Q2′. Is the tee target a `goal/console` cell, set by custos through porta, and is `OEVM_LOG_UDP6` deleted?**
Recommended: yes to both. One mechanism, not a build-time one plus a run-time one. Deletion lands after the
wall bell shows the cell working (P4 then becomes "push an image without the bake, then write the cell").

**Q2′ DECIDED (David, 2026-10-12): yes, and build it.**

## Q8: a profile request that survives sleep (David's question, 2026-10-12; awaiting his answer)

David: "can we save the address on the target to nvm, so that ... in wakeA tell it we want to profile + custos
address, next time it wakes, wakeB, it profiles for the set period or until it sleeps again?"

The address needs no NVM, because `goal/console` is a cell, and porta replays every cell into each cold boot's first
check-in. The request is different. A `dbg/req` row is served once, ever (#262). It profiles the rest of wake A,
from its check-in to its sleep (P3), and never anything before a check-in. Two facts make David's version cheap:
the manifest already carries a `goal/profile/<app>` cell (`Profile{action, durationS, continuous}`) that nothing
implements (the #342 spike found it dead), and the boards keep RTC_NOINIT memory across deep sleep (`g_rtc[]`,
`main.c`; the boot counter).

**Q8. How does a profile request reach a node through porta?**
- **(a) A cell, retained.** *Recommended.* Replace the dead record with `Profile{ms}`. On receipt the node opens a
  window at once, which covers the always-on bell, and keeps `ms` in RTC memory, so every later boot opens a window
  before `main:` (wake B, from its boot). The window closes at `ms` or at `deepSleep:`, whichever is first. An
  empty cell clears it, and custos clears the cell once a dump arrives. porta's `dbg/req` stops carrying
  `profile`, and the cable keeps the same `profile <ms>` line. This revisits #312 Q1's "the same line reaches A and
  A′".
- **(b) The `dbg/req` line, as decided.** No NVM, no boot coverage. Delete the dead `goal/profile` record.
- **(c) Both.** Two mechanisms.

The build on 2026-10-12 does not depend on Q8. custos's request is one PUT whose path and value follow the answer.

## As built (2026-10-12, overnight, branch `415-profile-porta`, not merged)

Plan: `docs/superpowers/plans/2026-10-12-415-profile-through-porta.md`. Two findings came from reading the code while
planning. **F5:** the dump never reached the tee. `mprof_report` wrote with `fprintf(stderr)`, which reaches a board's
UART and nothing else; the tee caught only `printk` and nsl's `out`. No record shows a dump that crossed it. **F6:** the
tee's ring is 4 KB, and a dump is ~10 KB written in one burst.

- **protocol** (T1): `goal/console` = `Console{addr: tstr, port: uint}` in `MANIFEST.cddl`, the descriptors
  (with a validity rule: the cell says where the node was told to send, not that anyone listens), PROTOCOL-NSL's table,
  and RECORDS §4.5 and its path table.
- **porta** (T2): `PUT /node/{id}/dbg/req` writes a stream row. `goal/**` stays cells, and nothing else is admitted.
- **VM** (T3): prim 164 `consoleTee:port:`, behind `platform_console_tee` / `_err` / `_drain` (`platform_backend.h`).
  posix sends one datagram per line. Zephyr's `log_udp.c` is built on `CONFIG_NET_SOCKETS`, not `OEVM_LOG_UDP6`. It
  holds the boot in the ring until the first target arrives, and discards lines after an off. A cell repeating the
  current target makes no new socket. The dump writes through `platform_console_err`, paced while the sender makes
  progress (F6). The fixed 150 ms wait before deep sleep is now a drain of at most 2 s. That 2 s is a guess, to be
  replaced by the bell's measured drain time. `OEVM_LOG_UDP6` survives only as the target a board boots on.
- **kernel** (T4): the bridge applies `goal/console` where it hands `goal/debug/main` to the debugger. A bad record,
  or an address that does not parse, is logged on `tel/log` and changes nothing. `Kernel>>deepSleep:` cuts an open
  window first (`profile: window cut by sleep`, then the dump).
- **custos** (T5): `--advertise <ipv6>` gives every porta node a mesh sink on an ephemeral port, its console verbs,
  and a `goal/console` write at start-up. That write is repeated before every profile request, because porta may be
  down at start-up. With `--map`, the node also serves `profile`. A map for another program is refused with 409.
  A node that publishes no `obs/apps/main` is ranked with an `UNCHECKED` warning. A newer request supersedes a
  pending one. `Capture` gains `Pending`, `Seq` and `Error`. A dump cut by sleep is a `CUT` warning. The page and the
  TUI re-read a pending capture every 5 s. The sink shares the cable's subscriber list (`console.fanout`).
- **Gates:** `22_spacesync/30_console_tee` (red with the apply removed); `23_mprof/08_sleep_cut` (red without the
  cut) and `09_sleep_no_window`; C units 43/0 here; custos Go units, including the whole path against a fake porta
  and a UDP sender; **`porta_sync_host.sh` leg 8c**, an agent's request through custos and a real `porta serve`,
  47 methods ranked from the teed dump and no `mprof` row on the space (**PORTA SYNC OK**, 2026-10-12, opti7040).
- **Board builds (msi, compile only; nothing flashed):** the bell (`ShipsBellNodeC6`, its recipe) links with sram0 at
  361,792 B against master's 355,344 B. The +6,448 B is the tee's ring and thread, now in an image that had no tee.
  The MagTag (`MagTagDeepSleepNode`, its T10 recipe) links at dram0 220,176 B against master's 220,048 B with
  `OEVM_LOG_UDP6` baked: +128 B, 2,032 B left. Found on the way: `MagTagSleepyDebugNode.nsl`'s header recipe overflows
  dram0 by 9,040 B on master. It is stale, and not this issue's.
- **The full nsl suite (msi, `415-sync-2`):** 302 passed, 0 failed. Two aux gates were red. `nsl-tools-py` wanted a
  golden for `console`, now added (`415-sync-3`: the node's encoder and cval agree byte for byte, 3/3). The
  **footprint ratchet** is red and is **David's call**, because every re-bank has been his:

  | | bank | master (`5e1d6d45`) | branch | of which #415 |
  |---|---|---|---|---|
  | host text | 76,929 | 78,029 (+1.4 %, passes) | 78,837 (+2.5 %) | +808 B: the posix tee, prim 164, `mprof_out` |
  | MCU text | 45,930 | 46,796 (+1.9 %, passes) | 46,893 (+2.1 %) | +97 B |

  Most of each overrun is drift already on master; #415 tips both over 2 %. The proposed re-bank is host 76,929 →
  78,837 and MCU 45,930 → 46,893, with the attribution above.
- **Not done, and why:** the bench (plan Task 8: gw's porta manifest, a bell image without the literal pushed
  through porta, the two board profiles, the drain measurement, deleting `OEVM_LOG_UDP6`) waits for David. Q8 is open.
