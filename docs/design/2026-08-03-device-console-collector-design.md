# Device console collector and offline symbolizer — design

**Date:** 2026-08-03
**Issue:** #56 (sub-project 1 of 4)
**Status:** approved 2026-08-03 (David), with the D6 scoping, the D4 escape hatch and the
§10 MagTag-script disposition added at approval
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
| **D4** | ELF archived per flash, keyed by build id; symbolizing **refuses** on a miss. One named exception: `--assume-build <key>`, which must be typed and is echoed in the output (D4a) | Symbolizing a pre-reflash panic against the current build dir yields confident, precise, wrong locations — worse than the CORRUPTED backtrace #56 complained about, because it looks like an answer. But refusal alone makes the *earliest* faults permanently opaque: a fault before the banner prints (§5.4) leaves a run with no build id at all, and that is exactly the early-driver case §9 calls out. The exception keeps refusal as the default while letting a human say what they know — never a fallback the tool picks for itself |
| **D5** | Build id = `git describe --always --dirty` + CMake build timestamp; host also stores `sha256(zephyr.bin)`. **GNU build-id was tried on hardware and rejected with evidence** (§7) | GNU build-id is the better identifier and the spike went after it. It breaks the ESP32-C6 image: any `SHT_NOTE` input section makes GNU ld synthesise a `PT_NOTE` program header over the whole rodata region, and the Espressif image builder mishandles it — the board boots and then emits garbage. Three-boot A/B/A in §7. The key is **opaque to everything downstream** either way: a run declares a key, the archive is filed under the same string |
| **D6** | The collector monopolises the console port; `--capture N` is redefined as a log read **for `--board esp32c6` only**. The esp32 and esp32s2 paths are untouched until their own collectors land (§10) | One handle, no split streams. `--capture` gains rather than loses: it now shows the boot you just triggered even if it fired before you asked, which is the exact failure that lost the first AMG8833 panic. The board qualifier is not pedantry — `run.sh:309` calls `capture_coredump.py` for all three boards, so an unqualified redefinition breaks the two that have no collector. It would also silently drop a side effect: `capture_coredump.py:9` pulses DTR/RTS to reset the target, which is a no-op on the C6's CP2102N but genuinely works on the esp32, where that same CP210 port is both the flash port and the console |
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

The one exception is D4a: `symbolize --assume-build <key>`, for the run that faulted before
its banner ever printed. The key must be typed out; the tool will not infer it, not even
from the `last-flashed` pointer (§5.3), which exists so a human can *look up* what to type,
not so the tool can guess on their behalf. Every line of an assumed symbolization is
prefixed with the key it assumed, so the assumption travels with the output into whatever
issue comment it gets pasted into.

### 5.3 ELF archive

`run.sh --flash` copies `zephyr.elf` into the archive keyed by build id **as part of
flashing**, not as a separate step. Alongside it: `sha256(zephyr.bin)`, so a key
collision is detectable after the fact rather than silently wrong.

The same step writes `last-flashed-<board>` — one line, the key it just flashed. It is a
human-facing note, read by a person deciding what to pass to `--assume-build`. Nothing in
the reader consults it automatically; if it did, D4's refusal would be a fallback wearing
a disguise, and a board flashed twice since the fault would hand back a confident wrong
answer.

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

## 7. Alternatives — all settled, one by hardware

The build-id question was **settled by the spike run on 2026-08-03**; the result is below.

**`git describe --always --dirty` + build timestamp** (the original D5; now the
fallback). Cheap and needs no linker work. Two arguments were made for it during design
and only one survives:

- *Survives:* it collides only if two builds land in the same second, which is not a
  real bench risk.
- *Does not survive:* it was claimed that build-id makes the archive re-derivable where
  this does not. Both are re-derivable — the baked string is a rodata literal, so
  `strings zephyr.elf` recovers it exactly as `readelf -n` recovers a note. The archive
  is repairable either way and that is not a discriminator.

What actually decides it is interoperability. GNU build-id is what coredump handlers,
symbol servers and crash-reporting backends key on; a `describe+timestamp` string is
legible only to our own tooling. #56 is labelled `revenue-gate` on the grounds that
reliability reporting *is* the product — and that argument applies to the identifier
format as directly as to anything else.

