# The timer token rides by value, and the handle goes

**Date:** 2026-08-05
**Issue:** #151 (this), #105 (closed by hardware, below), #149 (closed 2026-08-04, not this)
**Status:** design agreed 2026-08-05; implementation follows

## 1. What happened

The ship's bell C6 sounded the piezo **continuously** from ~07:00 on 2026-08-05 until it
was power-cycled. Not chirping — solid. That distinction is the whole diagnosis, and it
came from David, not from the logs: a reset loop produces repeated ~1 s bursts (that is
#149, closed the previous day by feeding 5 V and letting the module's LDO make 3.3 V).
A *solid* tone means the pin was driven high and stayed high.

There is no console record of the event. The serial collector had been `port=down` since
2026-08-04 14:54:33, and the UDP tee (#56) landed at 17:15 that day — after the board was
last flashed. So the event was unobserved in both channels at once.

## 2. What was measured

Three runs on the bench C6 (`PulseCoalesceProbeC6.nsl`, `StaleHandleProbeC6.nsl`).

| Probe | Result | Reading |
|---|---|---|
| Chain of one-shots, uncontended | 2000/2000, coalesced 0, drops 0 | control clean |
| Repeater at 10 ms under a 50 ms handler | 40 delivered, **160 coalesced**, drops 0 | coalescing destroys logical timer events; 40 + 160 = 200 exactly |
| A/B: identical but for one registration | **control 1, test 0** | a registration destroys an already-queued timer event |

The second result is a real defect and **is not** what killed the bell: a chain of
one-shots cannot hit it, because each registration owns its own handle and its post is
consumed before the handler re-arms. Coalescing needs two posts of *one* registration,
which only a repeater produces, and the bell registers none.

The third result is the bell's mechanism, and it was reproduced with a one-line
difference between the arms.

## 3. The defect

Three steps, in `nodus/tuvm/src/backend/zephyr/`:

1. `timer_zephyr.c:38` — one-shot A expires in ISR context and posts
   `pulse_post_isr(sys/timer, H)`, where `H` is the M6 handle pinning `TAG_SMI(token)`.
2. `timer_zephyr.c:56` — **any** later registration calls `reap_oneshots()` first, which
   sees A has fired and calls `handle_free(H)`; `handle_new` may hand `H` straight back
   out to the new timer.
3. `pulse_zephyr.c:76` — the pump resolves `handle_get(H)`, gets `NULL` for the stale
   generation, and discards the item. There is no counter on that branch.

A's timer event is destroyed by a later, unrelated registration. Nothing is dropped,
nothing is coalesced, nothing is logged.

This is exactly what `ShipsBellAlarmNodeC6` executes at every boundary, inside one
reaction:

```
ringSlot: → startPattern: → strikeOn
              self pin: true                    ← piezo ON
              timers after: 10 post: #stroke    ← arms A
           ('bell slot ...') out                ← UART write; since #56 also a UDP send
         → armAfter: → timers after: … #alarm   ← registration → reaps A → H freed
                     → timers after: … #watchdog
```

If the 10 ms elapses before `armAfter:` runs, A's event is destroyed, `strokeStep` never
fires, `pinOn` stays true and `striking` latches — so `startPattern:` refuses every later
boundary and nothing recovers it. Solid tone until reset. Rare, because it needs the
10 ms to elapse inside that window; concentrated at a boundary, because that is the only
place a registration follows the stroke arming within a single reaction.

### What #13 got right, and what it paid

`review/tests/CLAIMS.md` carries this as DONE:

> **A recycled M6 handle slot never mis-delivers** … `pulse_pump` drops a stale
> (NULL-resolving) item rather than posting the new occupant (tuvm#13, the ABA window)

That claim is true and the system is still broken. #13 saw the ABA window and chose
dropping over mis-delivery, which is the right call between those two. The third option —
do not free a handle while a post referencing it is in flight — was not reachable, because
nobody asked why a **SmallInteger** needed a handle at all.

## 4. Decisions

### D1 — The pulse item carries the token by value; the timer takes no handle

`timer_zephyr.c`'s own header already states the rule: the C side "carries DATA ONLY
(a token, a millisecond count)". A token is `TAG_SMI(token)` — an **immediate**. The M6
handle table exists to pin *heap* oops across ISR context so the GC cannot move them. An
immediate cannot move and needs no pinning.

So the pulse item gains a value payload, and a poster with an immediate uses it:

```c
struct PulseItem { int chan_id; int handle_idx; oop value; };
/* handle_idx >= 0 -> resolve via handle_get (heap oop, pinned, e.g. a GPIO message)
   handle_idx <  0 -> `value` IS the payload (immediate; never moves, never stales) */
```

With no handle, there is nothing to free, no slot to reuse, and no stale generation. The
bug class is deleted rather than guarded. GPIO pulses, whose payload is a heap message
object, keep the handle path unchanged.

### D2 — `reap_oneshots()` is deleted

The function exists only to call `handle_free`. Once a timer holds no handle, the sole
remaining job is marking a fired one-shot's slot reusable, which is a plain `int` store
and is ISR-safe. `timer_expiry` does it directly after posting:

```c
static void timer_expiry(struct k_timer *t) {
    struct TargetTimer *e = CONTAINER_OF(t, struct TargetTimer, t);
    if (!e->live) return;
    pulse_post_isr_value(e->chan_id, TAG_SMI(e->token));
    if (!e->repeating) e->live = 0;
}
```

`reap_oneshots()`, the `handle_idx` field, the `handles.h` include and the
`k_timer_status_get` probe all go. This is a net deletion.

### D3 — Coalescing keys on the payload, and the repeater question is deferred

Once the item carries the token, the coalescer's comparison can include it, and two
*different* timers stop looking identical. That falls out of D1 at no cost and should be
taken.

What it does **not** fix is a **repeating** timer firing twice before a pump: same
registration, same token, so the posts genuinely are identical and still merge. Measured
above at 160 lost ticks in 2 s. That is a real violation of the model — a logical event is
an obligation, not a sample — but removing coalescing for timers reopens #54/#55, where a
repeating timer starved a GPIO edge out of a 16-deep mailbox.

**Deferred, with what would earn it recorded:** a repeater whose handler outruns its
interval is a node that is already failing to keep up; the honest fix is probably to
report that rather than to silence it. This needs one datum we do not have — whether any
shipped node registers a repeater whose handler can exceed its interval. The bell does
not; it registers no repeaters at all. Until such a node exists, coalescing stays and the
loss is documented here rather than designed around.

### D4 — Some tests can only run on target, and that is now stated

The two backends deliver timer events by different paths:

```
target   timer_zephyr.c:38    pulse_post_isr(e->chan_id, e->handle_idx)
host     timer_posix.c:122    space_post("sys/timer", 9, tok)
```

The host posts **straight into the space**. It never enters the pulse mailbox, so on host
a timer event can be neither coalesced nor dropped nor staled. All 239 suite tests run on
that path, which is why a green suite was silent through all of this and always would have
been.

`StaleHandleProbeC6.nsl` is therefore kept as a **target gate**, not converted to a host
test — converting it would delete the only thing it proves. The general rule goes to
`review/tests/CLAIMS.md`: *a claim about ISR-context event delivery is unproven by the
host suite, because the host does not use that path.*

### D5 — #105 is closed in hardware, and it is a separate defect

The 47 kΩ base pull-down (schematic rev B, `2026-08-03-shipsbells-3v3-piezo-driver.svg`)
stopped the buzzer sounding at power-up — witnessed on the bench 2026-08-05. That confirms
the boot window in which GPIO18 is an undriven input, before `wire_gpio_outputs()` runs.

It is unrelated to this bug and does not mitigate it: R2 holds the base down only while
the pin is **undriven**. A solid tone is the pin being *driven* high, and no resistor
helps with that. Both fixes are needed and neither substitutes for the other.

## 5. Test plan

- `nodus/tuvm/tests/test_handles.c` — the existing `aba` property must stay green; the
  ABA window is not being reopened, it is being routed around for immediates.
- `nodus/tuvm/tests/test_coalesce.c` — extend for payload-aware coalescing (D3).
- `StaleHandleProbeC6.nsl` on the C6 — must flip to `control 1 / test 1`. That is the gate.
- `PulseCoalesceProbeC6.nsl` phase 1 must stay 2000/2000; phase 2 is expected to still
  coalesce (D3 defers it) and its number is the regression baseline.
- Full gates: nsl suite, `go test ./...`, `porta_sync_host.sh`.
- Then reflash `ShipsBellAlarmNodeC6`, return the board to wallwart power, and soak.

## 6. Rollback

D1/D2 are confined to the pulse and timer backends plus their two headers. Reverting is a
single revert; the probes and the CLAIMS row are additive and can stay either way.
