# Profile through porta: the last 0.1 lifecycle row (rete#415)

> Status: **DRAFT, Q1–Q7 for David** (2026-10-12). #415 asks to settle whether "profile local or OTA" (#26's
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

**F2. The tee goes to an address baked into the image.** `OEVM_LOG_UDP6` is a literal ULA chosen at build time.
The rows land on whichever machine the image names (msi for the MagTag), and the custos that ranks them must run
there. For 0.1, with one bench and one dev machine, this is fine. It is not "any agent anywhere", which is a gap
to record rather than fix here.

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
- **A tee that follows the agent** (F2): a runtime-set tee target is a `goal/` cell and a reconnect story.
  What would earn it: a second dev machine that needs a remote node's rows.
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
