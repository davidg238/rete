# Tutorial 2 — a board: flash a node, watch it join a mesh, run the gates

A real ESP32-C6, a real 802.15.4 radio, and the eight-hundred-odd lines of harness that
exist because a board can fail in ways a host cannot. By the end you will know which cable
does what, why a console you attach to after boot reads zero bytes, what a healthy boot
prints, why half the scenarios in `nodus/oevm/tests/target/` deliberately have no radio in
them, and what a full sweep of the target gates actually costs in wall-clock time.

> **This page is NOT executed, and that is the one thing to know before you trust a line
> of it.**
>
> Tutorials [1](01-host.md) and [3](03-porta.md) carry `bash gate` blocks that the nsl
> suite extracts and runs on every pass, so a stale step there reads red within four
> minutes. **This page carries none, and it cannot.** Every command below needs a devkit on
> a USB cable, a Zephyr toolchain, and — from §5 on — a Thread mesh and a running gateway.
> A gate that degraded to a SKIP without hardware would be a gate reporting success for a
> run that never happened, which is the failure `nodus/oevm/tests/target/run.sh` refuses in
> its own header and the reason it is not part of the nsl suite. (One claim here *is* checked
> — §7's list of scenario names is a fact about this repository rather than about a board, so
> the suite compares it against the manifests. Nothing else on this page is.)
>
> So the page's honesty is its own gate, and here is how it is kept: **every transcript
> below is copied out of a bench log, and carries the date it was recorded.** Nothing here
> is illustrative output written to look plausible. Where something could not be run, the
> page says so rather than inventing it. If you find a command here that no longer works,
> nothing told you — that is the exposure this page has and the other two do not, and it is
> why the executable half of the board story lives in `tests/target/` instead.

## 0. What you need

- Everything [tutorial 1](01-host.md) needed, and tutorial 1 run once — the host VM and its
  layout file are what the compiler builds a board image against.
- **An ESP32-C6 devkit** (`esp32c6_devkitc`). The tree also ports to the ESP32-WROOM-32
  (`esp32`) and the Adafruit MagTag / ESP32-S2 (`esp32s2`), but the C6 has been the primary
  target since #57: it is the only one of the three with a native 802.15.4 radio and a
  console that survives a target reset, which together make it the board everything else is
  debugged on.
- **The ESP32 west workspace and the Zephyr SDK.** These live outside the repo, and they are
  deliberately isolated from the nRF one:

  ```
  ~/zephyr-esp32/          west workspace + .venv         (ESP32 family)
  ~/zephyr-sdk-0.17.4/     the toolchain, incl. arm-zephyr-eabi-gcc
  ~/ncs/                   the nRF workspace — a different tree, on purpose
  ```

- **For §5 onward:** a second Thread device or a border router, and a `porta serve` the node
  can reach. `~/workspace/rete/docs/tutorials/03-porta.md` is the gateway half, on loopback,
  with no board at all — read it first if you have not.
- **Time.** A build-and-flash cycle is about two minutes. One target scenario is 2–12
  minutes. A full sweep of all fourteen is **70–100 minutes and is not unattended** (§7).

## 1. What a board tests that a host cannot

The whole nsl suite runs on the host, and the host is structurally blind to one seam.

`nodus/oevm/src/timer_posix.c` posts a fired timer **straight into the space** via
`space_post`. `nodus/oevm/zephyr/src/timer_zephyr.c` posts it **through the pulse mailbox**,
from an ISR. The host path has no mailbox, so on a host a timer event can be neither
coalesced, nor shed, nor left stale — three behaviours that only exist on a board, and every
one of which has produced a bug.

That is why there are two test trees and not one. It is also why the nsl suite prints, on
its own aux line:

```
  target gates: NOT RUN BY THIS SUITE — the posix timer backend bypasses the pulse
    mailbox, so coalescing, shedding and stale handles are UNTESTED here.
    (target-observe-py above is the harness's OWN logic, not the seams — #216.)
    Run: nodus/oevm/tests/target/run.sh   (12 scenarios, needs a board)
```

