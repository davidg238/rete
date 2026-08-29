# rete — project instructions

`rete` is one repository holding both halves of a two-sided embedded fleet system and the
wire contract between them. It was consolidated on 2026-08-02 from four repos — `tuvm`,
`nsl-tuvm`, `nsl-tests` and `porta` — by restructuring `tuvm` in place, which is what
preserved the issue numbers and the `#NN` references the program navigates by
(#79; design `docs/design/2026-08-02-rete-repo-consolidation-design.md`).

## Current focus

- **Focus issue:** #79 (go-to-public) — **the tutorial ladder is COMPLETE.** 1 and 3 are executed by the suite; 2 is prose (a board cannot be gated on a host) with its one checkable claim gated as `tutorial-board`. The next rung is the **v2 rewrites in #279**.
- **First concrete action:** read #279 and pick its first page. The v1-era docs it covers are `porta/docs/CLI.md`, `DEVSDK.md`, `GETTING-STARTED.md` and `porta/docs/ARCHITECTURE.md` (stale in parts — written for v1's command protocol and TFTP transport). Tutorial 3 is the model for what "current" reads like, and `porta/docs/README.md` is the map.
- **Done 2026-08-29, do not redo:** docs passes 1–4 (`ab097fc`, `10255ac`, `df7912a`, `9462ca4`, `2abde14`, `fdae4de`). Pass 4 = **tutorial 2 (`docs/tutorials/02-board.md`)**, prose with a banner saying why, every transcript copied from a dated bench log, including a real `bridge-health` run on `c6bench` with its full 180 s observation window — driven by hand over #181's path, because **the target harness is single-host and the toolchain and board were on different machines**. Plus the **`tutorial-board`** gate (§7's eleven scenario names vs `tests/target/*.env`, names only, falsified three ways including vacancy). Suite: 278/0, all three tutorial gates PASS, no SKIPs.
- **Also 2026-08-29 — a 48-day soak was found and banked:** three nRF52840s have echoed over Thread to the gateway since 2026-07-11 (720,878 exchanges) with the record living **only** in `journalctl -u udp-echo6` on gw85224-01. Now `docs/results/2026-08-29-thread-echo-soak.md`. The 2026-08-18 double failure has four hypotheses closed by evidence (firmware, credentials, RSSI, shared power) and **the cause recorded as UNKNOWN, gateway side leading** — including a retraction of that session's own "it was power". All three nRFs are re-powered, healthy and **physically labelled `7adf` / `4ec0` / `92c2`**. Filed: **#281** (P1 — the nRF nodes produce zero rows in porta, so `gw/liveness` never sees them), **#282** (P3 — `porta` has no systemd unit; OTBR logs die with the container). Earlier finds still open: **#280**, and **#274** is TRACED not fixed (fix shape on the issue: bridge with `socket: nil`, `liveness:` + `startTicking:` unconditionally; same in `ShipsBellAlarmNodeC6`; update `observer_core.h:92-96`; board-owed after: `liveness-heartbeat` once WITHOUT `CONF_EXTRA`). Nothing in the liveness thread is board-owed.
- **Bench state:** `c6spare` is **UNPOWERED** (its cable went to gw85224-01 on 2026-08-29) — it is `TARGET_DEFAULT_BOARD`, so **no `class=disruptive` scenario can run until it is back**. `c6bench` on gw85224-01 has both cables and carries a `PortaBridgeNodeC6` build (`g9462ca4`).
- **Awaiting David:** the #273 design call (which deadline number is the source of truth) — the only thing blocking #272's age rule in `verdictFor`. #277 (three `review/` files cite the deleted `run_c_tests.sh`; `review/` is not swept by me). Whether `review/tests/CLAIMS.md` should absorb the soak evidence — that tree is his.
- **Not before 2026-09-01:** David is on NEMT until then.

## Where truth lives (do not duplicate across channels)

| Kind | Home | Never in |
|---|---|---|
| Open work / status / bugs | GitHub issues, bands `P0`–`P4`, pinned tracker **#26** | memory, chat |
| What is top of mind right now | `## Current focus` above + `docs/status/dev-status.html` (live at `http://gw85224-01:6971/dev/status/dev-status.html`; refresh = edit + scp, recipe in `.claude/commands/conclude.md`) | memory |
| Direction | `docs/next-steps-overview.md`, `review/program-sequence.md` | dated design docs |
| Design decisions | `docs/design/` (porta: `porta/docs/design/`) | memory, chat |
| Session-to-session state | git + GitHub + this block; sessions end with `/conclude` | memory |

Memory store = David's working preferences and gotchas ONLY. "Save this for next session" means: update the issue, the doc, or the block above. **Session shape:** one focus issue per session; anything else found is filed P3 `found-during #N` and left for the weekly review.

## Tree map

```
porta/       the orchestrator (Go) — the northbound half
nodus/       the device end
  oevm/      the VM (C) + the Zephyr board ports
  nsl/       the dialect, its compiler and the kernel
  tests/     the nsl suite
protocol/    the wire contract both sides answer to (v1 frozen under protocol/v1/)
review/      David's review-agent tree: the program ladder and the claims ledger
docs/        the VM's own documents, the design records and the execution plans
tools/       repo-wide scripts (docmap.sh)
```

**Being in one repo does not couple the two halves.** porta and the nodes are coupled
**only** over the wire; they never import each other's code, and `protocol/` is the whole
of the contract. A change that makes one side compile against the other is a bug, not a
convenience.

## Standing rules

- **Never create a GitHub repo named `tuvm`.** The repo was renamed `tuvm` → `rete`, and
  GitHub serves the old name as a redirect. Creating anything called `tuvm` again silently
  takes that redirect down, and with it every old issue URL and every clone URL in the
  historical record. There is no warning when this happens.
- **`review/` is David's review-agent tree — never sweep, move, tidy, reorder or delete
  files there.** Path fixes forced by a restructure are the one exception, and only with
  explicit permission (granted 2026-08-02 for #79).
- **Cite absolute paths** when referencing documents in conversation
  (e.g. `~/workspace/rete/review/program-sequence.md`, never a bare `review/`) — the
  program still spans two workspace roots, because `~/workspaceToit/stic` (the cross-VM
  benchmark harness) stays outside `rete` on purpose.
- Cross-repo document map: `review/DOCMAP.md` (regenerate with `tools/docmap.sh`).
- Content search across the program:

  ```
  rg -i "<term>" ~/workspace/rete ~/workspaceToit/stic
  ```

  Add `~/workspaceToit/nodus` to reach the frozen Toit v1 implementation (the GitHub repo
  is now `nodus-v1`; the local directory keeps its old name).

## Design posture (David, 2026-08-04)

How to make decisions here. These override the instinct to be thorough.

- **This is an evolving story, so expect the documents to contradict each other.** When
  two documents of record disagree, that is a finding, not an error to route around —
  resolve it and make them agree. Do not preserve both readings, and do not implement to
  whichever one you happened to read.
- **Resolve for simplicity, robustness and expressiveness, with a dash of elegance.**
  In that order. See also the standing preference for **mechanism minimization**: fewer
  distinct mechanisms and API symmetry beat convenience or low blast radius, and
  call-site churn is an acceptable price.
- **There is ZERO need to maintain backward compatibility, and no "fleet" to protect.**
  Both ends of the wire are reflashed together. A shape change is an edit to the
  documents and a rebuild — never a versioned field, a dual-read, or a deprecation
  window. This is what makes corrections possible instead of accretions: the usual way a
  record grows wrong is that each field was added *beside* what was there rather than
  *instead of* it.
- **Prefer progress, spikes and data over complex decisions made early.** Limit scope and
  get something working. **Do not invent tunables before there is data to choose them
  from** — if a design needs N invented constants before it can run once, ship the
  degenerate case whose cost can be measured, and let the measurement earn the knobs
  back. Record what would earn them, so the deferral is falsifiable rather than vague.
- **Remember the lineage: `st/berry` → `ns/berry` → `ns/oevm` → `nsl/tuvm` (now `rete`, the VM at `nodus/oevm`).** This system
  has been rewritten wholesale four times to get here. Nothing is built to last; it is
  built to teach. Design accordingly.

## Docs convention (2026-07-14)

- **Design specs are decision records** and live in `docs/design/` (moved from
  `docs/superpowers/specs/` on 2026-07-14). When a superpowers skill (e.g. brainstorming)
  says to save a design to `docs/superpowers/specs/`, save it to `docs/design/` instead —
  this instruction overrides the skill's default path. porta's own designs live in
  `porta/docs/design/` under the same rule.
- **Implementation plans** stay in `docs/superpowers/plans/` (porta's in
  `porta/docs/superpowers/plans/`) — they are execution scaffolding, historical once the
  work closes.
- **A document leaves the mainline by move + banner, never by deletion and never by a bare
  move** (2026-08-29, #79). Superseded documents go to the `archive/` of the subsystem that
  owns them (`nodus/oevm/docs/archive/`, `porta/docs/archive/`) with a
  `> **HISTORICAL — archived DATE.**` first line naming what replaced them and what was
  deleted since; then rewrite the in-tree citations to the new path (`review/` excepted —
  list those for David) and rerun `tools/docmap.sh`. Kept rather than deleted so that `#NN`
  issues and commits still resolve by path; the banner is what stops an archived spec being
  read as current, which is how `spec.md` described NLR for seven weeks after Milestone A
  deleted it.
- **Reference docs live with their subsystem; dated records live at the program level**
  (the 2026-08-02 consolidation's rule, executed 2026-08-29): `nodus/oevm/docs/` (start at
  `vm.md`), `nodus/nsl/docs/`, `porta/docs/`, each with a `README.md` map; the root `docs/`
  holds tutorials, status, `next-steps-overview.md`, and the dated `design/`, `results/`,
  `spikes/`, `superpowers/plans/` — which stay under their date because that is where the
  issue citations point. `docs/README.md` is the map of the whole.
- **Promotion rule — part of every close-out.** An issue / rung / milestone is not closed
  until its durable knowledge is promoted to a mainline document of record: the ladder
  (`review/program-sequence.md`), `review/tests/CLAIMS.md`, the owning doc under
  `review/lib/`, or — for porta — `protocol/PROTOCOL-NSL*.md` and
  `porta/docs/ARCHITECTURE.md`. The dated design doc stays behind as the historical "why".
  Every plan's close-out task must name which mainline doc absorbed the knowledge.

## Gates

```
cd nodus/nsl && python3 tools/run_tests_oevm.py     # 278 passed, 0 failed (2026-08-27)
cd porta      && go test ./...
cd nodus/oevm && bash tests/porta_sync_host.sh      # PORTA SYNC OK
```

The nsl suite prints `Passed: N / Failed: 0` **and** a line of aux gates — read the aux
line, because it can be red while the count reads zero failures.
**`c-units` leads that aux line and covers ALL 43 C unit tests** (#275, 2026-08-29):
`nodus/oevm/tools/run_c_tests.sh` is DELETED — it was a second aggregate runner nothing
invoked, so it sat red (four aborts) without anyone learning it, which is #261's shape.
Every C test is now registered individually in `nodus/nsl/tools/run_tests_oevm.py`'s
`C_UNIT_TESTS` table, which also owns the `ENG_A`/`ENG_B` link sets and the
`OEVM_BUILD=asan|ubsan` knob. Adding a C test is one row in one file; a test registered in
two places is one that gets forgotten in one of them. The registry runs every test and
reports every failure rather than stopping at the first.
**`tutorial-host`, `tutorial-porta` and `tutorial-board` close that aux line** (#79, 2026-08-29):
`docs/tutorials/gate.sh` extracts every `bash gate` block from a page and runs them in a
fresh directory, canary first. A tutorial is the one document nobody who knows the system
re-reads, so it is executed instead — edit the page and the gate follows; a new tutorial is
one more call to `run_tutorial_gate(page, label)`. **`tutorial-porta` (03-porta.md, ~12 s)
runs both halves of the repo against each other**: a real `porta serve` and a real
`PortaBridgeNode` image on loopback. It therefore binds **udp 16969/16970** — the node's
`gwPort` is a source constant — so it must never run beside
`nodus/oevm/tests/porta_sync_host.sh`, which claims the same pair (that gate is standalone,
so the suite does not collide; two concurrent suites would). It is the ONE gate here that
needs a Go toolchain, and `needs=("go",)` makes an absent `go` a named SKIP rather than a
hard failure, because a missing Go must not couple the nsl suite to the gateway half.
**`tutorial-board` is the odd one and deliberately so**: `docs/tutorials/02-board.md` is PROSE
— its walkthrough needs a devkit, a toolchain and a mesh, so it carries no `bash gate` blocks
and its banner says why. The gate checks the ONE claim on it that is a fact about this repo
rather than about a board: §7's scenario NAMES against `nodus/oevm/tests/target/*.env`. Names
only — a missing scenario is a bug, a stale description is a judgement call, and a gate that
scores prose is one that gets disabled. An unparseable table yields the empty set and FAILS,
because vacancy must not read as agreement.
**`porta-cli` closes it after them** (#279, 2026-08-29): `porta/docs/cli-gate.sh` builds
porta, walks its `--help` tree, and fails if `porta/docs/CLI.md` names a command or flag the
binary does not have, or omits one it does. The tutorial answer — execute the page — is not
available to a REFERENCE, whose claims nobody runs; cobra's own account of itself is. Names
only, both directions, transcripts included; it falsifies itself on an empty page and on an
invented flag before it clears anything. Needs Go, so `needs`-style SKIP applies for
tutorial-porta's reason.
**`footprint: SKIP` is NO LONGER expected, and it never should have been** (#177, fixed
2026-08-22): arm-zephyr-eabi-gcc ships inside `~/zephyr-sdk-0.17.4` and always did. What was
broken was the gate's own un-SKIP instruction, which named a path the #79 consolidation
deleted — so the SKIP was unfollowable, and this file recorded it as normal. `footprint.sh`
now finds the SDK itself. **A SKIP line here is a finding now, not a fixture.** Never run
two nsl suites concurrently; they share `/tmp` paths. **The count above drifts upward as
tests land — treat a mismatch as staleness in this file, never as a missing test.** The
suite also prints `target gates: NOT RUN BY THIS SUITE`: the posix timer backend bypasses
the pulse mailbox, so coalescing, shedding and stale handles need `nodus/oevm/tests/target/run.sh`
and a board.

**What `tests/target/run.sh` actually runs — eleven scenarios, and a full pass is ~60–90 min
of board time** (`liveness-heartbeat` alone budgets a 300 s timeout plus 180 s of observation;
`cad-rearm` was added 2026-08-26 for #240.2, `porta-refuse` the same day for #232, and
`porta-silent` on 2026-08-27 for #269 — and **both porta-* scenarios need their injector porta
started by hand**, so a full sweep is not unattended; see their envs). The runner globs
`*.env`, so a new manifest joins the sweep the moment it lands. Budget the time rather than
discovering it. `CLASS=disruptive` means the scenario
deliberately wedges, burns or reboots the node: **a disposable board only** (`c6spare`, which
is `TARGET_DEFAULT_BOARD` — resolve boards through the collector env files, never a table).

**A scenario whose node opens a socket needs `CONF_EXTRA` naming the radio, and only two nodes
do.** `prj.conf` alone has `NETWORKING/IPV6/UDP/SOCKETS` but **no L2**, so a C6 built without
`prj_esp32c6_thread.conf` has no interface and `net udpBindPort:` answers nil. The wedge probes
never notice — `WedgeProbeC6`, `SlowTimerNodeC6`, `PulseCoalesceProbeC6` and `StaleHandleProbeC6`
arm `timers every:` directly and open nothing. `PortaBridgeNodeC6` and `TimerlessNodeC6` bind, and
for `TimerlessNodeC6` the bind is load-bearing: the base tick is registered by the **bridge**, so
no socket means no timer, no wedge, and no deadline for the detector. The "no `CONF_EXTRA`"
convention is about `wedge-note`'s synthetic-trigger Kconfig split and **was never about the
radio** — conflating the two cost a board cycle on 2026-08-22 (#236, and `zephyr/run.sh`'s #165
comment records the same shape from before).

| scenario | class | what it gates |
|---|---|---|
| `bridge-health` | observe | the node half of the porta gate — obs acked, `goal/name` converged, nothing evicted (#streamDropped 0) |
| `cad-rearm` | observe | an adopted cadence is a KEPT cadence (#240.2) — the re-arm from inside the old tick's own reaction kills the old registration over the real pulse mailbox, keeps ONE registration, and holds the new period. No radio: `PortaBridge socket:nil` |
| `bridge-forensics` | observe | the same workload under the forensics overlay (#222) |
| `porta-refuse` | disruptive | **a refused node survives past the 157 s death record** (#232, gating #199) — a C6 against a second porta instance answering `#err` to everything (`--test-refuse-all`, gw85224-01:16971, started by hand per the env), watched 240 s past the TAPs. `ref` climbing is the PASS; silence is the FAIL. The host cannot answer this: it never reproduced the death |
| `porta-silent` | disruptive | **a node whose UPLINK is gone survives it** (#269) — porta-refuse's mirror, and the gate porta-refuse could not be. A C6 against a second porta instance that DROPS every datagram and answers nothing (`--test-drop-all`, gw85224-01:16973, started by hand per the env — and it must be `./porta-269`, since the long-running `./porta` there predates the flag), watched 300 s past the TAPs. **`misses` climbing with the node ALIVE is the PASS**; the node going quiet is the FAIL — and TAP 1 is a conjunction (`misses >= 30` AND `ref = 0`), so a run that reaches #232's refusing injector by mistake fails loudly instead of scoring. **This gate caught something it was not built for**: it reproduced a node wedging at 111.7 s under a silent uplink, and the node was HEALTHY — `observer.c` judged the pulse mailbox's depth at a sampling instant, which aliases against a 4.007 s tick sampled every 1001 ms, so the detector was rebooting a node that was still draining and still printing ticks. Fixed by judging PROGRESS (`pulse_drained()`) not depth; GREEN since, 357 s under the condition that had killed it, with `wedge-detect` still firing. The archive was then swept (#270, 2026-08-27) and **the false trips are confined to that one session**: of the 58 archived `deadline=16000ms` events, 49 are REAL wedges on 08-19/20 — boot, ticks 1-6, then 18.2 s of silence against a 2 s cadence, and 35 of those also read `reactor=NOT-ADVANCING` with vm_sp frozen, which is the #222 prim-cache spin and does not depend on depth at all — 2 are `liveness-heartbeat`'s deliberate ones (#231 D1), and 7 are this session's. No attribution was overturned |
| `pulse-coalesce` | disruptive | the coalescer does not destroy logical timer events (#151) |
| `stale-handle` | disruptive | a timer event survives a later unrelated registration (#151) |
| `wedge-note` | disruptive | the `RebootNote` survives a watchdog reset (#215, row 1) |
| `wedge-detect` | disruptive | the detector fires on a real wedge, dumps the condition, reboots, reports `#watchdog` (#215, row 3) |
| `wedge-quiet` | disruptive | **no false trip on a legitimately slow node** — one 10-minute timer, and the 600 s observation is why this run is long (#215, row 5) |
| `liveness-heartbeat` | disruptive | **the detector can see a node with NO app timer** — the kernel owns the base tick, so the deadline exists and nothing speaks between the wedge and the reboot (#231 D1). Needs the Thread `CONF_EXTRA`: its node binds. **GREEN ON A BOARD 2026-08-22** — `deadline=16000ms` (the kernel's 2000 ms tick × slack 8, the only timer in the image), 18.03 s of total console silence between the wedge and the reset, `pass=2 fail=0` on boot 2 |

A scenario passing its TAPs is **not** the verdict: every one carries an observation window
that watches past it, because a gate that scores at tick 2 against a node that dies at tick 6
is a false green (#216).

---

# porta — the gateway half

`porta` (Latin "gate/door") is the **gateway project**. It is the northbound half of a
two-sided system whose unit of coupling is a **space**: every node owns a tree of paths,
and porta holds a mirror of that tree. Operators write intent under `goal/**`; the node
writes what it sees under `obs/**`, `tel/**` and `sys/**`. Convergence is not a mechanism —
`goal/X` versus `obs/X` is a tree diff, so the desired-vs-observed view and the self-heal
loop are the same query.

The two sides exchange the mirror over **plain UDP carrying CBOR**, in two node-initiated
idempotent messages: a `pull` that fetches writes porta has for the node, and a `push` that
delivers the node's entries. Both are bounded by a 512 B batch and a 256 B value, which are
radio constraints and not preferences (6LoWPAN fragments above ~102 B and a datagram
survives only if every fragment does).

There are **no verbs on the wire and none in the store.** The operator vocabulary is CLI
sugar over path writes and tree reads: `porta put` is a write to a `goal/` path, `porta get`
is a tree read, `porta log` is a query over the append-only event log.

> **The porta auto-memory namespace (`~/.claude/projects/-home-david-workspace-porta/memory/`)
> is v1-era and has NOT been reviewed against the rewrite.** It holds memories written
> against the command queue, TFTP delivery and the report schema — all deleted. Treat
> anything from it as a claim about a system that no longer exists until you verify it
> against the code. The memories this work depends on live in the **tuvm** namespace
> (`nsl-cbor-wire-shapes`, `rewrite-may-not-delete-a-capability`), which is the namespace
> `~/workspace/rete` sessions get.

## Status

v1 was deleted in one commit (design D7: demolition precedes construction, so no decision
is made with v1 still in the room). The last v1 tree is tagged **`porta-v1-final`**.

What v1 was — a command vocabulary and report schema, a command queue, container image
delivery over TFTP, and telemetry ingest — is gone, along with nine per-feature sqlite
tables. The **capability audit in
`porta/docs/design/2026-08-01-porta-space-sync-rewrite-design.md` §4.2 gives every v1 verb,
route and MCP tool a fate**, because a rewrite may delete a mechanism but may not silently
delete a capability. Three of v1's fourteen `nodes` columns turned out to have no successor
until they were enumerated; all three are closed.

The core is built and green: `cval`, `space` and `wire` all landed
(`porta/docs/superpowers/plans/2026-08-01-porta-space-sync-core.md`, DONE), the node side
landed (`docs/superpowers/plans/2026-08-02-porta-space-sync-node.md`, DONE), and the C6
hardware gate closed the rung 5/5. See `docs/next-steps-overview.md` Part 2 for what is
next.

## Layout

- `porta/cmd/porta/` + `porta/internal/portacli/` — the cobra CLI (`serve`, `get`, `put`,
  `log`). The operator verbs are sugar over path writes and tree reads.
- `porta/internal/cval/` — the nsl message algebra's CBOR codec, **hand-rolled**. Not
  layered on a CBOR library, and this is not a preference: the algebra's map keys are
  themselves tagged values (Symbols, tag 39), a Record's *name* is a Symbol rather than a
  text string, and decoding must be **closed** so a mis-versioned peer cannot smuggle a
  value through. The node's `Cbor` class in `nodus/nsl/lib/kernel.nsl` is the authority,
  not the RFC; the encoder is golden-tested against the node's own output.
- `porta/internal/space/` — the store: `nodes`, `cells` (latest value per path), `events`
  (append-only, both directions, with `actor`/`reason` provenance), `exchanges` (one row
  per request — two per check-in, a `pull` and a `push`. **There is no `leg` column**: it
  was dropped with the cadence inference rete#231 D4 deleted, which was its only reader
  (and which had to count pulls alone or infer a period half the node's real one, #217
  C1). What the table is for now — the replay index and the `last_seen` heartbeat — wants
  every request rather than one leg, and **both** legs stamp `nodes.last_seen_ms` via
  `RecordExchange`). Four tables where v1 had nine, because the unit of
  extension is now a path: a new feature adds rows, not tables.
- `porta/internal/wire/` — pull/push framing as a pure `Handle([]byte) []byte`, wrapped by
  a thin UDP loop.
- `porta/internal/httpsrv/`, `porta/internal/serverstat/` — survived the demolition
  untouched. `httpsrv` now depends on a one-method `NodeCounter` interface rather than a
  concrete store, which is how it outlived v1's store and should outlive the next one.
- `porta/devsdk/` — the public dev-SDK (`exec`, `provision`; `apiclient` was deleted with
  v1's HTTP surface). All language/hardware toolchain lives on the node side, which imports
  `devsdk`; porta itself stays language-neutral.
- `porta/deploy/` — the deb build and systemd unit.

## Documents of record

- `protocol/PROTOCOL-NSL.md` — the current wire protocol: the space model, path namespaces,
  cells vs streams, the exchange. **§§1–4 are the conformance target.**
- `protocol/PROTOCOL-NSL-RECORDS.md` — the record catalogue: what shape rides at each pinned
  path, plus the size law (§2.1/§2.2).
- `protocol/MANIFEST.cddl` — **the machine-normative half of the catalogue** (#243,
  2026-08-25): path → kind + class + dir + shape, enforced by the nsl suite's
  `nsl-tools-py` aux gate (formerly `manifest-py`), porta's store (all three write doors), and the seam gate.
  Editing it has hard zcbor rules — see the `manifest-cddl-editing-rules` memory or the
  file's own header.
- `porta/docs/design/2026-08-01-porta-space-sync-rewrite-design.md` — the rewrite's decision
  record (D1–D8), the store schema, the wire shapes, and the §4.2 capability audit.
- `porta/docs/ARCHITECTURE.md` — system architecture. **Rewritten for v2 2026-08-29**
  (#279) and current: the three planes, both UDP transports, modes and app lifecycle as
  paths, the manifest gate, the store's two open doors, one-repo topology. §7 records the
  Toit lineage as history.
- `protocol/v1/PROTOCOL.md` — **v1. Deleted, historical only.** It describes the command
  verbs, JSON args and CRC32 payload delivery that no longer exist. Do not implement from
  it; read it only to understand what `porta-v1-final` did, and it is what the frozen Toit
  nodes still speak.
- `porta/docs/CLI.md` — **the v2 command reference and current** (rewritten 2026-08-29,
  #279). Gated: `porta/docs/cli-gate.sh` walks the binary's own `--help` and fails if a
  command or flag named on the page does not exist, or if one that exists is unnamed.
  Names only — it does not score the prose, and the transcripts on it are dated evidence.
- `porta/docs/DEVSDK.md`, `porta/docs/GETTING-STARTED.md` — **stale in parts**, written
  against v1's verb set. GETTING-STARTED §1–§2/§5 (the `.deb`, the unit, `/health`) are
  current; §3–§4 are not.

## Key decisions & constraints (don't re-litigate)

- **Verbs live only at the CLI.** The store and the wire have none (D2). Adding a feature
  adds a path, not a table, a verb or an endpoint.
- **All values on the wire and in the store are raw deterministic CBOR** (D4.1); decode only
  for display. All timestamps are **milliseconds** (D4.2). All paths are **node-relative**
  (D4.5) — the `node/<id>/**` prefix is a view, never storage, so a captured session can be
  re-pointed at a different node.
- **One monotonic seq per space per side** (D5). Kind is a property of the leaf, set by the
  first write (D6); `KindStream = 1`, `KindCell = 2`, matching `SPACE_KIND_*` in
  `nodus/oevm/src/space.h` — shared with the VM, do not renumber.
- **The integer band is ±(2^64−1)**, which exceeds `int64`. Represent out-of-range values
  exactly; never silently clamp.
- **There is no tombstone.** Stopped state is written explicitly as a value; genuine removal
  is an action on a `do/` stream.
- **A rewrite may delete a mechanism, but it may not silently delete a capability** (D8).
  Anything the old system could answer that the new one cannot is a regression wearing the
  costume of a simplification. This is why demolition was preceded by an audit.
- **Replay-from-0 is designed behaviour, and since 2026-08-26 it is safe per kind.** Cold
  boot = cursor 0 replays cells as latest-value (idempotent); streams are guarded — porta
  never re-serves a row the node has acked as applied (`nodes.cursor_hw`, bounded by
  `served_hw`), so `goal/do/*` cannot re-execute across reboots. See
  `porta/docs/design/2026-08-26-space-sync-durability-trio-design.md` and
  `PROTOCOL-NSL.md` §3.
- Bulk transfer (images, profiler blobs) is **deliberately off the space** and deferred to a
  CoAP blockwise plane; auth (G5) is out of this rung.
- Go 1.26, sqlite via `github.com/mattn/go-sqlite3`, cobra. Copyright header
  `// Copyright (c) 2026 Ekorau LLC` on every new Go file. `gofmt -l .` must print nothing
  before every commit.
