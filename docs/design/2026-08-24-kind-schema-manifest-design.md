# The kind+schema manifest — design (#243, 2026-08-24)

**Status: ratified by David, 2026-08-24.** Decision record for rete#243. Starts from the
five inputs recorded on the issue that same day (manifest authored in `protocol/`, served
by porta; snapshot-pin at compile, live-validate at provision; generate-at-compile +
check-at-ingest; restricted CDDL subset with zcbor at the gates) — those are inherited,
not re-argued here. This document records the decisions *below* that level: file layout,
matching rules, tag spelling, pin mechanics, gate seats, failure surface, and document
authority.

**The failure class this closes:** kind-pinned-by-first-write has produced silent,
permanent, per-leaf divergence twice (#25 read side, #174 write side), the rule is
enforced by convention on the node and not at all at porta (#180: `Store.Write` pins no
kind), and `PROTOCOL-NSL-RECORDS.md` pins kind and schema per path — in prose. This
design makes the catalogue machine-readable and puts it at the gates, so a divergent
write is a build/ingest error instead of a field incident. Subsumes the enforcement half
of #180.

**This is a design, not a plan.** The implementation plan follows separately.

---

## 1. Decisions

| ref | decision |
|---|---|
| D1 | **One annotated file, `protocol/MANIFEST.cddl`** — shapes are plain CDDL rules; the path→(kind, class, dir) binding rides `;#` annotation lines. 100% stock-valid CDDL throughout. |
| D2 | **Matching is closed, segment-wise, overlap-free.** `<var>` matches exactly one segment; no deep wildcard; overlapping entries are a lint error, so no precedence rule exists; an unmatched path is a gate error on both ends. |
| D3 | **Full-wire tag spelling via named symbol-key rules.** The CDDL describes the exact wire bytes — tag-27 records, tag-39 names and keys — with each symbol key a named `sym-*` rule. **Probed green on zcbor 0.9.1** (§4.1). |
| D4 | **The pin travels both ways: archive stamp AND `obs/build.man`.** The build stamps the full manifest hash into the #256 archive entry and bakes the truncated 8 bytes into the image beside `buildId`; the node publishes it once per boot. |
| D5 | **Compile-time source is porta's served manifest, with a loud repo fallback.** Unreachable porta → build from `protocol/MANIFEST.cddl`, print a build note, stamp the archive entry `source=repo`. The provision gate is what makes the fallback safe. |
| D6 | **The ingest gate is seated in `Store.Write` and checks path + kind + head-bytes + dir.** Both write doors (wire push and operator `put`) pass the same checks. No deep shape validation in Go. |
| D7 | **A failed write is refused-but-kept: entry not applied, seq acked, bytes retained** as an `events` row with `actor=gate` and a reason naming the failed check. No new table; the refusal ledger is rows. |
| D8 | **`MANIFEST.cddl` is machine-normative** for path/kind/class/dir/shape; `RECORDS.md` keeps the why and each row names its rule. |
| D9 | **Field maps validate CLOSED at the gates.** An undeclared field is a build/test error. `RECORDS.md` §5 is rewritten for the zero-compat fleet (§7). |
| D10 | **`dir=` is part of the binding** — the manifest carries the write-authority map, and `diag/spans/ctl` is recorded `dir=up` (tuvm#159 stays open as the issue that would flip it). |
| D11 | **porta serves the manifest at `GET /manifest`** on `httpsrv` — file bytes plus hash. The one endpoint this design adds. |
| D12 | **The pin hash is SHA-256 of the file bytes.** A semantic (comment-stripped) hash was considered and rejected: a second canonicalization mechanism, bought to avoid a rebuild that zero-compat makes cheap. |

Deferred, with the trigger that would earn each back (§8): code generation from the
manifest; deep-shape ingest validation; deep wildcards / precedence; a manifest version
field (the hash is the version).

---

## 2. The file — `protocol/MANIFEST.cddl`

Lives beside `PROTOCOL-NSL-RECORDS.md`, whose machine-readable half it is. Three kinds
of line: comments, `sym-*` prelude rules, and annotated shape rules.

```cddl
; --- symbol prelude: one rule per distinct key or record name,
;     shared across records ---
sym-Health = #6.39("Health")
sym-up     = #6.39("up")
sym-bid    = #6.39("bid")

; --- shapes ---

;# path=obs/health  kind=cell  class=routine  dir=up
health = #6.27([sym-Health, {
  sym-up  => uint,
  sym-bid => uint,
  sym-wak => uint,
  sym-pto => uint,
  sym-sdr => uint,
  sym-ref => uint,
}])

;# path=goal/config/<app>/<key>  kind=cell  class=routine  dir=down
config-scalar = scalar

;# path=tel/log  kind=stream  class=drop-first  dir=up
log = #6.27([sym-Log, { sym-level => sym-any, sym-text => tstr }])

scalar  = int / tstr / bool / #6.39(tstr)
sym-any = #6.39(tstr)
```

### 2.1 The annotation grammar

One `;#` line immediately above the rule it binds:

```
;# path=<pattern>  kind=cell|stream  class=routine|must-deliver|drop-first  dir=up|down
```

- `path` — segments separated by `/`; a segment is a literal or `<var>` (exactly one
  segment, any name). Every wildcard in today's catalogue is single-segment
  (`goal/apps/<name>`, `goal/config/<app>/<key>`, `tel/m/<name>`, `diag/spans/<span>`,
  …); no entry needs more.
- `kind` — matches `SPACE_KIND_*` (`stream` = 1, `cell` = 2; shared with
  `nodus/oevm/src/space.h`, do not renumber).
- `class` — the delivery class, per-entry (a namespace-wide statement like "`tel/**` is
  drop-first" becomes one annotation per row, not a namespace mechanism).
- `dir` — who writes this path on the wire, from the node's point of view: `up` (node
  writes) or `down` (porta writes). There is no `both`: the one bidirectional namespace
  splits cleanly (`dbg/req` down, `dbg/resp` up), and a path that genuinely needed both
  directions would be a design change, not a vocabulary entry. This is §3-of-`PROTOCOL-NSL.md`'s
  authority rule made machine-readable: porta's writable set is exactly the `dir=down`
  entries, and today that must equal `goal/**` + `dbg/req`.

**Optional fields use CDDL's `?`.** Absence keeps its catalogue meaning — "the node
cannot say" — e.g. `obs/build`'s `sdk` and `image`, and `AppObs`'s omitted fields.

**Enum-valued fields** may pin their symbols as a choice
(`#6.39("running") / #6.39("stopped")`) or stay `sym-any = #6.39(tstr)` where the
catalogue leaves the set open. Start with `sym-any` except where the catalogue already
enumerates (e.g. `Panic.reason`, `Log.level`); tightening later is a manifest edit.

### 2.2 The lint

Runs wherever the gates run (§5). All findings are errors:

1. Every shape rule has exactly one annotation; every annotation names a rule that
   exists.
2. No two `path` patterns can match the same concrete path (overlap check is decidable
   segment-wise: two patterns overlap iff same length and every position is
   literal-equal or at least one side is `<var>`).
3. Every `sym-*` rule is referenced; every referenced rule is defined.
4. Annotation values are from the closed vocabularies above.
5. The file parses as the restricted subset (flat maps of scalar fields, record-valued
   fields by rule reference — `BlobRef` nests one level, which the subset allows —
   lists of scalars, choices of scalars).

The lint plus D9's closed maps is the **completeness gate**: no path and no field ships
before the manifest declares it, and no manifest entry outlives its record.

---

## 3. Matching and closedness (D2)

- Segment-wise match; `<var>` = exactly one segment; no `**`.
- **No precedence rule exists** because the lint makes ties unrepresentable. If
  literal-specializes-wildcard is ever needed (e.g. a literal `goal/apps/main` row
  beside `goal/apps/<name>`), that need is what earns a precedence rule — and it is a
  design change, not a quiet addition.
- **The manifest is closed**: a path no entry matches is a gate error at build time and
  a refused write at ingest — the codec's closed-decode posture applied to paths.

---

## 4. Tag spelling (D3) — and the probe that settled it

The CDDL describes the **exact wire bytes**. The gates validate raw store/wire bytes
with zero transformation — no unwrap shim whose correctness the gate would then be
trusting. The cost is the `sym-*` prelude (~one line per distinct key, shared across
records) and noisier rules; the alternative spellings (symbol-erased with a per-gate
unwrap shim; authored-clean with a generated wire file) were rejected as second
mechanisms seated exactly where the gate must be trustworthy.

### 4.1 Probe record (2026-08-24, zcbor 0.9.1; vendored copy in
`~/zephyr-esp32/modules/lib/zcbor` is the same 0.9.1)

| probe | result |
|---|---|
| inline tagged key `#6.39("up") => uint` | **fails to parse** — zcbor: "A key cannot be a group". A zcbor limitation, not CDDL's; the named-rule form below is the subset's answer |
| named key rule `sym-up = #6.39("up")` used as `sym-up => uint`, full-wire instance | **exit 0** |
| same rule, bogus key in instance | **exit 1**, error names the key |
| same rule, wrong value type | **exit 1**, error names field and types |
| map openness | zcbor validates maps **closed by default** — the bogus-key control failed on exactly that, so D9 costs nothing |

Operational pins for every gate that shells to zcbor:

- **`cbor2 < 6`** — cbor2 6.x removed `CBORDecodeValueError` and breaks zcbor 0.9.1's
  import.
- **Pipe CBOR via stdin** (`-i -`): 0.9.1's CLI crashes opening a CBOR file
  (`open(path, "rb", encoding="utf-8")`). Worth an upstream patch; do not wait for one.
- A pipeline's verdict is **zcbor's exit code, not the pipe's** — `zcbor … | tail`
  answers with `tail`'s status. (This false green appeared during the probe itself.)

---

## 5. The gates

Input 4 resolved generate-vs-check as *both, at different times*. Where each check runs:

### 5.1 Build time (dev tools)

1. Fetch the served manifest from `GET /manifest`; pin it (D5's fallback if
   unreachable).
2. Run the lint (§2.2).
3. zcbor-validate the suite's golden record encodings against the pinned manifest.
   Mismatch = **build error** — the Elm ports lesson: the contract is declared where
   the worlds meet, and a mismatch never leaves the desk.
4. Stamp the pin (§6).

**Generation is deferred.** The build *checks*; it does not yet generate code from the
manifest. The trigger that earns generation back: a third hand-maintained copy of any
shape appearing (today there are two — the node's builders and porta's cval goldens —
and the manifest now gates both).

### 5.2 Test gates (both sides)

- An **nsl-suite aux gate**: the node's own `Cbor>>encode:` output for each catalogue
  record, validated against the manifest with zcbor.
- A **porta golden test**: `internal/cval`'s encoder output for the same records,
  validated the same way.

The two codecs are today golden-tested against *each other*; after this they are
golden-tested against **one declared contract**, and a divergence names which side
moved.

### 5.3 Ingest (porta, runtime) — seated in `Store.Write` (D6)

porta parses the restricted subset directly at startup (~100 lines; annotations + rule
heads) — no generated table, no second artifact. From it, a per-entry tuple:
`(pattern, kind, class, dir, expected-head)`.

Four checks on every write, **both doors** — the wire handler's node push and the
operator's `porta put` both land in `Store.Write`, so #180's hole closes for both in
one seat:

1. **Path is matched** by exactly one entry (closed set).
2. **Kind matches** the entry.
3. **Head bytes match**: record rows carry a constant expected prefix (the
   deterministic encoding makes `d8 1b 82 d8 27 66 …name…` computable once, via cval,
   at manifest load); scalar rows carry a major-type class. A wrong-record-at-path
   write — the same silent-permanent shape as kind divergence — fails on a constant
   byte compare, with no decode: the store stays opaque (rewrite design D4.1).
4. **Direction matches** `dir` — a node writing `goal/**`, or an operator writing
   `obs/**`, is refused.

Deep shape validation deliberately does **not** run here (input 5): the compile and
test gates own it, and a value that passes those gates and still arrives malformed at
ingest is a codec bug, which head-bytes will usually catch and the event ledger will
always record.

### 5.4 Failure surface (D7)

The two doors fail differently, because only one has a peer that must not wedge:

- **The operator door** (`porta put`, or any northbound caller of `Store.Write`): the
  write is refused with an immediate error naming the failed check. Nothing is written;
  the operator is at a keyboard and the error is the fix's first line.
- **The wire door** (a node's push): a refused entry

- is **not applied** — no cell upsert, no stream delivery;
- has its **seq acked** — the node advances instead of wedging in a retry loop (a
  refused node's data going dark is the near-fatal shape the catalogue documents; a
  manifest mismatch is a build error to fix at the bench, not a runtime condition to
  negotiate with);
- is **kept**: an `events` row with the refused bytes, `actor=gate`,
  `reason=manifest:<check>:<detail>` (e.g. `manifest:kind path=obs/health got=stream
  want=cell`). Nothing is silently lost — the bytes are in the log, `porta log` finds
  them, and a per-node refusal count is a query over rows, not a new counter mechanism.

The node side needs no runtime half: the node carries nothing extra (input 4), and its
conformance is enforced by §5.2 before an image ever ships.

---

## 6. Pin mechanics

The pin is **SHA-256 of `MANIFEST.cddl`'s file bytes** (D12).

1. **Build**: the dev tool fetches and pins (D5). The full hash is stamped into the
   #256 content-addressed archive entry beside the ELF and `.stmap`, with
   `source=porta|repo`; the truncated **8 bytes** are baked into the image beside
   `buildId`.
2. **Boot**: the node publishes `obs/build.man` (8-byte truncated hash) once per boot,
   on `obs/build.image`'s truncated-hash precedent — it identifies a manifest against a
   known set; it does not authenticate one. Absence means "the node cannot say"
   (a pre-manifest image). ~13 B in a once-per-boot record measured at 82 B against the
   256 B value cap; not part of the steady-state frame.
3. **Provision**: gatekeeper resolves the build's archived pin (build-id → archive
   entry) and requires it equal the hash of the manifest porta currently serves.
   Mismatch = **provision error**; a missing archive entry is a provenance failure and
   refuses the same way.
4. **Live**: porta can tree-diff every node's reported `man` against the served hash —
   contract divergence becomes an ordinary desired-vs-observed row.

Why the pin travels **both** ways (D4): the archive chain (build-id → archive → pin)
has links that break exactly where fleet-scale mistakes live — the build that skipped
the pipeline, the `archive.sh` that didn't run, the stale entry. The in-image pin is
the provenance that survives a broken pipeline, and disagreement among node-pin,
archive-pin and served-hash **localizes** the fault: image vs archive vs deployment.

Serving (D11): `httpsrv` gains `GET /manifest` — file bytes plus hash — on the surface
that already outlived v1 behind a one-method interface. Wire coupling, not code import;
the repo file is the authored source, pushed to porta at deploy (input 2).

---

## 7. Document edits

- **`PROTOCOL-NSL-RECORDS.md`**: each catalogue row names its manifest rule; the
  document keeps the why — the size law, every measurement, the history. Its **§5
  (Evolution) is rewritten** for the fleet it actually governs: a field exists when
  `MANIFEST.cddl` says so; gates validate closed; additive-vs-breaking becomes a
  manifest-diff question; rules 3–4 (consumer tolerance for rolling upgrades) are
  retired with a note, since both ends reflash together. Optional fields keep their
  absence semantics.
- **`PROTOCOL-NSL.md`**: a pointer to the manifest from §§1–4; no rewrite.
- **Authority (D8)**: when manifest and prose disagree, that is a finding to resolve —
  but the machine facts (path, kind, class, dir, shape) default to the manifest,
  because the manifest is what the gates enforce; prose that disagrees with it
  documents a system that does not exist.
- **tuvm#159**: the manifest records `diag/spans/ctl` as `dir=up` (today's truth — its
  only writers are node-local); widening porta's authority remains #159's decision to
  make, and making it would be a one-line manifest edit plus the gates re-running.

