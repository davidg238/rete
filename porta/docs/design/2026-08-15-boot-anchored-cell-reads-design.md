# Boot-anchored cell reads — `porta get` can no longer blend two boots

**Date:** 2026-08-15
**Closes:** rete#210
**Touches:** rete#161 (where it was found), rete#162 / rete#117 (which put `bootId` on the
wire), `docs/design/2026-08-05-target-test-harness-design.md` §8 addendum

## 1. The defect, in one paragraph

`cells` holds the latest value per `(node_id, path)`. A rebooted node rewrites its whole tree,
but the rows arrive **spread over several seconds** — in the run that exposed this, sixteen
`diag/spans/**` rows landed across an 11 s window (`09:10:31`–`09:10:41`), which begins *after*
the target harness's console TAP verdict has already returned. A `porta get` taken inside that
window answers with a **mixture**: this boot's values for the paths that have arrived, the
previous boot's for the paths that have not. **Nothing in the row says which boot it came
from**, so the mixture is indistinguishable from a coherent answer.

It nearly inverted a hardware finding. Two builds one commit apart were flashed minutes apart
to compare `pushTrim`; the control's read returned the *previous* boot's numbers
(`188 / 51 923 µs`, byte-identical), supporting the conclusion that the change costs nothing at
cold boot. Its real figure was `66 / 185 µs` — the opposite conclusion. It was caught only
because two runs of different binaries producing identical microsecond timings is impossible.
With two closer-behaving builds it would have passed as data.

## 2. The finding that reframes it

**The anchor already exists, and porta already receives it.** `bootId` is a persisted,
NVS-backed **monotonic** counter (`PRIM_pf_bootId`, `nodus/tuvm/src/prim_ids.h`), added for
rete#162/#117 precisely so porta could tell a rebooted node from a node whose ack it merely
missed. It rides both envelopes as `bid` (`internal/wire/message.go` `EncodePull`/`EncodePush`),
and porta stores it on `nodes` and on every `exchanges` row.

`Store.Ingest` holds it **at the exact moment it writes `cells` and `events`**
(`internal/space/store.go`) — and discards it for both tables.

