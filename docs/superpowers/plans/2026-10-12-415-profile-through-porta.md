# Plan: profile through porta (rete#415)

Design: `docs/design/2026-10-12-415-profile-through-porta-design.md` (Q1–Q7 and Q2′ decided; Q8 open). Branch
`415-profile-porta`. Written overnight 2026-10-12, after David went to bed ("plan overnight"). Tasks 1–7 do not depend
on Q8. Board work (Task 8) waits for David: nothing here flashes or pushes to a board.

## Found while planning (read from the code, to be confirmed on a board)

- **F5. The dump never reaches the tee.** `mprof_report` (`vm.c`) writes with `fprintf(stderr)`. The tee catches
  `printk` (its hook) and nsl's `out` (`platform_console_write` calls `log_udp_tee`), and nothing else. A board's
  `stderr` goes through picolibc's stdout hook straight to the UART. No record shows a profile dump crossing the UDP
  console: every board profile on file was read from the cable. D4's "the console is already teed" was true of the
  console and not of the dump.
- **F6. The tee's ring is 4 KB, and a dump is about 10 KB written in one burst.** The ring drops the newest
  characters when full, so even a teed dump would lose its tail, including the `end` line custos waits for.

## Tasks

1. **protocol: `goal/console`.** `Console{addr: tstr, port: uint}` in `MANIFEST.cddl` (`sym-Console`, `sym-addr`,
   `sym-port`; keys in wire order), `descriptors/records.yaml`, PROTOCOL-NSL's path table, RECORDS (a §4.5 note:
   the rows leave by the tee, whose target is this cell). An empty `addr` turns the tee off.
2. **porta: the PUT takes `dbg/req`.** `PUT /node/{id}/dbg/req` writes a stream row (`cval.KindStream`). `goal/**`
   stays a cell, and every other path stays refused. Unit test in `ops_test.go`.
3. **VM: the tee's target at run time (prim 164 `consoleTee:port:`).**
   - Seam (`platform_backend.h`): `platform_console_tee(addr, port)` answers 1 when the address parses and 0
     when it does not (`""` turns it off and answers 1); `platform_console_err(bytes, n)` is the dump's write
     (stderr plus the tee); `platform_console_drain(max_ms)` waits for queued tee lines to leave.
   - Zephyr (`log_udp.c`): guarded by `CONFIG_NET_SOCKETS`, not `OEVM_LOG_UDP6`. With no target the ring holds the
     boot chatter (as now, before the route) until the first target arrives. After an off, lines are discarded.
     `platform_console_err` paces the writer, which is the VM thread, never an ISR. It waits while the ring is
     too full for the line, up to a budget, so a dump is slowed and not truncated (F6). The deep-sleep wait
     (`main.c`, 150 ms under `OEVM_LOG_UDP6`) becomes a drain.
   - posix (`platform_posix.c`): a UDP socket, one datagram per console line, the same seam, so the host gate
     exercises the same path (Q6).
   - `vm.c`: `mprof_report` writes through `platform_console_err` (F5).
   - Deleting `OEVM_LOG_UDP6` waits for the bell (Q2′). Until then the build-time literal only sets the boot
     target, and the cell overrides it.
4. **kernel: apply `goal/console`; close a window before sleep.** The bridge's `applyRow` calls the platform tee on
   a `goal/console` cell, as it hands `goal/debug/main` to the debugger, so a waking node re-tees before its work.
   A bad record is counted and logged, never raised. `Kernel>>deepSleep:` closes an open window first, printing
   `profile: window cut by sleep`, so the dump leaves before the VM ends (P3). Tests: `nodus/tests/23_mprof/`.
5. **custos: a porta node with a console.** `--advertise <ula>` turns it on. Each porta node gets a UDP sink on an
   ephemeral port, its console verbs, and a `goal/console` write at start-up (`Console{addr: <advertise>, port:
   <bound>}`). `--map` is allowed on a porta node, and with a map the node serves `profile`:
   `POST /profile/capture?ms=N` checks that the map's sha256 is `obs/apps/main`'s running hash (a profile ranked
   against another program's map is the silent failure), writes `profile <N>` to `dbg/req`, and answers
   `{Pending: true, Seq}` at once. The dump is taken from the sink's lines. `/profile/last` answers the pending
   request until the `end` line, then the same `Capture` as the cable. The report counts `mprof:` rows against the
   header's `methods=` (F3).
6. **Gate: `porta_sync_host.sh` leg 8c.** custos serves the host node as a porta node (`--advertise ::1`, its map),
   and one `POST /profile/capture` reaches a ranked `/profile/last` through porta, the cell and the tee. The C unit
   `test_platform` (or a new one) covers the posix tee. Go units cover the route and the porta profile against a
   fake porta and a UDP sender.
7. **Docs.** The design's "As built"; custos `docs/console.md` (a porta node's console); PROTOCOL rows; FOCUS at
   `/conclude`.
8. **Bench (morning, with David).** Update gw's porta manifest to one carrying `goal/console`. Push the bell an
   image whose target comes from the cell (Q5/P4). Profile the bell through porta; profile the MagTag across a wake
   (P3); count rows against the header over Thread (F3). Then delete `OEVM_LOG_UDP6` (Q2′).

## Close-out (promotion)

The knowledge goes to `protocol/PROTOCOL-NSL-RECORDS.md` §4.5 (the rows' path and the cell), the peer design's D19
addendum (custos's porta node), paper E §9's lifecycle table, and #26's 0.1 table.
