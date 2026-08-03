# Device console collector and offline symbolizer — design

**Date:** 2026-08-03
**Issue:** #56 (sub-project 1 of 4)
**Status:** design, awaiting approval
**Refs:** #57, #74, #87, #53, #63, #110

---

## 1. What this is, and what it is not

#56 records a session in which finding one crash consumed most of the time, and
essentially none of that time went on the bug — it went on *observing the device at
all*. It proposes four capabilities in dependency order:

1. persistent serial collector
2. symbolized panics
3. out-of-band periodic metrics pushed over serial
4. device test harness

**This design covers (1) and (2) only.** Neither (3) nor (4) can be built honestly until
something reliably captures device output. (3) and (4) get their own design cycles.

They are *substantially* host-side, but not purely: D5 adds a boot banner and D8 adds a
Kconfig-gated fault prim, both device-resident. Two reasons that is acceptable where
#56's device-cost warning would otherwise bite. First, both are small and one of them is
compiled out by default. Second, #56's constraint section is written against the
WROOM-32's ~816 B of DRAM spare (#53) — and per #57 the C6 has 512 KB of `sramhp`
against that board's 192 KB `dram0`, which is why #57 describes the C6 as dissolving
#53 rather than easing it. **The ceiling that shapes #56 is not the ceiling this design
runs against.** It returns the moment any of this is ported back to the WROOM-32, which
is a reason to keep the device-side surface as small as it is here.

Target board is the **ESP32-C6 only** (D1). The other consoles on the bench behave
differently enough to change the architecture, and designing against hardware that is
not attached is what the standing bench rule warns against.

## 2. What already exists

