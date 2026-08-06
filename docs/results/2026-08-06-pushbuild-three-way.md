# `pushBuild` is three spans — and it is one thing done twice, not three things

tuvm#161's falsifiable precondition: *split `pushBuild` into collect / encode / trim
before touching either proposed fix, or "the trim loop is expensive" is a reading of the
source rather than a finding.* The split landed, is green on all three gates, and **the C6
capture was taken on 2026-08-06** — on a third devkit, because `c6bench` had left the
bench (see "Which board", below).

**The answer: the trim loop is 0.07% and the encoder is everything.** Two more findings
came out of the host half on the way, and one of them says why that 0.07% must not be
read as "trimming is cheap".

The spans are `pushCollect` / `pushEncode` / `pushTrim`, nested inside `pushBuild`. They
carry the leg prefix the other six use rather than #161's method names
(`collectOutbound` / `encodePush` / the trim loop), so the nine sort adjacent in a
rendered table; the mapping is one-to-one and is stated in the bracket comment on
`PortaBridge>>pushFrame`.

## The C6 numbers

`nodus/tuvm/tests/target/run.sh bridge-health`, build
`porta-v1-final-835-g18cea69-20260806T113150`, gate `TARGET 1/1 PASS`, one
boot-to-verdict window, n = 2. Read with
`ssh gw85224-01 'cd ~/porta-v2 && ./porta get --db soak-2026-08-04.db c6-bench diag'`.

| span | µs / check-in | % of `checkIn` |
|---|---:|---:|
| `pushBuild` | 186 636 | **59.3%** |
| ` ├ pushEncode` | 118 571 | 37.7% |
| ` ├ pushCollect` | 67 685 | 21.5% |
| ` └ pushTrim` | **139** | **0.04%** |
| `pushWait` | 46 473 | 14.8% |
| `pullWait` | 34 936 | 11.1% |
| `applyIn` | 23 439 | 7.4% |
| `pullSend` | 19 700 | 6.3% |
| `pushSend` | 3 096 | 1.0% |
| — | | |
| `checkIn` (the six) | 314 719 | 100% |
| `jobTick` | 323 769 | |

Both partitions hold: `checkIn − Σ(six)` is 439 µs (0.14%), `pushBuild − Σ(three)` is
241 µs (0.13%). **The three extra brackets are free**, as the six were —
`checkIn` reads 314 719 against 305 334 on 2026-08-05 and `pushBuild` 186 636 against
188 968, so this is comparable to the earlier capture rather than to a differently
instrumented node.

### What it says

**`pushBuild` is one thing done twice, not three things.**

- **`pushTrim` is 139 µs — 0.07% of `pushBuild`.** #161's fix (2), sizing the frame
  incrementally instead of re-encoding to trim, is **not earned by this data**. Read the
  next section before concluding it is refuted: this window is structurally unable to see
  the ticks where the loop runs, and 139 µs is the cost of a failed predicate, not of a
  loop that ran and was cheap.
- **`pushEncode` and `pushCollect` are 63.5% / 36.3% of `pushBuild`, and the same split
  falls out on host (62.9% / 36.8%)** — the same ratio on a 160 MHz RISC-V and on an
  x86 host VM, which is what makes it a property of the code rather than of the platform.
  #161's fix (1) — carry the bytes `valueFits:` already produced into the row instead of
  discarding them — is aimed at `pushCollect`, so **up to ~21% of a check-in** is on the
  table there. Not all of it: `pushCollect` also does the pending walk and three cell
  scans, and this window does not separate those from the encode.

**And the number the issue did not ask about is the bigger one.** 118.6 ms to encode a
single frame of at most 512 B is **~230 µs per byte — roughly 37 000 C6 cycles a byte.**
The same rate explains `pushCollect` (its per-value encodes cover a few hundred bytes at a
comparable rate), so one model covers both spans: *encoding costs ~230 µs/B and a check-in
encodes ~800 B.* Deleting the duplicate encode removes the ~300 B of it that is redundant
and leaves ~119 ms in place.

So "the same value is encoded three or more times" is true and worth fixing, but it is a
**~20% fix on top of a codec that costs 230 µs/byte**. The next question after #161 is not
how many times `Cbor>>encode:` runs — it is why one run is that expensive, which is a
question about allocation and copying inside the encoder, and is the first thing a fourth
span inside `encodePush:` would answer.

## The host numbers

Taken first, and kept because it is what predicted the C6 result.
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

**This was written before the C6 run, as a shape rather than an answer** — one sample on a
host VM ~60x faster per span, on a frame of three rows. It held: the C6 reproduced the
same 63/37/0 split to within a percentage point. Worth recording as a data point on when
the host suite *is* predictive: a pure-VM cost with no timer, no mailbox and no radio in
it is exactly that case. The standing warning — that the posix timer backend bypasses the
pulse mailbox, so the host says nothing about coalescing, shedding or stale handles —
is about a seam this measurement does not cross.

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

**Written as a prediction before the C6 run, and it came true: 139 µs, 0.04% of the
check-in.** So the C6 number is not evidence that trimming is cheap; it is the same blind
spot at 60x the cost per span. Fix (2) is neither earned nor refuted, and the thing that
would settle it is a window that is still open when the frame is over 512 B.

**This is what `PortaBridgeNodeC6.nsl` named as the thing that would earn a repeating
window** ("what would earn it: this capture showing that one boot-to-verdict window does
not answer where the time goes"). It does not answer it. That is an argument for the
node-chosen free-running cadence — tuvm#159's fourth option, which adds no mechanism and
no authority change — and not for porta driving `diag/spans/ctl`.

The counter-hypothesis that would have made the C6 number meaningful — that a ≤90 s mesh
attach leaves a `pending` backlog over 512 B, so the first check-in trims hard — is
**dead**: 139 µs is two failed predicates, not a loop that ran. Whatever the attach costs,
it does not leave a frame that needs trimming.

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

## Which board this ran on — a THIRD C6, and the registry does not know it

`c6bench` (console `ac2dce6f…`, JTAG `40:4C:CA:45:47:A4`) dropped off USB at
**07:46:45** — `port lost: device reports readiness to read but returned no data` — and
the ship's bell took its place on the cable one minute later, at 07:47:01, gaining a
second cable (its USB-JTAG node) at 09:12. At 11:28 a **third ESP32-C6** appeared:
console `d2e21074…`, JTAG `40:4C:CA:44:6E:60`, neither of the two the harness registry
knows, found running a stock ESP-IDF example (`I (nnnnn) example: stdout` once a second)
and therefore carrying no rete image.

The capture was taken on that third board, as a new collector instance **`c6spare`**
(`~/.config/rete/console-c6spare.env`, `systemctl --user start rete-console@c6spare`),
run with `./run.sh --board c6spare bridge-health`.

**The `c6bench` entry was deliberately NOT repointed at it.** A by-id hash names one
physical adapter; quietly rebinding a registry entry to a different board is exactly the
mix-up the pinning exists to prevent, and it cost most of a morning on 2026-08-05 when
two boards were confused for one. A third board gets a third name until someone says what
it is.

Nothing about the measurement depends on which of the disposable C6s it was: same SoC,
same image, same `c6-bench` node id on the mesh, and the numbers land within 3% of the
2026-08-05 capture on the real `c6bench`.

Gates green with this change: nsl suite 246/0, `go test ./...` ok, `PORTA SYNC OK`,
`TARGET 1/1 PASS`.

Related: tuvm#161, tuvm#150, tuvm#159,
`docs/results/2026-08-05-c6-checkin-decomposition.md`.
