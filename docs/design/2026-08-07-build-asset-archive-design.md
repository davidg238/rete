# Build-asset archive — keyed by build id, retained by intent, eventually at porta

**Status:** design COMPLETE, unimplemented. Two rungs.
**Issues:** follows #74 (`platform buildId`), #164/#165 (the profiler), #56 (the console collector).
**Supersedes nothing.** Extends the ELF archive built in #56 §5.3.

## The problem

A runtime observation — a panic address, a profile's method offset — is meaningless
without the metadata that maps it back to source. That metadata is regenerated on every
build and overwritten in place. The observation may need resolving weeks later.

This is not a new problem and it has one settled answer, which the industry arrived at
independently in every ecosystem: **the running binary carries an immutable id; the
metadata is stored content-addressed by that id; the resolver fetches by id and fails
loudly on a miss.** Windows keys PDBs by GUID+age through SymSrv. Linux keys `.debug`
files by GNU build-id through debuginfod, which `gdb` and `perf` query automatically.
Breakpad, Crashpad and Sentry key `.sym` files by debug-id. Apple keys dSYMs by UUID.
Android keys `mapping.txt` by version. Source maps grew a `debugId` for the same reason.

Two things those ecosystems consistently do **not** do, both of which decide this design:

- **They do not commit build outputs to version control.** Git has no way to expire an
  artefact by id, it churns binaries on every build, and — decisively here — a dirty tree
  has no commit at all, so git could never hold the artefact for exactly the builds that
  get flashed most.
- **They do not regenerate metadata on demand** unless the build is bit-reproducible.
  Reproducibility is the only thing that makes regeneration sound, and the `-dirty`
  suffix in this system's build id is the explicit statement that a given build is not
  reproducible from a revision. That makes archive-at-build-time not a preference but
  the only correct option.

**Most of this already exists here.** `nodus/tuvm/zephyr/tools/console/archive.sh` writes
`~/.local/state/rete/elf/<build-id>/zephyr.elf` on every `--flash`, and
`console/symbolize.py` resolves a panic against it and **refuses** when the archive has no
match, with `--assume-build` as a typed override echoed onto every line of output. That is
a symbol server, including the property most homegrown ones get wrong.

The profiler's metadata joined it on 2026-08-07 (`fbf604d`, on the #165 branch): `archive.sh`
now copies `<app>.oimg.methods.json` and `<app>.stmap` into the same build-id directory and
fails loudly by name if either is absent. **What remains missing is that nothing reads the
archive by id, and that it has no retention policy at all.**

## What is there today, measured 2026-08-07

```
~/.local/state/rete/elf     831 MB     65 directories     4 days     no prune, ever

each directory holds exactly:
    17.6 MB   zephyr.elf
    0.1 KB    zephyr.bin.sha256

size spread    30 dirs @ 5 MB    16 @ 18 MB    18 @ 24 MB    1 @ 4 MB
flashes/day    Aug 3: 6    Aug 4: 19    Aug 5: 32    Aug 6: 8
```

Per build, the three artefacts that matter and what each resolves:

| artefact | size | resolves |
|---|---|---|
| `zephyr.elf` | 17.6 MB | a panic address → source line |
| `<app>.stmap` | 112 KB | a method offset → source line |
| `<app>.oimg.methods.json` | 81 KB | a profile offset → method name |

**The metadata is ~1% of what is archived.** Every build's metadata, for all 65 builds to
date, is ~12.5 MB against 831 MB. That ratio is the hinge of this design: it is what makes
indefinite retention of profile-resolution affordable, and it is what makes the move to
porta possible at all — 12.5 MB travels, 831 MB per four days does not.

---

## Rung 1 — the id-keyed asset archive, bench-local

### D1 — Resolution goes through the archive, by build id

`view.py` and `mprof_resolve.py` take the build id from the run's own `tuvm: build-id`
line in the console log they already have open — `symbolize.py` already extracts it
exactly this way — and fetch the matching sidecar from the archive. `--sidecar` is
demoted to a typed override.

Rationale: today the view takes a path a human picked, which is precisely the
"picked it wrong" failure #74 was built to end, and a mismatched sidecar resolves nothing
while looking identical to a quiet program. It also makes the archive a thing that is
*read*, not merely written — an archive nothing reads is discovered to be broken on the
day it is needed.

Rejected: getting the build id from porta's `obs/build`. More correct in principle, but it
couples the viewer to porta's sqlite, and the host-locality of that store is a live defect
(see D6). The console log is already open in front of the reader.

### D2 — Two retention tiers, split by weight

- **Metadata** (`.oimg.methods.json`, `.stmap`, a one-line manifest naming app and board):
  retained indefinitely. ~193 KB per build.
