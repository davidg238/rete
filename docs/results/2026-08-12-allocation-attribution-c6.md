# Allocation attribution on the C6 — the boot window, the steady-state window, and an incident

**Date:** 2026-08-12
**Board:** `c6spare` (console `d2e21074`, JTAG `40:4C:CA:44:6E:60`) — **never the bell**
**Builds:** `porta-v1-final-974-g870674e-20260812T151735` (§1, the boot window) and
`porta-v1-final-976-g5ce33a8-20260812T162955` (§1a and §5, after the store repair) —
both `OEVM_METHOD_PROFILE=1`, `PortaBridgeNodeC6.nsl` + Thread FTD
**Design:** `docs/design/2026-08-12-allocation-attribution-design.md` (stage 1, D7)

**Status: the measurement D7 specified is TAKEN.** The boot window is §1; the
steady-state window — blocked for four hours by the incident in §3 — is **§5**,
taken once that store corruption was repaired. §4 records what the boot window
alone could and could not support, and is left standing because §5 answers two of
its three gaps. **One is still open and is not claimed anywhere in this document:
the hook's runtime cost (§6).**

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

## 2. What the boot window could not answer — now answered in §5

The steady-state bridging window is the measurement D7 actually specified, and
every attempt on 2026-08-12 afternoon wedged the node (§3). The boot window is
CBOR-heavy by nature and must not be read as steady state. **§5 is that window**,
taken at 16:33 once the store was repaired.

## 3. The incident — a kind-mismatched write wedged the node (this is #180)

**Cause: a `porta put` to `dbg/req` wrote it as a CELL onto a path already
established as a STREAM.** Not a defect in allocation attribution.

```
26426  c6-bench dbg/req seq=2 kind=1  'profile 10000'  08-06 22:41 out claude
276691 c6-bench dbg/req seq=3 kind=2  'profile 20000'  08-12 15:20 out david
276742 c6-bench dbg/req seq=4 kind=2  'profile 0'      08-12 15:27 out david
```

