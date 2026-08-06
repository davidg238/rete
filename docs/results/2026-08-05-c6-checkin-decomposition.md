# Where the 300 ms goes — c6-bench, 2026-08-05

The decomposition the [first capture](2026-08-05-c6-span-capture-first.md) said was the
next thing to do. Same node, same window shape (one boot-to-verdict capture, opened in
`main:args:` and closed in `emitVerdict`), same n = 2 — but `checkIn` is now six spans
inside `PortaBridge` rather than one span measured from the app.

Build `porta-v1-final-832-gf54f2d2-20260805T231753`, gate `TARGET 1/1 PASS`
(bridge-health 3/3). Read with
`ssh gw85224-01 'cd ~/porta-v2 && ./porta get --db soak-2026-08-04.db c6-bench'`.

## The numbers

Per check-in, from a two-sample window (`sumUs / 2`):

| span | µs / check-in | % of `checkIn` | what it covers |
|---|---:|---:|---|
| `pushBuild` | 188 968 | **61.9%** | `collectOutbound` + CBOR encode + the trim re-encodes |
| `pushWait` | 44 256 | 14.5% | wait for the `#ack`, and apply it |
| `pullWait` | 25 102 | 8.2% | wait for the `#batch` |
| `applyIn` | 23 628 | 7.7% | apply the batch, publish `sys/metrics` |
| `pullSend` | 19 818 | 6.5% | encode the `#pull` frame and send it |
| `pushSend` | 3 070 | 1.0% | send the `#push` frame |
| — | | | |
| `checkIn` (the six together) | 305 334 | 100% | |
| `jobTick` (whole tick) | 314 473 | | |
| `publish` (health + `tel/log`) | 8 180 | | |

**The partition holds.** `checkIn − Σ(six)` is 492 µs per check-in, 0.16%. That is the
number that makes the rest of the table readable: there is no un-bracketed time hiding
inside `checkIn`, so every percentage above is a share of something real rather than a
share of what happened to be measured.

**The brackets are free.** `checkIn` reads 294 525…316 143 µs here against 298 546…314 583
in the first capture, and `jobTick` 303 920…325 026 against 307 808…323 373. Twelve extra
`begin:`/`end:` prim calls per check-in cost nothing at this resolution — which is what
makes the comparison to the earlier capture legitimate rather than a comparison of two
differently-instrumented nodes.

## What it says

**The 300 ms is the VM, not the radio — 70/30.** Two sends and two waits together are
92 ms, 30.2%. `applyIn` and `pushBuild` together are 213 ms, 69.6%.

**And it is one method.** `pushBuild` alone is 61.9% of the check-in and 60.1% of the
whole tick. `PortaBridge>>pushFrame` is where a bridged node spends most of its life.

**The fragmentation hypothesis is dead, and it was the argument for taking this split
rather than the cheap one.** The reasoning was that a push frame runs to the 512 B cap and
rides five or six 6LoWPAN fragments, so `pushSend` was the place a surprise could hide,
and a coarser "radio wait versus everything else" split would report a fragmentation cost
as interpreter cost. `pushSend` measured **3.1 ms — the smallest span in the table.**
Sending the big frame is the cheapest thing the bridge does.

So the split earned its keep, but not for the reason it was taken. The cheap two-span
probe would in fact have reached the right *verdict* — waits are 69 ms against 236 ms of
everything else, which reads as VM-dominated — and would have pointed at nothing. What the
six-way split bought is the **name of the method**, which is the difference between a
finding and a direction.

## Why `pushFrame` is expensive — read from the source, not measured

Not decomposed further, so this is a reading of `nodus/nsl/lib/kernel.nsl` and is stated as
a hypothesis:

- `valueFits:` encodes **every candidate cell in full** (`(Cbor new encode: v) byteSize`)
  purely to check it against the 256 B value cap, and **throws the bytes away**.
- `encodePush: rows` then encodes the whole frame, re-encoding every one of those values a
  second time.
- The trim loop re-encodes the **entire frame again** for each row it drops, and it drops
  rows one at a time.

So a row's value is encoded once to be measured, once to be sent, and once more per trim
iteration — on a hand-rolled CBOR codec running in an interpreter. That shape is superlinear
in the number of outbound rows, which matters now: this change took a bridged gate node from
5 registered span slots to 11, and the cell count per capture window went from 5 to 11 with
it.

Two things this window does **not** settle, and both are cheap to settle next:

1. **How much of the 189 ms is the trim loop.** The window's two check-ins happen before
   `emitVerdict` writes the span cells, so the `diag/` tree held only `ctl` while these
   samples were taken — the measurement is *not* inflated by the eleven cells this change
   added. But it also means the number is a floor, and a steady-state tick draining a
   finished window has more rows to trim.
2. **Whether the encode is one slow thing or many.** Splitting `pushBuild` into
   `collectOutbound` / `encodePush` / `trim` needs three more slots, which the raised knob
   already affords.

Filed as **tuvm#161**.

## What it does not say

- **n = 2, still.** Two check-ins is a window, not a distribution. min and max differ by
  7% on `checkIn` and 17% on `pushBuild`; the `ge1s` bucket is empty on every span, which
  is the only tail claim supported.
- **Nothing about a loaded mesh.** The bell was the only other node checking in.
- **`pushBuild` is not decomposed.** The three-way reading above is from the source.

## What this earns for tuvm#159

The deferral stands, and it is now better argued. #159 asks whether porta should be able to
drive `diag/spans/ctl` — and the reason to want that was a *repeating* window, to get a
distribution instead of a boot-to-verdict snapshot. On this evidence the thing worth
characterising is `pushFrame`, which is VM-side, deterministic, and needs no mesh and no
remote control to measure — it wants three more spans and another boot, not an authority
change. **Nothing here earns a second exception to the inbound authority rule.** The
option to beat remains a node-chosen free-running cadence, which adds no mechanism at all.

Related: `protocol/PROTOCOL-NSL-RECORDS.md` §4.6, `protocol/PROTOCOL-NSL.md` §4.1,
`nodus/tests/22_spacesync/11_bridge_span_brackets.nsl` (the host gate on the bracketing),
`docs/results/2026-08-05-c6-span-capture-first.md`.
