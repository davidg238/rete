# Profile through porta: the last 0.1 lifecycle row (rete#415)

> Status: **Q1, Q3–Q7 and Q2′ DECIDED (David, 2026-10-12); Q8 DECIDED (a), one-shot, and Q9 DECIDED (a) (2026-10-13); footprint re-banked (2026-10-13). Host half BUILT on branch `415-profile-porta` (see As built); the bench half waits.** #415 asks to settle whether "profile local or OTA" (#26's
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

## Q8: a profile request that survives sleep (DECIDED (a), David, 2026-10-13; Q9 DECIDED (a))

David: "can we save the address on the target to nvm, so that ... in wakeA tell it we want to profile + custos
address, next time it wakes, wakeB, it profiles for the set period or until it sleeps again?"

The address needs no NVM, because `goal/console` is a cell, and porta replays every cell into each cold boot's first
check-in. The request is different. A `dbg/req` row is served once, ever (#262). It profiles the rest of wake A,
from its check-in to its sleep (P3), and never anything before a check-in. Two facts make David's version cheap:
the manifest already carries a `goal/profile/<app>` cell (`Profile{action, durationS, continuous}`) that nothing
implements (the #342 spike found it dead), and the boards keep RTC_NOINIT memory across deep sleep (`g_rtc[]`,
`main.c`; the boot counter).

**Q8. How does a profile request reach a node through porta? DECIDED: (a), a cell, retained** (David, 2026-10-13:
"a profile is a ONE time trigger, until triggered again"). (b), the `dbg/req` line alone, and (c), both, were the
alternatives. porta's `dbg/req` stops carrying `profile`; the cable keeps the `profile <ms>` line. This revisits
#312 Q1's "the same line reaches A and A′".

**One-shot, and why the cell needs an id.** The recommendation as first written ("every later boot opens a window",
cleared by custos after the dump) profiled every wake until the clear landed. That is not a one-time trigger, and the
clear races the next boot. A cell is also replayed into every cold boot's first check-in, so a cell alone cannot say
"already done". Hence `Profile{ms, id}`. `id` is the request's identity: custos's request number, rising. The node keeps
`(ms, id)` armed and `served` (the last id it dumped) in RTC memory. A cell whose id is `served` does nothing. A new id
arms it. After its one dump, `served := id`. A new request is a new id. An empty cell disarms.

**Q9. When does a window open on a request that arrives mid-wake? DECIDED: (a)** (David, 2026-10-13: "simple !"). David: "if a profile request arrives mid-wake,
it starts next". Taken literally, the always-on bell never boots again, so it would never profile.
- **(a) At once, and carried once.** *Recommended.* The window opens on receipt. If it reaches `ms`, it dumps and
  is served: the bell's case, and a long wake's. If sleep cuts it first, it dumps nothing, stays armed, and the next
  boot opens a fresh window of `ms` before `main:`. That window ends at `ms` or at sleep, whichever is first, and it
  dumps and is served either way: David's wake B. One request, one dump, one rule, and no per-node mode.
- **(b) Next boot only, and at once on a node that never sleeps.** Two rules, and the node must know which kind it is.
- **(c) At once, and the cut dump is kept as well.** Two dumps per request on a sleepy node, and custos must decide
  which answers the request.


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
  through porta, the two board profiles, the drain measurement, deleting `OEVM_LOG_UDP6`) waits for David.

## As built, Q8/Q9 (2026-10-13, branch `415-profile-porta`, not merged)

- **Footprint re-banked** (David: "re-bank the footprint"): host 76,929 → 78,837, MCU text 45,930 → 46,893
  (`ac854ecf`, the attribution in the baseline headers). The Q8 work below adds no C text: it is the kernel's.
- **protocol:** `goal/profile` = `Profile{id: uint, ms: uint}` replaces `goal/profile/<app>` and its dead
  `{action, durationS, continuous}` (the three symbols deleted, `sym-id` added). Descriptor `live`, with a
  validity rule on `id`. Golden `d81b82…a2d82762696407d827626d73191388` (id 7, ms 5000).
- **kernel:** the bridge applies `goal/profile` beside `goal/console`; a bad record is logged on `tel/log`.
  The Profiler keeps the request in RTC slots 10–13 (ms, id, served, a check word); `OEVM_RTC_FIELDS` is 14 on
  host and board, and an app still owns 0–9. The rule is Q9 (a). The cable's and `dbg/req`'s `profile <ms>` line
  is unchanged: it is #312's command reader, which the debugger shares, and it is not retained.
