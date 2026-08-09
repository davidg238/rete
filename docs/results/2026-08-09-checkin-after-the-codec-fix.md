# The check-in after the codec fix — `pushBuild` is still the top span, and it is no longer the story

The re-capture #163 needed: a `checkIn` whose largest term had just fallen ~70 ms may no
longer have `pushBuild` at the top, and that is what decides whether more codec work buys
anything. **It does still have `pushBuild` at the top — by 1.8x over the next span — but
the check-in has stopped being VM-dominated.**

Taken on **2026-08-09** on `c6spare`, bridged, `./run.sh bridge-health`, `TARGET 1/1 PASS`
on every arm. Read with
`ssh gw85224-01 'cd ~/porta-v2 && ./porta get --db soak-2026-08-04.db c6-bench diag'`.

**Three arms, all on the same board within four minutes**, because the stored baseline was
not a clean control (see "Why a matched before arm", below):

| arm | build | window |
|---|---|---|
| before | `porta-v1-final-910-gc5fd77b-20260809T103622` | 10:36:36 |
| after ①  | `porta-v1-final-913-ge2275c4-20260809T103326` | 10:33:40 |
| after ②  | `porta-v1-final-913-ge2275c4-20260809T103737` | 10:37:55 |

`c5fd77b` is `216a38b~1` — it carries the whole #25 OTA tail and neither codec commit, so
the delta below is the two codec commits and nothing else.

## The numbers

Per check-in, n = 2 per window, after = mean of the two after windows:

| span | before µs | after µs | delta | speedup | % of `checkIn` |
|---|---:|---:|---:|---:|---:|
| `pushBuild` | 196 792 | **98 250** | −98 542 | **2.00x** | **43.2%** |
| ` ├ pushEncode` | 123 978 | 53 105 | −70 872 | **2.33x** | 23.3% |
| ` ├ pushCollect` | 72 408 | 44 762 | −27 646 | 1.62x | 19.7% |
| ` └ pushTrim` | 140 | 138 | −2 | 1.01x | 0.1% |
| `pushWait` | 46 580 | 54 818 | +8 237 | 0.85x | 24.1% |
| `pullWait` | 29 050 | 30 240 | +1 190 | 0.96x | 13.3% |
| `applyIn` | 24 966 | 25 004 | +38 | 1.00x | 11.0% |
| `pullSend` | 19 513 | 15 020 | −4 492 | 1.30x | 6.6% |
| `pushSend` | 3 089 | 3 062 | −26 | 1.01x | 1.3% |
| — | | | | | |
| **`checkIn`** (the six) | **321 244** | **227 647** | **−93 598** | **1.41x** | 100% |
| `jobTick` | 330 303 | 236 812 | −93 490 | 1.39x | |
| `publish` | 8 152 | 8 238 | +87 | 0.99x | |

Both partitions still hold: `checkIn − Σ(six)` is 1 253 µs (0.55%), `pushBuild − Σ(three)`
is 245 µs (0.25%). Every percentage above is a share of something real.

**The same frame rode in every arm.** `tick 1 acked 7`, `tick 2 acked 14`, `hw=14 seq=17`
on all three boots — identical rows, identical bytes. The speedup is not a smaller frame.

**Repeatability, across two separate flashes of the same commit:** `pushCollect` 44 760 /
44 763 µs (3 µs apart), `pushEncode` 53 004 / 53 206 (0.4%), `pushBuild` 98 142 / 98 358
(0.2%), `checkIn` 229 576 / 225 718 (1.7%). The VM spans are near-deterministic; all of the
run-to-run variance lives in the radio waits. That is what makes an n = 2 window readable
here, and it is a property of these spans rather than a general licence.

## What it says

### 1. The synthetic bench is now validated on BOTH arms, to under 1%

`CborBenchNodeC6.nsl`'s `batch` payload — the frame `encodePush:` builds, 425 B by
`22_spacesync/09_batch_size` — measured 123 800 µs/encode before and 53 600 after. The real
`pushEncode` on the bridged node measured **123 978 before and 53 105 after** — **0.14% and
0.9% apart**, from an entirely different program with no bridge and no Thread.

e2275c4 could only validate the before arm, against a stale capture, at 4%. With the
matched arm on the same board it is 0.14%, and the *speedup* agrees too: bench 2.31x,
board 2.33x. **The synthetic payload prices the real frame**, on both arms, and a codec
change can now be priced on the bench in one flash instead of a bridged capture.

**What this does NOT establish is the frame's byte count, and the agreement is closer than
it has any right to be.** `09_batch_size` deliberately models a node that has been up
*thirty days*, because CBOR is shortest-form and a long-lived node encodes wider; the gate
node is four seconds old and its counters are one byte where the model's are five. Its
frame is therefore narrower than 425 B in the fields they share, and it also carries a
different row set (pending rows and the `diag`/`sys` cell scans, against the model's three
records). Two differences that must roughly cancel for the totals to land within 1%. So
the per-byte rate #163 stated against the 512 B bound — 230 µs/B before — is now bounded
rather than pinned: **the encoder went from ~242–292 µs/B to ~104–125 µs/B** across the
425–512 B range, ~2.3x either way. Pinning it needs the frame measured, which the node
already knows and does not report.

