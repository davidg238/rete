# A wedged node never faults — detecting the silence that raises no error

**#213 / #192.** Design record, 2026-08-16.

Two changes on the check-in path — `CHeapStats fromList:` (#192) and one line of
`valueFits:` (#213) — have each killed an ESP32-C6 within six check-ins. Both passed every
host gate. Both die the same way: **no panic, no coredump, no `VM Error`, no `VM GC Error`,
no reboot banner, console `port=up`, `obs/health` frozen.** #213's arm was held on hardware
today at tick 216 to confirm the healthy side, so the boundary is exact and the failure is
real.

Neither mechanism is understood. This design does not try to explain them. It closes the gap
that makes them so expensive to chase: **the node has no way to say that it stopped.**

---

## 1. This is not a new subsystem — it is the missing trigger for one already designed

`docs/design/2026-08-10-node-memory-and-liveness-design.md` §5 already designs the whole
reporting path, and `protocol/PROTOCOL-NSL-RECORDS.md` §2.4 already pins the record. What
this design adds is one thing, and it is the thing neither document has:

> §5's seam is `k_sys_fatal_error_handler`. **A wedge raises no fatal error**, so that
> handler never runs. `Panic.reason` already enumerates `#watchdog`, and nothing in the tree
> can ever produce it.

So the shape here is deliberately small: **a detector that converts "no fault" into a fault
worth reporting**, feeding a path that is already specified.

| Piece | Where it is specified | Built? |
|---|---|---|
| `Panic { reason, bootId, monoMs, detail, core }`, `reason: #watchdog` | RECORDS §2.4 | **no** |
| `RebootNote` in `RTC_NOINIT_ATTR`, persist-then-reboot | 2026-08-10 §5.1 | **no** |
| Loop breaker — 3 consecutive, reset by a successful check-in | 2026-08-10 §5.2 | **no** |
| `gw/liveness` + porta's silence sweep | 2026-08-10 §6 | **no** |
| **Wedge detection** | *nowhere* | **no** |

The only `RTC_NOINIT_ATTR` object in the tree today is `g_rtc` (`zephyr/src/main.c:285`, the
MagTag deep-sleep state), and `k_sys_fatal_error_handler` is not overridden anywhere. §5's
choice of `.rtc_noinit` over `CONFIG_RETAINED_MEM` is therefore already proven in-tree by a
second user, which is one risk this design does not have to carry.

**Nothing here supersedes those decisions. They are adopted verbatim.** Where this document
and 2026-08-10 could be read as disagreeing, 2026-08-10 wins on the reporting path and this
one governs only detection.

---

## 2. Why the obvious detector is wrong

`pumpEvents` blocks on `k_msgq_get(&pulse_q, &it, K_FOREVER)`
(`src/backend/zephyr/pulse_zephyr.c:93`). That single call is **both**:

- the healthy idle state — the node has nothing to do and is sleeping efficiently, which is
  the whole point of the design; and
- the wedged state — the node is waiting for a wakeup that is never coming.

A liveness check of the form *"has the VM completed a pump recently?"* cannot separate them.
It works today only because the bridge happens to hold a 2 s repeating timer, and it would
falsely reboot the first node whose app legitimately sleeps longer than the chosen timeout.
That node is not hypothetical: a battery sensor waking every ten minutes is the ordinary case
this system is aimed at.

**The detector must not carry its own idea of how often a node should wake.** It has to ask
the app.

---

## 3. The detector: overdue against the app's own declared timers

Two conditions, OR'd. Both derive their deadline from state the app already declared, not
from a constant chosen here.

### 3.1 Overdue armed timer

`timer_zephyr.c` keeps `target_timers[]`. Each live repeating timer knows its own period, so
each one is a statement by the app of how often it expects to be woken.

```
for each live repeating timer t:
    if now_ms - t.last_fire_ms > t.period_ms * SLACK:   WEDGED
```

A node with a ten-minute timer is judged against ten minutes. A node with a 2 s timer is
judged against 2 s. Nothing global is assumed.

This catches the timer subsystem itself stopping — the ISR no longer firing at all.

**This requires a change to `struct TargetTimer`**, which today holds only
`token / chan_id / repeating / live` (`timer_zephyr.c:29-35`). It gains `period_ms` and
`last_fire_ms`; `timer_expiry` stamps the latter. Both are plain 32-bit stores in ISR
context, which is the same discipline `live = 0` already relies on and which the
timer-token-by-value work (#151) established as safe here.

### 3.2 Undelivered pulse

```
if (posted - consumed) > 0 continuously for longer than
   (shortest live period_ms) * SLACK:                   WEDGED
```

Catches the complement: the ISR is firing and the mailbox is filling, but the VM is not
draining it — a VM stuck in the interpreter rather than blocked at the mailbox.

Neither condition alone covers both wedge shapes, and each is a handful of lines. They are
kept as two named conditions rather than one merged predicate **because the dump reports
which one fired**, and that is the first fact anyone chasing the next wedge will want.

### 3.3 The one invented constant

`SLACK` is the only number this design invents. It ships at **8**, and it belongs in
2026-08-10 §8's table under the same discipline:

| Constant | Value | Standing |
|---|---|---|
| wedge slack | 8 × the timer's own period | **Chosen.** Deliberately generous: the cost of a false reboot is far higher than the cost of a slow detection, and the measured worst-case `pushBuild` is 132 ms against a 2 s period. Earns revision from the first false trip, which is self-reporting — the dump names the condition and the overdue margin |

**It is falsifiable by construction.** A wrong `SLACK` produces a reboot with a dump saying
exactly how overdue it thought the node was. A constant that announces its own error when
wrong is worth more than one argued to be right in advance.

---

## 4. The timeout path

`task_wdt` (`subsys/task_wdt/`, present in the tree) with the C6's hardware WDT as fallback —
`wdt_esp32.c` exists and `esp32c6_common.dtsi:208` carries `wdt0`/`wdt1`.

`task_wdt_add(period, callback, NULL)` runs **our** callback on expiry;
`sys_reboot(SYS_REBOOT_COLD)` is only the built-in fallback when the callback is NULL
(`task_wdt.c:121-125`). So dump and recovery are one mechanism with a policy line between
them, not two designs:

```
on wedge:
    printk the dump   — which condition, the overdue margin, all counters, uptime
    fill RebootNote   — reason = #watchdog, per 2026-08-10 §5.1's struct
    consecutive += 1
    if consecutive >= 3:  keep the note, HALT      (2026-08-10 §5.2)
    else:                 sys_reboot(COLD)
```

The dump goes to the console, which the collector already persists — so the crime scene
survives the reboot in `~/.local/state/rete/console/`, and the node still recovers. That was
the deciding property: it is the field-correct behaviour that also keeps the evidence.

**`task_wdt_feed()` is called where the VM demonstrates progress**, not where it merely runs
— a fed watchdog must mean "the reactor completed a cycle", or the instrument reports
liveness for a node that is spinning.

### The two kinds of reset, which are not the same thing

The question "does it need to be resettable?" has opposite answers for the two pieces of
state, and conflating them is how this goes wrong:

- **The counters are monotonic and never reset.** They are the evidence. Deltas are the
  reader's job; it holds both endpoints.
- **The deadline is reset constantly**, by `task_wdt_feed()`. That is a reset of a liveness
  window, not of evidence.
- **`consecutive` is reset by a successful check-in**, per 2026-08-10 §5.2 — which invents no
  interval and is the honest definition of a loop: a node that cannot get far enough to
  report.

---

## 5. What porta sees

On the next boot the bridge finds a valid `RebootNote`, publishes one `Panic { reason:
#watchdog, … }` on `sys/panic`, and clears the magic so it reports once. `detail` carries the
condition and margin as text, truncated to fit — RECORDS §2.4 names `Panic.detail` as the one
place truncation is legal.

Nothing new goes on the wire. The record, the path, the delivery class (must-deliver) and the
`#watchdog` symbol are all already pinned.

### The sequencing constraint this inherits

2026-08-10 §5.2 states that the loop breaker **"is only safe because of sub-project D"** — a
halted node reports nothing, so without `gw/liveness` the breaker trades a noisy zombie for an
invisible corpse. §6 is unbuilt. That constraint now binds this work: **either `gw/liveness`
lands alongside, or the breaker's HALT arm does not ship** and a wedging node reboots
indefinitely until a human notices. The second is acceptable for a bench rung and is not
acceptable in a field build, and this document does not get to leave that unstated.

---

## 6. Boundary and the host

One module, `src/backend/zephyr/observer.c`, owns the detector, the counters and the
callback. Two readers, one source of truth:

- **healthy** — a C function the existing metrics prim reads, so the console dump and
  `sys/metrics` can never disagree about `pdr`/`pco`;
- **wedged** — the `task_wdt` callback.

`pulse_drops()` / `pulse_coalesced()` are re-exported through it rather than duplicated. The
posix backend gets a stub that always answers "alive": the host suite must **compile**
against this seam, never simulate it. A host test that appeared to exercise wedge detection
would be the exact false green this program has repeatedly been caught by — and #213 is
itself the second recorded case of a branch passing the full host suite and hanging a C6 in
seconds.

---

## 7. A second silent loss path, found while designing this

`pulse_pump` (`pulse_zephyr.c:96`):

```c
oop o = pulse_payload(&it);
if (o) { space_post_id(it.chan_id, o); drained++; }
```

When `pulse_payload` resolves NULL — tuvm#13's stale-handle guard, correctly preferring a
loss to a mis-delivery — **the event is discarded and nothing counts it.** `mb_drops` counts
only mailbox-full sheds. So the pulse seam has two ways to lose an event and an instrument
for one of them.

This is not caused by anything here and is not this design's to fix, but a lost wakeup is
precisely the failure shape under investigation, and an uncounted one is invisible to the
detector's own dump. **Filed as #214; the counter is a one-line addition taken while in the
file.**

---

## 8. How each step is falsified

| Step | Falsified by |
|---|---|
| `RebootNote` survives a watchdog reset on C6 | Deliberate `sys_reboot` from the callback; read the note back on the next boot. **Fails by reading zero, which is indistinguishable from "no wedge ever happened"** — so this is proven before anything is built on it |
| `task_wdt` + hw WDT does not collide with the Thread stack | Boot a C6 with Thread up and the watchdog armed; confirm no unexpected reset over a soak. 802.15.4 bring-up is the one subsystem plausibly already installing on `wdt0`/`wdt1` |
| The detector fires on a real wedge | A target scenario that **induces** one — a workload that stops draining — asserting reboot, dump, and `Panic` on `sys/panic` |
| The detector does not fire on a healthy node | The existing c6spare soak, armed, over a window long enough to matter. A false trip is the failure mode that would make this instrument worse than nothing |
| The detector does not fire on a legitimately slow node | A workload whose only timer is minutes long. This is §2's whole argument and the one an in-tree bridge-shaped test would never exercise |

The target scenario above is also **the first gate that watches past its own verdict** —
#213 records that `bridge-health` scores at tick 2 and the node dies at tick 6, so a target
gate that passes and then lets the node die is a false green. That assertion belongs to the
harness regardless of this work.

---

## 9. Out of scope

- **Explaining #192 or #213.** This makes a wedge loud and recoverable. Finding the mechanism
  stays theirs.
- **Anything on the wire beyond `Panic`.** The whole counter set is already published on the
  healthy path by `sys/metrics`.
- **A Kconfig bench/field split.** It would mean the target gates exercise a different failure
  path than what ships — the false-green class this bench already catalogues.
- **Core dumps.** `Panic.core` stays `nil`; bulk stays off the space.
- **Resetting counters.** §4.

---

## 10. Promotion — what absorbs this when it closes

- `review/program-sequence.md` — the rung.
- `docs/design/2026-08-10-node-memory-and-liveness-design.md` §8 — the `SLACK` row.
- `protocol/PROTOCOL-NSL-RECORDS.md` §2.4 — `#watchdog` ceases to be aspirational; note what
  produces it.
- The owning doc under `review/lib/` for the pulse seam — §7's second loss path.

---

## 11. Addendum — what building it settled, and what the bench said (2026-08-19)

Built on branch `215-vm-liveness-observer`. Plan:
`docs/superpowers/plans/2026-08-19-vm-liveness-observer.md`, which carries the full
argument for each decision below.

### 11.1 Three things §4 could not have known, all forced by the tree

- **`task_wdt` cannot be the detector.** `task_wdt_feed()` calls `k_sched_lock()`
  (`subsys/task_wdt/task_wdt.c:240`), which asserts `!arch_is_in_isr()`
  (`kernel/sched.c:774`) — and **`CONFIG_ASSERT` is not set in the C6 build**, so an
  ISR-context feed would not fail loudly, it would decrement the interrupted thread's
  `sched_locked`. The feed must happen in thread context, and the only thread that could
  otherwise do it is the reactor, which is the thing that wedges. So the supervisor is a
  **cooperative-priority thread**, and `task_wdt` sits **behind** it as the backstop for
  the supervisor itself dying, with the hardware WDT behind that. Cooperative priority is
  load-bearing: at a preemptible priority the trip would come from a missed feed against a
  fixed channel period, which is exactly the global constant §2 forbids.
- **`struct arch_esf` on this SoC carries no `mcause`** — it is behind
  `CONFIG_CLIC_SUPPORT_INTERRUPT_LEVEL`, unset, and `CONFIG_RISCV_SOC_HAS_ISR_STACKING` is
  unset too. 2026-08-10 §5.1's field stays and is filled from `csr_read(mcause)`. Verified
  on hardware: the note's `mcause=0x0000000b` matched the fatal handler's own register
  dump line `E: mcause: 11` exactly.
- **A HALT that leaves the hardware watchdog armed is a reboot loop wearing a halt's
  costume.** `task_wdt_init()` installs a hardware timeout with `WDT_FLAG_RESET_SOC`
  (`task_wdt.c:135-138`); a halted node feeds nothing, so the SoC resets ~11 s later and
  the HALT line prints either way. `observer_halt()` calls `wdt_disable()` first.
  **Confirmed on hardware: the node stayed halted for 4 minutes, ~22x the 11 s window,
  with zero boot banners.**

### 11.2 §6's module location is superseded

The detector lives at **`nodus/tuvm/zephyr/src/observer.c`**, app-side beside `main.c`,
not at `src/backend/zephyr/observer.c`. §6's posix stub does not exist and is not needed.
Placing it in the engine backend would have cost nine source-list edits (#63) to carry a
stub nothing on the host calls. §6's actual requirement — one source of truth for the
counters — is met by calling `pulse_drops()`/`pulse_coalesced()`/`pulse_stale()` directly,
the same source `PRIM_pf_sysStats` already reads. The note's logic is a header
(`src/reboot_note.h`) and its storage follows the `rtc_target_*` split, so no engine `.c`
file was added at all.

### 11.3 §3.3 undercounted the invented constants by two

| Constant | Value | Standing |
|---|---|---|
| `OBSERVER_TICK_MS` | 1000 | **Derived.** A sampling resolution, which must sit far below the shortest deadline in play. Measured: it caught an 8000 ms deadline **8 ms** past it and a 16000 ms deadline **16 ms** past it — i.e. on the very next tick both times |
| `OBSERVER_WDT_MS` | 30000 | **Chosen, deliberately generous.** The backstop for the supervisor thread itself dying, and nothing else. It has never fired |

### 11.4 What the bench said — §8's rows 1, 3 and 5

| Row | Result |
|---|---|
| 1 — the note survives a watchdog reset | **PASS 2/2** (`wedge-note`). Trip at 15037 ms, `rst:0xc (SW_CPU)`, boot 2 read the note back with `reason=1` and `up=15037ms` unchanged |
| 3 — the detector fires on a real wedge | **PASS 2/2** (`wedge-detect`). Wedged at tick 30; caught 9.0 s later, `cond=pulse-undrained overdue=8ms deadline=8000ms pending=1 pco=8 pdr=0`. **`deadline` is 8 x the app's OWN declared 1000 ms period.** `pdr=0` says the detector is faster than the shed rate |
| 5 — no false trip on a slow node | **PASS 1/1, twice** (`wedge-quiet`, 600 s and 621 s). One 10-minute timer, `slow-node tick` at **599.99 s**, zero `WEDGE` lines, one boot banner |

**Rows 2 and 4 closed 2026-08-21 — §8 is 5/5.** The armed soak ran on c6spare for
**22 h 23 min on one unbroken boot**: tick 40 284 (which agrees independently at 2 s a tick),
ONE `build-id` line and ONE `rst:0x` in the whole window — the POWERON from the flash that
started it — and **zero `WEDGE` lines** with the detector armed throughout. `misses 254` of
282 008 acked, `dropped 0`. That is row 4 (no false trip on a healthy node) and row 2
(`task_wdt` + the hardware WDT do not collide with the Thread stack over a soak).

The build under it was the one that fixed #222, so the same run is that fix's real
verification: this is the bridge that wedged at tick 6 on eight consecutive boots the
previous morning.

Row 5 then grew a 10-hour version by accident, and it is worth banking because it is the
only long armed run this rung has. The 2026-08-19 22:28 power failure rebooted the
`wedge-quiet` board mid-soak and it came back on the same image; the collector was down
from 22:33 to 07:23, so the console is blind across most of it. **The tick counter closes
that blind window without needing the console**: `slow-node tick 60` landed at 08:28:18 and
every tick is 10 min apart, which puts tick 1 at 22:38 and the boot at 22:28 — a reboot
anywhere in the gap would have restarted the count at 1. So 60 consecutive 10-minute timer
firings, **10 h on one unbroken boot, zero `WEDGE` lines and zero resets**, against a
detector whose largest plausible fixed deadline is 30 s. Row 5's 600 s number was chosen to
beat a 5-minute constant; this beats a 10-hour one.

The whole reporting path is proven end to end by row 1's TAP:
`TAP wedge ok 2 reason is watchdog reason=#watchdog`, i.e. prim 153 ->
`Kernel>>panicReasonFor:` -> `Panic.reason`, with `detail` carrying the register triple.

### 11.5 The loop breaker ran itself, by accident, and every part of it worked

`platform panicNote` answers a prim-built indexable that carries `basicAt:` and **not**
Object's protocol, so an `isNil` send to it is a doesNotUnderstand. The bug landed on the
boot that was trying to *report* the previous wedge, which walked the whole of 2026-08-10
§5.2 on hardware without being asked to:

```
wedge  -> note reason=1, consec=1, reboot
abort  -> note reason=3, consec=2, reboot     (mcause=0xb, mepc/ra real)
abort  -> consec=3 -> HALT, and it HELD
```

confirming the fatal handler runs, `K_ERR_KERNEL_PANIC` maps to `#assert`, `consecutive`
ratchets across cold reboots, the breaker trips at three, and §11.1's `wdt_disable()`
claim. Fixed by `== nil` at all three sites, which is what `sensorFrame`'s callers already
do (`ThermalDiagNode.nsl:53`).

### 11.6 The open regression — #222

**The armed bridge wedges at ~31 s on every boot.** The same board ran the pre-#215
bridge for **21.4 h** on one boot with zero faults (boot 46, `#up: 77002062`, `#sdr: 0`),
so this is new, not #213 — even though stopping at **tick 6** is #213's recorded signature
exactly. That resemblance is a coincidence to be bisected, not evidence.

`sys/panic` never reaches porta despite a valid note at every boot and a demonstrably
running ack. The decisive one-line A/B is in #222.

**Two things this does not put in doubt.** The detector caught this wedge, classified it,
dumped it and recovered the node — the same event on the pre-#215 build produced no
evidence whatsoever. And §8's three testable rows are green.

**One finding for 2026-08-10 §5.2 regardless of #222's cause:** `consecutive` reads 1 on
every boot here, because the bridge acks on its first clean check-in and the ack resets the
count. By §5.2's definition ("a reboot loop is a node that cannot get far enough to
report") this node is not looping — while in fact it reboots every 31 s forever. A node
that reports and *then* wedges is a noisy zombie the breaker will never stop. It is at
least a visible one now.