- **custos:** the porta node's request is the cell, with a new id per request: the clock's seconds below 2^30, or
  one more than the last, so a restarted custos never repeats an id the node served. porta's `PUT dbg/req` (T2)
  stays, as a general route for the debugger's lines, though custos's profile no longer uses it.
- **Gates:** `23_mprof/10_profile_cell` (red with the served check deleted: `replay 2`); `test_deepsleep_node`'s
  `test_profile_request_survives_one_sleep`, four wakes over one RTC file (carried, from boot and cut, the replay
  ignored, a new id served); `porta_sync_host.sh` leg 8c through the cell, **PORTA SYNC OK** (195 methods
  ranked); the full suite on msi (`415-sync-4`), **303 passed, 0 failed, every aux gate green** (footprint
  included). Go: protocol and porta green, custos green but #430. Not compiled for a board: the change there is
  `g_rtc`, 40 → 56 B of RTC memory.
- **F7 (for the bench).** A window opened from boot dumps at `ms` or at sleep. If `ms` passes before the first
  check-in delivers `goal/console`, the dump meets a tee with no target. Today the build-time `OEVM_LOG_UDP6`
  boot target covers it. Once that literal is deleted (Q2′), the ring holds 4 KB and the paced writer gives up,
  so such a dump would be lost. The bench run will show whether a MagTag wake ever reaches `ms` before its
  check-in. If one does, the answer is to keep the tee's target in RTC as well.

## One mechanism, the kept target, and the first board (2026-10-13, David: "a single mechanism for both board test and porta?", "keep the console address in sleep memory too", "do the board test")