---

## 8. Not doing, and what would earn each back

| deferred | earns it back |
|---|---|
| code generation from the manifest | a third hand-maintained copy of any shape |
| deep-shape validation at ingest | a malformed value that passed both zcbor gates and head-bytes — i.e. evidence the current depth misses something real |
| deep wildcards / precedence | a catalogue row that cannot be written segment-wise, or a genuine literal-under-wildcard specialization |
| semantic (comment-stripped) pin hash | comment-only edits re-pinning at a measured, painful rate |
| a manifest version field | never — the hash is the version; a version integer only says "you cannot understand this", which the hash already says exactly |
| node-runtime validation | never, by input 4 — the node carries nothing extra |

---

## 9. Testing the gates themselves

- The lint and the ingest table get unit tests in their own languages (Python beside
  the lint; `go test` for the `Store.Write` checks, including the four refusal reasons
  and the acked-seq behaviour).
- The **negative controls are part of every gate**: a bogus key, a wrong kind, a wrong
  record name, a wrong direction each must go red — a validation gate that has never
  been seen to fail is the false-green shape this program keeps re-learning.
- `porta_sync_host.sh` (real node against real gateway) is the seam test: it must stay
  green with the manifest loaded, and a deliberately mis-kinded write must appear as an
  `actor=gate` event row.

