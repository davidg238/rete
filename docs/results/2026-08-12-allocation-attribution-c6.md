# Allocation attribution on the C6 — the boot window, and an incident

**Date:** 2026-08-12
**Board:** `c6spare` (console `d2e21074`, JTAG `40:4C:CA:44:6E:60`) — **never the bell**
**Build:** `porta-v1-final-974-g870674e-20260812T151735`, `OEVM_METHOD_PROFILE=1`,
`PortaBridgeNodeC6.nsl` + Thread FTD
**Design:** `docs/design/2026-08-12-allocation-attribution-design.md` (stage 1, D7)

**Status: PARTIAL.** The boot window succeeded and is reported below. The
steady-state window was never obtained, because driving it wedged the node — see
§3, which turned out to be a live reproduction of **#180** and not a defect in
this work.

---

## 1. The boot window — 80 distinct allocating methods

`dbg/req` is a stream and the node's cursor lives in RAM (#117), so a cold boot
replays every request porta ever wrote (#119). That fired a profile window
automatically at boot. Convenient here; see §3 for the same mechanism being
anything but.

```
mprof[window]:  methods=223 activations=409298 overflow=0 cap=256
mprofa[window]: noframe_arena=0/0 noframe_cheap=0/0 allocOverflow=0
distinct allocating methods: 80
```

| | arena | C heap |
|---|---|---|
| bytes | 2 429 488 | 40 427 |
| allocations | 72 677 | 8 343 |

| arenaB | arenaN | cheapB | cheapN | % | method |
|---|---|---|---|---|---|
| 778 064 | 27 788 | 0 | 0 | **31.5%** | `Block_Kernel_Cbor_block40>>value:` |
| 293 080 | 8 620 | 0 | 0 | 11.9% | `Kernel_Cbor>>appendColl:into:` |
| 160 276 | 449 | 0 | 0 | 6.5% | `Kernel_ByteBuilder>>grow` |
| 133 824 | 5 077 | 26 451 | 5 077 | 6.5% | `Kernel_Cbor>>encodeSymbol:into:` |
| 158 652 | 4 407 | 0 | 0 | 6.4% | `Kernel_ByteBuilder>>init_new` |
| 120 680 | 4 310 | 0 | 0 | 4.9% | `Kernel_Cbor>>encodeEntry:value:keys:vals:depth:` |
| 120 680 | 4 310 | 0 | 0 | 4.9% | `Block_Kernel_Cbor_block39>>value::` |
| 110 204 | 4 537 | 0 | 0 | 4.5% | `Kernel_Cbor>>encode:into:depth:` |
| 109 856 | 194 | 0 | 0 | 4.4% | `Kernel_ByteBuilder>>frozenBytes` |
| 82 160 | 3 252 | 13 849 | 3 252 | 3.9% | `Kernel>>valueCompare:with:` |

### What it says

**The host number was not the bound.** Host reported **64** distinct allocating
methods; target reports **80**. `docs/results/2026-08-12-allocation-attribution-host.md`
flagged this as an open possibility and it went the unhelpful way: sizing stage 2
from the host figure alone would have been wrong. 80 of 740 image methods, still
comfortably under a 128-slot table.

**`ByteBuilder`'s grow-then-freeze is 270 KB of a 2.4 MB window — 11.1%** — for
what #163 already identified as pure overhead, on a node whose whole arena is
128 KB. `grow` averages 357 B per call across 449 calls; `frozenBytes` averages
566 B across 194. #163's proposed fix (size the accumulator instead of growing
then freezing it) now has an allocation-volume argument on top of its
activation-count one.

**The C-heap finding from the host holds on target, and stays confined to the
same two methods** — `encodeSymbol:into:` and `valueCompare:with:`, both on the
map-key path, together 40 427 B across 8 343 allocations. Two independent
environments, same two names.

**The no-frame bucket is again zero**, so design §8's third risk does not bite on
this path either. Same caveat as the host result: boot allocation before
`mprof_start()` is structurally invisible to a window.

### One number that refutes D4's framing

D4 projected the hook at ~0.02% on the reasoning that allocations are "roughly
two orders of magnitude rarer than activations". On the host window that held —
1 374 arena allocations against 14.1 M activations, about **1 in 10 300**. On this
target window it does **not**: 72 677 arena allocations against 409 298
activations is about **1 in 5.6**.

