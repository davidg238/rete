# The C6 ship's bell as an always-on node — design

**Date:** 2026-08-03
**Tracker:** #102 (shipsBellsV3), #105 (the boot hazard, blocking a coil)
**Board:** ESP32-C6 devkit, wall-powered, DS3231 on i2c0, piezo ringer on GPIO18
**Supersedes nothing.** Extends `zephyr/boards/shipsBells/2026-07-26-shipsbellsv3-design.md`
(hereafter "the design", cited by §).

## 1. What this is, and what it replaces

The C6 rig can ring (§18) and can keep time (§17.1, §16), and **it cannot do both without a
person typing.** `ThreadNtpNode.nsl` — what the board runs today — syncs the RTC, prints
`NTP OK` and **returns from `main:`**; it was written as a bring-up witness for the timebase and
was never a clock. The only strike path in the image is `strike_console`, a C thread reading the
UART, so a wall-powered board with no data cable has no trigger at all (§18.5).

This design is the node that closes that gap: **`ShipsBellNodeC6.nsl`, which never returns,
reads the DS3231, and strikes the watch on the half hour.**

It is deliberately the smallest thing that is a bell. What it is not is in §9.

## 2. Decisions

### D1 — The trigger is a poll of the DS3231, not Alarm2. §16.4 stands for the battery version.

A 1 s repeating VM timer reads `platform wallClock` and compares slots. §16.4 settled the
trigger as Alarm2 with a minutes-match mask, and **that decision is not being reversed — it is
being scoped.** Alarm2 buys "the MCU does nothing at all until the boundary", which is a *power*
argument, and §16.1 states this version is mains-powered precisely because of the striker. §9's
battery version still wants Alarm2 and this design does not touch that.

