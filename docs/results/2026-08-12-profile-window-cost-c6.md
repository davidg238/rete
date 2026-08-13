# What an open attribution window costs on a C6 — +1.9% of local CPU, +0.13% of elapsed

**Date:** 2026-08-12 (evening)
**Board:** `c6spare` (console `d2e21074`, JTAG `40:4C:CA:44:6E:60`) — **never the bell**
**Build:** `porta-v1-final-984-gef954ac-20260812T190858`, `OEVM_METHOD_PROFILE=1`,
`ProfileCostNodeC6.nsl` + Thread FTD
**Design:** `docs/design/2026-08-12-allocation-attribution-design.md` §9.7 (the one
thing stage 1 still owed)
**Issues:** rete#203 (this measurement), rete#166 (bounded here, not settled),
rete#205 (§7 answers the re-measurement it left owed)
**Rig:** `nodus/tuvm/zephyr/ProfileCostNodeC6.nsl`, read by
`nodus/tuvm/zephyr/tools/profile_cost.py`

---

## 1. The answer

Opening a per-method attribution window — both hooks, the activation counter and
the allocation columns, since one flag gates both — costs

| framing | figure |
|---|---|
| of the node's **local CPU** (the work the VM actually does per check-in) | **+1.88%** |
| of **elapsed time** at the 2 s check-in cadence | **+0.13%** |
| per hooked event (activation or allocation), derived — see §6 | **~0.30 µs** |

measured at a table load factor of **0.77** (196 methods in 256 slots), which is
the load a real bridging node produces — not the 0.008 the quoted 0.90% came
from.

**This replaces D4's ~0.02% projection, which §9.2 refuted and which no one may
quote.** The projection was wrong by roughly an order of magnitude on the elapsed
framing and by two on the CPU framing.

**It is one number, not two.** `g_mprof_enabled` gates the activation counter and
the allocation columns together, so no runtime A/B can separate them. §6 says what
that leaves open for #166.

---

## 2. The rig, and the three things it is defending against

```
settle (10 ticks of acked bridging)
  -> [ A1 off | B ON (dump at close) | A2 off ] x 8 rounds
```

Every block is 30 measured ticks at the 2 s cadence (60 s), behind a 3-tick
lead-in that is thrown away.

**One binary, one boot, one cache state.** Not a preference. A cross-binary OFF/ON
of the activation hook once put the ON build 4% *faster*
(`MethodProfileNodeC6.nsl:10`) — code-layout roulette. Here the only difference
between arms is one byte of VM state.

**A2 is the control, and it is free.** An OFF arm needs no prim call, so bracketing
B costs nothing and buys the only honest yardstick for the ON delta: the spread
between two OFF blocks taken either side of it, *during the same measurement*.
Measured drift is 0.58%, against a 1.88% effect.

**The claim is a sign test, not a mean.** `on` being the peak of its own triple in
8 rounds out of 8 is something a lucky triple cannot manufacture: under the null —
drift only, no hook cost — the middle block in time is the peak with probability
1/3, so 8/8 lands at p = (1/3)^8 ≈ 1.5 × 10⁻⁴. The first run of this rig produced
a single triple reading +1.56%, with drift a third of that, and that is exactly the
shape of a number that should not be published.

**No porta write anywhere.** The spans are read out of the table and printed, not
published through `SpanCapture`, and the node deliberately does **not** wire the
`dbg/req` reaction. Every `dbg/req profile` request is a permanent replay row that
re-arms #206 on the next cold boot, and a replayed request would also open a third
window underneath the arms.

---

## 3. The result

`LOCAL` is `pullSend + applyIn + pushBuild + pushSend + publish` — everything
inside `checkIn` that is not a radio wait, plus `publish`. Microseconds summed over
each block's 30 ticks.

| round | off-a | **on** | off-b | on − off | on − off % | drift % |
|---|---|---|---|---|---|---|
| 1 | 3 974 875 | **4 071 642** | 4 021 923 | 73 243 | 1.832 | 1.177 |
| 2 | 4 013 876 | **4 071 690** | 4 011 465 | 59 020 | 1.471 | 0.060 |
| 3 | 4 013 207 | **4 089 865** | 3 994 960 | 85 782 | 2.142 | 0.456 |
| 4 | 4 011 861 | **4 091 859** | 3 998 005 | 86 926 | 2.170 | 0.346 |
| 5 | 4 038 281 | **4 114 933** | 4 009 576 | 91 004 | 2.262 | 0.713 |
| 6 | 4 012 069 | **4 090 581** | 4 000 168 | 84 462 | 2.108 | 0.297 |
| 7 | 4 034 969 | **4 093 753** | 4 000 116 | 76 210 | 1.897 | 0.868 |
| 8 | 4 044 040 | **4 076 333** | 4 013 668 | 47 479 | 1.178 | 0.754 |

