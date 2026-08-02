# porta space-sync — the C6 hardware gate (plan 3 of 3) — Design

**Status:** approved 2026-08-01. Implementation plan to follow under
`docs/superpowers/plans/`.

**What this closes.** `~/workspace/porta/docs/design/2026-08-01-porta-space-sync-rewrite-design.md`
§9 states the gate for the space-sync rewrite: witnessed on the C6 over a real Thread
mesh, not simulated —

1. `goal/name` written at porta appears in the node's space;
2. `obs/health` pushed by the node appears in porta's cell tree;
3. `tel/log` entries arrive **exactly once** across a deliberate datagram drop.

Plan 1 built the Go gateway. Plan 2 built the node side and proved all three over UDP
loopback (`tuvm/tests/porta_sync_host.sh`), leaving the hardware run as the thing that
closes the rung. This design is that run.

Because legs 1–3 are already green on loopback in the same shape, a failure of legs 1–3
here is a *hardware or mesh* failure and not a protocol one. That separation is the
whole reason plan 2 ran first, and the gate is written to preserve it.

---

## 1. Topology

```
  laptop                              gw85224-01                      C6 devkit
  ──────                              ──────────                      ─────────
  tests/porta_sync_c6.sh   ──ssh──▶   porta (v2, plan 1)   ◀─Thread─▶  PortaBridgeNodeC6
    builds .oimg, flashes             :6969 udp, run as `david`        FTD; attaches, then
    holds ONE console handle          --db ~/porta-v2/space.db         the VM starts
    drives porta over ssh             --http-port 0                    peer fdc5:…:8567
    scores 4 legs from both ends      --test-drop-ack N (leg 4)        localPort 16970
                                      wpan0, fdc5:9109:9afe:97ea::/64
```

### D1 — v2 replaces v1 on :6969, retired once by hand

gw85224-01 runs `porta.service` **v1** (0.7.6, `ExecStart=/usr/bin/porta serve --db
/var/lib/porta/porta.db`, up since 2026-06-26, bound `*:6969`), and it still has live
LAN nodes checking in — 192.168.0.240 and .241, most recently 2026-07-30. Plan 1's
`serve` is a rewrite of that same command with the same `--udp :6969` default, so the
two collide on the port and on the binary name.

**Decision:** v1 is retired. One interactive step, with the operator present:

```bash
cp /usr/bin/porta ~/porta-v1-0.7.6      # the ONLY 0.7.6 rollback artifact:
                                        # the newest .deb in ~ is 0.7.1
sudo systemctl disable --now porta.service
```

**Consequences, accepted deliberately:** the LAN nodes at .240/.241 have no gateway from
that moment, and there is no migration between v1 and v2 — they are different wires, not
different versions of one. Filed at close-out rather than left to memory (§6).

**Why this shape:** `sudo` on gw85224-01 requires a password, so nothing needing root can
run unattended from the gate script. Retiring v1 once, by hand, means every subsequent
gate run starts and stops the v2 binary **as `david`** — 6969 is above 1024 — and no run
ever prompts. The systemd unit stays disabled; the gate runs the binary directly.

**Rejected:** a second instance on another port (non-destructive, but leaves two porta
processes and an addressing story that differs from every other deployment); running the
gateway on the laptop (would require the mesh to route to the dev box across a ULA OMR
prefix — unproven, and a prerequisite slice of its own).

### D2 — a fresh db per gate run, at a path v2 owns

`~/porta-v2/space.db`, never v1's `/var/lib/porta/porta.db`. The schemas are unrelated,
and leg 3 counts `tel/log` rows — a carried-over db makes "exactly once" unfalsifiable.

Fresh per *run*, not per *leg*: legs 3 and 4 deliberately share one db (§2).

### D3 — the node waits for the mesh; the gate waits for the node

`tuvm_thread_bringup` blocks until the role is CHILD or ROUTER before the VM starts, so
`PORTA NODE READY` on the console already means attached. The gate keys off that line
rather than sleeping a guessed interval.

### D4 — `--http-port 0`

v1's operator surface on :6970 goes away with it. Nothing in this gate wants it, and
leaving it off holds the blast radius to the one port being taken.