### 2. The 70/30 VM-vs-radio split has flipped to 54/45

| | before | after |
|---|---:|---:|
| VM (`pushBuild` + `applyIn`) | 221 758 µs, **69.0%** | 123 254 µs, **54.1%** |
| radio (two sends, two waits) | 98 232 µs, 30.6% | 103 140 µs, 45.3% |

2026-08-05's headline — "the 300 ms is the VM, not the radio — 70/30" — is now half true.
The check-in is roughly balanced, and the radio half is a floor no codec work can touch.

**Some of the VM saving was handed straight to the radio.** `pushBuild` fell 98.5 ms but
`checkIn` fell only 93.6 ms: `pushWait` rose 8.2 ms (+18%). The plausible mechanism is that
a node which finishes building sooner simply arrives earlier and waits longer for porta's
ack — the wait is a rendezvous, not work. **Not established**: `pushWait` spans 45.2–59.1 ms
across these windows, so 8 ms is inside the spread of a two-sample window. It is flagged
because it caps what any further VM work can return, and it is cheap to settle with a
repeating window (#159).

### 3. `pullSend` fell 1.30x — an independent confirmation

`pullSend` encodes the `#pull` frame and was not a target of either commit. It fell 4.5 ms
on the same change, which is what a codec-wide fix should do to every span that encodes,
and is a check that the `pushEncode` result is the codec rather than something local to
`encodePush:`.

### 4. `pushTrim` is unchanged at 138 µs, and the blind spot is unchanged with it

Still two failed predicates, still a window that closes before its own span cells exist.
2026-08-06's Finding 1 stands verbatim: this shape of capture is structurally unable to see
the ticks where the trim loop runs, so #161's fix (2) is still neither earned nor refuted.

## The decision this was taken for: is more codec work worth anything?

**Yes, but no single remaining fix is a 2x, and `copyFrom:to:` is not the one to build
next.**

Encode-attributable time still standing per check-in: `pushEncode` 53.1 ms + `pullSend`
15.0 ms + the unmeasured encode share of `pushCollect` 44.8 ms — call it **~50% of a
227.6 ms check-in.** Driving the encoder to zero would take the check-in to ~176 ms
(−23%); driving all of `pushBuild` to zero would take it to ~129 ms (−43%). That is the
ceiling, and the radio half of the tick is not going anywhere.

Against that ceiling:

- **#28's `copyFrom:to:` is priced at ~25–30% of the encode** (3d13920's re-ranking, after
  the streaming change removed the copies it would have eliminated). On today's numbers
  that is ~13–16 ms of `pushEncode` plus a share of `pushCollect` and `pullSend` —
  **roughly 20–25 ms, or 9–11% of a check-in** — and it costs a new VM accessor surface.
  #28's own forcing function is the driver ladder (BME280's mixed-width registers), not
  this. **Recommendation: do not build it for the codec.** Land it with the drivers, where
  the issue always said it belonged.
- **#161's fix (1) — carry `valueFits:`'s bytes into the row instead of discarding them —
  is the better lever and adds no mechanism at all.** It is pure nsl, and it deletes a whole
  redundant encode of every value, hitting `pushCollect` and `pushEncode` together.

**What must be measured before fix (1), same posture #161 and #163 both took.**
`pushCollect` is 44.8 ms of pending walk + three cell scans + `valueFits:`, and this window
does not separate them. If `valueFits:` is most of it, fix (1) is worth ~40 ms; if the scans
dominate, it is worth ~10 ms and is not obviously the next thing either. **A fourth inner
span around the `valueFits:` calls settles it in one boot, and it is affordable now** —
the bridge registers 14 of `OEVM_SPAN_SLOTS = 16`, so two slots are free and no knob has to
move.

## Why a matched before arm, rather than the stored baseline

The `diag/spans/*` cells standing in porta before this run were from 2026-08-06T22:41,
build `porta-v1-final-862-g8410661` — same board, same scenario, and it reads
`checkIn` 314 227 / `pushEncode` 121 878, within 2% of the matched arm taken today.

It was not used as the control, because `kernel.nsl` changed seven times between
`8410661` and `216a38b` and one of those changes (#25) **added an OTA tail to the check-in
itself**. A comparison across it prices the codec change plus whatever #25 cost. Both
readings happen to agree here — the codec conclusion is the same either way — but the
agreement is a fact discovered afterwards, not a reason to have skipped the control. It
cost one flash and 23 seconds.

Cross-binary by necessity (two kernels, two images), like e2275c4: the discipline's
within-binary rule protects small effects, and these are 1.4–2.3x.

Gates: `TARGET 1/1 PASS` on all three arms (bridge-health 3/3). No source changed.

Related: tuvm#163, tuvm#161, tuvm#28, tuvm#159,
`docs/results/2026-08-09-cbor-encode-shape.md`,
`docs/results/2026-08-06-pushbuild-three-way.md`,
`docs/results/2026-08-05-c6-checkin-decomposition.md`.