- **ELF**: retained under a byte budget, pruned oldest-first, never the newest.

Rationale: profile resolution survives indefinitely for ~1% of the cost, and only panic
symbolisation degrades with age. A single tier would evict an 81 KB sidecar to reclaim
space an 18 MB ELF was using.

**Known cost, recorded so it is falsifiable rather than discovered:** at the observed 16
flashes/day, metadata-forever is 3.1 MB/day ≈ **1.1 GB/year** — the same order as the ELF
budget, not free. The likely fix is content-addressing (a C-only rebuild plausibly yields a
byte-identical sidecar, and many of the 65 builds to date probably share one), which is a
measurement away rather than a guess. Ship the simple tier, measure the duplication, and
let the number earn the deduplication.

### D3 — Default prunable; pinning is explicit and lives in the build's own directory

The pin governs the **ELF tier**, because that is the only budgeted one — D2's metadata is
never pruned, so a pin does nothing for it today. If the 1.1 GB/year recorded in D2 later
forces a metadata budget, the *same* pin governs that tier too. One mechanism, not two.

Most builds are scratch — twenty debug flashes in a day are the norm, not the exception —
so the default must be evictable and keeping must be the deliberate act. A pinned build is
marked by a file inside `<build-id>/`, not by an index elsewhere: the pin must survive the
move to porta (D6) without a migration step, and an index on a laptop is a second thing to
keep in step.

Two ways to set it: at flash time when you already know the build matters
(`run.sh --flash --keep`), and after the fact once you discover it did.

Deliberately **not** inferred from `-dirty`. A dirty build is unreproducible and therefore
more valuable to keep, *and* is what most scratch builds are — the signal points both ways,
so it decides nothing. The prune reports how many of the builds it evicted were dirty, and
leaves the judgement to a human.

### D4 — Refuse, never fall back

Adopted unchanged from `symbolize.py`'s D4: a confident wrong source location is worse than
no answer, because it looks like one. A missing asset raises the `Refused` that module
already defines; the one way past it is typed and echoes onto the output so the assumption
travels with the answer.

"Unknown build id" and "known build id, asset missing" are **different messages**. The
second means the prune evicted something the tier rules say it should not have, which is a
bug report rather than a user error.

### D5 — Adopt `logstore`'s constant and prune shape; do not invent a number

`logstore.py` already carries `DEFAULT_BUDGET = 2 GB` and prunes whole oldest days, never
the newest, on the stated grounds that a single wedged day over budget is a fact to report
rather than a reason to delete. The ELF tier uses that constant and that shape. This is an
adopted constant, not a new tunable.

Pruning is **loud**, as `logstore`'s is: it writes a `pruned …` marker. A silent eviction
is a policy you discover by its absence.

---

## Rung 2 — the assets live at porta

Direction set by David, 2026-08-07: *all assets must reside at porta, at some point in the
tooling being transferred there* — where assets means the ELF and whatever else debug,
profile and console work requires.

### D6 — porta the *host*, not porta the *program*. No new porta code.

The archive becomes a directory on porta's host. `porta` itself stays language-neutral and
asset-ignorant; nothing is added to its Go surface, its store, or the wire.

**This dissolves a defect rather than merely relocating one.** The `POST /capture` route
shipped writing to a `porta.db` relative to the view's working directory: `porta put` is a
local sqlite writer, not a network client (`portacli/root.go:56` defaults `--db` to a
relative path, `space/store.go:43` runs `db.Exec(schema)` on open, so sqlite *creates* an
empty store and the write exits 0). It was patched by making `--porta-db` required and
documenting that the view must run where the served store is. Colocating the readers, the
served store and the logs makes that failure structurally impossible instead of documented.

### D7 — The readers move; the collector cannot; the console path already exists

**Corrected 2026-08-07 by reading the code, having first claimed otherwise.** Only
`logread.py` and `check.py` take a `--root`. `view.py`, `mprof_resolve.py` and
`symbolize.py` do **not** — three of the five. So this is not pure configuration: the move
carries a small, real code change, and `--root` should be added to those three as part of
rung 1 rather than discovered under rung 2.

`logread.py --root <copy>` being a supported path today is what makes that addition a
pattern to copy rather than a decision to make. It is also the proof the pattern works
against a synced copy, which is the whole of what rung 2 asks of it.