---

## 2. The legs

| leg | stimulus | asserted at porta | corroborated at the console |
|---|---|---|---|
| 1 | `porta put c6-bench goal/name c6` | `porta get c6-bench obs/name` contains `c6` | — |
| 2 | none — an idle check-in is the heartbeat | `obs/health` and `obs/build` present; `chip=esp32c6` | `PORTA NODE READY` |
| 3 | ≥ 10 ticks, no drop | `tel/log` rows == distinct `tick N`, ticks contiguous | per-tick line |
| 4 | ≥ 10 further ticks, `--test-drop-ack 3` | still rows == distinct, still contiguous | `misses` > 0 |

Ten ticks at the 2000 ms cadence inherited from plan 2 is ~20 s per leg; with
`--test-drop-ack 3` that is at least three swallowed acks, so leg 4 cannot pass on a
single lucky exchange.

**Between legs 3 and 4 the gateway is restarted onto the flag, keeping the same db, and
the node keeps running.** The flag cannot be toggled in place, and a fresh db would reset
porta's high-water to 0 while the node's `acked` kept climbing — which would break the
contiguity assertion for reasons that have nothing to do with the drop. Reusing the db
also means the tick series runs unbroken across the restart, so leg 4 incidentally
witnesses that the high-water survives a gateway restart. The node sees the restart
window as ordinary loss and re-pushes, which is the designed behaviour (D6).

### D5 — assertions live at porta; the console corroborates

Every leg is asserted from the gateway side (`porta get` / `porta log` over ssh), because
porta is the independently written implementation: if it says `obs/health` is there, two
codebases agree about one wire. The console supplies only what porta cannot see — the
node's own `misses` and tick counts — and a leg fails if either side disagrees.

**Rejected:** porta-only (cannot distinguish "the drop was injected and survived" from
"the drop never fired", which is precisely leg 4's claim); console-only (the node
reporting its own success is not evidence, it is the thing under test).

### D6 — the drop is a gateway flag, and it fires *after* the commit

`--test-drop-ack N` (default `0` = off) swallows every Nth outbound `#ack` **once the
push has been applied and the high-water advanced**.

That ordering is the entire point. Dropping *before* the commit only reproduces a lost
push: the node re-pushes, porta applies once, and the assertion passes without ever
exercising the `seq <= hw` dedupe. Dropping *after* it is the case design §9 asks for —
porta has applied the entries and moved its high-water, the node's `acked` has not moved,
so the next check-in re-pushes the same rows under the same seqs, and only the gateway's
dedupe stands between that and a duplicate.

The loss semantics that make this the interesting direction (read from
`nsl-tuvm/lib/kernel.nsl`, `PortaBridge>>checkIn` / `pushOnce` / `applyAck:`):

- **lost PUSH** → porta never saw the entries; `acked` unchanged; next check-in
  re-pushes → delivered once. Exactly-once holds trivially, and proves nothing.
- **lost ACK** → porta applied them; `acked` unchanged; the node re-pushes the same seqs.
  The dedupe is the only thing under test. **This is the leg.**
- **lost PULL or BATCH** → the cursor does not advance, the same range is re-pulled, and
  cell application is idempotent by construction.

**Rejected:** an `ip6tables` OUTPUT rule on the gw (most faithful — the datagram really
dies in the stack — and feasible, since otbr runs `--network host` and `wpan0` with
`fdc5:9109:9afe:97ea::/64` is in the host netns; but every toggle needs a sudo password,
so the gate could not run unattended); a node-side drop knob (changes the thing under
test); a deliberate reboot (a real and worthwhile failure, but it resets cursor and
bootId to their RAM defaults and so proves full-replay behaviour, porta#29, not
exactly-once).

### D7 — exactly-once is two assertions, not one

`rows == distinct` catches duplication. **Ticks forming a contiguous 1..K run** catches
loss. Plan 2's host gate checks only the first, because loopback loses nothing; a radio
can, so plan 3 checks both — and the contiguity assertion is folded back into
`porta_sync_host.sh` so one definition of exactly-once serves both gates.

