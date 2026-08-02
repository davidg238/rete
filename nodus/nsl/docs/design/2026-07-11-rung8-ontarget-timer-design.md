# Rung 8 — On-target timer seam + the self-gating Thread node — Design

**Goal:** Give the nsl `Timers` seam a REAL on-target backend (Zephyr `k_timer`), and use it
to run the Thread echo node on a **15 s autonomous cadence** instead of a BUTTON1 press — so
the node works on the **nRF52840 dongle**, which is HEADLESS (Issue A: USB-CDC console dies
under radio) and has no practical button.

**Non-goal:** beating Issue A. The dongle console stays dead; this rung makes the node's
correctness observable *without* it.

---

## 1. The problem this rung solves

Rung 7 shipped `BRIDGE OK` on the nRF52840-DK: a BUTTON1 press posts a `Record`, the
`NetBridge` CBOR-encodes it over 802.15.4 to the gateway's `udp-echo6`, the reflection is
received, decoded value-equal, and printed. That gate is bench-verified and stays.

The dongle attaches to the mesh (MTD child `0x2008`) but is **headless** — so the
`BRIDGE OK` print is invisible — and a button press on a stick jammed into a USB port is
not a real trigger. Two things are therefore missing:

1. **A cadence source.** The nsl `Timers` seam exists (`timerAfter:token:` / `timerEvery:token:` /
   `timerCancel:` / `monotonicMillis`, posting to the reserved `sys/timer` channel), but it is
   backed by a **virtual clock** in `src/platform.c` advanced only by the `advanceMillis:` prim.
   `src/platform.c:21` says so outright: *"an on-target backend would back this with a real
   timer facility instead."* This rung writes that backend.

2. **A proof of the INBOUND leg with no console.** The only observation point on a headless
   board is the gateway's `udp-echo6` journal, which logs a line per datagram it *receives* —
   i.e. it witnesses only the **outbound** leg.

---

## 2. The timer seam (a pure lift, mirroring the pulse seam)

```
src/timer_backend.h              NEW — the seam contract (sibling of pulse_backend.h)
backend/posix/timer_posix.c      NEW — the virtual clock, MOVED VERBATIM out of platform.c
backend/zephyr/timer_zephyr.c    NEW — k_timer, real wall clock
src/platform.c                   prims 85-88 call the seam instead of the static host_timer_*
```

Contract:

```c
void     timer_backend_after(int token, uint64_t ms);
void     timer_backend_every(int token, uint64_t ms);
void     timer_backend_cancel(int token);
uint64_t timer_backend_millis(void);
int      timer_backend_advance(uint64_t ms);   /* posix: virtual clock. zephyr: no-op, returns 0 */
```

**Why the Zephyr side is nearly free.** A `k_timer` expiry callback runs in ISR context, so it
cannot call `space_post`. It does not need to: it takes the *identical* path the GPIO ISR
already takes (`pulse_backend.h` states the contract as "A **GPIO/timer** ISR calls
`pulse_post_isr`" — a timer ISR is a sanctioned pulse source, anticipated by the original
design). At registration — thread context, inside the `timerEvery:` prim — we do exactly what
`pf_gpioPin` does today:

```c
int chan_id = space_intern("sys/timer", 9);   /* the reserved channel */
int hidx    = handle_new(TAG_SMI(token));     /* pin the token oop via M6 */
k_timer_start(&t, K_MSEC(ms), K_MSEC(ms));
```

and the expiry callback is one ISR-safe line:

```c
pulse_post_isr(chan_id, hidx);
```

`pulse_pump` (already blocking `K_FOREVER` on target) then resolves the handle in thread
context and calls `space_post_id(chan_id, TAG_SMI(token))` — which is **exactly** what
`host_advance` does today via `space_post("sys/timer", 9, tok)`. Same channel, same payload,
same semantics; only the clock differs. C carries **data only** (a token, a millisecond count);
which channel and what message stay in nsl's `Timers` class.

Three consequences:

1. **`pumpEvents` idles the CPU between ticks.** It already blocks `K_FOREVER` on the pulse
   msgq, so the node sleeps 15 s and yields to the OpenThread thread — no busy-wait, and the
   right behavior for a battery node.
2. **`monotonicMillis` becomes real on target** (`k_uptime_get()`), which §4 uses to bound a wait.
3. **The host floor cannot move**, because `timer_posix.c` is the current code relocated
   unchanged: the five tests that exercise the virtual clock keep their deterministic clock and
   their `advanceMillis:` prim.

**Invariants.**
- `timer_backend_cancel` MUST `handle_free` the M6 handle, or a cancel leaks a handle slot.
- `advanceMillis:` becomes a no-op returning 0 on Zephyr (the virtual clock does not exist
  there). No native_sim node may rely on it.

---

## 3. The headless proof: a self-gating train

The gateway journal witnesses only the outbound leg. So **make the next send conditional on
the previous reflection having closed** — then an outbound log line becomes evidence of the
inbound leg, because it could not have happened otherwise.

The node holds one flag, `awaiting`:

- **timer reaction** (every 15 s) → send the `Record` **iff** `awaiting = false`; then `awaiting := true`.
- **inbound reaction** (`whenever:` on `echo/in`) → decode; **iff** value-equal to what was sent, `awaiting := false`.

The reflection is the only thing that can re-arm the sender. Reading the gateway journal:

