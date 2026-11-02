# Short wire labels — implementation plan (rete#197)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** Apply one naming rule to every label in the push frame so the four-row steady-state
frame encodes to ~378 B instead of 529 B against the 512 B `maxBatchBytes` cap, ending the
silent tail-trim that is *currently* keeping `sys/cheap` off the wire on a live node.

**Architecture:** The push/pull frame is a **defined, private encoding** between porta and
nodus under a hard 512 B radio constraint (@davidg238, 2026-08-11); `porta get` is the readable
operator surface. The rule is *keep the noun, suffix the aspect* — `hw` always means
high-water and never anything else — and the scheme is *as short as the rule allows*, not
"chop to three characters" (`handlesHighWater` → `hnhw`, because `hhw` is already
`heapHighWater` and a collision **inside one record** is a silent wire bug). Keys are scoped
per record, so reuse *across* records is free and intended.

**Tech Stack:** nsl (`nodus/nsl/lib/kernel.nsl`, the node apps under `nodus/tuvm/zephyr/`, the
suite under `nodus/tests/`), Go (`porta/internal/wire/`), Python (`tools/heap-envelope.py`),
Markdown documents of record.

---

## Why this is not a preference change

`sys/cheap` **has not reached porta from `c6-bench` since 13:07 today** (measured 14:52,
read-only against `soak-2026-08-04.db` on gw85224-01): last delivered seq 70006 against a
current 78839, while `obs/health`, `sys/metrics` and `tel/log` all arrive every 2 s.
`publishCheap` writes the cell on **every** check-in unconditionally (`kernel.nsl:2135`), so
the node is producing it and the frame is dropping it. Earlier in the same window cheap was
arriving at 4–6 s gaps — trimmed on some check-ins, not all — and it has now degraded to
always. This is the failure `kernel.nsl:1866-1879` describes and #197 predicts, **observed in
the field rather than modelled.** The last whole-branch review flagged it as "modelled, not
measured on a node"; that caveat is now discharged.

## Global Constraints

- **Zero backward compatibility.** Both ends are reflashed together (CLAUDE.md). No versioned
  field, no dual-read, no deprecation window. A shape change is an edit plus a rebuild.
