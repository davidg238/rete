# Table capacity under a real bridged node (#165 Task 1, design D9)

**Date:** 2026-08-06 · **Verdict:** 224 of 256 slots used, no overflow — **but D9 branch 2**,
because 224 is well above the ~180 the load factor allows.

## What was measured, and where

`nodus/tuvm/tests/porta_sync_host.sh`, built with
`-DOEVM_METHOD_PROFILE -DOEVM_MPROF_RUNTIME_GATE -DOEVM_MPROF_FLIP_AT=1 -DOEVM_MPROF_DUMP_AT=1000001`.

**The host gate, not the device — deliberately, and it exercises MORE of the path.** The
plan originally called for `PortaBridgeNodeC6` on a C6. That was rescoped (David, 2026-08-06)
because the flashed node never executed enough to produce a number (below).

> **Correction, same day.** An earlier revision of this document said "`~/porta-v2` does not
> exist, no `porta` binary or service is running anywhere." **That was wrong** — it was
> checked on the workstation instead of on **gw85224-01**, which is where the gateway lives
> and where `porta serve --db soak-2026-08-04.db` was running the whole time (pid 231625).
> The rescope stands on the evidence below, which does not depend on porta at all; the
> reason given for it was partly false and is corrected here.

**The device was tried first and produced nothing at all.** c6spare was flashed with the same
profile build (`FLIP_AT=1000000`) and polled for 7½ minutes: **zero `mprof` lines**, meaning
the counter never even reached its flip threshold. One million activations in 450 s is ~2200/s,
a rate no live VM would fail to exceed — so **the node was not executing**. That matches
`pulse_pump` being `K_FOREVER`-blocking on Zephyr while non-blocking on posix: with nothing
arriving to wake the pump, the bridge sleeps.

Note what this does and does not say. porta *was* up on gw85224-01 throughout, so the node's
silence is **its own** — it showed no Thread or network activity after `bind`, i.e. it never
joined the mesh. Why is untracked and out of scope here; what matters for this measurement is
that **the device could not produce a number at all**, whatever the cause.

Care was needed reading this: the log store is day-partitioned, and an unfiltered `tail`
reaches back into the *same day's* earlier spike run, which reports `methods=2` and exactly
2,621,430 activations (Hanoi). Read without a timestamp filter that looks like a valid bridge
measurement showing a 2-method program. Every read here is filtered to after the boot banner.

The host gate stands up a real porta on `127.0.0.1:16969` and drives a real node through
pull → apply → push → encode → decode. The span table from the run confirms a genuine
exchange occurred (`pullSend`, `pullWait`, `pushBuild`, `pushCollect`, `pushEncode`,
`pushSend`, `pushWait`, `pushTrim` all `n=1`). This is sound because **a distinct-method
count is a property of the program, not the processor** — the same reason a call-count ratio
measured on host holds on a C6.

Caveat kept honest: the host image is `PortaBridgeNode.nsl` and links the posix http and
sensor backends, so its method set is not identical to `PortaBridgeNodeC6`'s. A device
number remains worth taking as corroboration; it is not the gate.

## The number

```
mprof[dump]: methods=224 activations=71000000 overflow=0 cap=256
```

Stable across 71 consecutive one-million-activation windows — `methods=224` from early in the
run to the last dump, so this is the program's whole working set, not a partial warmup.

| | |
|---|---|
| distinct methods | **224** |
| capacity | 256 |
| **load factor** | **0.875** |
| overflow | **0** |
| activations observed | 71,000,000 |

**Nothing was lost.** `overflow=0` means no method was ever dropped, so every count in that
window is exact and complete. The "~224 rows" figure quoted in earlier drafts of the design
was an estimate that happens to have been right; it is now a measurement.

## Why this is D9 branch 2 anyway

Design D9 set the threshold at the ≤0.7 load factor `nodus/tuvm/src/vm.c:388` warns about,
which is ~180 distinct methods. **224 is 25% past it.** Two consequences:

**1. There is almost no headroom.** 32 free slots. Any app that adds ~30 distinct methods —
one more record type, one more driver, the `dbg/` handler this very rung adds — starts
dropping methods. Overflow is counted rather than absorbed, so the failure is visible; but it
arrives as degraded completeness in exactly the tool you reach for when something is wrong.

**2. The 0.90% cost figure does not describe this workload.** That number was measured on
Hanoi, which touches **2** methods — a load factor of 0.008, where a lookup is essentially
always a single probe. At 0.875, the standard linear-probing cost for a successful search
rises from ~1.00 probes to ~4.5.

**Do not read that as "4.5× worse."** The 4.5 is an *unweighted* average over random keys.
With linear probing a key inserted while the table was still sparse sits at its home slot, and
the hot methods are exactly the ones inserted early and hit constantly — so the
*activation-weighted* probe count could be far closer to 1. Which it is, is **unmeasured**.

The honest statement: **0.90% was measured at a load factor 100× lower than the real one, and
the cost at the real load is unknown.** It may still be under 1%. It has not been shown to be.

## What this earns

- Measuring the cost at a realistic load factor. The runtime gate makes this a
  within-binary A/B, so it is a defensible measurement rather than code-layout roulette —
  run the ON arm against a workload that has populated the table, not against Hanoi.
- **A correction to design D10's reasoning, recorded here because it is load-bearing.** D10
  argued Space-Saving displacement at k=64 would be nearly free because "misses happen once
  per distinct method, ever — the working set is small and stable." That premise **fails at
  224 methods in 64 slots**: the table would be permanently saturated and displacing
  continuously, putting the linear min-scan on a hot path instead of a cold one. k=64 is
  viable **only in combination with** D10's scoping, which shrinks the working set below the
  table. It is not viable on its own, and the design says otherwise today.

## Not decided here

Whether the answer is a larger table, displacement, scoping, or some pair of them. Per the
standing posture the measurement earns the knob back rather than choosing it — this document
supplies the number; the choice is David's.