## Addendum — where implementation refined this design (2026-08-25)

Recorded per the doc-agreement posture: the design said one thing, the code or the
tools said another, and these are the resolutions. None reverses a ratified decision;
each narrows one.

1. **The gate's seat is a shared `Check`, not `Store.Write`.** The inbound door was
   never `Write` — node pushes land in `Store.Ingest` — so D6's "one seat" is one
   checker (`internal/manifest`) called from `Write`, `WriteGateway` and `Ingest`.
2. **`dir` gained `gw`.** The store has a third, gateway-authored direction
   (`WriteGateway`, `gw/liveness`) the design never mentioned; a closed manifest must
   name it or refuse porta's own liveness verdicts. There is no `both`: `dbg/req` is
   down, `dbg/resp` is up.
3. **Literal-shadows-wildcard was earned immediately.** §8 deferred precedence until "a
   genuine literal-under-wildcard specialization"; the existing catalogue already
   contains one — `diag/spans/ctl` and `diag/spans/result` under `diag/spans/<span>`.
   A literal entry shadows a wildcard entry; two overlapping *wildcards* remain a lint
   error, so no general precedence rule exists.
4. **Annotations stack.** `obs/X` reuses `goal/X`'s shape by design (and `LatencySpan`
   rides two paths), so several `;#` lines may bind one rule — §2.2's "exactly one
   annotation" became "at least one".