What the scoping buys: **no new prim, no Kconfig change, no ISR path.** `CONFIG_RTC_ALARM=n`
stays as `prj_esp32c6.conf:92` intends ("flip it in the same commit that lands the alarm seam,
not before"), and this node uses only seams already witnessed on this board.

**§16.4's "no accumulated error" objection does not apply to this poll,** and the reason is D2
rather than a concession: nothing here counts. The cost that IS real: a strike lands up to one
poll period late — under a second, against a bell that rings for seconds.

### D2 — The slot is DERIVED from the clock, never counted.

`slot := secondsOfDay // 1800`. Every tick re-derives the answer from the RTC; no state
accumulates between ticks except `lastSlot`, which is a *has-this-one-rung* marker and not a
clock. A missed tick, a late tick, a GC pause and a reboot all produce the same answer, because
the answer is a property of the time and not of the node's history.

This is the same instinct §6 applied when it deleted the disciplining loop: **the RTC is not
something to measure against, it is the thing that says when.**

### D3 — The pattern lives in nsl. C keeps only `gpioSet`.

§11.2 says the real striker "will go through the gpio seam from nsl"; this is that. The node
writes the pin through `platform gpio set: 18 to:` and owns the phrasing — width, pair gap, set
gap, the odd tail, the 1..8 guard. `strike_bells` in `zephyr/src/main.c` stays exactly where it
is, as the bench instrumentation §11.2 always called it.

The kernel tick is 1000 Hz and tickless (`CONFIG_SYS_CLOCK_TICKS_PER_SEC=1000`), so a
timer-driven width is accurate to ~1 ms plus reactor dispatch latency. That is ample for a tone
burst. **It would not be ample for the coil**, whose 5–10 ms bracket (§17.2) is why
`solenoid_oneshot` busy-waits — when the coil returns, the width path is the part to re-examine,
not the phrasing.

The count mapping is **not** re-implemented. `Bell>>bellsForHalfHour:` in `nodus/nsl/lib/kernel.nsl`
already owns it (i mod 8, 0 reading as 8, anchored at 00:30→1, 18:30→5, 12:00→8) and its own
comment draws the line this design keeps: *"Bell owns the CONVENTION … the node owns the
SCHEDULE."* Note this is plain modulo — **no dog watches**; 18:30 rings 5. Changing that is a
`kernel.nsl` change shared with the MagTag, not a change to this node.

### D4 — A missed strike is skipped silently. The marker never moves backward.

On boot the node seeds `lastSlot` from the clock **and rings nothing**. Power restored at 21:45
does not produce a 21:30 bell (slot 43 → three bells); the next sound is 22:00's four.

This follows the ratchet `MagTagDeepSleepNode` already establishes — *"a sync may push the count
FORWARD (skip missed strikes, silently) but NEVER backward"* — and it is what makes a reboot
loop inaudible. **A bell at a time that is not a half hour is worse than a missing bell**, because
the missing one is merely absent while the wrong one is misinformation.

The cost, stated so it is not discovered later: after a power cut there is up to 30 minutes of
silence and nothing announces that the bell came back. The console print does; the bell does not.

### D5 — Midnight is a case, not an inequality.

`slot > lastSlot` is false at the 47 → 0 wrap, so midnight is handled as its own explicit branch.
This is not defensive coding: **this exact wrap rang eight bells twice on 2026-07-24** — a 66 s
lead read as a 23.5 h lag — and the deep-sleep node carries a paragraph about it (design §2 as
restated by §11). Any node that turns time into a slot index inherits that bug unless it says
midnight out loud.

### D6 — The clock may be improved by the network and may never be stopped by it.

One NTP sync at boot, then one a day at `slot = 0` with `secondsOfDay >= 300` — §17.5's "a few
minutes after midnight", 25 minutes clear of a strike, which matters because `ntpSyncTz:` BLOCKS
up to the SNTP timeout and `kernel.nsl` says to call it only between strikes.

A `nil` from NTP is shrugged off and the bell keeps ringing on the DS3231 (`MagTagClockNode`'s
rule). The boot sync is additional to §17.5's daily one and justified separately: a boot is rare,
and a board that has been off may have been off long enough to matter.

### D7 — Never strike before the time is known.

§16.3's mechanism, restated as node behaviour: `wallClock` answering `nil` — no DS3231, or a part
that has never been set — means **the node polls and prints and does not ring**. If NTP then
supplies a time and the RTC accepts it, ringing begins at the next boundary.

A `nil` appearing mid-run (a dead part, a bus fault) stops striking until readings resume. Silence
is the correct failure mode for a bell; a guess is not.

### D8 — A bench slot, and why it is not the 6× demo clock's mistake.

`slotSeconds`, default 1800, set to 60 for bench work: four boundaries in four minutes instead of
two hours, with boundary detection, the ratchet, the wrap, the phrasing and the in-flight guard
all running unchanged.

`MagTagClockNode` ran a 6× demo tick at stages 2–3 and had to delete it when NTP arrived, because
there **one tick advanced a half-hour of clock time** — the counter and real time disagreed, so
every sync yanked the display back. Here the clock is untouched: shrinking the slot changes only
how often the derived index changes. What goes wrong is confined and obvious — the *counts* stop
matching wall time (1..8 cycling every 8 minutes). Eight bells is ~7 s, so a pattern still
finishes well inside a 60 s slot.

### D9 — Standalone this rung. No porta check-in.

The node rings and prints; the #56 collector captures the prints. Nothing on the wire but the
daily NTP request. Space-sync reporting is real value for a run of five and is a separate node's
job — folding it in here would put design §8's replay hazard in scope for a rung whose claim is
"it rings on time".

## 3. Architecture

One file, `nodus/tuvm/zephyr/ShipsBellNodeC6.nsl`, Model B, flashed with the Thread overlay as
tonight's image was.

**One `Timers` instance, held in a local.** `platform timers` answers a FRESH instance per call,
each with its own pending registry; registering on one and dispatching on another drops every
event silently. That mistake cost `MagTagClockNode` an entire overnight run and it is the single
easiest thing to get wrong in this file.

Three channels:

| channel | fed by | reaction |
|---|---|---|
| `sys/timer` | the seam | `timers dispatch: e` |
| `clock/tick` | `timers every: 1000` | read the RTC; decide whether a boundary passed |
| `bell/stroke` | `timers after: …` one-shots | advance the striker one step |

**The striker is a state machine, not a loop**, so the poll keeps running while the bell rings:
`set: true` → one-shot at *width* → `set: false` → one-shot at *pair* or *set* gap → next stroke.
Even index = first of a pair, so the short gap holds a pair together; odd = the long gap separates
them; the last stroke has no trailing gap, which is what produces the lone final stroke on an odd
count.

Constants, in one place: width 50 ms, pair 500 ms, set 1600 ms. These are what was approved by
ear on the piezo on 2026-08-03 and they carry §18.3's caveat — **a continuous-tone piezo has no
decay and cannot settle a cadence for the gong.** They are a starting point, not a result.

## 4. Error handling

| condition | behaviour | why |
|---|---|---|
| `wallClock` nil at boot | sync NTP, set RTC, re-read; still nil → poll, print, never strike | D7 |
| `wallClock` nil mid-run | stop striking; resume when readings resume | D7 |
| NTP nil (no route, timeout) | print, carry on ringing off the RTC | D6 |
| `gpio set:` raises `#gpioUndeclared` | catch at the striker, print once, keep the schedule running | a bell with no ringer should still keep time and say why it is silent |
| count outside 1..8 | refuse, print | `Bell` owns the convention; a count the convention cannot read is a bug, not a sound |
| pattern already in flight | refuse the new one, print | impossible at 1800 s, reachable at D8's 60 s; two rings interleaved on one pin is neither |

## 5. Verification

**Host** — the schedule's decisions as ordinary nsl tests: slot derivation, the midnight wrap, the
boot seed, the count mapping, the in-flight refusal. Written by injection, because **the host
clock does not tick**: `OEVM_WALLCLOCK_SECONDS` is loaded once into `g_wc_secs` and never
advances (`src/platform.c:328`), so a host run cannot witness a boundary arriving of its own
accord. Making the host clock move would change a shared leaf that existing tests rely on; the
logic is written to be testable instead.

**On target** — flashed with `slotSeconds` 60, watched through the collector:

1. Four consecutive boundaries ring, counts ascending, each phrased correctly.
2. No boundary rings twice (the marker holds across the whole run).
3. A reset mid-slot rings nothing until the next boundary (D4).
4. The 47 → 0 wrap rings once (D5) — reachable in bench mode by seeding the RTC near the wrap.

Then rebuilt at 1800 and left running to hear a real half hour.

**The acceptance claim is audible and it is one sentence:** with the USB cables carrying nothing
but power, the bell rings the correct count on the half hour, and rings it once.

## 6. What this leaves open

- **#105 is untouched and still blocking.** The boot beep happens before the VM exists; no node
  can prevent it. It must be answered before a coil is reconnected (§17.6, §18.4).
- **The cadence is unsettled** (§18.3). 50/500/1600 is a piezo starting point.
- **Alarm2 is deferred, not dropped** (D1). §9's battery version needs it.
- **porta check-in** (D9) is tomorrow's node.