That is a three-orders-of-magnitude difference in the ratio, and it means the
D4 projection cannot be carried into stage 2 unexamined. The honest reading is
that the two windows measure different things — the host window spans a long
mostly-non-allocating run, this one spans CBOR-heavy boot — and that **neither is
the steady-state number stage 2 needs.** The cost measurement D4 asked for
remains unmade.

## 2. What was NOT obtained

The steady-state bridging window — the measurement D7 actually specified. Every
attempt wedged the node (§3). The boot window is CBOR-heavy by nature and should
not be read as steady state.

## 3. The incident — a kind-mismatched write wedged the node (this is #180)

**Cause: a `porta put` to `dbg/req` wrote it as a CELL onto a path already
established as a STREAM.** Not a defect in allocation attribution.

```
26426  c6-bench dbg/req seq=2 kind=1  b'mprofile 10000'  08-06 22:41 out claude
276691 c6-bench dbg/req seq=3 kind=2  b'mprofile 20000'  08-12 15:20 out david
276742 c6-bench dbg/req seq=4 kind=2  b'iprofile 0'      08-12 15:27 out david
```

`KindStream = 1`, `KindCell = 2`. `dbg/req` was established as a stream on
2026-08-06. Both of today's writes landed as **cells**, and the node wedges when
it applies one: silent, no panic, no `VM GC Error`, no `ZEPHYR FATAL ERROR` —
it simply stops mid-stride about 5 s after boot.

**This is #180 observed live**: *"Store.Write does not pin kind, so D6 is
unenforced on the gateway side — the write that wedges a node (#174) is legal
here."* The gateway accepted both writes without complaint. It also created a
`cells` row for a stream path, which should not exist.

### The elimination that got there

Three builds were flashed, and **all three hung identically at the same tick**:

| build | allocation hooks | table | result |
|---|---|---|---|
| `OEVM_METHOD_PROFILE=1`, 256 slots | in | 256 | hang, tick 6 |
| `OEVM_METHOD_PROFILE=1`, 1024 slots | in | 1024 | hang, tick 6 |
| `OEVM_METHOD_PROFILE=1`, hooks compiled out | **out** | 256 | hang, tick 6 |
| **no `OEVM_METHOD_PROFILE` at all** | **absent** | — | **hang, tick 6** |

The last row is decisive: a build with no profiling code whatsoever hangs the
same way, so the cause is in the **store**, not the binary. Table capacity is
refuted along the way, which also clears #166 of any part in this.

The bell (`c6`) stayed healthy throughout — check-in 2029, `misses 35` — which is
what rules out the mesh and porta itself.

### Why it looked like a code defect for so long

Because the first window *worked*. The boot window in §1 replayed `seq=2`, which
is `kind=1` and correct. Only once a `kind=2` row existed did every boot wedge.
A capability that works, then stops working after an unrelated write to a
different tool's path, is a genuinely hard shape to attribute — and the instinct
to blame the code that changed most recently was wrong.

### Bench state left behind — NEEDS DAVID

- **`c6spare` is wedged** and will re-wedge on every boot while events 276691 and
  276742 exist, because a cold boot replays from cursor 0. It ended a ~15.8 h
  soak (tick 29248, `acked 204756`) that had been running since 2026-08-11.
- **The fix is to delete those two `events` rows and the `cells` row for
  `c6-bench dbg/req`**, restoring the store to its pre-2026-08-12 state. That is a
  destructive write to a live shared production store and was **not** done — it
  needs David's call. Back the db up first.
- The bell is untouched and healthy.
- A git worktree remains at
  `<scratchpad>/ctrl` (detached at `0c91571`); removing it was also declined.

## 4. What this result does and does not support

**Supports:** the instrument works on target, names real methods, agrees with the
host on the C-heap finding, and independently corroborates #163. 80 allocating
methods is a usable stage-2 sizing input with the caveat below.

**Does not support:** any claim about steady-state allocation, about the hook's
runtime cost, or about D4's projection. Stage 1 is **not complete**, and its
central deliverable — a steady-state window — still needs one clean run once the
store is repaired.