5. **Key order is wire order.** zcbor 0.9.1 validates map keys in CDDL-listed order
   (probed), and the wire sorts bytewise — so the manifest lists every record's keys
   exactly as they appear in the byte stream. A constraint that became a feature: the
   file now mirrors the bytes.
6. **zcbor cannot quantify a tagged list element** (`[* #6.39(tstr)]` → StopIteration,
   every quantifier form probed). `App.triggers`/`AppObs.triggers` stay `any` with the
   probe cited inline; tighten when zcbor can. The wildcard-keyed `arguments` map,
   flagged as the risk, validates fine.
7. **The manifest persists in the store** (a `meta` table; `serve --manifest` writes
   it, every `Open` re-arms from it). D6's gate must hold for `porta put --db` — a
   separate process, and the door both #180 and #206 came through — which in-process
   arming cannot reach. `PathLiveness`-style CLI reads of manifest-less stores are
   unchanged: no row, no gate.
8. **The gate's first real catch: `dbg/*` carries a bare line.** RECORDS §4.5's
   `DbgReq{line}`/`DbgResp{line}` records never rode the wire — the node parses the
   raw value as a string and the CLI writes one. The manifest says `dbg-line = tstr`
   and the catalogue now records the correction. One session-neighbouring find rode
   along: the seam script's `porta get | grep -q` under pipefail turns an early match
   into a SIGPIPE failure — a latent false-RED the slightly slower store `Open`
   exposed.

Two items remain **board-unverified** (the first C6 flash witnesses both): the
`gen_manifest_pin.cmake` hook inside a real `west build`, and prim 156 answering the
baked pin (host answers nil by design, so no host gate can see the non-nil arm).

## Close-out promotion

This rung is not closed until: the manifest and its lint are the source of truth in
`protocol/`, `RECORDS.md` §5 and the row→rule links are landed, and this document's
durable rules are absorbed by `protocol/PROTOCOL-NSL.md` (the pointer),
`protocol/PROTOCOL-NSL-RECORDS.md` (the rewritten §5) and `porta/docs/ARCHITECTURE.md`
(the ingest gate and `GET /manifest`). This dated design stays behind as the why.