### D8 — a drop that never fired must fail leg 4, not pass it

The gate requires positive evidence from both ends: porta logs each swallowed ack, and
the node's `misses` — already carried in `obs/health` as `pollTimeouts`, and printed per
tick by the C6 node — must be non-zero. If neither moves, leg 4 fails as *drop never
injected*.

### D9 — a sibling node file, not a parameterised one

`zephyr/PortaBridgeNodeC6.nsl` is plan 2's node with C6 constants (`c6-bench`, peer
`fdc5:9109:9afe:97ea:9541:d963:f8d9:8567`, port 6969, `chip #esp32c6`) plus one console
line per tick — `tick N misses M cursor C acked A` — which the host node does not print.
The constants and that line are the *only* differences; the plan states that a change to
one body is a change to both.

This matches the repo's existing per-board idiom (`PulseNode` / `PulseNodeDK`,
`ThreadEchoNodeDK`) and leaves the host gate's node untouched so plan 2 keeps passing.
**Rejected:** editing the one node to be C6-first (the two gates would then share one set
of constants, so a change for one silently moves the other); a build-time overlay (no
drift and no duplication, but it invents a parameterisation mechanism the toolchain does
not have — a slice of work before any hardware runs).

---

## 3. Bench facts and hazards

Verified on 2026-08-01, not assumed:

- **The board is one C6 with two USB interfaces.** `/dev/ttyACM0` (USB-JTAG) and
  `/dev/ttyUSB0` (CP210x) both answer ESP32-C6 QFN40 rev v0.0, MAC
  `40:4c:ca:ff:fe:45:47:a4`.
- **Flash and console are different interfaces.** `run.sh` flashes the C6 over the
  USB-JTAG (`/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_40:4C:CA:45:47:A4`);
  `prj_esp32c6.conf:30` documents the Zephyr console as uart0 → the CP210x bridge
  (`/dev/serial/by-id/usb-Silicon_Labs_CP2102N_…`).
- **Therefore the console handle is opened BEFORE the flash and held across it.** The
  flash cannot disturb it and the boot banner is captured in full. The standing
  "esptool's reset eats the banner" hazard does not apply on this board — it applies
  where flashing and console share one interface.
- **Ports are addressed by `/dev/serial/by-id/`, never by ttyUSB/ttyACM number** — the
  idiom `run.sh` already uses, and the by-id name embeds the MAC, so identification is
  free. `esptool read_mac` remains the confirmation of last resort.

Hazards the gate is written against:

- **One handle, binary-safe.** `tests/porta_c6_console.py` is the only opener, decoding
  with `errors="replace"`: a second reader silently splits the stream, and one NUL byte
  has turned a grep monitor binary before, which reads exactly like a dead node.
- **Attach is slow and can fail on its own.** The gate distinguishes *never attached* (no
  role line — a mesh/radio problem, nothing to do with porta) from *attached but no
  check-in* (a protocol or address problem). Different diagnosis, different owner.
- **A 512 B batch is not one radio frame.** The 802.15.4 PHY payload is 127 B, so a
  full-size datagram is five or six 6LoWPAN fragments and losing any one kills the whole
  datagram. Real loss is therefore *expected* even with the flag off, and the gate never
  requires `misses == 0` anywhere — it requires no duplicates and no gaps. If loopback and
  radio ever diverge, this is where it shows, and that is a finding worth having.
- **`awaitReply` takes the first datagram that arrives, whatever it is.** A batch delayed
  past the pull's budget and arriving during the push's wait is consumed as if it were
  the ack; `applyAck:` rejects it and the real ack is missed. Microseconds on loopback,
  milliseconds over a mesh — a plausible source of intermittent failure. Named in advance
  so it is recognised rather than re-debugged, and if it fires it is a real bridge defect
  to file, not a gate bug.
- **On any failure**, both sides' logs are kept and porta's stderr is checked for
  `rejecting datagram: malformed message` — that message names the offending field.

---

## 4. Components

**`~/workspace/tuvm`**