```
echo 32B <-> [fdc5:...]:54360    t=0s     <- send #1 (first tick, unconditional)
echo 32B <-> [fdc5:...]:54360    t=15s    <- send #2 PROVES reflection #1 returned and decoded
echo 32B <-> [fdc5:...]:54360    t=30s    <- send #3 PROVES reflection #2 returned and decoded
```

**N log lines = N-1 proven complete round-trips.** A broken inbound leg (lost datagram, decode
mismatch, an `ENETDOWN`-class regression) leaves `awaiting` stuck true: the node never sends
again and the train **stops dead after one line**. There is no way for a broken inbound leg to
fake a train.

`awaiting` is **load-bearing, not decorative**: without it, a timed-out wait would simply send
again on the next tick, producing a healthy-looking train while the inbound leg was broken —
precisely the false pass this design exists to make impossible.

**Deliberate consequence:** a single dropped datagram wedges the node permanently. For a
**gate** node that is a feature — failure must be loud, never silently passing. It is not how a
production sensor node would behave, and this node is not one.

---

## 4. The node

**One source, both boards:** `zephyr/ThreadTimerNode.ns`. Console and trigger IO are the only
per-board differences and both are already board seams (`prj_*_thread.conf`, the DT overlay),
per `BOARD_PORTS.md`'s "one core, per-board IO" principle. The DK build prints `BRIDGE OK` to
VCOM; the dongle build prints to a console nobody can read — harmless. The shipped
button-triggered `ThreadEchoNodeDK.ns` is untouched; it remains the rung-7 bench gate.

**The loop** — the ordering is the one subtle part:

```
[ true ] whileTrue: [
    platform pumpEvents.          "blocks K_FOREVER until the 15s k_timer ISR posts sys/timer"
    space drain.                  "timer reaction: send the Record iff armed"
    self awaitReflection.         "bounded wait: pumpInbound + drain until it lands, or 3s"
]
```

The reflection MUST be consumed in the **same iteration as the send**. Returning straight to
the blocking `pumpEvents` after sending would let the reflection arrive during the 15 s sleep
and sit unread in the socket buffer; the next tick's reaction would then run *before* we drained
it, still see `awaiting = true`, and skip the send — the node would fire every 30 s instead of
15. The bounded wait is the pattern the DK node already proved (net RX preempts the poll), and
it is bounded with the newly-real `monotonicMillis` (3 s budget) — the first thing that pays for
the seam refactor.

**Shape change:** the node's state (`space`, `bridge`, `rec`, `awaiting`, `got`) lives in
**instance slots**, not `main:` locals, so the reaction blocks are one-line self-sends
(`[:e | self tick]`, `[:v | self reflected: v]`). This is the house style that dodges the
reified-block LD_SLOT gotcha (nsl-tuvm#10) rather than discovering it a third time.

---

## 5. Proof matrix (cheapest first)

| # | Gate | Where | Passes when |
|---|------|-------|-------------|
| 0 | **Floor unmoved** | host, both paths | `180/0/12` + the 5 named gates, green. The `timer_posix.c` lift is a no-op; confirmed BEFORE any hardware. |
| 1 | **Timer backend** | **native_sim** (no radio, no hardware, CI-able) | a short-interval `TimerNode.ns` proves `k_timer -> pulse msgq -> sys/timer -> reaction` fires N times: prints `TIMER OK`. |
| 2 | **Cadence + round-trip** | **nRF52840-DK** (console) | `BRIDGE OK` printed every 15 s **and** the gateway train ticking in lockstep. |
| 3 | **Headless** | **nRF52840 dongle** (DFU) | `ot-ctl child table` shows the attach; a sustained train (>=4 lines over a minute) in `udp-echo6`. |

Gate 1 is the real payoff of proving this on a target we can watch: `timer_zephyr.c` is proven
with **zero hardware**. Gate 2 is why the DK comes first — seeing both ends at once
**calibrates the headless gate** of §3 before we rely on it alone on a board where we cannot
check it.

---

## 6. Risks

| Risk | Mitigation |
|---|---|
| The `timer_posix.c` lift moves the host floor | It is a verbatim relocation; Gate 0 runs before any hardware work. |
| `timerCancel` leaks an M6 handle slot | `handle_free` in the cancel path; asserted by the existing timer tests + a targeted check. |
| A new engine `.c` is forgotten in a build list | ~6 scattered sites (`tools/run_c_tests.sh`, `zephyr/CMakeLists.txt`, the `run_tests_oevm.py` build list, the `tests/*.c` gcc lines, the direct-build recipe). Known gotcha; enumerate and grep. |
| `advanceMillis:` no-op on Zephyr breaks a native_sim node | Confirm by grep that no native_sim node uses it. |
| OT-thread starvation during the bounded wait | Already disproven in rung 7 (net RX preempts the non-blocking poll). |
| Dongle still headless | Out of scope by design — §3 exists precisely so this does not block the gate. |

---

## 7. Out of scope (named, not built)

- **Beating Issue A** (USB-CDC console under radio). Unchanged; the dongle stays headless.
- **An LED output seam** (`gpio set:to:`) — the natural companion to the input-pulse seam and
  the way a console-less board could self-evidence. Considered and deliberately deferred: the
  self-gating train (§3) proves the same thing with no new prim.
- **A production retry/backoff node.** This node wedges on a lost datagram *on purpose* (§3).
- FTD topology reporter; porta protocol; framing; a unified pump.
