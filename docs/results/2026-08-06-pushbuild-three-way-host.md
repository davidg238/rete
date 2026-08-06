# `pushBuild` is three spans — the host half, and the two things it found on the way

tuvm#161's falsifiable precondition: *split `pushBuild` into collect / encode / trim
before touching either proposed fix, or "the trim loop is expensive" is a reading of the
source rather than a finding.* The split has landed and is green on all three gates. The
**C6 capture it exists for has not been taken — `c6bench` has been off the bench since
07:46 today** (see "What is blocked", below), so what follows is the host half plus two
findings the host run produced that change how the C6 number must be read.

The spans are `pushCollect` / `pushEncode` / `pushTrim`, nested inside `pushBuild`. They
carry the leg prefix the other six use rather than #161's method names
(`collectOutbound` / `encodePush` / the trim loop), so the nine sort adjacent in a
rendered table; the mapping is one-to-one and is stated in the bracket comment on
`PortaBridge>>pushFrame`.

## The host numbers

`nodus/tuvm/tests/porta_sync_host.sh`, one boot-to-verdict window, n = 1, posix backend:

| span | µs | % of `pushBuild` |
|---|---:|---:|
| `pushEncode` | 1 312 | 62.9% |
| `pushCollect` | 768 | 36.8% |
| `pushTrim` | 1 | 0.05% |
| — | | |
| `pushBuild` (the three together) | 2 085 | 100% |

`pushBuild − Σ(three)` is 4 µs, 0.2%. The partition holds, which is the only property
that makes the shares readable.

**Read this as a shape, not as an answer.** It is one sample on a host VM at ~90x the
C6's speed, on a frame of three rows. What it does establish is that the bracketing is
correct and that the two terms #161 wants to separate — the `valueFits:` encode-and-discard
in `collectOutbound`, and the single full-frame encode — are both real and are roughly
1:2. Fix (1) of #161, carrying `valueFits:`'s bytes into the row, is aimed at the smaller
of those two on this evidence.

## Finding 1 — the boot-to-verdict window is structurally blind to the trim loop

`pushTrim` reads **1 µs against a full sample count**, and that is not "trimming is
cheap". It is the bracket firing on a tick where the loop never ran.

The trim loop only runs when the frame exceeds 512 B, and on this node the thing that
first makes a frame exceed 512 B is **the span capture's own output** — one
`diag/spans/<slot>` cell per slot, 14 of them, written in one burst when the window
closes. The window closes *before* those cells exist. So the capture cannot see the
ticks it causes.

That is worth saying plainly: **the instrument's output is what creates the condition
the instrument wants to measure, and the window ends first.** A capture taken in this
shape will report `pushTrim ≈ 0` on the C6 too, and a reader who takes that at face
value will conclude the trim loop is free and close #161's fix (2) as unearned.

**This is what `PortaBridgeNodeC6.nsl` named as the thing that would earn a repeating
window** ("what would earn it: this capture showing that one boot-to-verdict window does
not answer where the time goes"). It does not answer it. That is an argument for the
node-chosen free-running cadence — tuvm#159's fourth option, which adds no mechanism and
no authority change — and not for porta driving `diag/spans/ctl`.

One caveat in the other direction, so this is not over-read either: on the C6 the mesh
attach takes up to 90 s and `tel/log` accumulates in `pending` throughout, so the first
check-in may well face a backlog over 512 B and trim hard. The C6's `pushTrim` may
therefore be large where the host's is zero — which is itself a finding, and is a second
reason the number has to be taken on the node rather than inferred from here.

## Finding 2 — the span cells starve `sys/metrics`, and it is tuvm#150 biting

Adding three span slots turned `porta_sync_host.sh` **red**, at
`sys/metrics appears 1 time(s) in the event log, expected >= 2`. Verified as a
regression against a stashed tree — the same gate is green at `dd66d1e` and red with the
three spans, so this is the change's doing and not a flake.

The mechanism is #150 exactly, and it is transient rather than permanent:

- the capture publishes 14 `diag/spans/*` cells in one burst;
- that burst is a backlog far bigger than one 512 B frame, so `pushFrame` trims;
- it trims **from the tail — the highest seq — which is `sys/metrics`**, written
  immediately before the push;
- so until the backlog drains, the newest cell is the one that never rides.

Measured directly rather than argued (scratch probe, 30 samples at 2 s, same host setup):
the burst drains in **~8 ticks / ~16 s** with 14 slots, and from t=18 s onward
`sys/metrics`, `obs/health` and `tel/log` each accumulate exactly one row per tick. Before
that, `sys/metrics` rode once — at t=2 s, before the burst existed.

Corroborated by falsifying it the other way: raising the node's `maxBatchBytes` to 2048
makes porta reject every push after the burst (`obs/health never reached porta`), which
is only possible if those frames are over 512 B. They are.

**What changed in the gate, and what deliberately did not.** The assertion is unchanged —
still `>= 2`, still a hard FAIL, never a skip. What changed is that it now *waits* for
the second row (bounded, 60 s) instead of counting after a fixed `sleep 3`. The old form
was passing on the drain finishing inside 7 ticks, which is an assumption that gets
shorter every time any node registers another span. A gate whose green depends on a race
it does not name is a gate that will go red for the wrong reason later.

**What was NOT done, on purpose.** No trim policy was invented — no reserved byte budget
for `sys/`, no trim-by-type, no protected write-once list. All three are the design's own
parked question (`kernel.nsl`, the comment above `pushFrame`, and #150), all three need
constants there is no data to choose, and #161's whole posture is that the measurement
comes before the fix. What this run adds to #150 is that it is now **measured, reproducible
on host in 20 seconds, and coupled to the span count** — a node that registers more spans
starves its own metrics for longer.

## What is blocked

`c6bench` (adapter `ac2dce6f…`) dropped off USB at **2026-08-06T07:46:45** —
`port lost: device reports readiness to read but returned no data` — and has been
`port=down` since. It is not in `/dev/serial/by-id/`, so the board is unplugged or
unpowered; `./run.sh bridge-health` refuses at the collector check, correctly.

The only C6 on the cable is `c6` — **the ship's bell**, which is up, bridging, and at
check-in 82. It is not a substitute: flashing it ends a soak that cannot be restarted,
and the harness refuses it twice over (`assert_not_the_bell`, then the by-id backstop).

**To take the capture: reattach `c6bench`, then**

```
nodus/tuvm/tests/target/run.sh bridge-health
ssh gw85224-01 'cd ~/porta-v2 && ./porta get --db soak-2026-08-04.db c6-bench diag'
```

and read the three new cells against `pushBuild`. Gates green as of this writing:
nsl suite 246/0, `go test ./...` ok, `PORTA SYNC OK`.

Related: tuvm#161, tuvm#150, tuvm#159,
`docs/results/2026-08-05-c6-checkin-decomposition.md`.