rather than a green SKIP. It is deliberately not coloured like one either: a skip names a
cause it has not verified, and this names one that *is* verified. A positive statement of
what a suite cannot test is worth more than a silent pass — and the reader most likely to be
misled by "278 passed, 0 failed" is the one who reads that line and nothing else.

## 2. Two cables, and which one is which

**Each C6 devkit enumerates two host adapters.** This is the single most useful fact about
the board, and getting it backwards costs hours.

| adapter | what it is | what uses it |
|---|---|---|
| **CP2102N** (`/dev/ttyUSB*`) | the devkit's own USB-UART bridge | the harness: **flash *and* console**, one cable |
| **USB-Serial-JTAG** (`/dev/ttyACM*`) | the SoC's own peripheral | hand-driven `zephyr/run.sh` only |

The harness uses one adapter for both jobs deliberately —
`tests/target/lib/deploy_flash.sh` sets `ESPTOOL_PORT` *and* `CONSOLE_PORT` from the single
adapter it resolved and vetted. If it detected the ports itself it could flash one board and
read another board's console, and **every symptom of that reads as "the board went quiet"**.
Since 2026-08-22 the convention on this bench is that the JTAG cables are simply unplugged:
the harness never needed them, and one port per board instead of two was the actual fix for
port scarcity.

**Boards are identified by their by-id hash, never by `ttyUSBn`.** `ttyUSB*` numbering is
allocation order, and this bench has had three physically identical C6 devkits on it at once.
Look before every deploy — which board is on which cable changes *within* a session, not just
between:

```bash
ls -l /dev/serial/by-id/
```

Here is that command's real output on the two machines of this bench at 14:08 on 2026-08-29.
It is worth walking through, because the answer it gives is not the obvious one:

```
# the workstation — the machine with the toolchain
usb-Espressif_USB_JTAG_serial_debug_unit_40:4C:CA:44:6E:60-if00  -> ../../ttyACM0

# gw85224-01 — the machine with no toolchain
usb-Silicon_Labs_CP2102N_..._ac2dce6f...-if00-port0              -> ../../ttyUSB0
```

Two different boards, one cable each, and neither could run a target scenario: the workstation
had `c6spare`'s JTAG node but not its CP2102N, so that board could be flashed and never read;
`gw85224-01` had `c6bench`'s CP2102N but no toolchain, so that board could be read and never
flashed.