**GNU build-id — tried on hardware, rejected.** Everything about it worked except the
last step, and the last step is the only one that matters.

*What worked.* `--build-id=sha1` overrides Zephyr's base `--build-id=none` by
last-flag-wins. The note links correctly: `namesz=4 descsz=20 type=3 "GNU\0"` followed by
20 bytes, at `0x42800000` in `.flash.rodata`, allocated and flash-mapped, bracketing
symbols resolving to a 36-byte span. The bootloader maps it —
`DROM segment: paddr=00050000h vaddr=42800000h size=25010h map`. Build id
`7f638d0a01fe1302f38121bbd4187ff2e0597e5b`.

*What broke.* The board boots and then emits **garbage**. Not a crash, not a silent
board — a working bootloader, a verified flash hash, and then 2360 lines of noise where
the console should be.

*Attribution, by three boots with one variable each.* This is the part that matters,
because the first hypothesis was wrong:

| build | garbage lines | outcome |
|---|---|---|
| note spliced into rodata | **2360** | garbled |
| control — nothing added | 0 | `Booting Zephyr` → `TIMER OK` |
| 36 bytes of padding, same place, no note | 0 | `Booting Zephyr` → `TIMER OK` |

The obvious explanation — 36 bytes inserted at the DROM base shifts rodata and breaks the
image — is **false**. Padding alone is harmless. The cause is that any `SHT_NOTE` **input**
section makes GNU ld synthesise a `PT_NOTE` program header spanning the whole rodata
region (observed: `NOTE 0x42800000 size 0x24e20`), even when the note is spliced into a
PROGBITS output section. The Espressif image builder mishandles that segment typing.
There is no standard linker flag to suppress a `PT_NOTE`, so fixing this is upstream
work, not configuration.

**This is #98's family, and it fails the same way: silently.** The image builds, flashes,
passes its hash check and boots. Nothing reports a problem. Only a console read attributes
it — which is the argument for the collector this design is about.

*A second finding, recorded because it cost time.* `zephyr_linker_sources(ROM_SECTIONS …)`
is **accepted, generates its snippet file, and is then silently dropped on Espressif
SoCs.** That hook exists only in the generic RISC-V template
(`zephyr/include/zephyr/arch/riscv/common/linker.ld`);
`soc/espressif/esp32c6/default.ld` honours only rwdata, data-sections, ram-sections and
rodata. The symptom is an orphan-section warning and undefined symbols, never anything
naming the cause.

*A method note, since it nearly produced a wrong answer.* An intermediate check searched
`zephyr.bin` for the 20 build-id bytes, did not find them, and looked like proof the note
never reached the image. The control killed it: `"Booting Zephyr"` is not findable in
`zephyr.bin` either. The image is not a byte concatenation, so that search could not have
found anything. **The instrument was broken, not the note.**

**What the spike would have had to establish, for the record.** Not free, and not a gap
Zephyr forgot:
`cmake/linker/ld/linker_flags.cmake:7` sets `--build-id=none` **deliberately**, in the
base property beside `--gc-sections`. Overriding it is a standing commitment — every SDK
bump is an opportunity for it to revert silently. `riscv64-zephyr-elf-ld` 2.38 supports
`--build-id[=STYLE]`, and the firmware-side note parser is ~25 lines. None of that was
the obstacle.

**Reopen this if** the `PT_NOTE` behaviour is fixed upstream, or if a way is found to
suppress the synthesised segment without patching the image builder. The identifier is
still the better one; only the platform is in the way.

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

Because D8 defaults to `n`, this gate does not build the default image. It builds with
`CONFIG_TUVM_FAULT_TRIGGER=y`, carried in a checked-in overlay
(`prj_esp32c6_faultgate.conf`, applied via `run.sh --conf-extra`) rather than a flag
typed at the prompt, so the gate is reproducible and so the one build that can fault on
purpose is named in the tree. Both names follow what is already there:
`CONFIG_TUVM_NET_HEARTBEAT` is the precedent for a default-`n` diagnostic, and
`prj_esp32_netdiag.conf` is the precedent for the overlay that turns one on.

**Falsify the gate, not just the code.** The acceptance test must fail when the
collector is not running. Stopping the unit and re-running the check must go red; a
check that passes with the daemon stopped is measuring nothing.