`collect.py` stays on the bench, pinned by physics: it holds a USB serial port. It does not
need to move. **`udpsink.py` already runs on the gateway** (#56 §10, built 2026-08-04),
binding the mesh-local address the node already reaches for NTP and writing the *identical*
on-disk format through the *same* `LogStore` — which is why `logread.py --root <copy>` works
against it unchanged. Console-to-porta is built and proven, not a thing to design.

### D8 — Transport is rsync; the local archive becomes a spool

`archive.sh` keeps writing locally exactly as it does now, then rsyncs to porta. rsync is
idempotent and incremental, so an unreachable porta is caught up by the next flash: one
mechanism, no queue, no sync state machine to get wrong, and no second home to keep
consistent.

Rejected for now: an HTTP asset endpoint on porta (a real debuginfod). porta's `httpsrv`
exists and outlived the v1 demolition, so it is cheap to add later — but today there is one
producer and one consumer, both on machines under direct control. That endpoint is earned
by a consumer that cannot ssh, not assumed now.

Rejected: a shared network filesystem. A mount that is not there fails as an *empty
archive*, which reads identically to "no captures yet" — the one failure shape this
program keeps being bitten by.

### D9 — The fleet pin: a build that is running somewhere is never evictable

Since #74, `obs/build` carries the node's own build id, so **porta knows which builds are
live in the field.** The prune queries `obs/build` across nodes and pins those ids
automatically.

This is pinning by evidence rather than by memory, and it is the reason the archive belongs
at porta *specifically* rather than at any server: nothing else in the system is positioned
to know it. It needs no new porta surface — it is a read.

### D10 — Assets never travel on the space

They move over the LAN into a directory. `PROTOCOL-NSL-RECORDS.md` §4.5 states that
developer-facing assets — console history, images, symbol sidecars — live off the space by
the same rule that keeps profiler blobs off it. This stays inside that line.

### D11 — Staleness must be visible

An archive whose sync silently stopped three weeks ago looks exactly like an archive: same
directory, older contents. Whatever ships must make staleness assertable, the same way the
collector's heartbeat exists because a log that stopped looks identical to a board that
went quiet, and the same way `check.py` asserts a *recent heartbeat is present* rather than
the absence of an error.

---

## Testing

**Rung 1.** `archive.sh` is already exercised three ways as of `fbf604d` (all artefacts
present, sidecar absent, `OIMG` empty), so this rung inherits that and adds: a prune test
asserting a pinned build survives a prune that evicts its unpinned neighbours, and a
`--root` test for each of the three readers that gain one under D7.
Resolver unit tests for hit, miss and typed override. And a view test asserting a
refusal renders as prose, not as an empty table — the invariant the profile view broke twice
before it shipped.

**Rung 2.** The host move is not unit-testable; three things around it are. The resolver
against a `--root` that is a synced copy — the pattern `logread --root <copy>` already
proves. An integration check that a synced archive resolves a known build id end to end.
And the fleet-pin query against a real porta store, asserting a live node's build id
survives a prune that would otherwise evict it.

## Deferred, with what would earn each back

- **Content-addressing / deduplicating sidecars.** Earned by measuring how many of the
  archived sidecars are byte-identical. If a C-only rebuild reproduces the sidecar exactly,
  D2's 1.1 GB/year largely evaporates.
- **An HTTP asset endpoint on porta.** Earned by a consumer that cannot ssh to the host.
- **Reproducible builds**, which would make regeneration sound and shrink the archive to
  the unreproducible builds only. Foreclosed today by `-dirty`; earned by wanting it enough
  to make clean builds the norm.
- **Auto-pinning by evidence** — a build that produced a panic, or that produced a profile
  capture worth keeping. Attractive, but the archive cannot know and the reader can; defer
  until D3's manual pin has been used enough to show what the rule should be.

## What this design does not do

It does not put assets on the space, add a verb, add a table, or add anything to porta's Go
surface. It does not attempt reproducible builds. It does not version any format: both ends
are reflashed together, so a shape change here is an edit and a rebuild.

---

## Addendum 2026-08-24 — D8's debuginfod deferral superseded (#256)

David's direction, 2026-08-24: dev tooling is declared Linux-only, and the archive
SHOULD be served over the debuginfod protocol now rather than when a non-ssh consumer
appears — the bench's gdb 15.1 is already a native client. Two ordered pieces, filed
as #256: (1) stamp `NT_GNU_BUILD_ID` into the Zephyr link (`-Wl,--build-id=sha1`) and
record both ids in the per-build manifest FIRST — every build archived without the
note is stock-invisible forever; (2) serve `/buildid/<id>/{debuginfo,executable}` +
`/source/<path>` — stock elfutils debuginfod over the archive directory, or three
routes on porta's httpsrv, whichever Rung 2's colocation makes cheaper. The nsl
sidecars stay custom by design (no industry server speaks them); they ride the same
build-id key per D2. Context: the porta separation-of-concerns ruling recorded on
rete#243 (porta owns what must outlive any node, build, or developer machine).