So this is not "add a freshness heuristic to `porta get`" (rete#210's option 1). It is *record
the boot id you are already given, on the rows the operator actually reads*. The answer is
exact rather than inferred, and it needs no new concept: `boot_id` is already a column name in
this schema, twice.

## 3. Decisions

### D1 — `cells` and `events` each gain `boot_id`, written where the value is already in hand

`boot_id INTEGER NOT NULL DEFAULT 0` on both tables. `upsertCell` and `appendEvent` take one
more argument; `Ingest` passes the pushed `bid`; `Write` (the outbound operator path) passes 0.
No new statement is executed — the columns join writes that were happening anyway.

One column, one meaning, disambiguated by the `direction` already on the row:

| row | `boot_id` | reading |
|---|---|---|
| `in`, node has a persistent identity | the `bid` of the push that carried it | which boot published this |
| `in`, node has none | `0` | **unknown**, never *stale* |
| `out` — porta's own `goal/**` writes | `0` | not applicable; never filtered, never counted |

`0` is not overloaded, because `direction` already separates the two cases. A second sentinel
would be a mechanism added beside the one that works.

Nodes that legitimately report `bid = 0` are real: the posix backend without `TUVM_BOOTID_FILE`,
and any board built without `CONFIG_SETTINGS` (`platform_zephyr.c` returns 0 there). For those
the guard cannot fire, and saying "unknown" is the honest output — claiming "stale" would be a
fabrication, and claiming "current" would be the bug wearing a fix's costume.

### D2 — Migration is an idempotent `ALTER TABLE`, not a store rebuild

`space.Open` applies the schema with `CREATE TABLE IF NOT EXISTS`, which cannot add a column to
an existing table. Open therefore executes `ALTER TABLE … ADD COLUMN boot_id …` for each of the
two tables and ignores the "duplicate column name" error.

The standing rule that there is **no backward compatibility to maintain** governs the *wire and
the record shapes*; it is not licence to destroy an operational store. The live store
(`soak-2026-08-04.db` on `gw85224-01`) carries the soak history that rete#190's evidence rests
on, and rebuilding it would additionally force every node through a full re-push. Six lines that
run once are cheaper than either, and leave no standing mechanism behind.

Pre-existing rows read `0` / unknown, which is truthful — they predate the anchor — and each
path acquires a real boot id the next time the node writes it. The store heals itself one write
at a time; nothing needs backfilling.

### D3 — `Tree` returns a read row; `Entry` stays the wire row

`Entry` is what rides the radio: four fields, deliberately. `Event` is already `Entry` +
provenance. The symmetric addition is therefore

```go
type Cell struct {
    Entry
    BootID    int64
    Direction string
    TsMs      int64
}
```

returned by `Tree`, with `BootID` added to `Event`. Putting a display field on `Entry` and
teaching the encoder to ignore it would be exactly the failure mode the project keeps naming:
a field added *beside* what was there rather than *instead of* it. Every existing caller reads
fields off the elements, so embedding keeps them compiling.

### D4 — No `--boot N` on `get`; the boot filter belongs to `log`

`cells` holds only the latest value per path, so filtering it by boot could never mean "the
tree as of boot 40". It would mean "paths whose *last* write happened to be boot 40" — a new
way to be misled, sold under a flag name that promises otherwise.

Reconstructing a past boot is a **log** question, and the log can answer it truthfully because
it is append-only. So:

- `get` is honestly *the latest value per path, annotated with which boot each came from*.
- `log` gains `--boot N`, which is a `WHERE boot_id = ?` over rows that each record their own
  boot.

This is the sound half of "redirect `get` at the log": the reconstruction moves to the log,
while the tree read — which the pull path needs for a different reason (below) — stays a tree
read.

### D5 — `cells` is not retired in favour of the log

Worth recording because it is the obvious next thought. `OutboundSince` reads `cells` so that a
**superseded cell write never rides the radio**, and so that a cursor-0 replay is bounded by
*tree size* rather than by history length. Those are radio constraints, not conveniences. The
collapsed tree stays.

Note what this scopes: only the `direction = 'in'` half of `cells` is exposed to the defect at
all. The `out` half is porta's own intent, and porta does not reboot.

### D6 — The banner goes to stdout, and there is no `--strict`

```
$ porta get c6spare diag/spans
! MIXED READ: 4 of 16 rows predate boot 41 — see porta log --boot 41
  diag/spans/checkIn        seq=8987   boot=41  221247 / 243307
  diag/spans/pushBuild      seq=8985   boot=41   91730 / 136337
~ diag/spans/pushTrim       seq=8203   boot=40      188 /  51923   STALE
```

**stdout, not stderr.** The wedged-node record's seventh item is a monitor that reported three
half-hour wakes as *nothing happening* because GNU grep sent its warning to stderr and the
stdout-only pipeline never saw it. A warning a pipe eats is not a guard.

**No `--strict` and no non-zero exit.** `! MIXED READ` is greppable, so a script that cares
already has its hook. A flag here would be a tunable invented before any caller asked for it.

In the ordinary case — a node that has not rebooted — every row carries the current boot,
nothing is marked, and the banner never prints. The guard is invisible until exactly the moment
the hazard exists.

## 4. Why a documented rule was not enough

The harness design's §8 addendum (2026-08-15) records the discipline: after a deploy, read
published state from `porta log`, never `porta get`. rete#210 named the same option and its own
objection to it — it "relies on remembering, which is what this project keeps learning does not
hold."

That objection is the reason this design exists. The addendum's rule now becomes a *fallback*
rather than the defence: `get` says when it cannot answer coherently, so the operator does not
have to have read a design document to avoid a wrong number. The addendum is rewritten to point
at the mechanism.

## 5. Testing

- `Ingest` at boot 41 stamps `boot_id` on both `cells` and `events`; `Write` stamps 0.
- **The exact regression**: ingest a full tree at boot 40, then ingest a *subset* at boot 41,
  and assert `Tree` reports rows from both boots — the 2026-08-14 reading, reproduced in the
  store rather than on a board.
- `get` prints the banner on **stdout** when the read is mixed, and prints no banner when the
  tree is coherent.
- A node reporting `bid = 0` renders unknown, not stale.
- `log --boot N` returns only that boot's rows.
- Migration: open a store created without the columns, and confirm it opens, adds them, and
  that a second open is a no-op.

## 6. Close-out — which mainline document absorbs this

- `porta/docs/ARCHITECTURE.md` — the store schema section gains `boot_id` on both tables.
- `docs/design/2026-08-05-target-test-harness-design.md` — the §8 addendum is rewritten to
  point at the mechanism rather than prescribe the discipline.
- rete#210 — closed, recording that its option 1 was superseded by something exact, because the
  boot id was already on the wire and no freshness heuristic was needed.