**mean +1.883%, sd 0.356%, drift 0.584%, sign test 8/8.**

Per span, same eight rounds:

| span | mean on − off | drift | sign test |
|---|---|---|---|
| `pushBuild` (the whole outbound build) | **+2.05%** | 0.77% | 8/8 |
| `pushEncode` (CBOR encode, nested in pushBuild) | **+1.98%** | 1.08% | 8/8 |
| `publish` (the two local writes) | **+1.89%** | 0.47% | 8/8 |
| `applyIn` (inbound apply) | **+1.72%** | 0.58% | 8/8 |

The four agree to within their own spread, which is what a real per-event cost
should look like: these spans do different work in different proportions, and a
constant tax per activation-and-allocation lands on all of them alike.

**Misses were not what produced it.** The bridge caught 20 missed check-ins across
the run; per block they fall on OFF arms as often as ON (`r5[4/3/0]`, `r7[1/0/0]`).
On the four rounds where the ON block caught **no** miss at all, the effect is
**+1.68%** — smaller, and still there.

### The elapsed-time framing

Local CPU is 4.00 M µs per 60 s block, i.e. the VM does ~6.7% of a wall second's
work per wall second at this cadence. The delta is 75.5 ms per 60 s block =
**+0.126% of elapsed time**, or ~2.5 ms added to a 2 s tick.

Both framings are needed and they answer different questions. +1.9% is what it
costs *while the VM is running*; +0.13% is what an operator would see. Quoting only
the second is how "acceptable in the field" becomes unfalsifiable.

---

## 4. The radio spans are noise, and that is a check on the method

The exclusion rule (`LOCAL` above) was written into `profile_cost.py` before these
eight rounds existed. Running the same analysis on the spans it excludes:

| span | mean on − off | drift | sign test |
|---|---|---|---|
| `pullWait` | +38.8% | 8.3% | 3/8 |
| `pushWait` | +32.2% | 75.2% | 2/8 |
| `checkIn` | +15.2% | 31.4% | 3/8 |
| `jobTick` | +14.7% | 30.5% | 3/8 |

Sign tests at chance, drift larger than any effect, and means whose sign flips when
the four clean rounds are taken alone (`jobTick` goes from +14.7% to **−13.2%**).
This is what the measurement looks like when the radio is in it. `jobTick` and
`checkIn` are excluded for the same reason: `pullWait` + `pushWait` are 30% of
`checkIn`, and they swamp a 2% signal.

---

## 5. The activation count swings 1.7×, and it is one method

Per-window activations ranged 287 159 … 493 553 while the timing stayed flat — a
1.7× swing in work with no swing in time, which is the shape of a measurement
lying. It is not. **The entire swing is `Kernel_Socket>>receive`**, 46 305 …
253 177 activations; every other method in the table moves by less than 0.5%.

| | mean | range | sd |
|---|---|---|---|
| activations per window | 357 899 | 287 159 … 493 553 | — |
| of which `Socket>>receive` | 117 204 | 46 305 … 253 177 | — |
| **everything else** | **240 695** | **240 360 … 240 916** | **227 (0.09%)** |

`receive` is the non-blocking poll inside the wait loop: it spins until the reply
lands, so its count is a measure of *radio latency* and its time is charged to
`pullWait`/`pushWait`. The non-poll population is constant to 0.09% across all
eight windows, which says the workload really is identical block to block, and
independently justifies §4's exclusion — the poll and the noise are the same thing.

Consequence worth stating: the hook fires on those poll activations too, and that
cost lands in the excluded spans. **§1's +1.9% therefore understates total CPU** by
whatever the hook costs on ~117 k poll activations per window. It is time the node
spends waiting with nothing else to do, which is why it is not charged here — but
it would be charged on a node that had other work.

---

## 6. Per event, and what it does and does not say about #166

Per 30-tick block: **218 813** non-poll activations and **34 352** arena
allocations. Against a 75 516 µs mean delta:

> **~0.30 µs per hooked event** ≈ 48 cycles at 160 MHz.

Two assumptions, both of which push the true figure **up**, not down: that the
activation hook and the allocation hook cost the same per event, and that every
non-poll activation falls inside a measured span (some are in the wait spans and in
`drain`, so the real denominator is smaller). Read it as a floor.

**For #166 this is a bound, not an answer.** #166 asks what the *activation*
counter costs at a realistic load, and it names exactly this experiment — "run the
ON arm against a workload that has populated the table (the bridge, not Hanoi) and
compare to the OFF arm in the same binary". This build cannot honour the "activation
counter alone" half: stage 1 put the allocation columns behind the same
`g_mprof_enabled`, so the ON arm is both hooks. What can be said:

