# Porta ↔ nsl — the record schemas

**Status: draft for review (2026-07-12).** Companion to `PROTOCOL-NSL.md`, which
defines the *model* (retained/event channels, cursor sync, the grant boundary). This
document pins the *values* — one schema per path prefix — and the size law that keeps
bulk data off the channels entirely.

The addendum in `review/program-sequence.md` (2026-07-11) requires these schemas
be pinned **before first implementation**. That is what this is.

Nothing here presumes *where* retained channels are implemented (C space vs an nsl
library). Records are the same either way; that decision is deliberately left open.

---

## 1. The encoding contract

Every value on the wire is a **CBOR-encoded value of the nsl message algebra** — the
codec that shipped at rung 3 (`Cbor` in `nodus/nsl/lib/kernel.nsl`, RFC 8949). No second
codec, no bespoke framing, no JSON.

Inherited from that codec, and therefore **already true and already tested** — this
document adds no new encoding rules, it only *uses* them:

- **Deterministic** (RFC 8949 §4.2): shortest-form ints and floats, definite lengths
  only, map keys sorted bytewise over their *encoded* bytes. Two nodes encoding the
  same value produce byte-identical output — so a retained value can be compared, and
  a golden fixture can be pinned, by bytes.

  > **Why *bytewise*, since a Map is already stored in a canonical order** (asked as
  > #204, 2026-08-12). Emitting in the Map's stored order would also be
  > deterministic — that order is canonical too, and the two differ only when keys
  > differ in encoded length. The bytewise rule is kept for **interop, not
  > determinism**: it is obeyable by an implementation that knows nothing about the
  > nsl algebra, where matching the Map's order would require every peer to
  > reimplement `valueCompare:with:`'s rank-first total order exactly. porta pays one
  > `sort.SliceStable` (`internal/cval/encode.go:125`); a third implementation would
  > pay the same. **Do not "optimise" this into the Map's order** — the encode-side
  > sort is cheap and the portability is the point. (The expensive part was never the
  > sort: it was `printString` in the Symbol comparison, deleted in #205.)
- **Closed**: unknown tags and types are a decode *error*, not a pass-through. A
  hostile or mis-versioned peer cannot smuggle a value the node will faithfully relay.
- **Depth-bounded at decode** (32). A hostile peer cannot blow the node's stack.
- **Total**: every byte sequence yields exactly one value or exactly one of
  `not a value` / `message depth exceeded` / `invalid utf-8` / `integer out of range`.
- **Records** are CBOR tag 27 — `[record-name, {field-map}]`. **Symbols** are tag 39.
- **Integers** are the ratified L7 band, ±(2^64−1).

### Types used below

| notation | algebra type | CBOR |
|---|---|---|
| `int` | integer | major 0/1 |
| `str` | string (UTF-8) | major 3 |
| `bytes` | bytes | major 2 |
| `sym` | symbol | tag 39 |
| `bool` | boolean | simple 0xf4/0xf5 |
| `[T]` | list of T | major 4 |
| `Name{…}` | record | tag 27 |

**Enums are symbols, never strings.** `#deepSleep`, not `"deepSleep"`. A symbol is a
distinct algebra rank, so a typo'd enum fails to decode as the wrong *kind* of thing
rather than silently comparing unequal to every branch.

---

## 2. The size law — why bulk never rides a channel

This is the rule the rest of the catalogue is built to respect, so it comes first.

> **A channel carries a *reference* to bulk data. It never carries the bulk data.**

`PROTOCOL-NSL.md` §3.3 states the intent ("big bytes never ride channels"). This section
makes it enforceable: a number, a mechanism, and a record.

### 2.1 Why — three independent ceilings, and the radio is the binding one

**The radio.** An IEEE 802.15.4 frame carries ~102 bytes of payload after MAC headers.
6LoWPAN compresses IPv6 and then *fragments* anything larger, up to the 1280-byte IPv6
MTU. Fragmentation is not free and it is not graceful: **a datagram survives only if
every one of its fragments survives.** With per-datagram loss `p`, an `n`-fragment
datagram arrives with probability `(1−p)^n`.

That exponent is not theoretical. The bench soak fleet (2026-07-12, three nRF52840
nodes on a real OpenThread mesh, 32-byte payloads) measures **0–5% datagram loss**
depending on the node — the weakest, at Link Quality In = 2, sits around 4.6%. Take
that node at p = 0.05:

| payload | ≈ fragments | delivery |
|---|---|---|
| 32 B | 1 | 95% |
| 512 B | ~6 | 74% |
| 1024 B | ~12 | 54% |
| 4 KB | — | *cannot* be sent at all (> IPv6 MTU) |

A 1 KB check-in batch to a marginal node fails **half the time**, and every failure
costs a full retransmit of the whole batch. Small messages are not a stylistic
preference here; they are the difference between a fleet that converges and one that
thrashes.

**The heap.** The node's arena is 48 KB total (config-D, nRF52840). A decoded value is
a live object graph in that arena. A 200 KB firmware image is not "a large value" — it
is *four times the entire heap*. There is no cap that makes it work; it must never be
a value at all.

**Overflow policy (G2).** Channels have bounded depth and a defined overflow vocabulary
(coalesce/drop + a `sys/overrun` counter — tuvm#20). A single huge value defeats
per-entry accounting: you cannot coalesce half a firmware image.

### 2.2 The caps

| limit | value | rationale |
|---|---|---|
| `MAX_VALUE_BYTES` | **256** | one encoded value (a single retained value, or one event). Fits comfortably inside one 6LoWPAN fragment with room for the batch envelope. |
| `MAX_BATCH_BYTES` | **512** | one check-in datagram (many entries). ~6 fragments worst case; ~74% delivery on the *worst* bench node, ~97% on a healthy one. |

**`MAX_BATCH_BYTES` WAS BREACHED at thirty-day widths for one day — measured 2026-08-11
(rete#190), witnessed in the field the same day, and resolved by rete#197.** The steady-state
push frame became four rows when `publishCheap` joined `publishMetrics` on every check-in —
`obs/health` 91 B, `sys/metrics` 195 B, `tel/log`, `sys/cheap` 79 B, plus 17 B of
seq/path/kind per row and the envelope — and encoded to **529 B against 512**. It now encodes
to **378 B**. The guard is `nodus/tests/22_spacesync/09_batch_size.nsl`, which pins it and
carries the reasoning; the honest series is 425 → 429 → 433 → 529 → 385 → 378.

**The breach was not hypothetical, and that is the part worth keeping.** While the overflow
stood, `sys/cheap` stopped reaching porta from `c6-bench` altogether — last delivered seq
70006 against a live 78839, a gap of ~8 800 — while `obs/health`, `sys/metrics` and `tel/log`
kept arriving every 2 s. The node was writing the cell on every check-in; the frame was
dropping it. Earlier in the same window it arrived at 4–6 s gaps, i.e. trimmed on some
check-ins and then on all of them as the counters widened. **A record that overflows the cap
does not fail loudly — it goes quiet, and the gauge that would have told you is the one that
disappears.**

Two consequences worth reading together, and **both survive the fix**. **The victim is
deterministic**: `pushFrame` trims from the tail, `sys/cheap` is written last in `checkIn`, so
it holds the highest seq and is the first row dropped whenever the cap binds. **The trim
cannot simply be reordered**: porta acks one high-water and `applyAck:` sets `acked:: hw`, so
a frame that omits a low seq while carrying a high one loses the omitted entry permanently.
That is why the trim takes the tail, and why the real fix is structural — reserve bytes for
`sys/` before filling from `obs/`, or trim by type — which is **rete#150**, already parked in
`PortaBridge>>pushFrame` for this and for `sys/panic`. **rete#197 bought headroom; it did not
close #150.** At ~10 B a field, the headroom below is roughly thirteen more fields, after
which this paragraph applies again unchanged.

Shortening `CHeap`'s five field names was proposed and **measured to be insufficient**:
one-character names, the floor at which the names stop meaning anything, land the frame at
503 B; any legible scheme lands ~514 and still overflows.

**The resolution, LANDED 2026-08-11, is rete#197: one naming rule applied to ALL the frame's
labels.** MEASURED through the real `Cbor>>encode:` path, not estimated — `Health` 91 →
**61 B**, `Metrics` 195 → **101 B**, `CHeap` 79 → **62 B**, and the **frame 529 → 378 B**,
i.e. 17 B over the cap becomes **134 B under it** (roughly 13 further fields at ~10 B each, against a
`Metrics` that has grown from 9 fields to 11 in its entire history). The rule is *keep the
noun, suffix the aspect* — `hw` always means high-water and never anything else — and keys
are scoped per record, so reuse across records is free, exactly as `LatencySpan` below
already exploits with its bare `n`. It is **not** "chop to three characters":
`handlesHighWater` → `hnhw` breaks that rule deliberately, because `hhw` is already
`heapHighWater` and a collision inside one record is a silent wire bug.

**This is legitimate rather than a hack, and the reason is a boundary this document should
state once.** The push/pull frame is a **defined, private encoding** between porta and nodus
under a hard 512 B constraint; `porta get` is the readable operator surface. Forcing both to
share one vocabulary is precisely why `pulseCoalesced` pays wire bytes to be legible in a
place nobody reads it. D4.1 already says values are raw deterministic CBOR **decoded only for
display**, so a porta-side display map is a follow-up that needs no further wire change.

**One correction to how far that reaches, found by enumerating porta's sources rather than
reasoning about them.** "porta needs no decode change" is true of **values** — `cval` reads
payload maps generically, so `Metrics`/`CHeap`/`Health`/`Log` cost porta nothing — and false
of the **envelope**. porta builds and parses the envelope's keys **by name**
(`internal/wire/message.go`, `internal/wire/server.go`), so `bootId` → `bid` and `entries` →
`ent` are a **both-sides, same-commit** change. The trap is that a half-applied rename leaves
the nsl suite *and* `go test ./...` both green — each half is self-consistent — and only
`nodus/tuvm/tests/porta_sync_host.sh`, which runs a real node against a real gateway, goes
red. Any future change to an envelope key inherits that property.

**Numbered slots (`h1`, `m1`, …) were considered and REJECTED** (2026-08-11). They save only
23 B beyond the above (75 characters of name → 52), and they make meaning **positional**: a
mismatched reader decodes `m7` cleanly and silently means something else, where an unknown
*name* is loud. That is the same failure shape as rete#190's stale-layout bug — an ABI whose
meaning comes from position rather than from a name — and it adds a "never reuse a number"
discipline that does not exist today. 23 B does not buy that.

Both are enforced **at the bridge job**, on both legs:

- **Outbound**: a value that encodes larger than `MAX_VALUE_BYTES` is a **programming
  error** and crashes (L5 rule 2: let-it-crash — the supervisor is the handler). It is
  not truncated, and it is not silently dropped. If a job wants to publish something
  big, it publishes a `BlobRef` instead; that is a design decision, not a runtime
  accident. A batch that would exceed `MAX_BATCH_BYTES` is **split across check-ins**,
  never truncated — the cursor makes this free.
- **Inbound**: an over-size value from porta is a **decode error**, handled like any
  other malformed input (`ifInvalid:` → `sys/` event, cursor not advanced). One door,
  same rules (`PROTOCOL-NSL.md` §1, "the one law").

These numbers are a **first cut against measured mesh loss** and should be re-measured
on the G10 fleet-sim and the real mesh before they are frozen. The *structure* — a
per-value cap, a per-batch cap, and a reference record above them — is the part that
should not change.

### 2.3 The one indirection: `BlobRef`

```
BlobRef {
  hash:  bytes,   (* content hash; the identity AND the integrity check *)
  size:  int,     (* bytes, so the node can refuse before it starts *)
  alg:   sym,     (* #sha256 — pinned; UNWRITTEN today, absence means #sha256 *)
}
```

**`alg` is written by nobody, on either side, and that is the honest state** (recorded
2026-08-08). `porta blob put` emits `hash` and `size`; the node's `publishApps` emits
`hash` alone. Absence therefore means **`#sha256`** here rather than "the writer cannot
say" — the §4.2 reading — because there has only ever been one algorithm on this plane:
the C seam hashes SHA-256, porta signs a SHA-256, and a second algorithm would be a wire
change on both halves, not a field a reader could infer. The field stays pinned so the
day a second one exists it is a write and not a schema edit; until then, reading `alg`
and defaulting it is the same thing as not reading it.

Content-addressed, so it is **idempotent and cache-correct by construction**: a node
that already holds `hash` does nothing. A reflashed node, a node that resumes from
cursor 0, and a node seeing the value for the first time all take the same path. There
is no "have I already applied this?" state to get wrong — the hash *is* the answer.

The bulk bytes are then fetched **out of band**, on the data plane: today the
stop-and-wait block frame on exchange port + 1 (`PROTOCOL-NSL.md` §5.3), with CoAP
blockwise the ratified direction for Thread. Not TFTP — that was v1's surface and it was
deleted with v1. The space never sees them. Verifying the fetched bytes against `hash` is
what makes the data plane trustworthy without putting auth on the control plane.

### 2.4 What is a blob, and what is not

| data | why |
|---|---|
| **firmware / `.oimg` images** | blob. Kilobytes to hundreds of kilobytes. |
| **profiler dumps** | blob. Opaque and unbounded. |
| **panic cores** | **blob** — see below. |
| sensor readings, health counters, config scalars, log lines | values. Bounded and small. |

**`sys/panic` is the one place `PROTOCOL-NSL.md` contradicts itself,** and this is the
proposed resolution. §3.2 gives `sys/panic` the shape `{blob: bytes}` and marks it
*must-deliver*, while §3.3 says bulk never rides a channel. Both cannot hold.

Resolution: **`sys/panic` carries a bounded summary inline, and a `BlobRef` for the
rest.** The summary is the part you need at 3am and the part that must survive a node
that is about to die; the core dump is the part you want *if* the node comes back.

```
Panic {
  reason:  sym,            (* #oom | #hardFault | #watchdog | #assert | #stackOverflow *)
  bootId:  int,
  monoMs:  int,            (* uptime at death *)
  detail:  str,            (* bounded; TRUNCATED to fit MAX_VALUE_BYTES, and truncation
                              is fine here — this is the ONLY place truncation is legal,
                              because a partial reason beats no reason *)
  core:    BlobRef | nil,  (* nil if there is no retained core, or no room to keep one *)
}
```

Note what this buys: a node that dies of OOM can still *report* that it died of OOM,
because the report is 60 bytes and does not require allocating a buffer. Compare the
silent boot-OOM that shipped a dead dongle through rungs 7–8 — the failure there was
precisely that the node had no bounded way to say what happened.

---

## 3. Time (G6): monotonic only

**Nodes have no clock, and never pretend to.** Every timestamp on the wire is
monotonic-only; porta maps it to wall-clock at ingest using porta's own clock.

- `monoMs` — milliseconds since boot (`monotonicMillis`, real on target since rung 8).
- `bootId` — a counter incremented once per boot and persisted (NVS), **real on target
  since 2026-08-09** (rete#162). It was the source constant `1` from the space-sync rung
  until then, which made the ordering claim below degenerate; the storage was never the
  obstacle, since every `prj_*_thread.conf` already mounts NVS for OpenThread's own
  dataset, so this cost one settings key (`tuvm/bootid`) and no new subsystem.

The pair `(bootId, monoMs)` totally orders every observation a node ever makes, across
reboots, **without a clock**. A node that reboots does not emit timestamps that appear
to travel backwards, which is what a naive monotonic-only scheme does. This is also why
`bootId` is a persisted **counter** and not a per-boot random nonce: a nonce would
distinguish boots without ordering them, and the ordering is the claim.

**`bootId = 0` means the node has no persistent boot identity** — no settings/NVS backend
on that board, or a write that failed. It is a value to branch on, not an error: porta
reads it as "restart detection is unavailable for this node" and logs that on every
check-in. A node in this state must never be *guessed* about, because the guess would
rewind a watermark on a node whose ack was merely lost — the one case the `seq <= hw`
dedupe cannot survive.

`bootId` rides the **batch envelope, not every record** — it is constant for every entry
in a check-in, and paying 3–9 bytes per entry for it would be real money against a 512-byte
batch. Individual records carry only `monoMs` where they need a stamp.

---

## 4. The record catalogue

Kind is `R` (retained — path holds a latest value), `E` (event — append-only queue),
or `C` (cell — the post-tuvm#32 name for a path that holds the latest value; a stream
you empty is `E`, a cell holds). The `R` rows predate the tuvm#32 ruling; going
forward, `R` reads as "cell". Direction is from the node's point of view.

### 4.1 `goal/**` — what porta wants (retained; porta writes, node reads)

| path | kind | record |
|---|---|---|
| `goal/apps/<name>` | R | `App { image: BlobRef, sig: bytes, triggers: [sym], runlevel: int, lifecycle: sym, arguments: {…} }` |
| *(tombstone of above)* | R | delete = uninstall. The absence *is* the instruction. |
| `goal/mode` | R | `Mode { mode: sym, minAwakeS: int, maxAwakeS: int, maxAsleepS: int, loopSleepS: int }` |
| `goal/name` | R | `str` |
| `goal/forward` | R | `Forward { … }` — the whole policy, never a patch |
| `goal/config/<app>/<key>` | R | scalar (`int`/`str`/`bool`/`sym`) |
| `goal/debug/<app>` | R | `Debug { action: sym }` — `#attach` / `#detach` |
| `goal/profile/<app>` | R | `Profile { action: sym, durationS: int, continuous: bool }` |
| `goal/do/reboot` | **E** | `Reboot {}` — exactly-once via cursor |

`goal/apps/<name>` is where the size law earns its keep: the image is a `BlobRef`, so
this record is ~80 bytes regardless of whether the app is 4 KB or 400 KB.

**`sig` — signed at push, verified outside the image, added by rete#25 (2026-08-08).**
porta signs the image's SHA-256 with its ed25519 key at `porta blob put`, and the
signature rides in the record beside the `BlobRef` it covers. **The one seat that
verifies it is the runner** (#25 design D4) — the process that launches the VM, not the
VM: the node's C staging seam checks only the content hash, which is transfer integrity,
and never holds a key, because the thing that judges an image must live outside the image
it judges. The field is **required, and the bridge refuses an `App` without it**:
`PortaBridge>>otaWantedFrom:` answers nil for a record carrying no `sig`, so nothing is
staged rather than something being staged with its provenance quietly missing. Absent is
not "unsigned, carry on".

**This slice reads and writes exactly `apps/main`** (#25 design D5). tuvm has one image,
one heap and one world; the per-`<name>` shape is kept because multi-app later adds
per-name slots and per-name checks — rows, not mechanisms — but no node today answers for
a second name.

**`set-mode`'s atomicity rule dissolves.** v1 needed a paragraph demanding "accept the
whole thing or reject it"; here `Mode` is *one record at one path*, so it is atomic by
construction. There is nothing to enforce.

### 4.2 `obs/**` — what the node observes (retained; node writes, porta reads)

| path | kind | record |
|---|---|---|
| `obs/apps/<name>` | R | `AppObs { image: BlobRef, runlevel: int, lifecycle: sym, triggers: [sym] }` |
| `obs/config/<app>/<key>` | R | scalar echo |
| `obs/mode` / `obs/name` / `obs/forward` | R | same shapes as their `goal/` twins |
| `obs/health` | R | `Health { up: int, bid: int, wak: int, pto: int, sdr: int }` — liveness and delivery only; the VM internals live at `sys/metrics` (§4.4), which the bridge publishes on every check-in. `heapFree`/`heapLargest`/`overruns` were **removed** 2026-08-04 (#56 design D2) rather than duplicated. **61 B encoded, MEASURED 2026-08-11** (87 B → 91 B → 61 B: rete#162 made `bid` a real persisted counter, and CBOR is shortest-form, so a saturated counter is five bytes where the old constant `1` was one — the guard in `nodus/tests/22_spacesync/09_batch_size.nsl` pins the worst legal value rather than a fresh node's — then rete#197 shortened the five labels, which is the whole of the 91 → 61 B step) |
| `obs/build` | R | `Build { chip: sym, build: str, sdk: str, image: bytes }` — see below |

**`obs/apps/<name>` — the node currently writes image and lifecycle, and nothing else
(rete#25, 2026-08-08).** `PortaBridge>>publishApps` writes
`AppObs { image: BlobRef { hash }, lifecycle: #running }` once per boot: the hash of the
image actually executing, and the fact that it is executing. `runlevel`, `triggers` and
the `BlobRef`'s own `size` / `alg` are **omitted**, on `obs/build`'s precedent —
**absence means "the node cannot say"**, which is the honest value and not a default. It
is honest in both directions here: no node holds a runlevel or a trigger list yet (D5,
one image and one world), and `size`/`alg` are porta's facts about a blob porta stored,
not something the node observes. The four fields stay pinned, because this record is what
`goal/apps/<name>` is diffed against — a field the node learns to answer later is then a
write, not a schema edit.

**`obs/build` — build provenance, pinned 2026-08-01, `build` added by #74 (2026-08-07).**
Added by the space-sync rewrite (`design/2026-08-01-porta-space-sync-rewrite-design.md`
D8) because v1's `nodes.chip` / `nodes.sdk` columns had no successor path, and deleting
them without one takes fleet provenance from *partial* to *absent*. #74 (tuvm#74, a
labelled revenue-gate) is why a per-method profile (§4.x, `mprof:` lines) can be named at
all: the profile's offsets are only meaningful against the `.oimg.methods.json` sidecar
of the exact build that produced them, and a node that cannot say which build it is
running makes a mismatched sidecar indistinguishable from a quiet program.

- `chip` — the SoC family as a symbol (`#esp32c6`, `#esp32s2`, `#nrf52840`).
- `build` — the build actually running: `platform buildId`, which on Zephyr is
  `git describe --always --dirty` plus a build timestamp
  (`<describe>-<YYYYmmddTHHMMSS>`, baked in fresh on every build — see
  `nodus/tuvm/zephyr/cmake/gen_build_id.cmake`) and on host is the literal `"host"`.
  Deliberately **one opaque string**, not `sdk`/`image`/`builtAt` as separate fields:
  the timestamp it already carries is what `builtAt` would have answered, and a reader
  picks a sidecar by matching this string whole, not by comparing fields piecewise.
- `sdk` — the Zephyr/SDK version string as the build reports it. Still **omitted** —
  #74 covers only the build id, not this — absence means "the node cannot say", which
  is the honest value and not a default.
- `image` — a truncated hash of the running `.oimg`, **8 bytes**, not a full digest: it
  identifies a build against a known set, it does not authenticate one. Authentication is
  the signing story and rides the bulk plane. Still **omitted**, for the same reason as
  `sdk`.

**82 B encoded, MEASURED 2026-08-07** — `chip: #esp32c6` plus the 43-character
`porta-v1-final-862-g8410661-20260806T223944` this bench's C6 build actually produced,
through `porta/internal/cval`'s encoder (golden-tested against the node's own `Cbor`).
The host twin's `build: "host"` (with `chip: #host`) is **39 B**, and the pre-#74 `chip`-only record was 29 B.
Well inside §2.2's 256 B/value cap, with 174 B of headroom on the longest of the three.

A figure and not an assurance, deliberately: this line read "the record is ~40 B encoded"
until #74 added a ~43-byte string, at which point it became "well inside the cap" — a
document whose §2 is a size law had replaced its one measurement with a reassurance in the
same edit that made the record twice as large. `build` is the only field here that can
grow without a schema change (a longer `git describe`, a dirty suffix), so it is the one
that has to be measured rather than reasoned about.

**`obs/X` deliberately reuses the shape of `goal/X`.** Convergence is then a *structural*
diff — `goal/mode` vs `obs/mode` — rather than a bespoke comparison per feature. v1's
self-heal loop and the UI's desired-vs-observed view become the same query, which is the
whole argument of `PROTOCOL-NSL.md` §1.

`heapFree` / `heapLargest` come from `heapStats` (prim 104, rung 4). `heapLargest` is
the fragmentation signal — the number that tells you a years-long node is dying slowly
(G3), which `heapFree` alone will not.

**But no instantaneous heap gauge means anything on a collect-when-full VM**, and
`heapLargest` is the one an operator is most likely to misread. `review/tests/CLAIMS.md`
(row: strict live-bytes plateau) owns that finding and the numbers behind it — the short
version is that the trough of the sawtooth reports *the size of the request that tipped the
arena over*, not the state of the heap, so a single low reading is unfalsifiable. **The
readable gauge is the per-GC-cycle envelope**: bucket by `gcCycles`, then take
`max(largestFreeBlock)` and `min(liveBytes)` as post-sweep proxies.

Three rules for computing that envelope, added 2026-08-09 (#153) and established by
falsifying the instrument rather than by argument:

- **Discard the first ~5 GC cycles.** They run on an arena that has never been swept
  (`largestFreeBlock` starts at essentially the whole heap), so they are not comparable to
  steady state and they dominate any trend fitted across them.
- **Do not fit a `largestFreeBlock` trend to a window shorter than ~5 h.** It carries a slow
  oscillation of period hours. Slicing a run that is *flat over 9.95 h* into 1.5 h windows
  and re-running the same least-squares test yields alternating verdicts of "falling, 7.1σ"
  and "rising, 8.5σ", swinging ±350–420 B/cycle — all of it artefact, and a second flat run
  reproduces it independently. Treat σ from a short window as a measure of how well a line
  was fitted, never as evidence that the trend is real.
- **`min(largestFreeBlock)` is not a gauge at all** — it is the last-failed-request size, per
  CLAIMS. Trending it looks respectable and is meaningless: it flips "rising 3.2σ" to
  "falling 6.2σ" between adjacent 1.5 h slices of one boot. Report the ceiling and the live
  floor; leave the trough alone.

Finally, **cycles, not wall-clock, are the unit**: a node bridging at 2 s accumulates GC
cycles ~14× faster than the bell at 30 s, so 45 minutes there is a longer test than ten
hours on the bell — 542 flat cycles from `c6spare` on 2026-08-09 against 556 from a 9.95 h
bell run. Book the fast node, not the long night. Tool: `tools/heap-envelope.py`.

### 4.3 `tel/**` — telemetry (event, node → porta)

| path | kind | record |
|---|---|---|
| `tel/print` | E | `Print { text: str }` |
| `tel/log` | E | `Log { level: sym, text: str }` — `#debug`/`#info`/`#warn`/`#error` |
| `tel/m/<name>` | E | `Metric { value: int \| float, monoMs: int }` |

Lossy by policy: `tel/**` is the *first* thing dropped under G2 overflow pressure, and
the drop is counted (`Health.overruns`). Losing a log line to save a node is correct.

### 4.4 `sys/**` — the node's own faults (event, node → porta)

| path | kind | record |
|---|---|---|
| `sys/reset` | E | `Reset { reason: sym, code: int, bootId: int }` — `code` is the SoC's raw reset cause; `reason` is the symbol derived from it. Both are carried because the derivation is lossy and one-way: v1 stored `last_reset` and `last_reset_code` side by side, and the numeric form is the one an agent can compare across SDK versions (added 2026-08-01 by the space-sync capability audit, design §4.2) |
| `sys/overrun` | E | `Overrun { path: str, dropped: int }` — G2's counter |
| `sys/panic` | E, **must-deliver** | `Panic { … }` — §2.4 |
| `sys/metrics` | **C (cell)** | `Metrics { hsz: int, lb: int, hhw: int, lfb: int, gcc: int, hnl: int, hnhw: int, qhw: int, ovr: int, pdr: int, pco: int }` — the long names it carried until rete#197 were `heapSize`, `liveBytes`, `heapHighWater`, `largestFreeBlock`, `gcCycles`, `handlesLive`, `handlesHighWater`, `queueHighWater`, `overruns`, `pulseDrops`, `pulseCoalesced`, in that order — tuvm#24, widened 9→11 by tuvm#34; **101 B encoded, MEASURED 2026-08-11** (195 B under the long labels, pinned 2026-07-21, against an earlier ~185–230 B estimate), within §2.2's 256 B/value cap with **155 B** of headroom |
| `sys/cheap` | **C (cell)** | `CHeap { ob: int, ohw: int, alc: int, fre: int, fal: int }` — `outBytes`, `outHigh`, `allocs`, `frees`, `fails` until rete#197 — rete#190; **62 B encoded, MEASURED 2026-08-11** (79 B under the long labels; values representative of the ~200 KB C6 C-heap ceiling, `ob`/`ohw` above CBOR's 65536 5-byte-uint boundary — an earlier 75 B figure undercounted by measuring a sub-65536 case). The C heap, **not** the GC arena that `sys/metrics` reports — a leak ran for a week against a gauge that could only see the arena. A separate record rather than five more `Metrics` fields, per the `LatencySpan` lesson in this section |
| `sys/latency` | **C (cell)** | `LatencySpan { span: sym, n: int, fails: int, minUs: int, maxUs: int, sumUsLo: int, sumUsHi: int, b0..b5: int }` — tuvm#61; **140 B encoded, MEASURED 2026-07-21**. **One record PER SPAN**, not one record with a field per span: adding an instrumented job therefore adds a *record*, never a field, so this schema does not grow the way `Metrics` has. Field names are deliberately short — thirteen fields here total 46 characters of name against `Metrics`'s ~120 before rete#197. **This row is the precedent rete#197 generalized to the whole frame, and it is the one record the rename did not touch: its names were already at the rule.** 140 B is unchanged, which is the control that says the rename went where it was aimed |

**Delivery class is a property of the channel, not a paragraph of prose.** `sys/panic`
is must-deliver; `tel/print` is drop-first. That is the whole rule.

**Kind C is a cell** (tuvm#32 option A, rung 9): the pigeonhole holds the latest
letter. A reader — porta, an agent, a second job — `peek:`s current state *by
name*, non-destructively, without draining a stream some other consumer is
counting on (the WHY-AGENTS rider, folded in here). The node app is the writer,
on a user-requested cadence (G9: no unrequested periodic work); as of tuvm#24
no periodic publisher exists — the record is built by `kernel metrics` and
published explicitly.

**`Metrics` headroom — MEASURED 2026-07-21 (tuvm#61), superseding the estimate below; re-measured
2026-08-11 after rete#197 shortened the labels.**
`Metrics` encodes to **101 B** (195 B under the long labels) and `LatencySpan` to **140 B**
(unchanged — nothing in it was renamed), both through the real
`Cbor>>encode:` path, witnessed by `nodus/tests/19_latency/05_record_size.nsl` (which also
serves as the regression guard, since encoded size is what porta pays for on every
uplink). The measurement used the values read off a WROOM-32 rather than round numbers:
CBOR is shortest-form, so a field holding 7,820,873 costs five bytes where one holding
52,746 costs three, and a size taken against placeholders is not the size the fleet sends.

195 B landed inside the old ~185–230 B estimate, so the estimate was sound — but it was
still an estimate, and this section said so about itself. **The headroom is now a number:
155 B under the 256 B cap**, where it was 61 B — roughly thirteen more integer fields at the
current name lengths, where it was two.

**The 61 B figure is worth remembering rather than deleting.** Two fields of headroom was
already thin, and it was the *batch* cap and not this one that bound first: eleven fields at
long names left `Metrics` inside its per-value cap while pushing the four-row frame 17 B over
the 512 B batch cap. A per-value cap being comfortable says nothing about the frame, and
rete#190 added one 79 B record to discover it.

*The original note, kept because its reasoning is what prompted the measurement:*
`pulseDrops`/`pulseCoalesced` — the ISR pulse seam's shed and coalesce counters, added
when that seam stopped answering a full mailbox with `k_oops` and started shedding —
widened the record from 9 fields to 11. The ~185–230 B figure was **derived from
field-name lengths and plausible counter magnitudes; nobody had encoded the record and
measured it.** The pre-existing ~150–190 B figure it extends appears to be of the same
kind.

Two things keep this from being alarming. First, §2.2's 256 B is itself a **first cut
pending re-measurement on the G10 fleet-sim**, not a frozen protocol constant — the
*structure* (a per-value cap) is the invariant, not the number. Second, a breach is a
**crash at the bridge, not a silent truncation**, so it cannot corrupt data undetected.

Still worth acting on: `Metrics` is the one record that grows every time a subsystem gains
a counter, and it is the closest to the cap. It has now been measured, so the standing
instruction is discharged for field 12 — **but re-measure before field 13**, and note that
counter magnitudes move the figure as well as field count.

The forecast in this paragraph held. It said that if per-job latency histograms ever
wanted publishing (tuvm#61, where bucket counts are many fields at once) that should be
**its own record, split by subsystem** — and that is exactly what `sys/latency` above is.
`LatencySpan` is one record per span, so instrumenting another job adds a record rather
than a field, and `Metrics` is not dragged toward the cap by work that has nothing to do
with the heap.

### 4.5 `dbg/*` — remote debug (event, both ways)

| path | kind | record |
|---|---|---|
| `dbg/req` | E, ↓ | `DbgReq { line: str }` |
| `dbg/resp` | E, ↑ | `DbgResp { line: str }` |

**Porta does not interpret or own the `dbg/req` / `dbg/resp` session** (unchanged from v1
§8): it relays whatever line arrives, and the debug session itself — parsing the line,
running the window, formatting the reply — lives entirely in the node. Long responses obey
`MAX_VALUE_BYTES` like everything else — the debugger paginates, which it must do for a
serial console anyway.

**This is a claim about the session on this path, not about developer-facing assets.**
Console history, `.oimg` archives and symbol sidecars are not the `dbg/` session and are not
bound by this rule; they live off the space entirely, as host-side dev tooling
(`nodus/tuvm/zephyr/tools/console/`), by the same rule that keeps profiler blobs off the
space at all (§2.4). Conflating the two readings would make porta holding console history,
or a view built on that tooling, look like a violation of "stateless relay" when it is not
(`docs/design/2026-08-06-method-profile-over-porta-design.md` D7).

**Implemented 2026-08-06** (`docs/design/2026-08-06-method-profile-over-porta-design.md`,
issues #164/#165) — `dbg/*`'s first implementation since v1 was deleted. The line this
channel carries today is `profile <ms>`: the node opens a self-limiting per-method
activation-count window sized in milliseconds, counts for that duration, and reports without
needing a matching stop request, so a node that never hears from porta again still stops
counting and still reports. The rows themselves stay off this path — they leave over the
console already teed to the collector (#56), never over `dbg/resp`, which keeps a profile
table off the same plane the size law keeps every other blob off (§2.4) rather than testing
`MAX_VALUE_BYTES`'s pagination story against it.

**`dbg/req` is a STREAM, so it carries the cursor-0 replay hazard.** The node's cursor is
RAM-only, so cold boot = cursor 0 = full replay — safe for cells, which are idempotent by
construction, and dangerous for streams, which are actions
(`porta/docs/design/2026-08-01-porta-space-sync-rewrite-design.md` §8, whose example is
`goal/do/reboot` rebooting the node again). `dbg/req` is the second such path: after a
reboot the node re-receives every `profile <ms>` porta ever wrote and starts counting
unbidden. Bounded rather than fixed — the windows are self-limiting and only one end-timer
is outstanding at a time — and recorded rather than mechanised, because every fix is a
tunable no measurement has earned yet
(`docs/design/2026-08-06-method-profile-over-porta-design.md` D6).

### 4.6 `diag/**` — bounded diagnostic capture (both ways)

Added 2026-08-01, cataloguing what the node already ships (tuvm#72, `class SpanCapture` in
`nodus/nsl/lib/kernel.nsl`). This namespace was live in node source before it was written
down here; the entry below documents it rather than proposing it.

| path | kind | record |
|---|---|---|
| `diag/spans/ctl` | **C (cell)**, ↓ | `SpanCtl { state: sym }` — `#running` / `#stopped`. Writing `#running` while already running RESTARTS the window; a write is a statement of intent |
| `diag/spans/<span>` | **C (cell)**, ↑ | `LatencySpan { … }` — the §4.4 record, **one cell per span slot**, 140 B each. `<span>` is the slot's nsl name with its leading `#` stripped |
| `diag/spans/result` | **C (cell)**, ↑ | `SpanCapture { n: int, ms: int }` — the window summary ONLY: total samples across slots, and the window's elapsed ms (`-1` when the millis counter wrapped, published rather than a plausible wrong number) |

**`diag/spans/ctl` is marked ↓ and porta cannot write it.** §3's inbound authority is
exactly two paths wide — porta may write `goal/**` and `dbg/req`, and the node DROPS an
entry at any other path. `diag/spans/ctl` is neither, so the downward arrow in this table
describes an intent no gateway can currently exercise; the only writers today are the
node's own `/stats` page links. This is a genuine contradiction between two documents of
record, resolved here by naming it rather than by quietly widening the authority model —
that model is the one rule the whole direction argument rests on, and it should be widened
on purpose or not at all. **Filed as tuvm#159.** Until it is settled, a node that wants
span data published runs a FREE-RUNNING capture on its own cadence, which needs no inbound
path at all, and whose cost is measurable rather than argued about.

**Eight of the span names are not the app's to choose, and every bridged node publishes
them.** Two are C-owned (`httpQueue`, `egress` — `src/span.h` pins the slots so a node
cannot ship an uninstrumented HTTP path). Six more became kernel-owned on 2026-08-05:
`PortaBridge` registers `pullSend`, `pullWait`, `applyIn`, `pushBuild`, `pushSend` and
`pushWait` at construction, decomposing one check-in into its two radio sends, its two
radio waits and its two VM stretches. A reader of any bridged node's tree will find those
eight paths without any app having asked for them, and an app's own spans start after
them. Why they exist at all: the first capture off real hardware
(`docs/results/2026-08-05-c6-span-capture-first.md`) found a check-in costing ~300 ms with
no way to say whether that was radio or interpreter.

Consequence for the size law, and it is not free: the number of `diag/spans/**` cells a
node publishes per window is its **registered slot count**, not `OEVM_SPAN_SLOTS`. A
bridged gate node went from 5 cells to 11 on that date. Each is a separate push row under
§2.2's 512 B batch cap and `maxRows`, so a window's rows now take several check-ins to
drain rather than one. They are cells, so this costs latency and never loss.

**Why `diag/` and not `obs/`.** §1's inherited rule: diagnostics stay out of the
`goal/`/`obs/` convergence tree, so fleet anti-entropy never walks them. A span capture is
never a *desired* state, so it has no `goal/` twin and nothing to converge against.

**Why the per-span rows are siblings of the summary, and not nested inside it.** The node's
`SpanCapture>>reportMs:` used to return `#SpanCapture { n, ms, slots: [ … ] }` with every
slot's `LatencySpan` nested in a list. That shape **cannot ride the space**: with
the 8 slots registered at the time and `LatencySpan` measured at 140 B, the list alone is ~1120 B against
§2.2's **256 B** value cap — 4.4× over the value cap and 2.2× over the 512 B batch cap. §2.2
calls an oversized outbound value a *programming error*, and it is one; it has gone
unnoticed only because the capture is read locally by the node's own `/stats` page and has
never been synced.

Splitting it costs the node nothing it does not already have: `Spans>>recordAt: slot`
already builds exactly the per-slot `LatencySpan` this table wants, and the record already
carries a `#span` field naming its slot, so the path parameter and the field agree. What
changes is that the slots are *published* per-path instead of being nested into one value.
**Filed as tuvm#107, landed 2026-08-05**: `SpanCapture>>publishSlots` writes one cell per
slot and `summaryMs:total:` carries `{ n, ms }` only. `diag/spans/**` is syncable. Every
slot publishes each window, including one that took no samples — skipping the empty ones
would leave a slot that had samples last window holding that value forever, silently
attributing stale numbers to the current window.

This is the general shape for any capture whose size scales with a count: **the collection
is the path space, not a list inside one value.** `sys/latency` states the same principle
one level up — adding an instrumented job adds a record, never a field.

---

## 5. Evolution (G8 — fleet version skew)

1. **Additive only.** New state = a new path, or a new *optional* field on an existing
   record. Never a changed field type, never a removed field, never a re-purposed name.
2. **The record name is the contract.** A breaking change gets a **new record name at a
   new path** — old and new nodes then simply do not overlap, instead of misreading each
   other. This is why there is no version field: a version integer only tells you that
   you cannot understand the value, which is exactly what an unknown path already tells
   you, with no extra mechanism.
3. **Consumers ignore unknown fields.** (The *codec* is closed to unknown tags and types;
   a record's field map is open to unknown keys. These are different layers, and
   conflating them is the easiest way to break rolling upgrades.)
4. **Consumers must not require fields added after their release.** Every field added
   post-hoc needs a defined absent-behaviour, per L5 rule 1 (`at:ifAbsent:`).

---

## 6. Open questions (to close at phase-3 design)

1. **The caps (§2.2) are a first cut.** 256/512 come from measured 6LoWPAN loss, but
   they should be re-derived on the G10 fleet-sim and on the real mesh. Specifically:
   is a smaller `MAX_BATCH_BYTES` (one fragment, ~80 B) better than a larger one plus
   retransmit, on a 5%-loss link? The soak fleet can answer this empirically.
2. **`arguments` in `App`** is an open map — the only unbounded-shape field in the
   catalogue. Cap it, or make it a `BlobRef` too?
3. **Cursor persistence granularity** (NVS write per check-in vs per batch; flash wear,
   G7) — `PROTOCOL-NSL.md` §6.1, unchanged.
4. **`Metric.value` admits int or float.** Float on a soft-float M4 is expensive. Is a
   fixed-point int + declared scale better for a sensor fleet?
5. **Does `obs/**` push per-path or snapshot-batched?** (§6.6 of the parent doc.) The
   size law says: whichever fits 512 bytes; measure.
6. **Tombstone retention at porta** — until the cursor passes, or time-boxed? **This is not a
   shrug; it is a live hazard — now #121.** A delete is only visible as a tombstone in the
   cursor stream, so a node that is away long enough for porta to reclaim tombstones past its
   cursor comes back, asks for `seq > N`, and porta **silently answers a question it cannot
   answer**: the node keeps running an app that was uninstalled, and nothing anywhere notices.
   This is exactly etcd's compaction-vs-watch hazard, and their fix is cheap and should just be
   taken: porta keeps a **compaction revision**, refuses an incremental sync from a cursor older
   than it, and forces a **full resync from cursor 0** — a path §3.1 already requires for cold
   boot and which therefore costs one comparison and one error code. Pin it in the §4.3 framing
   *before* first implementation, not after.

---

*Companion to `PROTOCOL-NSL.md`. The catalogue above is a proposal: it is the v1 verb
list (`PROTOCOL.md` §3) re-expressed as paths, plus the size law needed to make the
"protocol is the space" claim survive contact with a 127-byte radio frame and a 48 KB
heap.*