A second cable went onto the gw board twenty minutes later, giving `c6bench` **both**
channels — and the gates still could not run, because the missing piece was never a cable.
`run.sh` and `deploy_flash.sh` build and flash in one process on one host, and the host with
the toolchain now had no board on it at all. What closes that gap on this bench is the hand
path in §5 plus `OEVM_BOARD_NAME`, the one door `run.sh` opens for a board it cannot see (#181; it carried the
node id then, and carries the board name now that the id is read from silicon, #118).

The general shape, and the reason it is in a tutorial rather than a bench note: **counting
adapters answers a different question than "can I run a gate."** Enumerate what is attached,
*and* read the code that will open it, on the host that will open it.

**A board exists for the harness exactly when its collector env file does.** There is no
table of hashes anywhere in the tree; both deploy paths resolve a board name through
`~/.config/rete/console-<board>.env`:

```
RETE_CONSOLE_PORT=/dev/serial/by-id/usb-Silicon_Labs_CP2102N_..._ac2dce6f...-if00-port0
```

A table in the code that disagreed with the collector's config would send the flash to one
board and read the console of the other. One home, two readers —
`nodus/oevm/tools/bench-identity.sh` (#170, #185).

When more than one adapter matches a glob, `run.sh` **aborts and names every match** rather
than taking `head -1`. That is not pedantry: `head -1` over an ambiguous glob picks by serial
hash, and doing exactly that overwrote the wrong board on 2026-07-16.

## 3. The collector: the console is always on

Every board on this bench has a systemd **user** unit holding its CP2102N open forever,
timestamping each line and rolling one log per day:

```bash
systemctl --user status rete-console@c6bench
ls ~/.local/state/rete/console/
python3 nodus/oevm/zephyr/tools/console/logread.py since -- -180s
```

There are three reasons it is always-on rather than attached when needed, and each one is a
bug that was paid for:

1. **The boot banner is already gone by the time you attach.** `esptool` ends a flash with a
   hard reset, so the node has booted before any reader could open the port. The banner is
   what a gate anchors on.
2. **On a C6, opening the port does *not* reset the board** — so attaching to an
   already-running node yields **zero bytes**, and that silence is evidence of nothing. (The
   WROOM-32's shared UART is the opposite: *any* open reboots it. Applying the WROOM rule to
   a C6 makes an empty read look like a dead board; that cost seven probes on 2026-08-04.)
3. **A tool that attaches at failure time to ask what happened reboots the node** on the
   boards where rule 2 does not hold, and then reads a fresh boot. On 2026-07-21 that
   produced an "all pools full" reading that was briefly taken as evidence of a crash, on an
   issue whose whole subject was a node going unreachable.

Two rules for reading the logs:

- **One handle per port.** Two readers on one tty silently *split* the byte stream — each
  drops about half the lines, which reads as missing output rather than as contention. When
  replacing a reader, confirm the old one is dead before starting the new one.
- **Filter by the timestamp field, never by grepping the time string.** The collector
  prefixes an ISO timestamp, and message bodies contain times of their own —
  `rtcalarm: armed 13:59:55` is *logged* at 13:30. `grep 13:59` matches it. Use the field:

  ```bash
  awk -F'T' '$2 >= "13:59"' console-c6bench-2026-08-29.log
  ```

## 4. The radio split — the mistake that costs a board cycle

`prj_esp32c6.conf` gives you `NETWORKING`, `IPV6`, `UDP` and `SOCKETS` — **and no L2.** A C6
built without the Thread overlays has no interface at all, and the failure is quiet:

```
--conf-extra "prj_esp32c6_thread.conf;prj_esp32c6_thread_ftd.conf"
```

Leave that off a node that binds a socket and the signature is (a) **no `thread:` lines** in
the boot log where a working node prints `thread: run=0 …, joining...` and `thread: attached
role=2`, and (b) the app printing a bare **`bind`** — which is `net udpBindPort:` answering
nil because there is no interface to bind to. Nothing errors. It took being diagnosed from
scratch twice.

**Most scenarios correctly have no `CONF_EXTRA`, and that is not a claim about the radio.**
Of the fourteen target scenarios, seven name no overlay, because their workloads open nothing:
`WedgeProbeC6`, `SlowTimerNodeC6`, `PulseCoalesceProbeC6`, `StaleHandleProbeC6`,
`RearmProbeC6`, `FastPollProbeC6` and `RunOnceProbeC6` all arm `timers every:` directly (or,
for `RunOnceProbeC6`, nothing at all) and never touch a socket. Only two nodes
bind — `PortaBridgeNodeC6` and `TimerlessNodeC6`.

The trap is that there is a *second*, unrelated "no `CONF_EXTRA`" convention in this tree:
`wedge-note` builds with `prj_esp32c6_wedgegate.conf` and `wedge-detect` deliberately builds
with **no** overlay, because the entire difference between those two scenarios is a synthetic
trigger in Kconfig. That convention is about the wedge gate. Conflating it with the radio
question cost a board cycle on 2026-08-22 (#236), and `zephyr/run.sh`'s #165 comment records
the same shape from earlier still.

For `TimerlessNodeC6` the bind is **load-bearing in a way that is worth understanding**,
because the node has no application timer at all: the base tick is registered by the *bridge*,
so no socket means no timer, no wedge, and no deadline for the liveness detector to judge
against. That coupling is a defect, not a design — it is open as **#274**, and the fix shape
(construct the bridge with `socket: nil` and arm liveness unconditionally) is on the issue.
Until it lands, `liveness-heartbeat` needs the radio overlay to test something that has
nothing to do with radios.

## 5. Flash one, by hand

`zephyr/run.sh` owns every step that has a wrong-by-default: the working directory, the seven
VM environment knobs, the per-board Kconfig and layout JSON, the esptool triple, the flash
offset, and restoring the tracked `src/solidified_image.c` afterwards. It owns **four things
it will not supply**, and it fails on them in this order:

1. **`--board` takes a board ID, not a bench name.** It is `esp32`, `esp32s2` or `esp32c6`,
   and it dies on `c6spare`. The bench names belong to `tests/target/run.sh --board` and to
   the collector units — two different flags, spelled alike.
2. **With two candidate adapters attached it refuses to guess**, at port resolution, before
   it builds anything. It refuses each port separately, one error at a time, so pin both.
3. **It does not source the west environment**, and says so only as `west: command not found`
   after a successful bake — or `west: unknown command "build"` if the venv is on `PATH` but
   `ZEPHYR_BASE` is not.
4. **It builds by default and flashes only with `--flash`.** There is no `--build-only`.

The env block that satisfies all four is the one `zephyr/flash-bell.sh` carries (its
`source … activate` plus three exports); copy that rather than reconstructing it:

```bash
source ~/zephyr-esp32/.venv/bin/activate
export ZEPHYR_BASE=~/zephyr-esp32/zephyr \
       ZEPHYR_SDK_INSTALL_DIR=~/zephyr-sdk-0.17.4 \
       ZEPHYR_TOOLCHAIN_VARIANT=zephyr

ESPTOOL_PORT=/dev/serial/by-id/usb-Silicon_Labs_CP2102N_..._<hash>-if00-port0 \
CONSOLE_PORT=/dev/serial/by-id/usb-Silicon_Labs_CP2102N_..._<hash>-if00-port0 \
bash nodus/oevm/zephyr/run.sh --board esp32c6 --node PortaBridgeNodeC6.nsl \
     --conf-extra "prj_esp32c6_thread.conf;prj_esp32c6_thread_ftd.conf" \
     --pristine --flash --capture 60
```

Notes on the pieces, in the order they bite:

- **Both ports are the CP2102N**, per §2 — that is the harness's own choice, and it is what
  makes the board-identity checks protective rather than advisory.
- **`--pristine` does not erase NVS.** `write_flash` at `0x0` writes bootloader and app only,
  so the persisted boot-id counter survives a reflash, which is what lets a gateway compare
  two boots.
- **`run.sh` pipes the build to `tail`, so a failed build still exits 0.** Read the output,
  not the status.
- **Stop the collector if — and only if — esptool and the collector share one tty.** They do
  on this path, because both ports above are the same adapter. A tty has no exclusive open, so
  the two split the ROM handshake and esptool reports *"device reports readiness to read but
  returned no data"*. The first flash of a session usually wins and a later one loses, which
  is why gates carried this for weeks without looking broken. `tests/target/run.sh` does the
  stop-and-restart for you; a hand run does not.
- **`--capture 60` is a read of the collector's log**, not a serial attach. It therefore shows
  you the boot you already triggered, even though it fired before you asked.
- The last line `run.sh` prints is `[console] <instance> <port>` — **the board that was
  actually flashed** (#170). Read it. It exists because the script used to hardcode a board
  name and archive every C6 flash under the ship's bell.

**One refusal you may meet, and should not work around.** One board on this bench drives a
physical ship's bell on a multi-day soak. Both deploy paths refuse to flash it by adapter
hash, and the harness has no override at all:

```
ERROR: /dev/serial/by-id/... is the ship's bell.
Flashing it ends a multi-day soak that cannot be restarted.
```

The refusal fires *first*, before the collector check and before the lock, because a check
that runs later reports some other cause and the operator then acts on the wrong instruction.
Its `port=down` is *healthy* — that board's console goes over Thread, not a cable — so a
message inviting you to plug it into USB would be the one action the interlock exists to
prevent.

## 6. What a healthy boot looks like

This is a real boot, `c6spare`, **2026-08-27 21:37**, copied from the collector log with the
timestamps stripped:

```
I (57)  soc_init: ESP Simple boot
I (64)  flash_init: SPI Flash Size : 8MB
I (98)  boot: IMAP: lma 0x00040000 vma 0x42000000 len 0x4d9c8  (317896)
I (134) boot: libc heap size 189 kB.
I: IEEE 802154 radio initialized
I: 8 Sectors of 4096 bytes
*** Booting Zephyr OS build v4.2.0-12-gbc47c6fab9cc ***
oevm: build-id porta-v1-final-1155-g2dd4e6b-dirty-20260827T213644
logudp: not built in (no OEVM_LOG_UDP6) — console is UART only
oevm: observer armed (slack=8 tick=1000ms backstop=30000ms hw=yes)
thread: run=0 net_if_up=-120, joining...
thread: attached role=2
PORTA NODE READY
TAP bridge-health plan=3
tick 1 misses 0 cursor 1 acked 8 dropped 0
tick 2 misses 0 cursor 1 acked 16 dropped 0
TAP bridge-health ok 1 obs-acked health=present hw=16 seq=19 unacked=0
TAP bridge-health ok 2 goal-converged obs/name=c6spare goal/name=c6spare
TAP bridge-health ok 3 no-stream-drops streamDropped=0 misses=0
TAP bridge-health done pass=3 fail=0
tick 3 misses 0 cursor 1 acked 22 dropped 0
```

Read it in five parts:

- **`build-id`** is the anchor. Every assertion the harness makes is tied to it, and
  `--no-deploy` is refused without one: asserting against "whatever happens to be running"
  scores a green against firmware that was never written.
- **`observer armed (slack=8 tick=1000ms …)`** is the wedge detector starting. It judges the
  node against a deadline derived from the node's *own* declared cadence — `slack` × the
  longest declared timer period — never a constant of its own. It samples every `tick`, and
  it judges **progress**, not queue depth: judging depth aliases against the workload and
  reboots a healthy node (§8).
- **`thread: run=0 …, joining...` → `attached role=2`** is the radio. Two seconds here. Their
  *absence* is §4's signature.
- **`TAP … ok N`** is the workload certifying itself. The scenario's assertions live in the
  `.nsl` node, not in the harness — the harness knows only about boards, build ids and TAP
  lines, which is what lets the same verification run over a different transport later.
- **`tick N misses M cursor C acked A dropped D`** is the steady state. `misses` counts
  exchanges the gateway did not answer; `dropped` counts entries the bridge shed rather than
  send. A node that is fine and a mesh that is lossy look different here, which is the point.

For contrast, here are the **last three lines of a seven-day run** — `c6bench` carrying the
build flashed on 2026-08-22, at 14:39:51 on 2026-08-29, a minute before it was deliberately
reflashed for §7:

```
tick 173333 misses 2267 cursor 1 acked 1213349 dropped 0
tick 173334 misses 2267 cursor 1 acked 1213356 dropped 0
tick 173335 misses 2267 cursor 1 acked 1213363 dropped 0
```

Seven days, 173,335 ticks, one boot. `misses 2267` against 1.21 M acked entries is a real
lossy radio over a week — about one exchange in 500 — and `dropped 0` is the claim that
matters: the bounded pending queue never evicted anything across the whole run.

## 7. The gates

```bash
cd nodus/oevm/tests/target
./run.sh                           # every scenario
./run.sh stale-handle              # one of them
./run.sh --no-deploy --build-id <id> stale-handle
```

The runner globs `*.env`, so a new scenario joins the sweep the moment its manifest lands.
The nsl suite counts that same glob for the line in §1 — and **it also checks the names in
the table below against it**, as the `tutorial-board` aux gate, so a fifteenth manifest turns
this page red rather than leaving it quietly wrong. Names only: a missing scenario is a bug,
a stale *description* is a judgement call, and a gate that scores prose gets disabled. As of
2026-09-17 there are fourteen, and a full pass is 70–100 minutes of board time. Budget
that rather than discovering it — `wedge-quiet` alone observes for 600 s because the thing it
must not do is false-trip on a legitimately slow node, and `liveness-heartbeat` budgets a
300 s timeout plus 180 s of observation.

| scenario | class | what it gates |
|---|---|---|
| `bridge-health` | observe | the node half of the porta gate — obs acked, `goal/name` converged to `obs/name`, nothing evicted |
| `bridge-forensics` | observe | the same workload under the forensics overlay (#222) |
| `cad-rearm` | observe | an adopted cadence is a *kept* cadence (#240.2): re-arming from inside the old tick's own reaction leaves exactly one registration, at the new period |
| `pulse-coalesce` | disruptive | the coalescer does not destroy logical timer events (#151) |
| `stale-handle` | disruptive | a timer event survives a later unrelated registration (#151) |
| `wedge-note` | disruptive | the `RebootNote` survives a watchdog reset (#215) |
| `wedge-detect` | disruptive | the detector fires on a *real* wedge, dumps the condition, reboots, reports `#watchdog` |
| `wedge-quiet` | disruptive | **no false trip** on a legitimately slow node — one 10-minute timer, watched 600 s |
| `liveness-heartbeat` | disruptive | the detector can see a node with **no application timer** (#231) |
| `liveness-noradio` | disruptive | the same node with **no radio at all** — the base tick is registered upstream of the socket bind, so a failed bind still leaves a deadline (#274) |
| `liveness-fastpoll` | disruptive | **no false trip on a fast timer beside a blocking reaction** — the observer judges the app's *declared* cadence, not the shortest timer × 8 (#273 D1); red on the pre-#273 build, green after |
| `liveness-runonce` | disruptive | a node with **no repeating timer at all** is still judged, at its declared number (#273 D1) |
| `porta-refuse` | disruptive | a node **refused** by its gateway survives past the 157 s death record (#232) |
| `porta-silent` | disruptive | a node whose **uplink is gone** survives it (#269) |

Four things about running them:

**`CLASS=disruptive` means the scenario deliberately wedges, burns or reboots the node.**
Those may only touch the one disposable board — `TARGET_DEFAULT_BOARD` in
`tests/target/lib/target.sh`. It is a single name and not a list, because "may I brick this?"
has exactly one safe answer per bench day and a list is a thing that goes stale pointing at
hardware nobody has seen in weeks. `CLASS=observe` is safe anywhere, including a live field
node.

**A scenario passing its TAPs is not the verdict.** Every manifest declares `OBSERVE_S` and
`OBSERVE_PROGRESS`, and the runner keeps watching that long *past* the done line. This is not
belt-and-braces: on 2026-08-16 `bridge-health` returned `PASS 3/3` in 22 s against a build
that was dead four ticks later. It scored at tick 2 and the node died at tick 6 (#216). Both
keys are mandatory in a manifest — `none` is a legal and *visible* answer for a workload that
halts by design — because the default that would have been applied silently is the one that
produced the false green.

**The two `porta-*` scenarios need their injector started by hand**, so a full sweep is not
unattended. Each runs against a *second* porta instance with a deliberate fault: `porta-refuse`
answers `#err` to everything (`--test-refuse-all`), `porta-silent` drops every datagram and
answers nothing (`--test-drop-all`). Their `.env` files carry the ports and the exact
invocation. Neither could be a host test: the host never reproduced the death.

**The observation window's own logic is falsifiable without a board.** It runs in the nsl
suite as the `target-observe-py` aux gate — the one thing under `tests/target/` a host can
execute — so the part of the harness that decides whether a node stayed alive is itself
tested. What it does *not* touch is the seams, which is why the suite says so on the same
line.

### What one of them actually looks like

`bridge-health` on `c6bench`, **2026-08-29 14:40**, run for this page. The workload is
`PortaBridgeNodeC6` with both Thread overlays; every assertion is the node's own:

```
oevm: build-id porta-v1-final-1177-g9462ca4-20260829T143912
oevm: observer armed (slack=8 tick=1000ms backstop=30000ms hw=yes)
thread: run=0 net_if_up=-120, joining...
thread: attached role=2
PORTA NODE READY
TAP bridge-health plan=3
tick 1 misses 1 cursor 1 acked 0 dropped 0
tick 2 misses 1 cursor 1 acked 8 dropped 0
tick 3 misses 1 cursor 1 acked 23 dropped 0
TAP bridge-health ok 1 obs-acked health=present hw=23 seq=26 unacked=0
TAP bridge-health ok 2 goal-converged obs/name=c6bench goal/name=c6bench
TAP bridge-health ok 3 no-stream-drops streamDropped=0 misses=1
TAP bridge-health done pass=3 fail=0
```

Six seconds from reset to `pass=3 fail=0` — **and that is not the verdict.** The manifest
declares `OBSERVE_S=180`, so nothing is scored until the node has gone on ticking for three
more minutes. It did: **90 tick lines between 14:40:13 and 14:43:13, and nothing else in the
window** but the collector's own `port=up` heartbeat. No reboot, no wedge dump, no silence.
*That* is the pass.

Three honest notes about this particular run.

**It was not driven by `./run.sh`.** On this bench the toolchain and the board are on
different machines (§2), so it went the hand way of §5: built here with `OEVM_BOARD_NAME=c6bench`,
`scp`, then `esptool write-flash 0x0` over ssh with the sha256 checked at both ends. It was
flashed over the **JTAG** node so the collector kept the CP2102N and caught the boot — which
is why there is a banner above at all, and why no second reset was needed.

**`OEVM_BOARD_NAME` is what makes that legal rather than a guess.** The image's porta space
is no longer something the build chooses: `platform nodeId` is the board's factory link
address (12 lowercase hex characters on ESP32 and nRF52840; an RP board has none, and the
primitive fails), so a hand build on another machine cannot land in the wrong tree. What a
hand build can still lose is the *name*: `run.sh` resolves it from the collector env file for
the console port, which the building host cannot see, so without `OEVM_BOARD_NAME` the image
bakes none and `platform boardName` answers nil (`run.sh` warns). The name is published as
the optional `board` field of `obs/build`; the human-facing name of a node is the role at
`goal/name`. The history: #186 found `nodeId` was once a source constant, and 36,153 events
from at least two boards and two different apps landed in one space with nothing recording
which was which; #181 then made the id a build-time define; #118 replaced the define with
silicon.

**`misses 1` at tick 1 is real and is reported rather than hidden.** It is one exchange lost
while the mesh re-attached; it climbed to 14 over the first forty ticks and then stayed flat
at 14 for the rest of the window. TAP 3 prints the number and passes anyway, because the
assertion is about *drops*, not about a perfect radio — a gate that demanded a clean radio
would be a gate that fails for reasons the node cannot control.

## 8. Two failure signatures worth recognising

**A failed JTAG flash leaves the board in the ROM loader.** It looks alarming — the node is
off the mesh, running nothing, printing `waiting for download` — and it is fully recoverable:
flash it again over the CP2102N. Do not conclude "bricked". But note the other half: parking
in the ROM loader **drives no GPIOs**, which for a node with an actuator means the pin is
left in whatever state it was in, unbounded. That is #201 and #202, and it is why flashing an
actuator node is scheduled for daylight.

**The wedge detector can false-trip, and it did.** In August 2026 `porta-silent` reproduced a
node wedging at 111.7 s under a silent uplink — and the node was *healthy*. `observer.c`
judged the pulse mailbox's **depth** at a sampling instant, which aliases against a 4.007 s
tick sampled every 1001 ms, so the detector was rebooting a node that was still draining and
still printing ticks. The fix was to judge **progress** (`pulse_drained()`) rather than depth;
it has been green since, 357 s under the condition that had killed it, with `wedge-detect`
still firing on real wedges. The general shape is worth carrying off this page: **an
instrument can report a failure that is not happening**, and a gate that watches a system is
also a thing that must be falsified.

## Where next

- `nodus/oevm/tests/target/run.sh` and the `.env` files beside it — the executable version of
  §7, and the one part of the board story that *is* gated. Read a manifest: it is eight lines
  and every one of them is a decision.
- `nodus/oevm/zephyr/docs/BOARD_PORTS.md` and `ZEPHYR_LESSONS.md` — what each board port costs
  and what was learned adding it. `nodus/oevm/zephyr/run.sh`'s own header comments are, for
  now, the most complete account of the per-board profiles.
- `nodus/oevm/docs/vm.md` — the VM itself: what it has, what it deliberately does not, and
  the three features that were built, measured and deleted.
- [Tutorial 3](03-porta.md) — the gateway half on loopback, with no board at all, and the one
  page that runs both halves of this repo against each other. It **is** executed.
- `docs/design/2026-08-05-target-test-harness-design.md` — why the harness is shaped this way:
  the deploy seam, the workload-certifies-itself rule, and the two interlocks.
