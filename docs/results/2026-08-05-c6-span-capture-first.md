# First span capture off a real node — c6-bench, 2026-08-05

**What produced it:** tuvm#107 (span rows became sibling cells) plus `diag/**` joining the
bridge's outbound cell scan. `PortaBridgeNodeC6` runs one capture window per boot, opened in
`main:args:` and closed in `emitVerdict`, so the window is the certification run itself.
Build `porta-v1-final-828-g97a4912-dirty-20260805T213000`, gate `TARGET 1/1 PASS`
(bridge-health 3/3).

Read with `ssh gw85224-01 'cd ~/porta-v2 && ./porta get --db soak-2026-08-04.db c6-bench'`.

## The numbers

| span | n | minUs | maxUs | decade |
|---|---|---|---|---|
| `jobTick` (whole tick) | 2 | 307 808 | 323 373 | b4 — 100 ms…1 s |
| `checkIn` (pull + push) | 2 | 298 546 | 314 583 | b4 — 100 ms…1 s |
| `publish` (health + tel/log writes) | 2 | 8 021 | 8 167 | b2 — 1…10 ms |
| `httpQueue`, `egress` (reserved, C-owned) | 0 | — | — | — |

Summary: `SpanCapture { n: 6, ms: 2339 }`.

## What it says

**A check-in costs ~300 ms and is 97% of the tick.** Everything else the node does in a tick
— writing `obs/health`, queueing a `tel/log` row — is 8 ms together. At the 2 s cadence the
node spends roughly 15% of wall-clock inside one `bridge checkIn`.

**The host figure is 5.7 ms for the same span** (`porta_sync_host.sh`, loopback UDP). That is
a **53x** gap, and it is the whole argument for measuring on target: the host number is not a
slow version of the target number, it is a measurement of a different thing — no radio, no
mesh, no 6LoWPAN fragmentation. Nothing in the host suite could have suggested 300 ms.

**`sys/metrics` cannot express this.** It reports how the heap stands, never where time goes.
The two records answer different questions and neither substitutes for the other.

## What it does not say

- **n = 2.** Two check-ins is a window, not a distribution. min and max differ by 16 ms
  (5%), which is suggestive of stability and proves nothing about the tail. The `ge1s`
  bucket (b5) is empty across both samples, which is the only tail claim currently supported.
- **Nothing about a loaded mesh.** The bell was the only other node checking in.
- **No breakdown inside `checkIn`.** Pull versus push, radio versus VM, retry versus first
  try — all invisible. `checkIn` is one span because that is where the app's instrumentation
  boundary is; splitting it needs spans inside `PortaBridge`, in the kernel.

## What this earns

The deferral in tuvm#159 (porta cannot drive `diag/spans/ctl`, so the window is
free-running) was made on the grounds that a boot-to-verdict window might be enough. On this
evidence it is enough to *find* the 300 ms, and not enough to characterise it — n = 2. The
next question is whether the answer wanted is a distribution (which wants a repeating window,
hence a period, hence #159 or a node-chosen cadence) or a decomposition of `checkIn` (which
wants kernel-side spans and no protocol change at all).

**The decomposition is the cheaper and more informative of the two, and it needs neither
#159 nor a tunable.**

> **ANSWERED THE SAME DAY — `docs/results/2026-08-05-c6-checkin-decomposition.md`.**
> `checkIn` is now six spans inside `PortaBridge`. The 300 ms is **70% VM and 30% radio**,
> and `PortaBridge>>pushFrame` alone is 62% of the check-in (tuvm#161). The paragraph above
> is left as written: it called the decomposition the informative one and it was right, but
> for the wrong reason — the fragmentation cost it expected to find in the push send is not
> there. `pushSend` is the cheapest span in the table.

Related: `protocol/PROTOCOL-NSL-RECORDS.md` §4.6, `protocol/PROTOCOL-NSL.md` §4.1,
`docs/results/2026-07-20-c6-heap-ceiling.md`.
