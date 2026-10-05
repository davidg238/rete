# custos as the third peer: the nsl developer's tool

**DRAFT, being iterated with David, 2026-10-04. Nothing built.** Edit this document in
place until it is right, then promote it (§7) and leave this as the historical record.
On adoption it replaces the custos paragraph of `porta/docs/ARCHITECTURE.md` §3 and the
dev-machine half of §7. The gatekeeper first-cut design
(`porta/docs/design/2026-08-22-gatekeeper-first-cut-design.md`) stays the record of
custos's query engine.

---

## 1. Decided (David, 2026-10-04)

- **D1 — Three peers.** rete holds `porta` (plumbing), `nodus` (the device end) and
  `custos` (the developer's tool), at the top level. They are coupled only through
  written contracts, never by importing each other's code.
- **D2 — porta is language-neutral; custos is nsl-specific.** A Toit equivalent of custos
  would be a separate tool with its own name, likely in its own repo, depending on rete
  only for `protocol/`. Nothing stops porta from serving Toit nodes.
- **D3 — Topology.** porta runs on a small NUC that is always on and always collecting.
  custos runs on the dev machine and is started and stopped daily. custos talks to porta
  over Tailscale, which is the security perimeter: authentication stays unaddressed for
  now (gatekeeper design, 2026-10-04 addendum). porta talks to nodes over weak,
  low-speed networks (Thread, WiFi).
- **D4 — Local first, then over the air.** Development starts with a USB cable from the
  dev machine to the target and later moves to over-the-air updates **of the nsl
  program**. Firmware over the air is deferred.
- **D5 — Fronts, in order:** a command line, then a web page, then a TUI in the Omarchy
  style. All three serve the first hour (`docs/overview.md`) and its promise of a better
  way to do embedded development: debugging, profiling and patching a running method,
  on the device.
- **D6: porta has an HTTP interface, and at least two clients use it:** a network
  monitor and custos. custos may depend on the monitor. In a simple home network custos
  is enough by itself, but a larger network would have monitoring as a separate
  function. ARCHITECTURE §7's "no network path from a dev tool to porta" is out of date,
  not a constraint.
- **D7: shared code is a dependency, never a copy.** The wire-contract code custos needs
  (the decoder for CBOR values, the manifest table, the staleness rule) is one
  implementation that both porta and custos depend on (§5 F2).
- **D8: custos does not serve the status page in future.**
- **D9: the showcase demo is remote debugging of a sleepy node.** Set a breakpoint on a
  sleepy node. Two hours later the node wakes, the breakpoint is hit, and the debugger
  becomes active. The node then most likely stays awake while the debugger steps (§3).
- **D10 (David, 2026-10-05): porta is the long-term custodian of commissioned images.**
  The image a field node runs (A′) must still be resolvable years later, and A and A′
  drift apart while a bug is chased on both. Developer images are disposable: custos
  needs only the `.oimg` and its map beside the source, so USB development works without
  porta. A developer who wants a store runs the same porta locally; custos gets no store
  of its own. Commissioning/release is the gate between the two kinds; its shape is
  undecided (#312, `2026-10-05-custos-profile-over-usb.md`).

## 2. The picture

```
  DEV MACHINE (started daily)                    NUC (always on)
 ┌────────────────────────────────┐  Tailscale  ┌──────────────────┐  Thread / WiFi
 │ custos   CLI · web · TUI       │◄───────────►│ porta            │◄──(weak, slow)──► nodes
 │ nsl compiler · west · esptool  │  (northbound│ store · blobs    │   protocol/
 └───────────────┬────────────────┘   contract) │ sweep · exchange │
                 │ USB: the local loop          └──────────────────┘
          ┌──────▼──────┐
          │ target board│  (dbg:* lines, console, flash)
          └─────────────┘
```

Three contracts, two of them unwritten today:

| Contract | Between | State |
|---|---|---|
| `protocol/` | porta ↔ nodes; also what custos decodes | written |
| **image artifacts** (`.oimg`, `.stmap`, `methods.json`) | nsl compiler → custos | **unwritten**: defined only by the compiler's code |
| **porta's HTTP interface** | porta ↔ the monitor and custos (D6) | **unwritten**: today it is only `/health` and `/manifest` (§5 F1) |

## 3. One set of verbs; the target picks the transport

custos has a single set of verbs: `build`, `flash`, `load`, `run`, `log`, `debug`,
`profile`, and later `patch` (#313). The target decides the transport; the person never
picks it. Moving from local to remote changes the transport, not the UI.

| Verb | Local (USB) | Remote (via porta) |
|---|---|---|
| load the nsl program | #310: the program in its own flash region, written over the cable | program push into **the same region**; proven on the desktop (`host_ota`), inert on boards (#233) |
| debug | `dbg:*` lines on the console; works today (#311) | the same lines carried through the space; see the cost note below |
| profile | trigger and result rows both on the cable | rows need a way up; D4 of the profile design deliberately keeps them on the console |
| log | console | `tel/**` in the store |
| firmware | `run.sh --flash` | **deferred** (D4) |

- **Today the profile trigger travels through the space** (`porta put … dbg/req`)
  while its rows come back on the cable. The local verb should send the trigger down the
  cable as well, so the local loop never needs porta.
- **Remote debugging of a sleepy node (D9).** This fits porta's model as it stands:
  - A breakpoint is intent, so it goes under `goal/**`. porta holds it until the node
    next pulls, whether that is seconds or two hours away.
  - On a hit, the node reports the stop under `obs/**` and stays awake. The `held`
    state the liveness observer already respects (#311) is what keeps a held node from
    being reset.
  - Each step after that is one exchange with an awake node. How fast stepping feels is
    a measurement to make on Thread, not a reason to drop the demo.

  Questions this raises, none decided:
  - **Who is told.** custos runs only during the working day and may not be up when the
    breakpoint hits. porta holds the stop as state; the monitor, or custos when it next
    starts, shows a node held at a breakpoint.
  - **How long a held node waits for a debugger** before going back to sleep, to protect
    its battery. Do not invent a number. Make the simplest version, held until a
    debugger detaches, and measure the battery cost of a hold on a sleepy node. That
    measurement is what would justify a timeout.
  - **What the node ships at the stop:** enough for the first screen (the stack and the
    locals, #323) in one exchange, so the debugger can show something before the
    first step.

## 4. The fronts

- **CLI.** One command for the whole first hour. Each verb gives a verdict a machine can
  read, finishes within a fixed time, never prompts and never skips silently
  (`docs/development.md` §4). This absorbs #314, and #312's command lands here.
- **Web.** A page on localhost, served by custos on the dev machine. The profile page
  comes first because a profile is a finished result: a ranked table, plus the source
  with counts beside each line. The debug page comes second because it needs a live
  session: the source, the current line, the stack and the locals. #323 (stack and named
  values) is what makes that page worth having.
- **TUI.** The same verbs in Bubble Tea, written in Go like custos and in the Charm style
  Omarchy uses. Source and stepping go in one tiled pane, the log tail in another.
- **Who holds the cable.** Whichever front is running owns the cable while it runs. It
  stops and restarts the console collector around itself, which is what `dbg.py` does
  today. A shared cable daemon, with the collector as the one owner and every front a
  client, is earned only when two fronts need the same board **at once**. Until then it
  is not built.

## 5. What making custos a peer costs: the findings

**F1: DEFERRED to the remote stage (David, 2026-10-04: "focus on custos talking to the
node locally over USB; once that works, do it remote").** The local loop needs no porta
at all. The text below is kept for when the remote stage starts. The confusion it caused
came from one fact: **today's custos is two things.** It is a query engine plus fleet
pages over porta's store, which is the monitor (question 3), and the store-pulling
options below belong to that half. The new custos starts as the development half only.

**Kept for the remote stage: porta's HTTP interface (D6), and how its clients read through it.** Today custos
opens porta's sqlite read-only on the gateway host and runs there as a systemd unit
(#282). A custos on the dev machine reads through porta's HTTP interface instead, and so
does the monitor. The custos that runs on the NUC today, with its `/fleet`, `/node` and
`/q/liveness` pages, is the monitor in all but name. **Proposed:** that half becomes the
monitor, and custos keeps the development half.

- *Reads.* (a) **Pull a snapshot**: `porta snapshot`, then copy it over. Measured: 352 MB
  in 8.5 s over Tailscale; 2.46 GB would take about 60 s by extrapolation, **not
  measured**. History already works this way (the `snap` binding). (b) **Read routes on
  porta.** (c) **An incremental replica**: custos pulls the rows added since a given
  sequence number and keeps its own local store. That is one porta endpoint, every custos
  query runs locally, and a daily start just catches up. *Proposed:* (a) now for history,
  and a spike for (c). **Unverified:** whether porta's tables carry a sequence number
  that makes "since N" pulls exact.
- *Writes.* The `goal/**` puts, `blob put` for the program push, and `dbg/req` all need a
  route too. The minimal shape is porta's existing CLI verbs (`get`, `put`, `log`,
  `snapshot`, `blob put`) served over HTTP on the tailnet, with the same meaning as the
  CLI. That is one mechanism with two fronts, not a new API.

**F2: custos imports porta's internals.** `porta/internal/gate` and
`porta/internal/custos` import `porta/internal/cval` (decoding CBOR values), `manifest`
(`Table`) and `space` (`IsStale`, `PathLiveness`). Go's `internal` rule forbids this once
custos is its own module, and D1 forbids it anyway. All three are wire-contract
knowledge. **Decided (D7): one implementation, used as a dependency, and its home is a
Go module under `protocol/`** (David, 2026-10-04), holding `cval`, `manifest` and the staleness rule, which
porta, custos and the monitor all import. That keeps the rule in CLAUDE.md: the halves
never import each other, and all of them import the contract. It is also the
"dependency on rete for protocol" that a Toit tool in another repo would take (D2).
Size: about 1.1 k non-test lines in `cval` and `manifest`. `space` is about 4 k, but
only `IsStale` and `PathLiveness` move.

**F3: `devsdk` belongs to custos.** `porta/devsdk` has two parts: `exec`, a runner that
narrates build and flash commands, and `provision`, the contract for the gateway address
a node is flashed with. It exists for "node-repo tooling", and custos is now that
tooling. *Proposed:* `exec` moves to custos, and `provision`'s contract moves into
`protocol/`.

**F4: the status page leaves custos (D8).** `http://gw85224-01:6971/dev/…` is served by
the custos on the NUC today. Its future host is not decided; the monitor is the obvious
candidate if the monitor stays on the NUC.

**F5: the documents disagree about the 0.1 picture.** `docs/overview.md:16` and
`docs/img/setup-0.1.svg` put the gateway on the dev machine. *Proposed resolution:*
steps 1–8 of the first hour need no porta at all, because they run over the cable.
Step 9 needs a porta somewhere. The architecture never assumes porta and custos share a
machine, so the picture shows two machines; a newcomer may still run both on one box.

**F6: two kinds of bench cable.** `c6bench` is cabled to the NUC for soak runs and its
collector. Boards for the development loop are cabled to the dev machine (`c6spare`).
The collector design (`docs/design/2026-08-03-device-console-collector-design.md`)
should say which host each cable is on. Not checked.

**F7: language. Decided (David, 2026-10-04): rewrite in Go.** The compiler and `west`
stay as subprocesses, because they are toolchain. The two translators, source map →
breakpoint and profile rows → ranked table, are rewritten in Go inside custos, and the
Python versions are deleted (§6, answer 4).

## 6. Questions for David

Answered by David, 2026-10-04:

1–2. **Deferred.** custos talks to the node over USB first; remote, and with it every
   question about porta's HTTP interface, comes after the local loop works (F1).
3. **Deferred.** Most likely today's NUC-side custos (`/fleet`, `/node`, `/q`, the query
   engine) becomes a lightweight network monitor.
4. **Rewrite in Go** (F7). `dbg.py`, `mprof_resolve.py` and the `.stmap` reader (about
   700 lines) are rewritten inside custos and **deleted in the same change**, along with
   their tests and the suite's tutorial-debugger gate, which are ported. The deciding
   reason: the web page and the TUI are clients of one live debug session, and that
   session should live in one Go process. The cost accepted: the `.stmap` /
   `methods.json` format must be written down, because a Python compiler writes it and
   Go reads it.
5. **Yes**: the shared code is a Go module under `protocol/`.
6. **Yes**: custos gets its own Go module, and the move within the repo is one commit.

## 6a. The first slice: local, over USB (proposed order)

1. **Write the image-artifact contract**: what `.stmap` and `methods.json` contain, and
   the parts of `.oimg` that custos reads. *Proposed home:* `protocol/`, so that every
   contract between peers lives in one place. The compiler owns the writer, custos the
   reader.
   *Addendum 2026-10-04 (#337, `cf8121e`):* done as `protocol/IMAGE-ARTIFACTS.md`. The
   two sidecars became one `<name>.oimg.map.json` (offset → name, file, lines, plus the
   image's SHA-256); custos reads only the 20-byte `.oimg` prefix. The word width in
   `flags` follows #330 as format 12 (#344); a one-expression block's missing lines are
   #343 (P0, before step 3).
2. **The move, as one commit** (§6, answer 6):
   - top-level `custos/` as its own Go module;
   - a Go module under `protocol/` holding `cval`, `manifest`, `IsStale` and
     `PathLiveness`, imported by porta and custos;
   - a `go.work` file at the repo root.

   Today's custos (the query engine and the fleet pages) moves with it unchanged. It is
   split out as the monitor only when the monitor work starts (§6, answer 3). Branch
   *after* `198-custos-views` merges, which waits on `329-c6-speed`, so the move does not
   collide with unmerged work.
   *Addendum 2026-10-04 (#338):* done. `protocol/` is the Go module
   `github.com/davidg238/rete/protocol` with four packages: `cval`, `manifest`, `store`
   (`IsStale`, `PathLiveness` and `DirectionGateway`, the store's shared vocabulary) and
   `allowlist` (porta's `httpsrv/cidr.go`, which custos already shared "so the two
   surfaces cannot drift"; F2 missed it). Two corrections to this step as written:
   - **No `go.work`.** Each of `porta/go.mod` and `custos/go.mod` carries
     `replace github.com/davidg238/rete/protocol => ../protocol`. That alone builds, tests,
     tidies and packages the `.deb`; `go mod tidy` cannot run in a workspace member
     without it, and a `go.work` does not let `go test ./...` at the root span the
     modules either, so it would have been a second mechanism doing nothing. The gate is
     `for m in protocol porta custos; do (cd $m && go test ./...); done`.
   - **The store's schema is a contract too, and F2 missed it.** custos's tests built
     their stores with porta's `space.Store`, which neither Go's `internal` rule nor D1
     allows across the line. The builders moved to porta
     (`porta/internal/space/fixtures_test.go`), which writes them as SQL under
     `protocol/fixtures/store/` and fails if they drift (`-update` regenerates); custos
     loads the text (`custos/internal/storefixture`). This is the image fixture's shape
     (`protocol/fixtures/image/`) applied to the store. It also fixed a latent defect:
     #198's "committed" `acceptance.db` was gitignored (`/porta/**/*.db`), so a fresh
     clone failed `TestAcceptanceFixtureIsCurrent`.
3. **`custos debug` over USB**: `dbg.py` rewritten in Go; `test_dbg.py` and the
   tutorial-debugger gate ported; `dbg.py` deleted.
4. **`custos profile` over USB**: the trigger sent down the cable, the rows ranked by
   Go; `mprof_resolve.py` deleted. This closes #312's command half; its cost
   measurement on a real program is still owed.
   *Addendum 2026-10-05 (David, #312):* the trigger is **one command reader on the
   node, in every image**, fed by the cable line or by `dbg/req` through porta; the
   `dbg:*` verbs and `profile <ms>` share its grammar, so custos sends the same line to A
   on USB and to A′ over the air. Profiling does not need the debug image. Whether the
   profile counter goes in the standard image (two images: std, debug) or with the
   debugger (std, debug+profile) waits on its cost on a real workload, measured on
   `c6spare`.
5. **`custos load`**: #310, the nsl program in its own flash region.
6. **The web profile page, then the web debug page** (#323), then the TUI.

Then the remote stage: F1, porta's HTTP interface, and the sleepy-node demo (D9).

## 7. On adoption: what changes

- `CLAUDE.md`: the tree map and the "being in one repo" paragraph (three peers).
- `porta/docs/ARCHITECTURE.md`: the §1 diagram, the custos paragraph in §3, and §7.
- `porta/CLAUDE.md`: custos is no longer under porta.
- `docs/overview.md` and `docs/img/setup-0.1.svg` (F5); `docs/development.md` §5,
  which is already out of date on what custos serves.
- A new `custos/docs/ARCHITECTURE.md` takes over §§2–4 of this document.
- Issues: re-scope #314 as "custos's verbs"; split #233 into program and firmware
  (firmware deferred); file the northbound surface, the replica spike, the web profile
  page, the web debug page and the TUI.