| thing | state |
|---|---|
| `nodus/tuvm/zephyr/run.sh --capture N` | fixed-duration, welded to the flash path, delegates to the script below |
| `nodus/tuvm/zephyr/capture_coredump.py` | 16 lines; port hardcoded at line 5 (`/dev/ttyUSB0`); does no symbolizing despite its name (#87) |
| `~/magtag-captures/capture.py` | 25 lines, untracked, outside the repo, named for one board. Already encodes one hard lesson — it strips NUL bytes — and has no reconnect, no rotation, and nothing enforcing the one-handle rule its own header comment warns about |
| symbolizing | none, anywhere |

So the tooling the recent sessions actually relied on lives outside the repository and
is not under review, not tested, and not discoverable.

## 3. The fact that makes this tractable on the C6

`#57` chose the two-port split deliberately, and `run.sh:200`'s comment records it:

> Flash on the NATIVE USB port (303a:1001). Console lives on the UART bridge, which
> stays enumerated across target resets — see #57.

Confirmed on the bench, 2026-08-03:

```
usb-Silicon_Labs_CP2102N_..._ac2dce6f529ced118f43fd6be259fb3e-if00-port0 -> ttyUSB0
usb-Espressif_USB_JTAG_serial_debug_unit_40:4C:CA:45:47:A4-if00        -> ttyACM0
```

`prj_esp32c6.conf:29` confirms the console side: the board DTS routes
`zephyr,console = &uart0`, i.e. the CP2102N bridge.

**Consequence: on the C6 there is no conflict between holding the console and flashing
the board.** They are different physical channels, and the console channel survives
target resets. An always-on collector is therefore possible here in a way it is not on
the MagTag (one run per flash) or on an nRF USB-CDC console (the device node vanishes
on reset). This is the single reason the design is small.

Note what run.sh does *not* do today: for the C6 it names only `ESPTOOL_PORT`
(ttyACM0). The console port exists nowhere in run.sh — it is a hardcode in
`capture_coredump.py:5`. Naming it is part of this work.

## 4. Decisions

| | decision | why |
|---|---|---|
| **D1** | C6 only; the port-acquisition seam stays explicit | Only board attached; only board whose design can be falsified rather than reasoned about. MagTag/nRF needs are written down (§10), not built |
| **D2** | Always-on `systemd --user` unit | The only arrangement that closes #56's lesson 1. `Linger=yes` is already set for this user, so a unit starts at boot and survives logout |
| **D3** | Dumb collector, smart offline reader | A parser bug in the reader costs an answer; a parser bug in a collector costs the evidence. Also makes the reader re-runnable over history and testable with no board |
| **D4** | ELF archived per flash, keyed by build id; symbolizing **refuses** on a miss | Symbolizing a pre-reflash panic against the current build dir yields confident, precise, wrong locations — worse than the CORRUPTED backtrace #56 complained about, because it looks like an answer |
| **D5** | Build id = `git describe --always --dirty` + CMake build timestamp; host also stores `sha256(zephyr.bin)` | Bench work is dirty by nature, so `--dirty` alone gives every dirty build the same key and the archive would overwrite one binary's ELF with another's. GNU build-id is the canonical fix but this Zephyr has no `LINKER_USE_BUILD_ID` (§7) |
| **D6** | The collector monopolises the console port; `--capture N` is redefined as a log read | One handle, no split streams. `--capture` gains rather than loses: it now shows the boot you just triggered even if it fired before you asked, which is the exact failure that lost the first AMG8833 panic |
| **D7** | Heartbeat marker every 60 s, written even when the device is silent | A log that stops looks identical to a board that went quiet. Every check asserts a *recent heartbeat present*, never an absence. 60 s is short enough that a dead collector is caught within one coffee-length gap and long enough to be invisible in a day's log |
| **D8** | Deliberate fault trigger, Kconfig-gated, default `n` | Otherwise the symbolizing path is only ever exercised by the incident it exists to explain |

## 5. Components

### 5.1 `console-collect.py` — the daemon

One process per console port. Its entire job:

- open the port it is given as a path. In production that path is the
  `/dev/serial/by-id/` symlink, never a `ttyUSB` number; in test it is one end of a PTY
  pair (§9), which is the same code path and is why the daemon is testable with no board
- read bytes; strip NULs; assemble lines
- stamp each line with wall-clock time and append to today's log
- write a heartbeat marker on a fixed cadence regardless of device traffic
- on port loss: write a marker, retry acquisition, write a marker on reacquire
- hold a lockfile; a second instance exits non-zero rather than silently splitting the
  stream

It parses nothing. It knows nothing about banners, panics, builds or Zephyr.

Run as `rete-console@<board>.service` — a systemd template unit, so a second board is a
second instantiation rather than a second unit file. That template *is* the D1 seam.

### 5.2 `console-log.py` — the reader

Offline, over the log store. Subcommands:

| subcommand | does |
|---|---|
| `tail [-n]` | last N lines |
| `since <time>` | everything after a wall-clock time |
| `runs` | list boot-to-boot runs with their build ids |
| `panics` | fault dumps found, each attributed to its run |
| `symbolize <run\|range>` | resolve addresses against **that run's** archived ELF |

All parsing lives here. `symbolize` resolves the ELF from the run's build-id banner and
refuses if the archive has no match. It never falls back to `build-*/zephyr/zephyr.elf`.

### 5.3 ELF archive

`run.sh --flash` copies `zephyr.elf` into the archive keyed by build id **as part of
flashing**, not as a separate step. Alongside it: `sha256(zephyr.bin)`, so a key
collision is detectable after the fact rather than silently wrong.

### 5.4 Build id and boot banner

CMake bakes `git describe --always --dirty` plus a captured build timestamp into a
generated TU, forced to rebuild every build. The firmware prints it at boot, **before
the VM starts**, so a VM that dies immediately still leaves a log segment that names its
binary.

This is #74's mechanism minus its nsl-side accessor. #74 stays open for the accessor
that `/hello` and the witness store need; its comment already states the dependency in
this direction: "the observability surface (#56) cannot attribute a symptom to a
version."

### 5.5 Fault trigger

A prim that deliberately faults, compiled in only under a Kconfig defaulting to `n`.
Not reachable — not present — in a normal build. It exists so the hardware gate can be
scripted end to end: flash, trigger, assert the fault resolves to a known source line.

## 6. Formats

**Log line:** `2026-08-03T09:14:03.412-07:00 <text>`

Full ISO-8601 with offset, not the bare `[%H:%M:%S]` the current `capture.py` writes. A
log spanning days needs the date, and a device whose own clock is under test needs the
host's zone stated rather than assumed.

**Files:** `console-<board>-YYYY-MM-DD.log` under `~/.local/state/rete/console/`, with a
2 GB total budget per board and oldest-day pruning. Daily files make `since` a
file-selection problem rather than a rotation-offset problem, and a wedged node printing
at line rate cannot fill the disk. For scale: the one real long capture on this machine
(`~/magtag-captures/2026-07-25-corrected.log`) is 405 KB for a multi-day run, so 2 GB is
roughly four orders of magnitude of headroom and the pruning path will normally be
dead code — which is exactly why the test suite has to exercise it deliberately.

**Location:** state directory, not the repo. A log is not source, and the unit must work
with no repo checked out.

**Runs:** the reader splits a log at the Zephyr boot banner plus the build-id line. A
run is the unit everything else is expressed in.

## 7. Rejected alternatives

**GNU build-id (`-Wl,--build-id` + reading `.note.gnu.build-id` at runtime).** The
canonical answer — the id *is* the content, so it cannot collide. Rejected for now:
`LINKER_USE_BUILD_ID` does not exist in `~/zephyr-esp32/zephyr`, so it means adding the
link flag and plumbing a symbol for the firmware to read its own note. More correct,
meaningfully more work, and more to go wrong in a build system that already carries
#63's nine-enumerated-source-lists problem. D5 is the cheap approximation; this is the
upgrade path if a collision is ever observed.

**Smart collector (parse and symbolize inline).** One artifact, already symbolized. Puts
parsing logic and an `addr2line` subprocess in the path of data capture — the observer
perturbing the observation, which #56 already flags as a risk for the flash-logging
variant.

**Collector yields the port automatically.** There is no reliable way to detect another
process wanting a port on Linux, so it would rest on a heuristic, and a collector that
silently stops collecting is precisely the failure mode #56 exists to eliminate.

**Refuse to archive dirty builds.** Honest, and it would refuse almost always, so it
would get worked around.

## 8. Failure modes and how each is gated

| failure | gate |
|---|---|
| Collector dies; log stops; board looks quiet | Heartbeat (D7). Acceptance asserts a heartbeat newer than 180 s — three missed beats, so one slow write is not a false alarm. A positive assertion, never an absence |
| Board unplugged / port lost | Explicit `port lost` / `port reacquired` markers. A gap is a recorded fact, not inferred from a timestamp jump |
| Two readers split the stream | Lockfile; second instance exits non-zero and says why |
| Disk fills | Byte budget; pruning writes what it deleted |
| Reader's parsers rot when upstream Zephyr changes the banner or fault format | Checked-in golden fixtures from a real C6 boot and a real fault |
| Reader finds zero runs in a non-empty log | An error, not an empty success. This is #110's lesson applied to the tool built to prevent the next #110 |
| Symbolizing against the wrong ELF | D4 refusal, plus the stored `sha256` |

## 9. Testing

**Without hardware.** The collector reads a PTY, so the whole daemon is exercisable on
the host: feed recorded bytes including embedded NULs, a boot banner, and a fault dump;
assert the log content, the heartbeat, the lockfile refusal, and the gap markers. The
reader is tested entirely against checked-in golden logs plus an archived ELF.

**With hardware — one C6 run.** Flash, trigger the D8 fault, assert `console-log.py
symbolize` resolves it to a known source line. This is the only step that needs the
board, and it is the step that proves the fault actually reaches the console on this
hardware — a driver failing silently for want of `CONFIG_LOG` is a documented way this
bench lies.

**Falsify the gate, not just the code.** The acceptance test must fail when the
collector is not running. Stopping the unit and re-running the check must go red; a
check that passes with the daemon stopped is measuring nothing.

## 10. Out of scope, recorded so it is deferred rather than forgotten

- Capabilities (3) and (4) of #56 — out-of-band metrics, and the device test harness.
- MagTag and nRF consoles. What they will need, written down now: the MagTag's PL2303
  header does not reset the board on open (so a collector may attach to a *running*
  board mid-life, and the log will start mid-stream); an nRF USB-CDC node disappears
  from `/dev` entirely on reset and returns as a possibly-different node, so
  by-id acquisition must tolerate the path itself vanishing.
- #74's nsl-side accessor.
- Flash-resident logging (#56's append-only-flash discussion). Serial is free and
  survives nothing; flash survives reset and costs timing. Complements, not
  alternatives — but only once serial is solid.

## 11. Closes / touches

- **Closes #87** — `capture_coredump.py`'s hardcoded port and no-op auto-reset stop
  existing; its job moves into the collector and the reader.
- **Consumes part of #74** — the CMake bake and the boot banner. #74 stays open for the
  accessor.
- **Unblocks** #56's (3) and (4).