- the activation counter at load 0.77 costs **at most** 0.30 µs per activation,
  against **0.106 µs** measured at load 0.008 (#164);
- so the honest reading of #166's worry is that the probe chain is **at most ~2.8×**
  longer in cost at the real load, not the ~4.5× an unweighted linear-probing
  average predicts — consistent with #166's own note that the hot methods are
  inserted early and sit at their home slots.

**Separating the two hooks needs a second workload in this same binary** — a block
that allocates nothing but activates (Hanoi), run OFF and ON like the others, which
gives per-activation cost directly and per-allocation cost by subtraction. The rig
already has the sequencer for it. Not built: it is beyond what #203 asked for.

---

## 7. The steady-state window #205 left owed — answered, without a porta write

`docs/results/2026-08-12-allocation-attribution-c6.md` §5.4 recorded that the
steady-state re-measurement after #205 was still owed and was blocked, because
taking it needed a `dbg/req` write and every such write is a permanent replay row
(#206). This rig drives its own window, so it is not blocked.

Every one of the eight ON windows reports **196 methods / 256 slots (load 0.77),
`overflow=0`, `allocOverflow=0`, no-frame bucket 0/0, C-heap 0 B in 0 allocations,
and 69 distinct allocating methods** — identical in all eight.

Three of those 69 rows are the measurement harness itself and belong to no bridging
node:

| row | bytes | allocs |
|---|---|---|
| `ProfileCostNodeC6>>printSpans` | 16 664 | 336 |
| `ProfileCostNodeC6>>endBlock` | 836 | 19 |
| `ProfileCostNodeC6>>beginBlock:` | 260 | 8 |

(The other five app-owned rows — `tick`, `report`, `publishHealth` and two reaction
blocks — have exact counterparts in `PortaBridgeNodeC6`.)

**So the post-#205 steady-state population is 66 methods, against 65 measured
before #205** (§5 of the c6 results doc). #205 removed two `printString`
allocators, and it did **not** move the steady-state allocating population —
those two were boot-window methods, which is why boot went 80 → 78 and steady state
did not move. The union bound stage 2 is sized from is unchanged.

Allocation density in steady state, in both conventions, because they are not
interchangeable:

| | this window | recorded pre-#205 (§5) |
|---|---|---|
| per **non-poll** activation | 1 per **6.4** | 1 per 5.35 |
| per **all** activations | 1 per **9.5** | 1 per 15.4 |

**Only the first row means anything.** §5 above showed the poll count swinging 5×
with radio latency alone, so the "all activations" denominator moves with the mesh
and not with the workload — 15.4 versus 9.5 is a statement about two afternoons'
radio conditions. Quote the non-poll figure. Either way D4's "two orders of
magnitude rarer than activations" is refuted here for the third time.

---

## 8. What is NOT claimed

- **The cost of the profiler being BUILT IN.** Both OFF arms still pay the
  load-and-branch in `mprof_bump` and the non-inlinable call into `mprof_alloc`
  from `gc.c`, because those are compiled in. That is a cross-binary question, and
  cross-binary is the comparison this rig exists not to make.
- **A split between the two hooks.** §6.
- **Anything about a table that overflows.** `overflow` and `allocOverflow` were 0
  in all eight windows at load 0.77. The cost of a saturated table is unmeasured.
- **Anything at another cadence.** At 2 s the node is ~6.7% busy; the elapsed-time
  framing in §1 scales with duty cycle and the CPU framing does not.
- **Generalisation off this workload.** The tax is per activation and per
  allocation, so a workload with a different mix pays differently — which is the
  whole lesson of D4 having been derived from the host window.

---

## 9. Reproducing it

```bash
source ~/zephyr-esp32/.venv/bin/activate
source ~/zephyr-esp32/zephyr/zephyr-env.sh
cd nodus/tuvm/zephyr
export ESPTOOL_PORT=/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_40:4C:CA:44:6E:60-if00
export CONSOLE_PORT=/dev/serial/by-id/usb-Silicon_Labs_CP2102N_USB_to_UART_Bridge_Controller_d2e21074c7beed11a5935a4371c9e8b5-if00-port0
OEVM_METHOD_PROFILE=1 bash run.sh --board esp32c6 --node ProfileCostNodeC6.nsl --flash \
  --conf-extra "prj_esp32c6_thread.conf;prj_esp32c6_thread_ftd.conf"

# ~3.5 min per round; it repeats until the board is reflashed
python3 tools/profile_cost.py ~/.local/state/rete/console/console-c6spare-$(date +%F).log \
  --span pushBuild --span publish --span applyIn
```

Both ports must be pinned: the bench has two C6 JTAG nodes and two CP2102Ns, and
`run.sh` refuses to guess either. `--conf-extra` is not optional — without it the
image has no OpenThread, and every arm measures a bridge talking to nobody.