> **The values above were misread when this section was first written, and the
> misreading is worth keeping.** Python's `repr` of the stored `value_cbor`
> printed `b'mprofile 10000'`, `b'mprofile 20000'` and `b'iprofile 0'` — so the
> command looked like `mprofile`, and the third looked like a *different* verb
> (`iprofile`, plausibly "inclusive profile", #167's work). All three were
> `profile`. **The leading byte is the CBOR text-string header, and for these
> lengths it is printable ASCII**: `0x6D` = major 3 / len 13 → `m`, `0x69` =
> major 3 / len 9 → `i`. A `repr` of a CBOR value silently concatenates its
> header onto the payload and reads as a longer string. Decode, or read the hex
> and split off the head — the grammar is `profile <ms>`
> (`platform parseProfileMs:`, `nodus/tests/23_mprof/02_dbg_req_profile.nsl`).

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

### Bench state left behind — RESOLVED 2026-08-12 16:12

The store was repaired: `soak-2026-08-04.db.bak-20260812-preclean` was taken, and
events 276691 and 276742 plus the `cells` row were deleted. The db now holds
exactly one `dbg/req` row — `seq=2 kind=1 'profile 10000'`, the 2026-08-06 write —
and `c6spare` boots, joins and bridges normally. §5 was taken on the repaired
store. The paragraph below is what the state *was*, kept because the diagnosis
above depends on it.



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

## 4. What the boot window alone supported

**Supported:** the instrument works on target, names real methods, agrees with the
host on the C-heap finding, and independently corroborates #163. 80 allocating
methods is a usable stage-2 sizing input with the caveat below.

**Did not support:** any claim about steady-state allocation, about the hook's
runtime cost, or about D4's projection. Two of those three are answered in §5;
the hook's runtime cost is still unmeasured and is the one open item of stage 1.

---

## 5. The steady-state window — 65 distinct allocating methods, nested inside boot's 80

**Build:** `porta-v1-final-976-g5ce33a8-20260812T162955`, flashed 16:29:55.
**Method:** the node was left bridging on the real mesh for ~3 minutes (2 s
cadence, `misses` steady, `dropped 0`), then one window was driven from the
gateway with the ordinary operator vocabulary — **`--kind stream`, explicitly**,
which is the whole lesson of §3:

```bash
porta put --db ~/porta-v2/soak-2026-08-04.db --kind stream \
  --actor claude --reason "rete#203: steady-state alloc window" \
  c6-bench dbg/req "profile 60000"
# wrote dbg/req seq=5
```

The node applied it at tick 93 (16:33:28) and dumped at 16:34:26. Rows were read
from the collector's log, never by opening the port, and resolved against the
sidecar archived beside *this* build.

```
mprof[window]:  methods=195 activations=645551 overflow=0 cap=256
mprofa[window]: noframe_arena=0/0 noframe_cheap=0/0 allocOverflow=0
distinct allocating methods: 65
```

| | arena | C heap |
|---|---|---|
| bytes | 1 347 052 | 28 394 |
| allocations | 42 054 | 7 101 |

Over a 60 s window that is **701 arena allocations/s and 22.4 KB/s of arena
churn** on a node whose whole arena is 128 KB — i.e. the arena turns over roughly
every 5.7 seconds, indefinitely, on a node that is only bridging.

| arenaB | arenaN | cheapB | cheapN | % | method |
|---|---|---|---|---|---|
| 264 152 | 9 434 | 0 | 0 | **19.2%** | `Block_Kernel_Cbor_block40>>value:` |
| 125 952 | 5 186 | 20 201 | 5 186 | 10.6% | `Kernel>>valueCompare:with:` |
| 108 324 | 3 186 | 0 | 0 | 7.9% | `Kernel_Cbor>>appendColl:into:` |
| 64 844 | 1 247 | 0 | 0 | 4.7% | `Kernel_OrderedCollection>>init_new` |
| 62 928 | 1 748 | 0 | 0 | 4.6% | `Kernel_ByteBuilder>>init_new` |
| 47 472 | 1 915 | 8 193 | 1 915 | 4.0% | `Kernel_Cbor>>encodeSymbol:into:` |
| 55 304 | 434 | 0 | 0 | 4.0% | `Kernel_ByteBuilder>>grow` |
| 44 604 | 1 593 | 0 | 0 | 3.2% | `Kernel_Cbor>>encodeEntry:value:keys:vals:depth:` |
| 44 604 | 1 593 | 0 | 0 | 3.2% | `Block_Kernel_Cbor_block39>>value::` |
| 44 576 | 310 | 0 | 0 | 3.2% | `Kernel_ByteBuilder>>frozenBytes` |

### 5.1 The steady-state population is a strict SUBSET of the boot population

This is the finding that settles D8's sizing, and it was not the expected shape:

| window | distinct allocating methods |
|---|---|
| host, full `porta_sync_host.sh` | 64 |
| C6 boot | **80** |
| C6 steady state | 65 |
| **union of the two C6 windows** | **80** |

Every one of the 65 steady-state methods also appears in the boot window; the
boot window adds 15 more and adds no method the steady window has that it lacks.
**Boot is therefore the bound on this workload, not merely the larger sample** —
so 80 stands as the stage-2 sizing input and a 128-slot table keeps its ~0.63
load factor. Note this is a claim about *this app*: a node that does work at
runtime it never does at boot would break the nesting, which is exactly why the
number is measured per workload rather than assumed once.

### 5.2 D4's projection is refuted in steady state too, and by more than boot suggested

| window | arena allocations per activation |
|---|---|
| host | 1 per 10 300 |
| C6 boot | 1 per 5.6 |
| **C6 steady state** | **1 per 15.4** |

D4 argued the hook costs ~0.02% because allocations are "roughly two orders of
magnitude rarer than activations" — 1 in ~100. On target it is **1 in 15** while
merely bridging, and the true figure is worse than that: **65.2% of all
activations in this window are `Kernel_Socket>>receive`** — 420 580 calls of the
non-blocking poll, which allocates on only **171** of them (7 536 B, 0.6% of the
window: it allocates when a datagram actually arrives and not otherwise, which is
itself a check that the attribution is landing where it should). Against the
224 971 activations that are not the poll,
the ratio is **1 arena allocation per 5.35 activations** — the same order as the
boot window, and the honest number for "how often does this hook fire relative to
the one already in `push_frame`".

So the two C6 windows agree with each other and disagree with D4; the host's
1-in-10 300 is the outlier, produced by a long compute-bound suite. **No cost
number is quoted here**, because the cost was still not measured — see §6.

### 5.3 What it says beyond the sizing

**The codec is the whole steady-state allocation story.** Every method above
7% is CBOR, and `ByteBuilder`'s grow-then-freeze is again ~7.4% of the window
(`grow` 55 304 B + `frozenBytes` 44 576 B of 1 347 052 B) for what #163 calls
pure overhead. On the boot window that pair was 11.1%. Two independent windows,
same finding.

**`Kernel>>valueCompare:with:` is now the second-largest allocator on the node**
and carries **71% of all C-heap allocations** in the window (20 201 B of 28 394 B,
5 186 of 7 101 allocations). Two corrections to how the boot result described it:

- **It is not the encoder's sort.** The encoder orders keys by their *encoded*
  bytes through `compareBytes:with:` (`kernel.nsl:854`). `valueCompare:with:` is
  the **Map's own** ordering — the construction-time insertion sort
  (`kernel.nsl:439`) *and* every key lookup, which is a binary search
  (`kernel.nsl:393`). Two different orders for two different jobs, which
  `kernel.nsl:713` already says outright ("NOT the Map's stored valueCompare
  order — the two diverge when keys differ in encoded length").
- **The C-heap bytes are not the comparison; they are `printString`.**
  `valueCompare:with:` rank 6 is `byteCompare: a printString with: b printString`
  (`kernel.nsl:3315`) — a Symbol has no `at:`/`size` fast path in `dispatch.c`, so
  **every Symbol-vs-Symbol comparison materializes two Strings** just to read
  their bytes. Map keys on this wire are overwhelmingly Symbols. That is the
  whole 20 KB, and it is an implementation detail of the comparison rather than a
  cost of ordering anything.

Both feed #204, and they narrow it usefully: the question "do keys need to be
sorted at all?" has a cheaper sibling — "why does comparing two Symbols allocate
two Strings?" — whose answer removes the C-heap allocator without changing any
ordering semantics.

**The no-frame bucket is zero for the third time** (host, C6 boot, C6 steady), so
design §8's third risk does not bite on any measured path. The same caveat still
applies: allocation before `mprof_start()` is structurally invisible to a window.

### 5.4 Re-measured after #205 — the target C heap is EMPTY too

The boot window on build `1c5c6d2` (2026-08-12 18:35, after the store repair),
against the same window from `870674e`:

| boot window | before | after #205 |
|---|---|---|
| distinct allocating methods | 80 | **78** |
| arena bytes / allocations | 2 429 488 / 72 677 | 2 213 304 / **64 334** |
| C-heap bytes / allocations | 40 427 / 8 343 | **0 / 0** |

**Arena allocations fell by exactly 8 343 — the number of C-heap allocations
removed.** The host saw the same equality at its own scale (229 and 229), and it
is the diagnosis' fingerprint: every `printString` was one malloc AND one String,
so removing it removes one of each. Arena bytes fell 216 184 B, 8.9% of the
window, on a node whose whole arena is 128 KB.

Two of the 80 allocating methods left the population entirely, so **stage 2's
sizing input moves from 80 to 78** — inside the same 128-slot table, and worth
noting only because it is the first time the number has moved for a reason.

The steady-state half is still owed: taking it needs a `dbg/req` write, and every
such write re-arms **#206** on the next cold boot until #206 is fixed.

## 6. The one thing stage 1 still owes

**The hook's runtime cost.** D4's 0.02% projection is refuted as a *projection*
(§5.2) but nothing has replaced it with a measurement. Half the apparatus is
ready and half is not, and the half that is missing is the one that matters:

- **Ready:** `OEVM_MPROF_RUNTIME_GATE` with `OEVM_MPROF_FLIP_AT`/`DUMP_AT` puts
  both the OFF and ON arms in **one binary, one boot, one cache state**, flipping
  at a fixed activation count. That construction is not optional — a cross-binary
  OFF/ON of the activation hook on host once put the ON build **4% faster**
  (`MethodProfileNodeC6.nsl:10`), which is code-layout roulette, not a result.
- **Missing: a workload.** `MethodProfileNodeC6.nsl` drives Hanoi, chosen because
  it predicts its own activation count — and **Hanoi allocates essentially
  nothing**, so it can time the *activation* hook (that is #164's 0.90%) and
  cannot time this one. Pointing it at the allocation hook would measure a hook
  that never fires. Whatever replaces it has to allocate the way the bridge
  allocates, i.e. CBOR.

The obvious candidate — and it is a proposal, not a result — is to put the
runtime gate in `PortaBridgeNodeC6` itself and read the **existing spans** across
the flip: `sPublish` and the `pushBuild` decomposition are VM-side and exclude
the radio, so the mesh's variance does not sit in the middle of the comparison.
One binary, one boot, N ticks OFF then N ticks ON, and the arms are already
published to `sys/latency`. That also answers #166's complaint about the 0.90%
being measured at load factor 0.008, because a bridging node runs the table at a
realistic load. Deciding it is #203's remaining work.