| file | fate | responsibility |
|---|---|---|
| `zephyr/PortaBridgeNodeC6.nsl` | create | the C6 node: plan 2's body, C6 constants, per-tick console line |
| `tests/porta_c6_console.py` | create | the single console handle; tees to a log the gate greps |
| `tests/porta_sync_c6.sh` | create | the gate: build → flash → console → drive porta over ssh → score 4 legs |
| `tests/porta_sync_host.sh` | modify | add the contiguity assertion (D7) |
| `review/tests/CLAIMS.md`, `review/program-sequence.md`, `docs/next-steps-overview.md` | modify | close-out promotion |

**`~/workspace/porta`**

| file | fate | responsibility |
|---|---|---|
| `internal/wire/server.go` | modify | the drop hook: a counter and one branch on the outbound ack path, *after* the commit, with the ordering explained in the doc comment |
| `internal/portacli/serve.go` | modify | `--test-drop-ack N`, default 0, help text marking it test-only |
| `internal/wire/server_test.go` | modify/create | with N=3 the third ack never leaves **and** the store shows that push applied |
| `~/workspace/tuvm/protocol/PROTOCOL-NSL.md` | modify | close-out |

No change to `deploy/` — the gate runs the binary directly and the unit stays disabled.

Build line for the node:

```bash
./run.sh --board esp32c6 --node PortaBridgeNodeC6.nsl --flash \
  --conf-extra "prj_esp32c6_thread.conf;prj_esp32c6_thread_ftd.conf"
```

(`--flash` is required — `DO_FLASH` defaults to 0. The two conf fragments are
semicolon-joined and later wins, so the FTD overlay must follow the Thread base.)

FTD, not MTD: a mains-powered bench node that should hold the mesh rather than depend on
a parent. The role is a build fact, so it is stated here rather than discovered later.

---

## 5. What closes the rung

1. `go test ./internal/wire` green, including the new drop-flag test. The mechanism is
   checked before it is trusted to score hardware.
2. `tests/porta_sync_host.sh` still green after D7's contiguity assertion — a regression
   check that a stronger definition did not change what plan 2 proved.
3. `tests/porta_sync_c6.sh` green **5 consecutive times**, unattended, on the real mesh.
   Runs are logged. A single flake is not rounded up to a pass, and §3's `awaitReply`
   window is the first suspect.

Five rather than tuvm#59's 20/20: this gate is minutes per run, not seconds, and the
property under test is deterministic rather than probabilistic — the radio's own
flakiness is what the contiguity assertion already tolerates.

---

## 6. Promotion and what is filed rather than built

**Promotion** (the close-out; the dated design stays behind as the historical "why"):

- `~/workspace/tuvm/review/tests/CLAIMS.md` — the space-sync row moves from
  `DONE (host); hardware gate is plan 3` to done, citing `tests/porta_sync_c6.sh` and the
  drop leg.
- `~/workspace/tuvm/review/program-sequence.md` — the rung closes, stating what the radio
  added over loopback: fragmentation-driven real loss, and the `seq <= hw` dedupe
  witnessed rather than reasoned about.
- `~/workspace/tuvm/protocol/PROTOCOL-NSL.md` §4.1 — one paragraph on what hardware taught,
  including the 512 B / 127 B fragmentation consequence, which is a protocol-sizing fact
  and not a bench anecdote.
- `~/workspace/tuvm/docs/next-steps-overview.md` — porta → channels moves off
  "plan 3 next".

**Filed, not built:**

- porta: the orphaned v1 LAN nodes at 192.168.0.240/.241 lose their gateway when v2 takes
  :6969, with no migration between the wires (D1).
- tuvm: the `awaitReply` reply-mismatch window (§3), filed whether or not it fires — it is
  a real narrowing of the bridge's assumptions that loopback could not have surfaced.
- Anything the five runs turn up that is not this rung's business.

**Inherited and still open**, each already filed by plan 2: porta#29 (cursor/bootId in
RAM), porta#30 (root space vs a capability pair), porta#31 (constant cadence), porta#32
(a stream entry written while the bridge is down is lost). tuvm#107 keeps
`diag/spans/result` out of the sync until its ~1120 B value is split.