## 10. Out of scope, recorded so it is deferred rather than forgotten

- Capabilities (3) and (4) of #56 — out-of-band metrics, and the device test harness.
- MagTag and nRF consoles — the *collector* for them. But `~/magtag-captures/capture.py`
  does not stay outside the repo: it moves to `nodus/tuvm/zephyr/boards/magtag/capture.py`
  **unchanged**, in its own commit, no behaviour edits and no attempt to generalise it.
  §2 names untracked out-of-repo tooling as the problem; shipping the sub-project that
  says so while leaving the specimen in `~` would be the diagnosis without the treatment.
  Tracking it is not endorsing it — it is deleted when the MagTag collector lands.
  What that collector will need, written down now: the MagTag's PL2303
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
- **Tracks** `~/magtag-captures/capture.py` verbatim under `boards/magtag/` (§10) — the
  only change to any non-C6 board in this sub-project, and not a functional one.
- **Unblocks** #56's (3) and (4).

---

## 12. Implementation addendum — what the build changed (2026-08-03)

Written after implementing §1–§11 on `feat/56-console-collector`. Everything here
is a correction to this document, not a change of plan.

**§11's "Closes #87" is wrong.** `run.sh` still calls `capture_coredump.py` for
`esp32` and `esp32s2`, which have no collector under D1, so its hardcoded port
survives — reachable from two boards instead of three. #87 closes when its last
caller does. Correction posted on #87; the claim above is left standing so the
error stays visible rather than being quietly rewritten.

**D8's names follow the tree.** `CONFIG_TUVM_FAULT_TRIGGER` and
`prj_esp32c6_faultgate.conf`, not the `RETE_`/`overlay-` spellings originally
written here — `CONFIG_TUVM_NET_HEARTBEAT` and `prj_esp32_netdiag.conf` are the
precedents. Amended in §9 (`93f053b`).

**§9's fixtures did not exist, and D8 is what makes them.** No C6 had ever
faulted on purpose, so the fault parser had to be written *after* the trigger,
not before. A parser written first would have been written against an invented
format — D4's failure one layer up.

**`prj_esp32c6.conf` has no coredump configuration.** §9 assumed a fault would be
decodable; on this board it would have printed a banner and nothing else. The
faultgate overlay enables `CONFIG_DEBUG_COREDUMP*` alongside the trigger.

**A boot has two banners, so §6's "splits a log at the Zephyr boot banner plus
the build-id line" splits it wrongly.** The C6 prints `ESP-ROM:esp32c6-...` and,
~30 lines later, `*** Booting Zephyr OS`. Treating either as a boundary puts a
fault in a different run from the build id naming its binary, so D4 refuses a
fault it has the ELF for. The ROM line is the boot; the kernel banner opens a run
only when none is in progress — which is also how a collector attaching to an
already-running board (§10's MagTag case) still gets a boundary.

**D7's heartbeat is not sufficient for the §8 health gate on its own.** The first
heartbeat is 60 s after start, so a heartbeat-only check reports a working
collector as down for a minute after every restart — a false red on exactly the
action taken to clear a real one. The collector's startup marker is the same proof
of life and counts. It does not become a permanent pass: a collector that starts
and wedges goes red when that marker ages out. Found by performing §9's "falsify
the gate" step, not by reading the code.

**Environment facts §5.2 needed and did not state.** Symbolizing requires the
Zephyr venv's Python (`scripts/coredump/` imports `pyelftools`, absent from the
system interpreter) and the SDK's `riscv64-zephyr-elf-gdb` — never the host
`gdb`, which yields plausible wrong output for a RISC-V core. The stub is driven
over `--pipe`: it does `listen(1)` + `accept()` once, so any readiness probe
consumes the connection gdb needs.

**One addition beyond the design.** `run.sh` cross-checks the console port it
resolves against the collector's env file and warns on a mismatch. A collector
watching a different adapter than the one just flashed is silent, and every
symptom of that looks like "the board went quiet" — the failure class this
sub-project exists to remove. Without it `CONSOLE_PORT` would have been named and
never read.

**First finding.** The collector immediately surfaced a DS3231 RTC init failure on
every C6 boot (`rtc_ds3231.c:253`), pre-existing and unrelated to this work.
Filed as #146.