- **One mechanism.** Every request, porta's `goal/profile` cell or a `profile <ms>` line from the cable or the
  debugger's loop, is one request with an id, through `Profiler>>requestMs:id:`. It is retained, served once, and
  carried across a sleep that cuts it. A line names no id, so the kernel mints one below every id it has armed or
  served (negative, so it never meets a cell's). The Profiler no longer listens on `dbg/req`; in a debug image
  `dbg/req` is the debugger's line, and its `profile` command reaches the same entry. No board scenario used the
  `dbg/req` line; three August probes (`ReplayProbeC6`, `ProfileRestartProbeC6`, `ProfileCostNodeC6`) describe it
  and are historical.
- **The tee target is kept** (F7 closed): `log_udp.c` keeps the last target, or off, in RTC no-init memory with a
  check; the host keeps it beside `OEVM_RTC_FILE`. A node that wakes or restarts tees from its first line.
- **Bench: c6b** (the second ESP32-C6, `c6bench`'s CP2102N on opti7040), over Thread, against a test porta on gw
  (its own database, the new manifest, UDP 6979 / HTTP 6980) and the branch's custos on gw, advertising gw's
  mesh-local address. The node is `PortaBridgeNodeC6` with `gwPort` 6979 (an uncommitted copy). The production
  porta (`porta-398`, the wall bell's and the MagTag's) was not touched: restarting it on the new manifest was
  refused by the session's permission check.
  - **F8, the tee lost the dump's tail over Thread.** A 272-line dump teed its first 82 lines at the radio's
    ~4.3 ms each, then one in 25, and never the `end` line, so custos stayed Pending. The ring dropped nothing and
    `sendto` reported nothing: the socket accepts the datagram, and OpenThread drops it later when its message pool
    is full. A first fix (a short back-off on `ENOMEM`, a 2 s give-up) changed nothing (98 of 272), because no error
    reached the sender. **Fixed** by waiting, before each datagram, until OpenThread's pool has 32 of its 128
    buffers free (at most 1 s, then send anyway): **278 of 278 lines, `end` included, in 1.27 s.**
  - **F9, custos ranked mixed windows.** A capture that had waited through the truncated dump kept its 98 lines and
    then took the next whole one: 294 rows for `methods=208`. **Fixed:** a new `mprof[window]:` header starts the
    dump again (the kernel's cut line just before it is kept).
  - **Results, after both fixes:** three agent requests, each `POST /board/c6b/profile/capture?ms=10000` through
    custos, the test porta, the cell and the tee: answered 12–13 s after the POST, **204 rows for 204 methods and
    205 for 205, no warnings**, the map's sha checked against the node's `obs/apps/main`. A stale map was refused
    with 409, naming both hashes.
  - **The kept target and the served id across resets.** A cable flash's reset pulses EN, a power-on reset that
    clears RTC no-init memory. The node then boots holding its console until `goal/console` arrives, and the
    replayed `goal/profile` cell profiles once more, because the served id is gone too. A program push through
    custos (161,240 B, staged over Thread, confirmed 40 s after the POST) reboots into the trial by a **software**
    reset. Its boot teed from the Zephyr banner on: `logudp: teeing console to [...]:48187 (kept from the last
    goal/console)`. No window opened from the replayed cell. So a power cycle repeats the last profile once, and a
    deep sleep or a software reset does not.
- **Not done:** the wall bell and the MagTag. Each needs a cable flash, because the tee and the kept target are
  firmware. Neither is cabled, and the production porta needs the new manifest. The drain time before deep sleep
  (the 2 s guess) needs the MagTag. `OEVM_LOG_UDP6` is not deleted yet (Q2′: after a board's run on the cell; c6b
  ran with no literal and was carried by the cell alone).
- **Bench: the MagTag (2026-10-13 noon).** The production porta (`porta-398`) now runs `MANIFEST-415.cddl` (David:
  "restart porta"). The MagTag was cable-flashed on msi (S2 ROM download mode, David's hand; the first cable was
  charge-only), with its goal written to the new image first (#426). The firmware has no build-time console
  literal. A second custos on gw (127.0.0.1:6991, `--advertise` gw's ULA `…fe2f:710`) serves it against the
  production porta.
  - Its boot teed whole once `goal/console` arrived, over WiFi, with no literal (program `ed7c71ee…`).
  - **Q9 seen on hardware:** a 3 s request taken at the 12:00 wake opened at the check-in near the end of an 8 s
    wake, and the node printed `profile: window carried to the next wake`.
  - **F10, open: the last lines before a sleep never arrive over WiFi** (`sleep: arming`, `space:`, `deep sleep`).
    That is true of the drain as built (the ring empty) and still true with the fix (the ring, Zephyr's net_pkt TX
    slab and OpenThread's send queues empty, `4480c256`). So the frames die below Zephyr, presumably in the WiFi
    blob's queue. A window cut by sleep therefore loses its dump's tail on the MagTag. Next: measure what the old
    fixed 150 ms delivered, and look for a WiFi-driver flush or TX-done count. The firmware keeps the drain's time
    and prints `logudp: the last sleep's drain took N ms` at the next wake's boot, which is the data the 2 s cap waits on.
  - Re-flashing at 12:17 (firmware 2) cleared the RTC request, as an EN reset does. The replayed cell re-arms at the
    next check-in, so the carry test continues at the next wakes.

## As built, F10 and F11 (2026-10-13 afternoon to 2026-10-14, branch `415-profile-porta`, not merged)

- **F10 closed: the WiFi blob's queue.** The driver (`esp32_wifi_send`) copies each frame into the blob with
  `esp_wifi_internal_tx` and frees the net_pkt at once, so an empty Zephyr TX slab says nothing about WiFi. The
  firmware now counts what the blob is given (the link wraps the driver's one call: `--wrap=esp_wifi_internal_tx`)
  and what it reports sent (its TX-done callback, registered at the first frame). The drain before a deep sleep waits
  until the two agree (`1d70fa23`). That delivered the tail (`space:`, `deep sleep`) at the next sleeps, and the 13:08
  wake then showed the second loss: a dump lost 116 of 338 lines in runs of 6 to 12, because the blob holds
  `CONFIG_ESP32_WIFI_DYNAMIC_TX_BUFFER_NUM` frames and refuses the next, which the driver drops out of the sender's
  sight. So the sender also paces on the count: before each datagram it waits while half that many frames are in
  flight, as F8 paces Thread (`a6639aaa`).
  - **Result (firmware 4, the 09:04 wake, 2026-10-14):** a window from boot, cut by the sleep, dumped **256 of 256
    rows, header and `end` included, in 1.2 s**, then `space:`, `deep sleep` and the drain's own line: `wifi frames
    given 338, done 338, refused 0`. The callback covers data frames one for one, so the pacer never waited out its
    1 s cap.
  - 349 lines arrived before that line, 11 more than the frames counted, with no duplicates. Not a second path: the
    drain prints its counts *before* it waits, while the dump's tail is still in the ring. Firmware 6's cold boot
    shows it directly: `given 59, done 59, refused 0; lines sent 34`, with 38 lines arrived before it.
- **F11: a timer wake's boot lines, up to `ip-evt`, never arrive** (nor, on some wakes, `wifi: up` and `dbg:ready`
  after it). The kept target tees from the first line, and a refused line is held, so these were *accepted*. Found
  in Zephyr (`subsys/net/ip/ipv6_nbr.c`): while the next hop's link address is unresolved, ND parks **one** packet
  on the neighbour entry and discards each later one ("already pending"), and `net_ipv6_prepare_for_send` still
  answers `NET_CONTINUE`, so `sendto` reports success. Before association the carrier is off and `sendto` fails, so
  the lines wait in the ring; the moment it comes up, the ring goes out as a burst behind the one parked line, and
  the burst dies. **Fix** (`c4d1b187`, `00a165a4`): before each datagram the sender waits until the stack holds no
  more TX packets than it held just before its last send. A synchronous send (`CONFIG_NET_TC_TX_COUNT=0`) that
  leaves one more behind is parked on ND, and it leaves when the neighbour answers. The first version waited for an
  *empty* slab; on c6b (Thread) the slab is never empty, every line waited the whole second, the paced dump stalled
  the reactor and the observer rebooted it at 30.9 s (`reason=1`). Against the pre-send count, c6b's boot burst goes
  out at ~4 ms a line and a 211-method window dumped **211 of 211 with `end`**, no reboot.
  - **On the MagTag the ND fix was not enough** (firmware 6, the 18:32 wake): `wifi: up` and `dbg:ready` now arrived,
    but the boot up to `ip-evt` still did not, and these lines were *sent* (`lines sent 358`, about 11 more than
    arrived) and reported done by the blob. Right after association the link-local address is still tentative and the
    ULA waits for the router's advertisement, so the stack picks an unspecified or link-local source, and gw drops the
    datagram. (gw has no tcpdump and no root, so this is inferred from the counts, not seen on the wire.) **Fix**
    (`3719e108`): a line waits in the ring, as for a missing route, until the stack can pick a source the destination
    can answer: preferred, and not link-local unless the destination is.
  - **Result (firmware 7, the 19:17 wake):** the log starts at `*** Booting Zephyr`, then `build-id`, `teeing … (kept
    from the last goal/console)` and **`logudp: the last sleep's drain took 20 ms`**; the cut window dumped 256 of 256
    with `end`; `given 365, done 365, refused 0`. c6b on firmware 7: 211 of 211, no reboot. **F11 closed.**
  - **The drain's cap.** The 2 s cap has waited since `4480c256` on that number: one sleep's drain, 20 ms, with an
    idle tail. A dump's tail was measured only as its whole send (1.2 s for 256 rows, paced). The cap stays at 2 s, a
    bound no measured drain approaches, rather than a tuned number; a sleep that hits it says so in the next boot's line.
- **The wall bell (2026-10-14 evening).** Cabled to msi (David), its goal first pointed at the cable's image (#426:
  `ShipsBellNodeC6-415.oimg`, `35eff2a1…`, blob-put and `goal/apps/main` seq 10), then `flash-bell.sh` from the
  branch (`c5b0b66d`): `slot a was written by the cable … active, unsigned, no trial`, no restage. A third test
  custos on gw (127.0.0.1:6992, `--porta-node bell=206ef1195438` against the production porta, advertising gw's
  mesh-local address) wrote `goal/console` at the first check-in, and the bell's boot arrived **whole from the
  Zephyr banner, over Thread, with no build-time literal**. That is Q2′'s condition, so **`OEVM_LOG_UDP6` is
  deleted**, with its port. An agent's `POST /board/bell/profile/capture?ms=5000` answered 8 s later: 183 rows for
  `methods=184`, custos warning of the missing row (5,700 of 5,701 activations), `Kernel_Socket>>receive` first at
  19 %. One line of 186 lost over Thread with no gap over 40 ms in the dump; c6b's 211 of 211 says it is not the
  rule. Not chased.