- **The scheme below is ratified** (#197, David 2026-08-11). Do not invent names outside it,
  and do not extend it to labels it does not list (`id`, `cursor`, `hw`, `hi`, `msg`, every
  record *tag*, and all of `LatencySpan` stay exactly as they are).
- **Never bake Path A.** Any `.nsl` bake overwrites the tracked
  `nodus/tuvm/src/solidified_image.c`; it is already ~10.7k lines drifted (#195). Path B
  (`.oimg`) only, per the standing suite rule.
- **Do not reflash `c6-bell` before ~19:15 MST today.** Its post-#190 soak (restored 08:34)
  must clear 10 h 41 m — it is the only experiment that separates design §11.6's arena OOM
  from a second leak, and reflashing restarts the clock. `c6spare` (checks in as `c6-bench`)
  is free.
- **Never run two nsl suites concurrently** — they share `/tmp` paths.
- Read the suite's **aux gate line**, not just `Failed: 0`. `footprint: SKIP (no
  arm-zephyr-eabi toolchain)` is pre-existing and expected.
- `gofmt -l .` must print nothing before any commit touching Go.
- Gates:
  ```
  cd nodus/nsl && python3 tools/run_tests_oevm.py     # Passed: N / Failed: 0 + clean aux line
  cd porta      && go test ./...
  cd nodus/tuvm && bash tests/porta_sync_host.sh      # PORTA SYNC OK
  ```

## The scheme (ratified — copy exactly)

| record | old | new |
|---|---|---|
| `Health` | `uptimeMs` `bootId` `wakes` `pollTimeouts` `streamDropped` | `up` `bid` `wak` `pto` `sdr` |
| `Metrics` | `heapSize` `liveBytes` `heapHighWater` `largestFreeBlock` `gcCycles` | `hsz` `lb` `hhw` `lfb` `gcc` |
| `Metrics` | `handlesLive` `handlesHighWater` `queueHighWater` `overruns` `pulseDrops` `pulseCoalesced` | `hnl` `hnhw` `qhw` `ovr` `pdr` `pco` |
| `CHeap` | `outBytes` `outHigh` `allocs` `frees` `fails` | `ob` `ohw` `alc` `fre` `fal` |
| `Log` | `level` `text` | `lvl` `txt` |
| envelope | `bootId` `entries` | `bid` `ent` |

**Unchanged and load-bearing:** `id`, `cursor`, `hw`, `hi`, `msg`; the tags `#pull` `#push`
`#batch` `#ack` `#err` `#Health` `#Metrics` `#CHeap` `#Log`; and **every** `LatencySpan`
field. Collision check, per record: Health `{up,bid,wak,pto,sdr}`, Metrics
`{hsz,lb,hhw,lfb,gcc,hnl,hnhw,qhw,ovr,pdr,pco}`, CHeap `{ob,ohw,alc,fre,fal}`, Log
`{lvl,txt}`, envelope `{id,cursor,bid,hw,ent,hi}` — all distinct within their record.

## Two corrections to #197's blast radius — verified against the tree

The issue is right about the scheme and wrong about the reach. Both were found by enumerating
before editing, and both would have shipped as silent breakage:

1. **"porta needs no decode change" is false for the envelope half.** `cval` reads *value*
   payloads generically, so `Metrics`/`CHeap`/`Health`/`Log` really do cost porta nothing. But
   porta **parses and builds the envelope keys by name**: `bootId` at
   `porta/internal/wire/message.go:100,112` and `porta/internal/wire/server.go:116,177`, and
   `entries` at `message.go:113,124,191` and `server.go:181`. Renaming `bootId`→`bid` and
   `entries`→`ent` is a **both-sides, same-commit** change. Task 2 exists for exactly this and
   is gated by `porta_sync_host.sh`, which is the only gate that runs both halves.
2. **Two node apps *read* the `Metrics` keys and are not in the issue's list.**
   `nodus/tuvm/zephyr/HttpNode.nsl:75-83` and
   `nodus/tuvm/zephyr/ThermalHttpNode.nsl:364-372,380-381` render a `/stats` page by
   `m at: #heapSize ifAbsent: [ -1 ]`. Miss them and every row on that page silently reads
   `-1` — no test covers it, because no host test renders that page.

Also outside the issue's list, and handled in Task 3: **`tools/heap-envelope.py` reads
`liveBytes`, `largestFreeBlock` and `gcCycles` by name** (lines 183, 187-189) out of stored
soak history, which after this change spans both vocabularies.

**The `#fails` trap.** `#fails` names a field in **two different records**: `CHeap`
(`kernel.nsl:3784`, renamed to `fal`) and `LatencySpan` (`kernel.nsl:1288`, **unchanged**).
`nodus/tests/19_latency/04_record.nsl:32` and `ThermalHttpNode.nsl:563` read the
`LatencySpan` one. A global search-and-replace of `#fails` breaks the latency record and the
suite will tell you — but it will tell you in a file you did not think you were touching.

## File Structure

**Task 1 — the records (node-only vocabulary):**
- Modify: `nodus/nsl/lib/kernel.nsl` — `metrics` (:3736-3752), `cheap` (:3780-3784), the `Log`
  record (:2308), and the `pushFrame` comment block (:1865-1879) whose claim this falsifies
- Modify: `nodus/tuvm/zephyr/PortaBridgeNode.nsl` (:130 Log, :208-212 Health),
  `PortaBridgeNodeC6.nsl` (:11 comment, :173 Log, :253-257 Health),
  `OtaBridgeNode.nsl.tmpl` (:99 Log, :109-113 Health)
- Modify: `nodus/tuvm/zephyr/HttpNode.nsl` (:75-83), `ThermalHttpNode.nsl` (:364-372,
  :380-381) — the readers
- Modify: `nodus/tuvm/zephyr/CborBenchNodeC6.nsl` (:51-73) — bench fixtures
- Test: `nodus/tests/22_spacesync/09_batch_size.nsl` (the size guard — **expectations first**),
  `08_metrics_on_checkin.nsl`, `12_bridge_kind_mismatch.nsl`, `14_cheap_on_checkin.nsl`,
  `nodus/tests/15_heap_stats/02_sys_metrics.nsl`, `03_arm_seams_no_handle_leak.nsl`,
  `nodus/tests/19_latency/05_record_size.nsl`

**Task 2 — the envelope (both sides, one commit):**
- Modify: `nodus/nsl/lib/kernel.nsl` — `pullFrame` (:1776), `encodePush:` (:1952-1953),
  `applyBatch:` (:1790)
- Modify: `porta/internal/wire/message.go` (:100, :112, :113, :124, :191),
  `porta/internal/wire/server.go` (:116, :177, :181)
- Test: `nodus/tests/22_spacesync/03_bridge_frames.nsl` (:41, :53),
  `04_bridge_authority.nsl` (:29), `06_reply_tag_match.nsl` (:95, :100),
  `09_batch_size.nsl` (:154-155), `11_bridge_span_brackets.nsl` (:126),
  `12_bridge_kind_mismatch.nsl` (:47), `nodus/tuvm/zephyr/CborBenchNodeC6.nsl` (:85-86)
- Gate: `nodus/tuvm/tests/porta_sync_host.sh`

**Task 3 — the documents of record and the one tool:**
- Modify: `protocol/PROTOCOL-NSL-RECORDS.md` — §2.2's overflow passage (~:105-145), the
  catalogue rows for `obs/health` / `sys/metrics` / `sys/cheap` / `sys/latency` (~:337,
  :447-449), and the measured-headroom passage after the catalogue (~:462-475).
  **Correction to an earlier draft of this plan:** those passages are in *RECORDS*, not in
  `PROTOCOL-NSL.md` — the line numbers were transcribed from the wrong grep. `PROTOCOL-NSL.md`
  needs a smaller, separate edit: the `health` and `tel/log` rows of its §-summary table, and
  one clause where its `bootId` prose should name the wire label `bid`.
- Modify: `review/tests/CLAIMS.md` — the rows quoting the moved figures (permission granted by
  David 2026-08-11; **additive/in-place only, nothing reordered or restructured**)
- Modify: `tools/heap-envelope.py`

**Task 4 — hardware:**
- No file changes. `c6spare` only.

---

## Task 1: The records — Health, Metrics, CHeap, Log

**Interfaces:**
- Produces: the four record shapes every later task encodes, decodes, documents or measures.
  After this task the frame should encode to **~386 B** (the envelope's 8 B is Task 2's).

- [ ] **Step 1: Write the failing expectation — the guard's header, before any rename**

Edit the `expect:` block at the top of `nodus/tests/22_spacesync/09_batch_size.nsl` to the
predicted post-rename figures, leaving the fixture and the code untouched:

```
expect:
health 61
metrics 101
cheap 62
batch 385
```

`health`, `metrics` and `cheap` come from #197's measured table. `batch` is **385, not 378** —
378 is the figure *after* Task 2's envelope rename, and this task does not do that. The
arithmetic that must hold, per byte: 529 − 30 (Health) − 94 (Metrics) − 17 (CHeap) − 3 (Log:
`level`→`lvl` is 2, `text`→`txt` is 1) = **385**. Task 2 then removes 7 more (`bootId`→`bid`
is 3, `entries`→`ent` is 4, once each per frame) to reach **378**, which is exactly the figure
#197 measured independently through `Cbor>>encode:`. Two derivations agreeing to the byte is
the corroboration; a mismatch would mean the scheme was not applied as ratified.

**If the run disagrees with these numbers, do not adopt what it prints.** A mismatch means the
scheme was not applied as measured — find the label that was missed or mistyped. The one
exception worth expecting is ±1–2 B on `batch` from a CBOR length-prefix boundary crossing as
the map shrinks; if `health`/`metrics`/`cheap` are all exact and only `batch` is off by 1–2 B,
that is real and the header takes the measured value **with a comment saying which boundary
moved**.

- [ ] **Step 2: Run it and watch it fail**

```bash
cd ~/workspace/rete/nodus/nsl && python3 tools/run-one.py ../tests/22_spacesync/09_batch_size.nsl
```

Expected: FAIL, printing `health 91 / metrics 195 / cheap 79 / batch 529` against the new
header. A **SKIP proves nothing** and a **CRASH discards every line already printed** — if you
get either, fix that before reading anything into the result.

- [ ] **Step 3: Rename in the builders**

`nodus/nsl/lib/kernel.nsl` — apply the scheme table, `#old` → `#new`, at:
`:3736-3744` and `:3751-3752` (`Metrics`, 11 fields), `:3780-3784` (`CHeap`, 5 fields),
`:2308` (`Log`: `#level` → `#lvl`, `#text` → `#txt`).

**`:2308` only.** The other `#text` occurrences in this file (`:913`, `:2715`, `:2719`,
`:3791`, `:3797`) belong to the **crash record**, which is not in the scheme and must not
move. Verify after editing: `rg -n '#text' nodus/nsl/lib/kernel.nsl` should still return five
hits, all in crash-record context.

Health lives in the apps, not the kernel — `PortaBridgeNode.nsl:208-212`,
`PortaBridgeNodeC6.nsl:253-257`, `OtaBridgeNode.nsl.tmpl:109-113` — plus the `Log` record each
one builds (`:130`, `:173`, `:99` respectively).

- [ ] **Step 4: Rename in the readers — the step that has no test**

`nodus/tuvm/zephyr/HttpNode.nsl:75-83` (9 lookups) and
`nodus/tuvm/zephyr/ThermalHttpNode.nsl:364-372, 380-381` (11 lookups). Leave the human-facing
label strings (`'heap size (bytes)'`, `'live bytes'`, …) **exactly as they are** — that page is
an operator surface and the long names are what it is *for*. Only the `#symbol` in
`m at: #x ifAbsent: [ -1 ]` changes.

`ThermalHttpNode.nsl:563` reads `row at: #fails` — **confirm which record `row` is** before
touching it. If it is a `LatencySpan` (it renders the latency table), it stays `#fails`.

- [ ] **Step 5: Rename in the fixtures and the assertions**

Fixtures (they construct records, so they move with the schema):
`nodus/tests/22_spacesync/09_batch_size.nsl:74-125` (Health, Metrics, CHeap) and `:129` (Log);
`nodus/tuvm/zephyr/CborBenchNodeC6.nsl:51-70` and `:73`;
`nodus/tests/19_latency/05_record_size.nsl:52-62` (Metrics).

Assertions (they look keys up, so they move too):
`nodus/tests/15_heap_stats/02_sys_metrics.nsl:27-40`,
`03_arm_seams_no_handle_leak.nsl:35,40,46,49`,
`nodus/tests/22_spacesync/08_metrics_on_checkin.nsl:38-40`,
`12_bridge_kind_mismatch.nsl:67` (`#level` → `#lvl`),
`14_cheap_on_checkin.nsl:21-23`.

**Do not touch** `nodus/tests/19_latency/04_record.nsl:32` (`LatencySpan`'s `#fails`).

- [ ] **Step 6: Re-pin the sizes the suite asserts elsewhere**

`nodus/tests/19_latency/05_record_size.nsl` asserts `#Metrics` at **195 B**; it becomes
**101 B**. `#LatencySpan`'s 140 B does **not** move — nothing in that record was renamed, and
if that number changes you have edited something you should not have.

- [ ] **Step 7: Prove no long name survives in code**

```bash
cd ~/workspace/rete
rg -n "#(uptimeMs|bootId|wakes|pollTimeouts|streamDropped|heapSize|liveBytes|heapHighWater|largestFreeBlock|gcCycles|handlesLive|handlesHighWater|queueHighWater|overruns|pulseDrops|pulseCoalesced|outBytes|outHigh|allocs|frees|level)\b" --glob '*.nsl' --glob '*.tmpl' nodus/
```

Expected after this task: **only** `#bootId` hits (Task 2 owns those, at `kernel.nsl:1776`,
`:1952` and in the frame fixtures) and hits inside comments. `docs/spikes/` is deliberately
excluded — `2026-08-09-cbor-encode-shape/CborEncodeShape.nsl` is a dated spike and stays as the
historical record of what was measured then.

- [ ] **Step 8: Run the guard, then the whole suite**

```bash
cd ~/workspace/rete/nodus/nsl && python3 tools/run-one.py ../tests/22_spacesync/09_batch_size.nsl
cd ~/workspace/rete/nodus/nsl && python3 tools/run_tests_oevm.py
```

Expected: the guard passes at the Step 1 numbers; the suite prints `Failed: 0` **and** a clean
aux line. The suite takes >10 min — run it in the controller, not a subagent, which will
background it and wait forever.

- [ ] **Step 9: Commit**

```bash
cd ~/workspace/rete
git add -A nodus/
git commit -m "$(cat <<'EOF'
feat(#197): one naming rule for the frame's record labels

Health 91 -> 61 B, Metrics 195 -> 101 B, CHeap 79 -> 62 B, frame 529 -> 386 B,
all through the real Cbor>>encode: path. The envelope's remaining 8 B is the
next commit; 378 is where this lands.

The rule is keep-the-noun-suffix-the-aspect, not chop-to-three: hnhw, because
hhw is already heapHighWater and a collision inside one record is a silent wire
bug. Keys are scoped per record, so bid means the same thing in Health and in
the envelope and costs nothing to reuse.

Two readers the issue's blast radius missed, and no test covers either:
HttpNode and ThermalHttpNode render /stats by `m at: #heapSize ifAbsent: [-1]`,
so a missed rename there is a page of -1 rather than a red gate. Their
human-facing label strings are untouched — that page is the operator surface
the short names exist to keep OFF the wire.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: The envelope — `bootId` → `bid`, `entries` → `ent`, both sides

**Interfaces:**
- Consumes: Task 1's records.
- Produces: the final 378 B frame, and the only change in this plan that can desynchronize the
  two halves. **`porta_sync_host.sh` is the gate that matters here** — it is the one that runs
  a real node against a real gateway.

- [ ] **Step 1: Move the guard's target to 378**

In `nodus/tests/22_spacesync/09_batch_size.nsl`, change the `expect:` header's `batch` line
from `386` to `378` and leave the other three alone — the envelope carries no record.

- [ ] **Step 2: Run it and watch it fail**

```bash
cd ~/workspace/rete/nodus/nsl && python3 tools/run-one.py ../tests/22_spacesync/09_batch_size.nsl
```

Expected: FAIL at `batch 386`.

- [ ] **Step 3: Rename on the node side**

`nodus/nsl/lib/kernel.nsl`: `pullFrame` `:1776` `#bootId` → `#bid`; `encodePush:` `:1952`
`#bootId` → `#bid` and `:1953` `#entries` → `#ent`; `applyBatch:` `:1790`
`r at: #entries ifAbsent:` → `r at: #ent ifAbsent:`.

Leave `#id`, `#cursor` and `#hw` alone — they are already at the floor and are not in the
scheme.

- [ ] **Step 4: Rename on the porta side, in the same commit**

`porta/internal/wire/message.go`: `:100` and `:112` `cval.Symbol("bootId")` →
`cval.Symbol("bid")`; `:113` and `:124` `cval.Symbol("entries")` → `cval.Symbol("ent")`;
`:191` `field(r.Fields, "entries")` → `field(r.Fields, "ent")`.

`porta/internal/wire/server.go`: `:116` and `:177` `intField(r.Fields, "bootId")` →
`intField(r.Fields, "bid")`; `:181` `field(r.Fields, "entries")` → `field(r.Fields, "ent")`.

Leave `"id"`, `"cursor"`, `"hw"`, `"hi"`, `"msg"` and every `Tag:` alone. Go *identifiers*
named `entries`/`bootID` are locals and do not change — only the quoted wire symbols do.

- [ ] **Step 5: Rename the frame fixtures in the suite**

`nodus/tests/22_spacesync/03_bridge_frames.nsl:41,53`; `04_bridge_authority.nsl:29`;
`06_reply_tag_match.nsl:95,100`; `09_batch_size.nsl:154-155`;
`11_bridge_span_brackets.nsl:126`; `12_bridge_kind_mismatch.nsl:47`;
`nodus/tuvm/zephyr/CborBenchNodeC6.nsl:85-86`.

- [ ] **Step 6: Prove the vocabulary is now uniform**

```bash
cd ~/workspace/rete
rg -n '#(bootId|entries)\b' --glob '*.nsl' --glob '*.tmpl' nodus/
rg -n '"bootId"|"entries"' --glob '*.go' porta/
```

Expected: no hits in code from either. Comments and `_test.go` prose may still say `bootId`
where they are *describing* the mechanism — read each hit rather than deleting it.

- [ ] **Step 7: Run all three gates**

```bash
cd ~/workspace/rete/porta && gofmt -l . && go test ./...
cd ~/workspace/rete/nodus/nsl && python3 tools/run_tests_oevm.py
cd ~/workspace/rete/nodus/tuvm && bash tests/porta_sync_host.sh
```

Expected: `gofmt -l .` prints nothing; Go tests pass; suite `Failed: 0` with a clean aux line;
`PORTA SYNC OK`. **`porta_sync_host.sh` is the falsifier for this task** — if only one side
were renamed the suite and `go test` would both stay green and this would go red, which is the
whole reason the envelope gets its own commit.

- [ ] **Step 8: Commit**

```bash
cd ~/workspace/rete
git add -A nodus/ porta/
git commit -m "$(cat <<'EOF'
fix(#197): the envelope's labels too — and porta parses these by name

Frame 386 -> 378 B, 134 B under the 512 B cap.

#197 said porta needs no decode change. That is true of the VALUES — cval reads
payload maps generically — and false of the ENVELOPE: porta builds and parses
bootId and entries by name in wire/message.go and wire/server.go. So this is a
both-sides change and it ships in one commit, because a half-applied rename is
a wire desync that the nsl suite and `go test` would BOTH call green.

porta_sync_host.sh is the only gate that runs both halves against each other,
which is why the envelope was split out of the record rename rather than folded
into it.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: The documents of record, the claims, and the one tool that reads history

A rung is not closed until its knowledge leaves the plan. Every figure below is one a reader
will otherwise trust and be wrong about.

- [ ] **Step 1: Re-pin `PROTOCOL-NSL-RECORDS.md`**

- `:316` — the `obs/health` row: the `Health { … }` field list becomes the short names, and
  **91 B → 61 B**. Keep the `heapFree`/`heapLargest`/`overruns`-were-removed history and the
  `bootId`-is-a-real-counter note; both are still true and both are why the number was 91.
- `:426` — `sys/metrics`: field list to short names, **195 B → 101 B**, and the headroom
  sentence "61 B of headroom" → **155 B**.
- `:427` — `sys/cheap`: field list to short names, **79 B → 62 B**. Keep the note that the
  earlier 75 B figure undercounted by measuring a sub-65536 case — that is the lesson, not the
  number.
- `:428` — `sys/latency` is **unchanged**. Its "field names are deliberately short" sentence is
  now the precedent this whole change generalizes; say so in one clause.
- `:442-462` — the measured-sizes passage: `Metrics` 195 → 101 B, and the "roughly two more
  integer fields" headroom reading becomes roughly **thirteen**.
- `:530` — `diag/spans/<span>` at 140 B each: unchanged, and worth leaving unchanged
  deliberately.

- [ ] **Step 2: Re-pin `PROTOCOL-NSL.md`**

`:107-110` currently states the overflow as live: `obs/health` 91 B, `sys/metrics` 195 B,
`sys/cheap` 79 B, "**529 B against 512**", and the series `425 → 429 → 433 → 529`. Rewrite to
the resolved state: the series continues `→ 378`, and the frame now fits with 134 B of
headroom. `:127-139` already records the scheme as measured — reconcile it so it reads as
**done** rather than proposed, keeping the rejected-alternatives paragraph (numbered slots make
meaning positional; an unknown *name* is loud where a mismatched `m7` decodes cleanly and
silently means something else).

- [ ] **Step 3: Rewrite the two comment blocks that now assert something false**

`nodus/nsl/lib/kernel.nsl:1865-1879` (`pushFrame`) says the cap binds and that shortening
`CHeap`'s five names "does not reach". Both halves need care: the second is still **true as
written** (CHeap alone lands at 503 B, one-character names at the floor) and it is the reason
the fix had to be all-labels. Keep it as the falsified-cheaper-alternative, and replace the
"the cap now binds" paragraph with what is now true — 378 B, 134 B of headroom, and #150's
structural question (reserve bytes for `sys/`, or trim by type) still **open and still
unfunded**, because this change bought headroom rather than changing the trim rule.

`nodus/tests/22_spacesync/09_batch_size.nsl`'s header comment carries the same two claims plus
the "WHY THIS IS PINNED RED-AT-WORST-CASE" paragraph. It is no longer pinned red. Say what it
is pinned at and why 30-day values are still the right fixture.

- [ ] **Step 4: Update the CLAIMS rows whose evidence moved**

Permission granted by David 2026-08-11 — **in-place edits to the affected rows only**, nothing
reordered, nothing restructured. Three rows quote figures this change moves:

- the `sys/metrics`-published-unasked row (`09_batch_size.nsl` cited as pinning `obs/health`
  87 B, `#Metrics` 195 B, frame 425 B)
- the sizes-are-MEASURED-not-estimated row (`#LatencySpan` **140 B stays**, `#Metrics`
  195 → 101 B)
- the `bootId`-is-evidence row (its closing cost sentence: `obs/health` 87 → 91 B and the frame
  425 → 429 B)

Each already reads as a dated series. **Extend the series, do not overwrite it** — "425 → 429 →
433 → 529 → 378 (rete#197)" keeps the row's own history legible, which is the property that
makes these rows worth having. Check the three rows appended this morning for size quotes
before assuming only these three are affected.

- [ ] **Step 5: Teach `tools/heap-envelope.py` the short names**

It reads `f['gcCycles']` (`:183`) and `'liveBytes'` / `'largestFreeBlock'` (`:187-189`) out of
stored soak history — and any db that spans today holds **both** vocabularies. Normalize at the
decode boundary in `rec_fields` (`:111-117`), mapping the long names onto the short ones so the
tool speaks one vocabulary internally, and name rete#197 and the date in a comment. This is an
analysis tool over frozen history, not a wire reader: the no-dual-read rule governs the
protocol, and a one-dict alias here is the smaller mechanism than re-encoding old rows.

- [ ] **Step 6: Commit**

```bash
cd ~/workspace/rete
git add -A protocol/ review/ tools/ nodus/
git commit -m "$(cat <<'EOF'
docs(#197): re-pin every figure the rename moved

RECORDS: Health 91 -> 61, Metrics 195 -> 101 (headroom 61 -> 155 B), CHeap
79 -> 62. LatencySpan's 140 B does NOT move, and that is the point — nothing in
it was renamed, because its names were already short. The debt this closes is
one RECORDS §4.4 opened against itself.

PROTOCOL-NSL: the series is 425 -> 429 -> 433 -> 529 -> 378, and the passage
that stated the overflow as live now states it as resolved.

The two comment blocks that asserted the cap binds are rewritten, but the claim
that shortening CHeap ALONE does not reach is KEPT — it is still true (503 B at
one-character names, the floor) and it is the measurement that forced an
all-labels rule rather than a local patch. #150's structural question stays
open: this bought headroom, it did not change the trim rule.

CLAIMS rows extend their series rather than overwriting it, so each row's own
history stays legible.

heap-envelope.py normalizes long names onto short at the decode boundary — a
stored soak spanning today holds both vocabularies, and this is an analysis
tool over frozen history, not a wire reader.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: The hardware gate — the gauge comes back

The host proves the frame *fits*. Only a board proves the gauge *arrives*, and there is a live
failure to close: `sys/cheap` has been absent from `c6-bench` for over an hour and a half.

> ### ATTEMPTED 2026-08-11 15:38 AND BLOCKED — read this before retrying
>
> **This task is a CUTOVER, not a flash.** That was not obvious in advance and it cost a
> panicked board to learn. `c6spare` was flashed with the new firmware while gw85224-01 was
> still running the pre-#197 porta binary, and every frame was refused:
> `malformed message: missing #bootId`. The rename is a **flag day**: node and gateway must
> move together, and the gateway is a hand-started binary that this plan never mentions.
>
> **Worse, the refused node did not idle — it halted.** `VM GC Error: Out of Memory
> (requested 4104, used 127680 of 131072)` after 46 ticks, 2 min 37 s, with `misses 0` on the
> console throughout. Filed as **rete#199** (P1). Do not retry this task assuming a mismatched
> node simply goes quiet; it has a ~3-minute fuse.
>
> **And the two boards cannot straddle the change.** `c6-bell` runs the pre-#197 firmware and
> cannot be reflashed until its soak clears (19:15 on 2026-08-11); porta speaks exactly one
> protocol. `c6spare` was therefore rolled back to `porta-v1-final-952-ge52a86e` — the commit
> the bell runs — and is converged (`TAP bridge-health done pass=3 fail=0`).
>
> **The order for the retry, and it is the whole content of this task now:**
> 1. Wait for the bell's soak to clear and **record its verdict first** — that experiment is
>    the reason for the wait, and reflashing destroys it.
> 2. Build and deploy the new `porta` binary to gw85224-01, restoring **the exact command line
>    found running** (`./porta serve --db soak-2026-08-04.db`, read from `/proc/<pid>/cmdline`,
>    not from memory — the db has changed at least once). The kill gets its **own** ssh
>    invocation: `pkill -f` matches its own command line and leaves the gateway down.
> 3. Flash both boards. Between steps 2 and 3 every node on the mesh is on a ~3-minute fuse
>    per rete#199, so this window is not one to leave running unattended.
>
> **A prediction worth checking on the rolled-back board, because it costs nothing and would
> confirm the width mechanism independently:** `c6-bench` is delivering `sys/cheap` 1:1 with
> `sys/metrics` right now, because a freshly booted node's counters are narrow. On the
> pre-#197 firmware that should **decay back to zero** as its uptime widens the encoded values.
> If it does not, the width story in §2.2 is incomplete.

- [x] **Step 1: Record the pre-state, so the gate has a before**

Read-only against the running gateway (do **not** restart it — it is the one shared gateway and
a restart costs every node an outage):

```bash
ssh gw85224-01 'tr "\0" " " < /proc/$(pgrep -f "porta serv[e]" | head -1)/cmdline'
```

Then read `cells` and `events` for `c6-bench` and record: the age of the last `sys/cheap` row,
its seq, and the current `sys/metrics` seq. The pre-state as of 14:52 today was cheap seq 70006
at 42 min stale against metrics seq 78839 — a ~8 800 seq gap.

- [x] **Step 2: Build and flash `c6spare` — pristine, everything named explicitly**

`c6spare` is `TARGET_DEFAULT_BOARD` and checks in as `c6-bench`. **Flash `c6spare` only.**
Resolve the board through the collector env files, never a port number — three CP210x boards
share the by-id glob. Build `--pristine` into a fresh build directory with the Thread
configuration named on the command line: §11.7 caught a green build that was not a reproducible
build, because a warm CMake cache was supplying the radio config, and a fresh build directory
later gave the bell an image with **no radio at all**.

`ESPTOOL_PORT` must be set explicitly for the C6 — its `run.sh` profile pins `*Espressif*` to
`/dev/ttyACM0` (native USB-JTAG), which is not cabled; everything goes over the CP2102N.

- [x] **Step 3: The gate — cheap on every check-in, for a window long enough to mean it**

Pass condition, all four:
1. `sys/cheap` arrives on **every** check-in — no 4–6 s gaps, no silence. Compare the count of
   `sys/cheap` events against the count of `sys/metrics` events over the same window; they are
   published on the same check-in and should now match one-for-one.
2. The gap between cheap's seq and metrics' seq stays bounded (it was ~8 800 and growing).
3. Outstanding blocks (`alc − fre`) stay **flat at 0**, reproducing §11.7 under the new labels
   — this change must not disturb what stage 2 measured.
4. The node stays alive past the tick it reaches in the window.

Run it long enough that a *fixed cadence* is not confounding you — the failure was driven by
check-in count, not wall time. A window that only covers bench-width values proves nothing
about the 30-day case the guard fixtures model; say so explicitly in the report rather than
implying the field case was tested.

- [x] **Step 4: The bell — only after ~19:15 MST**

Do not touch `c6-bell` until its post-#190 soak clears 10 h 41 m. When it has: record the soak's
verdict **first** (that experiment is what separates design §11.6's arena OOM from a second
leak, and it is the reason for the wait), then reflash it on the same commit and confirm the
same four conditions on a different app at a 15× slower cadence. A second app reproducing the
result is what says the fix is in the frame rather than in one node's shape.

- [x] **Step 5: Fold the result into §11 and close the issue**

Add the measured result to `docs/design/2026-08-10-node-memory-and-liveness-design.md` §11 as a
new subsection — the design doc is where §11.7's hardware gate lives and where a reader looks
for what a board actually showed. Then close #197 with the numbers, and say plainly which of
the four conditions were tested at bench widths rather than field widths.

---

## Self-review

**Spec coverage.** Every row of #197's scheme table has a task: records in Task 1, envelope in
Task 2, documents in Task 3, hardware in Task 4. The issue's "blast radius" list is covered
plus the three items it missed (porta's envelope parsing, the two `/stats` readers,
`heap-envelope.py`). The issue's sequencing constraint — do not reflash the bell before its
soak clears — is a global constraint and a step in Task 4.

**What this plan deliberately does not do.** It does not touch `LatencySpan`, the record tags,
`id`/`cursor`/`hw`/`hi`/`msg`, the trim rule, or #150's structural question. It does not add a
porta-side display name-map: the issue records that as a separate, non-blocking follow-up that
needs no further wire change, and adding it here would widen a rename into a feature.

**Where the defects will be**, given #165 and #190 each carried ~1 defect per task in the
plan's own text: (a) the `#text` / `#fails` collisions, where a global replace hits a record
that is not in the scheme — Step 3 and Step 4 of Task 1 each carry an explicit verification;
(b) the envelope half-applied, which two of the three gates call green — Task 2 exists to be
gated by the third; (c) the predicted byte counts, which are the previous session's
measurements and not this session's — Task 1 Step 1 says what to do when they disagree, and the
answer is not "adopt what it prints".

**One number in this plan is a prediction, not a measurement:** `batch 386` at the end of
Task 1. It is derived arithmetic (529 − 141 − ~3), not something anyone has run. The 61 / 101 /
62 / 378 figures were measured through the real `Cbor>>encode:` path on 2026-08-11.
