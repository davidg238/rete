# porta space-sync — the node side (plan 2 of 3) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give an nsl node the space-sync half of the porta protocol — a monotonic
per-space `seq`, a `changedSince:` cell query, and a `PortaBridge` job that pulls
`goal/**` from porta and pushes `obs/**`/`tel/**`/`sys/**` back — and prove it against
the *real* `porta serve` over host UDP loopback before any hardware is involved.

**Architecture:** Three layers, each testable alone. (1) `src/space.c` gains one
monotonic counter per space, a `last_seq` per channel, and a prefix-scoped
changed-since scan — additive storage, no existing verb changes. (2) Three platform
prims expose it, and `Channel>>changedSince:max:` resolves the scan prefix against the
capability's own root, so confinement is preserved by construction. (3) `PortaBridge`
in `kernel.nsl` is an ordinary nsl class holding a `Socket`, the root `Space`, a cursor
and a pending queue; `PortaBridgeNode.nsl` is the minimal node that runs it.

**Tech Stack:** C99 (`src/space.c`, `src/platform.c`, `src/dispatch.c`), nsl
(`nsl-tuvm/lib/kernel.nsl`, `tuvm/zephyr/PortaBridgeNode.nsl`), the nsl test suite
(Path B / `.oimg`), `tools/run_c_tests.sh`, and Go only as the counterparty
(`porta serve`, already built by plan 1).

**Source of truth for every decision below:**
`~/workspace/porta/docs/design/2026-08-01-porta-space-sync-rewrite-design.md` (§5 wire,
§6 node side, D5/D6/D8), `~/workspace/tuvm/protocol/PROTOCOL-NSL.md` §3–§4,
`~/workspace/tuvm/protocol/PROTOCOL-NSL-RECORDS.md` §2.2/§3/§4, and the shipped Go
counterparty `~/workspace/porta/internal/wire/message.go`.

---

## Global Constraints

- **The wire is fixed by plan 1 and is not to be renegotiated here.** Field names come
  from `~/workspace/porta/internal/wire/message.go`, *not* from the design's §5 sketch
  (which wrote `nodeSeqHW` where the code writes `hw`):
  - `#pull { id: str, cursor: int, bootId: int, hw: int }` → `#batch { hi: int, entries: [ [seq, path, kind, value], … ] }`
  - `#push { id: str, bootId: int, entries: [ [seq, path, kind, value], … ] }` → `#ack { hw: int }`
  - either request may instead be answered `#err { msg: str }`.
  - `kind`: `1` = stream, `2` = cell (matches `SPACE_KIND_STREAM` / `SPACE_KIND_CELL`
    and `cval.KindStream` / `cval.KindCell`).
- **Caps (RECORDS §2.2, enforced by porta and re-enforced here):** value ≤ **256 B**
  encoded, batch datagram ≤ **512 B**. A node that sends an oversize value gets its
  whole push rejected with `#err`, so the bridge must never emit one.
- **`=`, never `==`, on a decoded Symbol** (tuvm#75). A CBOR-decoded `#batch` is not
  the image's interned `#batch`; `==` is identity and will silently never match.
  Every tag/field comparison in this plan uses `=`.
- **Never send an ad-hoc predicate to a decoded value** (`isRecord`, `isList` on a
  builtin Integer receiver DNU-crashes). Inside `kernel.nsl` use `outer rankOf: v`
  (7 = List, 9 = Record); this is the idiom `List>>=` already documents.
- **A reified block body is one `self`-send.** A direct cross-class call written inline
  in a block stored by the reactor (`whenever:do:`) fails to resolve its outer context
  on this VM — see `NetBridge>>encodeFor:`'s comment in `kernel.nsl`. Prefer
  `to:do:`/`whileTrue:` (compiler-inlined) for loops.
- **nsl has no modulo and no `startsWith:`** (tuvm#97; grep-confirmed). Prefix tests are
  hand-rolled over `size`/`at:`, the idiom `Space>>isSysPath:` already uses.
- **Prim ids are counted, never computed.** The last id is `PRIM_pf_ntpDate` = 137
  (`src/prim_ids.h`). New ids are appended and arrived at by counting `PRIM_NAMES`;
  the `prim_names_enum_parity_check` typedef in `dispatch.c` must be advanced to the
  new last id in the same commit.
- **Run the nsl suite Path B only** (`OEVM_PATH=B`, the default; `run-one.py` needs an
  explicit `B`). Standing instruction — a per-test gcc rebuild is too slow.
- **`run-one.py` dirties `src/solidified_image.c`** (tuvm#106). Do not stage it.
- **Never run two nsl suites at once** — they share `/tmp` paths.
- **Deliberately out of scope**, and each already has a home: NVS persistence of the
  cursor and `bootId` (design §6 — RAM for this slice), the adaptive cadence
  controller (PROTOCOL-NSL §4.4), the back-pressure `cadenceHint` (reserved, not
  emitted), auth/grants (G5), the bulk plane, and `diag/spans/result` — which **must
  not be synced** until tuvm#107 splits its ~1120 B value into sibling cells.

### Node-side decisions this plan pins (design §6 leaves them open)

These are the gaps between "the space gains a counter and a `changedSince:` prim" and
working code. They are decisions, not preferences; each names what it rules out.

- **N1 — the scan answers CELLS only.** A stream's `last_seq` moves on every post but
  its value is *not* retained (`space_dequeue` kills the event on delivery), so a scan
  that returned streams would hand the bridge a stale-or-nil value under a fresh seq.
  `changedSince:` is the cell-replication query and nothing else.
- **N2 — stream entries are captured by reaction and stamped with `spaceStamp`.**
  The bridge registers `whenever:` on each outbound stream path and appends
  `{seq, path, 1, value}` to a pending queue, taking `seq` from a prim that *bumps* the
  space counter. Reading the counter would not do: two captures with no intervening
  write would collide on one seq, and porta drops `seq <= hw` as a duplicate — silent
  loss. One counter for both halves is D5's requirement, and it is what makes the push
  a single ascending sequence.
- **N3 — `PortaBridge` lives in `kernel.nsl`,** beside `NetBridge`, reached by
  `platform PortaBridge`. It needs `Cbor`/`Record`/`Map`/`List`/`OrderedCollection`,
  which an app cannot reach as classes; and porta#25 wants a *stable node-side
  contract*, which a per-node copy would not be. **The node holds ONE instance** — the
  `platform timers` lesson: an accessor answering a fresh instance silently drops
  everything held in the old one's registry.
- **N4 — the bridge takes the ROOT `Space`,** not a pair of attenuated capabilities,
  and enforces prefix authority itself in `mayApply:`. `Supervisor` has the same shape
  for the same reason. Splitting it into `goal`-write + `obs`-read capabilities is a
  follow-on when the grant machinery (G5) lands, and is filed as such.
- **N5 — porta may write only `goal/**` and `dbg/req`.** An inbound entry at any other
  path is **counted and dropped**, never applied and never a crash. Untrusted network
  input must not be able to write `obs/**` (which would forge the convergence view) —
  the `NetBridge` drop-on-malformed precedent, widened to a path rule.
- **N6 — node id and `bootId` are source constants** in this slice (porta#27), and the
  inbound cursor lives in RAM. Cold boot = cursor 0 = full replay is the *designed*
  behaviour (PROTOCOL-NSL §3); the `goal/do/*` replay hazard is porta#26 and is not
  built here.
- **N7 — one datagram at a time, bounded wait, never wedge.** A check-in is
  PULL→reply→PUSH→ack, each with a millisecond budget; a missing reply is counted and
  the next check-in re-pulls the same range. The exchange is idempotent by cursor, so
  there is no retransmit layer to write (design D1).

---

## File Structure

| file | fate | responsibility |
|---|---|---|
| `tuvm/src/space.h` | modify | declares the seq/scan seam (§ "Space-sync") |
| `tuvm/src/space.c` | modify | the counter, `last_seq`, the prefix-scoped scan |
| `tuvm/tests/test_space_seq.c` | **create** | C properties for the seam, no VM dispatch |
| `tuvm/tools/run_c_tests.sh` | modify | one `run` line for the new test |
| `tuvm/src/prim_ids.h` | modify | mirrors ids 138–140 |
| `tuvm/src/dispatch.c` | modify | enum + `PRIM_NAMES` + parity guard |
| `tuvm/src/platform.c` | modify | the three prim cases |
| `nsl-tuvm/lib/kernel.nsl` | modify | `Channel>>changedSince:max:`, `Space>>seq`/`stamp`, `class PortaBridge`, the `platform PortaBridge` accessor |
| `nsl-tests/22_spacesync/*.nsl` | **create** | seq/scan/confinement + bridge frame properties |
| `tuvm/zephyr/PortaBridgeNode.nsl` | **create** | the minimal node that runs the bridge |
| `tuvm/tests/porta_sync_host.sh` | **create** | the cross-repo gate: this node vs real `porta serve` |
| `protocol/PROTOCOL-NSL.md` | modify | §4.1 records what actually landed |
| `tuvm/review/tests/CLAIMS.md`, `tuvm/review/program-sequence.md` | modify | the promotion (close-out) |

---

### Task 1: The space's monotonic seq and the changed-since scan (C)

**Files:**
- Modify: `tuvm/src/space.h`, `tuvm/src/space.c`
- Create: `tuvm/tests/test_space_seq.c`
- Modify: `tuvm/tools/run_c_tests.sh`

**Interfaces:**
- Consumes: the existing `space.c` statics (`channels[]`, `n_channels`, `SPACE_KIND_*`).
- Produces, all new in `space.h`:
  - `uint32_t space_seq(void)` — the counter's current value; 0 before any write.
  - `uint32_t space_stamp(void)` — bump and answer; strictly increasing.
  - `uint32_t space_last_seq_of(int id)` — the seq of that channel's last write.
  - `int space_changed_since(uint32_t cursor, const char *prefix, size_t plen, int *ids, int max)`
    — fills `ids` with CELL channel ids whose `last_seq > cursor`, at or under
    `prefix`, **ascending by `last_seq`**, at most `max`; answers the count.
  - `const char *space_path_of(int id, size_t *len_out)`, `int space_kind_of(int id)`,
    `oop space_value_of(int id)` — read the row the scan named.

- [ ] **Step 1: Write the failing C test**

Create `tuvm/tests/test_space_seq.c`. It calls the `space_*` functions directly — no
VM dispatch anywhere, the same discipline `tests/test_space.c` states as its property 6.

```c
/* Unit test for the space-sync seam (src/space.h "Space-sync" section) — the
   per-space monotonic counter (design D5), the per-channel last_seq, and the
   prefix-scoped changed-since scan that backs `Channel>>changedSince:max:`.

   Build (Model-B link set, small capacities for determinism — see
   tools/run_c_tests.sh):
     gcc -std=c99 -Isrc -DOEVM_SPACE_MAX_CHANNELS=16 tests/test_space_seq.c \
         <ENG_B> -lm -o /tmp/tss && /tmp/tss

   Properties:
     1. the counter starts at 0 and every write advances it by exactly one;
        a cell write and a stream post both count (space_put fans out through
        space_post_id, so the stamp must not be applied twice).
     2. last_seq is per channel and equals the counter at that channel's last
        write; an untouched channel keeps its old last_seq.
     3. space_stamp() is strictly increasing and interleaves with writes in one
        sequence (N2: a captured stream entry can never collide with a write).
     4. the scan is CELLS ONLY (N1) and cursor-exclusive.
     5. the scan is prefix-scoped and SEGMENT-ALIGNED: prefix "obs" answers
        "obs/health" and "obs" itself, never "obscure".
     6. the scan is ascending by last_seq and honours max — which is what makes
        "advance the cursor to the last entry sent" safe under trimming.
     7. a shed write (ring full) still advances the counter: declaring the write
        is not contingent on delivery, exactly as the kind fix already is.
*/
#define _POSIX_C_SOURCE 200112L
#include "../src/oevm.h"
#include "../src/space.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

extern void heap_init(void);
extern void kernel_boot(void);

#define P(s) (s), strlen(s)

int main(void) {
    heap_init();
    kernel_boot();

    /* --- Property 1: the counter starts at 0 and advances by one per write --- */
    assert(space_seq() == 0 && "no write has happened yet");
    space_put(P("obs/health"), TAG_SMI(1));
    uint32_t s1 = space_seq();
    assert(s1 == 1 && "a cell write advances the counter by exactly one");
    space_post(P("tel/log"), TAG_SMI(2));
    assert(space_seq() == 2 && "a stream post advances it too, by exactly one");

    /* --- Property 2: last_seq is per channel --- */
    int obs = space_intern(P("obs/health"));
    int tel = space_intern(P("tel/log"));
    assert(space_last_seq_of(obs) == 1);
    assert(space_last_seq_of(tel) == 2);
    space_put(P("obs/health"), TAG_SMI(3));
    assert(space_last_seq_of(obs) == 3 && "rewriting advances this channel");
    assert(space_last_seq_of(tel) == 2 && "and leaves the other alone");

    /* --- Property 3: stamp interleaves in the one sequence --- */
    uint32_t a = space_stamp();
    uint32_t b = space_stamp();
    assert(b == a + 1 && "two stamps never collide");
    space_put(P("obs/name"), TAG_SMI(4));
    assert(space_seq() == b + 1 && "a write after a stamp continues the sequence");

    /* --- Property 4: cells only, cursor-exclusive --- */
    {
        int ids[OEVM_SPACE_MAX_CHANNELS];
        int n = space_changed_since(0, P(""), ids, OEVM_SPACE_MAX_CHANNELS);
        int saw_stream = 0;
        for (int i = 0; i < n; i++) if (space_kind_of(ids[i]) != SPACE_KIND_CELL) saw_stream = 1;
        assert(!saw_stream && "a stream must never ride the cell scan");
        assert(n == 2 && "obs/health and obs/name, not tel/log");

        n = space_changed_since(space_seq(), P(""), ids, OEVM_SPACE_MAX_CHANNELS);
        assert(n == 0 && "the cursor is exclusive: nothing is newer than now");
    }

    /* --- Property 5: prefix scoping is segment-aligned --- */
    space_put(P("obscure"), TAG_SMI(5));
    {
        int ids[OEVM_SPACE_MAX_CHANNELS];
        int n = space_changed_since(0, P("obs"), ids, OEVM_SPACE_MAX_CHANNELS);
        assert(n == 2 && "obs/health + obs/name; obscure is a different name");
        for (int i = 0; i < n; i++) {
            size_t len = 0;
            const char *path = space_path_of(ids[i], &len);
            assert(!(len == 7 && memcmp(path, "obscure", 7) == 0));
        }
        n = space_changed_since(0, P("obs/health"), ids, OEVM_SPACE_MAX_CHANNELS);
        assert(n == 1 && "a prefix equal to the whole path matches that path");
    }

    /* --- Property 6: ascending, and max is honoured --- */
    {
        int ids[OEVM_SPACE_MAX_CHANNELS];
        int n = space_changed_since(0, P("obs"), ids, 1);
        assert(n == 1 && "max caps the answer");
        assert(space_last_seq_of(ids[0]) == 3 && "and it is the OLDEST pending write");

        n = space_changed_since(0, P("obs"), ids, OEVM_SPACE_MAX_CHANNELS);
        assert(n == 2);
        assert(space_last_seq_of(ids[0]) < space_last_seq_of(ids[1]) && "ascending");
    }

    /* --- Property 7: a shed write still advances the counter --- */
    {
        uint32_t before = space_seq();
        for (int i = 0; i < OEVM_SPACE_QUEUE_DEPTH + 4; i++) {
            space_post(P("tel/flood"), TAG_SMI(i));
        }
        assert(space_seq() == before + (uint32_t)(OEVM_SPACE_QUEUE_DEPTH + 4) &&
               "every write is stamped, delivered or shed");
        assert(space_overruns(P("tel/flood")) > 0 && "and the shedding really happened");
    }

    /* --- the scan's value/path accessors answer the live row --- */
    {
        int ids[OEVM_SPACE_MAX_CHANNELS];
        int n = space_changed_since(0, P("obs/name"), ids, OEVM_SPACE_MAX_CHANNELS);
        assert(n == 1);
        size_t len = 0;
        const char *path = space_path_of(ids[0], &len);
        assert(len == 8 && memcmp(path, "obs/name", 8) == 0);
        assert(space_value_of(ids[0]) == TAG_SMI(4));
    }

    printf("test_space_seq: all properties passed\n");
    return 0;
}
```

- [ ] **Step 2: Run it and watch it fail to compile**

```bash
cd ~/workspace/tuvm && gcc -std=c99 -Isrc -DOEVM_SPACE_MAX_CHANNELS=16 -DOEVM_SPACE_QUEUE_DEPTH=8 \
  tests/test_space_seq.c \
  src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/platform.c src/serial.c \
  src/udp.c src/http.c src/http_core.c src/mirror.c src/space.c src/handles.c src/span.c \
  src/backend/posix/platform_posix.c src/backend/posix/serial_posix.c \
  src/backend/posix/udp_posix.c src/backend/posix/http_posix.c \
  src/backend/posix/pulse_posix.c src/backend/posix/timer_posix.c \
  src/backend/posix/span_posix.c src/backend/posix/sensor_posix.c \
  src/kernel_boot.c -lm -o /tmp/tss
```
Expected: FAIL — `implicit declaration of function 'space_seq'` (and the five siblings).

- [ ] **Step 3: Declare the seam in `src/space.h`**

Append, before `#endif /* OEVM_SPACE_H */`:

```c
/* ---- Space-sync (porta space-sync rewrite, design D5) ---------------------
   Replication needs three things the raw event queue never had: a latest-value
   slot per cell (rung 9 gave that), ONE monotonic counter per space, and a
   changed-since-cursor query. This is the second and third.

   ONE counter per space, not one per path (D5, resolving a PROTOCOL-NSL
   §3/§5.1 contradiction): a per-path seq would make a cursor a vector over
   every path, and the exchange's cursor is a scalar. Each channel additionally
   records the counter value of its own last write — exactly etcd's
   revision / mod_revision split.

   The counter is stamped inside space_post_id, which is on the path of BOTH
   writes (space_put fans out through it), so a cell write is stamped once and
   not twice. It is stamped BEFORE the full-ring check, for the same reason the
   kind is: a write the ring is about to shed still happened.

   uint32 wraps after 4.29e9 writes. At the fastest cadence this stack has ever
   run that is decades; a node that reaches it re-pulls from a cursor it has
   already passed, which the exchange absorbs as an ordinary full replay. */
uint32_t space_seq(void);

/* Bump the counter and answer the new value. The bridge stamps a CAPTURED
   stream entry with this (design §6 / plan-2 N2): a stream's value is not
   retained, so it is captured by reaction, and reading the counter instead of
   bumping it would give two captures the same seq — which the gateway drops as
   a duplicate. Silent loss; hence a bump. */
uint32_t space_stamp(void);

/* The counter value at the given channel's last write; 0 if it has never been
   written (interning is not a write). */
uint32_t space_last_seq_of(int id);

/* Fill `ids` with the ids of CELL channels whose last write is newer than
   `cursor`, at or under `prefix`, ASCENDING BY last_seq, at most `max` of them;
   answers how many were written.

   CELLS ONLY: a stream's last_seq advances on every post, but its value is
   killed on delivery, so returning one would publish a stale-or-nil value under
   a fresh seq. Streams reach the wire by reaction capture instead.

   Prefix matching is SEGMENT-ALIGNED, the A1 rule `Channel>>resolve:` already
   obeys: "obs" matches "obs" and "obs/health", never "obscure". plen == 0
   matches everything (the root capability). Never interns.

   ASCENDING is load-bearing, not tidiness: the caller sends a prefix of this
   list and advances its cursor to the last entry it sent, so an out-of-order
   answer would strand every entry it skipped. */
int space_changed_since(uint32_t cursor, const char *prefix, size_t plen,
                        int *ids, int max);

/* Read the row the scan named. `space_path_of` answers a pointer INTO the
   registry's static path buffer (not NUL-terminated — take the length), valid
   for the life of the process since channels are never removed. */
const char *space_path_of(int id, size_t *len_out);
int space_kind_of(int id);
oop space_value_of(int id);
```

- [ ] **Step 4: Implement it in `src/space.c`**

Add `last_seq` to the channel struct, beside `overruns`:

```c
    /* Space-sync (D5): the counter value at this channel's last write. 0 means
       never written — interning is not a write, exactly as `kind` records. */
    uint32_t last_seq;
```

Add the counter beside `total_overruns`:

```c
/* Space-sync (D5): ONE monotonic counter for the whole space, stamped on every
   write and bumpable by space_stamp for a captured stream entry. */
static uint32_t seq_counter = 0;
```

Initialise the field in `space_intern`, beside `c->overruns = 0;`:

```c
    c->last_seq = 0;    /* interning is not a write */
```

Stamp in `space_post_id`, immediately after the kind fix and **before** the
full-ring check:

```c
    /* Space-sync (D5): stamp the write. Here rather than in space_put/space_post
       because both of those reach this function — space_put writes the value
       slot and then calls us to fan out — so one stamp covers both and a cell
       write is not counted twice. Before the ring check for the same reason the
       kind fix is: a write the ring is about to shed still happened. */
    c->last_seq = ++seq_counter;
```

Then append the new entry points at the end of the file:

```c
/* ---- Space-sync (D5) ------------------------------------------------------ */

uint32_t space_seq(void)   { return seq_counter; }
uint32_t space_stamp(void) { return ++seq_counter; }

uint32_t space_last_seq_of(int id) { return channels[id].last_seq; }
int      space_kind_of(int id)     { return (int)channels[id].kind; }
oop      space_value_of(int id)    { return channels[id].value; }

const char *space_path_of(int id, size_t *len_out) {
    if (len_out) *len_out = channels[id].len;
    return channels[id].path;
}

/* Segment-aligned prefix test — the A1 rule Channel>>resolve: enforces in nsl,
   restated here because this scan takes a prefix rather than a whole path:
   "obs" is at-or-above "obs" and "obs/health" and is NOT above "obscure". */
static int under_prefix(const struct SpaceChannel *c, const char *prefix, size_t plen) {
    if (plen == 0) return 1;                       /* the root capability */
    if (c->len < plen) return 0;
    if (memcmp(c->path, prefix, plen) != 0) return 0;
    return c->len == plen || c->path[plen] == '/';
}

int space_changed_since(uint32_t cursor, const char *prefix, size_t plen,
                        int *ids, int max) {
    int n = 0;
    /* `taken` is the last seq already emitted, so the next pass picks the
       smallest last_seq strictly above it: a selection walk, ascending. Seqs
       are unique across channels (one counter), so there are no ties to break.
       n_channels is bounded by OEVM_SPACE_MAX_CHANNELS (32 by default), so the
       quadratic walk is bounded by a small constant and needs no scratch. */
    uint32_t taken = cursor;
    while (n < max) {
        int best = -1;
        for (int i = 0; i < n_channels; i++) {
            struct SpaceChannel *c = &channels[i];
            if (c->kind != SPACE_KIND_CELL) continue;
            if (c->last_seq <= taken) continue;
            if (!under_prefix(c, prefix, plen)) continue;
            if (best < 0 || c->last_seq < channels[best].last_seq) best = i;
        }
        if (best < 0) break;
        ids[n++] = best;
        taken = channels[best].last_seq;
    }
    return n;
}
```

- [ ] **Step 5: Run the C test to green**

Re-run the Step 2 command, then `/tmp/tss`.
Expected: `test_space_seq: all properties passed`, exit 0.

- [ ] **Step 6: Wire it into the aggregate C gate**

In `tuvm/tools/run_c_tests.sh`, immediately after the `run test_space …` line:

```bash
# Space-sync (porta plan 2): the per-space monotonic counter, the per-channel
# last_seq, and the prefix-scoped changed-since scan. Model-B link set, same
# reasoning as test_space above; the capacity overrides keep property 7's ring
# flood fast and the scan's channel walk short.
run test_space_seq "$ENG_B" -DOEVM_SPACE_MAX_CHANNELS=16 -DOEVM_SPACE_QUEUE_DEPTH=8
```

Run the whole gate: `cd ~/workspace/tuvm && tools/run_c_tests.sh`
Expected: every line PASS, including `test_space` (unchanged behaviour) and
`test_space_seq`.

- [ ] **Step 7: Commit**

```bash
cd ~/workspace/tuvm
git add src/space.h src/space.c tests/test_space_seq.c tools/run_c_tests.sh
git commit -m "feat(space): one monotonic seq per space, and the changed-since scan (porta#25)"
```

---

### Task 2: The three platform prims

**Files:**
- Modify: `tuvm/src/prim_ids.h`, `tuvm/src/dispatch.c`, `tuvm/src/platform.c`
- Test: `tuvm/tests/test_platform.c` (extend) — the prim-level round trip

**Interfaces:**
- Consumes: Task 1's `space_seq` / `space_stamp` / `space_changed_since` /
  `space_path_of` / `space_kind_of` / `space_value_of`.
- Produces, callable from nsl as `platform …`:
  - `spaceSeq` → SMI, the counter.
  - `spaceStamp` → SMI, the bumped counter.
  - `spaceChangedSince: cursor under: prefix max: n` → a raw indexable of up to `n`
    3-slot indexables `{pathString, seqSMI, value}`, ascending by seq. Empty
    (`basicSize` 0) when nothing changed. `prefix` is a String; `''` is the root.

- [ ] **Step 1: Write the failing prim test**

Append to `tuvm/tests/test_platform.c`, inside `main()` before its final `printf`:

```c
    /* --- Space-sync prims (porta plan 2): spaceSeq / spaceStamp /
       spaceChangedSince:under:max:. Driven through send() rather than the C
       functions (test_space_seq.c already covers those) — this proves the
       PRIM_NAMES wiring, the SMI tagging and the answer's shape. --- */
    {
        oop plat = platform_object();
        space_put("obs/one", 7, TAG_SMI(11));
        oop seq_before = send(plat, "spaceSeq", 0, NULL);
        assert(IS_SMI(seq_before) && UNTAG_SMI(seq_before) >= 1);

        oop stamped = send(plat, "spaceStamp", 0, NULL);
        assert(UNTAG_SMI(stamped) == UNTAG_SMI(seq_before) + 1);

        oop args[3];
        args[0] = TAG_SMI(0);
        args[1] = make_test_string("obs");
        args[2] = TAG_SMI(4);
        oop rows = send(plat, "spaceChangedSince:under:max:", 3, args);
        assert(rows != NULL);
        assert(basic_size_of(rows) == 1 && "one obs cell has been written");
        oop row = basic_at_of(rows, 1);
        assert(basic_size_of(row) == 3);
        assert(UNTAG_SMI(basic_at_of(row, 3)) == 11 && "the value rides with the row");
    }
```

Use the file's existing helpers for `send`, string construction and indexable
reads; if `make_test_string` / `basic_size_of` / `basic_at_of` are named
differently there, use that file's names — do not add new helpers.

- [ ] **Step 2: Run it and watch it fail**

```bash
cd ~/workspace/tuvm && tools/run_c_tests.sh 2>&1 | grep test_platform
```
Expected: `test_platform  FAIL` — the selector is not a primitive, so the send
reaches a DNU.

- [ ] **Step 3: Mirror the ids in `src/prim_ids.h`**

Append before `#endif`:

```c
/* porta space-sync, plan 2 (design D5): the space-sync seam.
     spaceSeq   -> SMI, the space's monotonic counter (0 before any write).
     spaceStamp -> SMI, the counter BUMPED. The bridge stamps a captured stream
                   entry with it; reading instead of bumping would let two
                   captures share a seq, which the gateway drops as a duplicate.
     spaceChangedSince: cursor under: prefix max: n
                -> a raw indexable of up to n 3-slot indexables
                   {pathString, seqSMI, value}, ASCENDING by seq. CELLS ONLY —
                   a stream's value is killed on delivery, so it cannot ride a
                   value-carrying scan. Segment-aligned prefix; '' is the root.
                   Does NOT intern (space_take's rule).
   Appended after ntpDate; no pre-existing id shifts. 138-140 were arrived at by
   COUNTING PRIM_NAMES (137 entries before this append), not by 137+1 arithmetic,
   per the standing rule above. */
#define PRIM_pf_spaceSeq          138
#define PRIM_pf_spaceStamp        139
#define PRIM_pf_spaceChangedSince 140
```

- [ ] **Step 4: Add the enum entries, the names, and advance the parity guard**

In `src/dispatch.c`, at the end of the prim enum (after `PRIM_pf_ntpDate,`):

```c
    /* porta space-sync, plan 2: the space-sync seam — spaceSeq / spaceStamp /
       spaceChangedSince:under:max:. Appended after ntpDate; no pre-existing id
       shifts. Counted (not computed) to be the 138th/139th/140th PRIM_NAMES
       entries. Platform-dispatched (mirrored in prim_ids.h). */
    PRIM_pf_spaceSeq,
    PRIM_pf_spaceStamp,
    PRIM_pf_spaceChangedSince,
```

At the end of `PRIM_NAMES` (after `"ntpDate",`):

```c
    "spaceSeq", "spaceStamp", "spaceChangedSince:under:max:",
```

And advance the guard — the whole point of that typedef:

```c
typedef char prim_names_enum_parity_check[(N_PRIM_NAMES == PRIM_pf_spaceChangedSince) ? 1 : -1];
```

- [ ] **Step 5: Service them in `src/platform.c`**

Add beside the other `space*` cases (after `PRIM_pf_spaceOverruns`):

```c
    case PRIM_pf_spaceSeq: {
        /* porta space-sync — `platform spaceSeq` -> the space's monotonic
           counter. No args, no allocation. */
        *out = TAG_SMI((intptr_t)space_seq());
        return 1;
    }
    case PRIM_pf_spaceStamp: {
        /* porta space-sync — `platform spaceStamp` -> the counter, BUMPED.
           See prim_ids.h: reading instead of bumping loses captured entries. */
        *out = TAG_SMI((intptr_t)space_stamp());
        return 1;
    }
    case PRIM_pf_spaceChangedSince: {
        /* porta space-sync — `platform spaceChangedSince: cursor under: prefix
           max: n`. args[0] = cursor SMI, args[1] = prefix String, args[2] = max
           SMI. Answers a raw indexable of 3-slot indexables
           {pathString, seqSMI, value}, ascending by seq.

           GC-hazard discipline (space_dequeue's, and for the same reason — this
           collector is precise and every indexable_new/space_make_string below
           can collect): the OUTER array is allocated first and pushed as a temp
           root, and each row is stored INTO it before that row's own path
           String is allocated. Nothing heap-shaped is ever held only in a C
           local across an allocation. The cell values need no pinning: the
           channel table roots them via space_gc_trace. */
        oop prefix = args[1];
        if (prefix == NULL || IS_SMI(prefix) || StringVtable == NULL ||
            obj_vtable(prefix) != StringVtable) {
            fprintf(stderr, "VM Error: platform spaceChangedSince:under:max: expects a String prefix\n");
            abort();
        }
        struct String *p = (struct String *)prefix;
        intptr_t want = UNTAG_SMI(args[2]);
        if (want < 0) want = 0;
        if (want > OEVM_SPACE_MAX_CHANNELS) want = OEVM_SPACE_MAX_CHANNELS;

        int ids[OEVM_SPACE_MAX_CHANNELS];
        int n = space_changed_since((uint32_t)UNTAG_SMI(args[0]),
                                    (const char *)p->bytes, p->length,
                                    ids, (int)want);

        extern void gc_temp_root_push(oop o);
        extern int gc_temp_roots_count;
        int saved = gc_temp_roots_count;

        oop rows = indexable_new(n);
        gc_temp_root_push(rows);
        for (int i = 0; i < n; i++) {
            oop row = indexable_new(3);
            ((struct ObjectInstance *)rows)->slots[i + 1] = row;   /* rooted now */
            size_t plen = 0;
            const char *path = space_path_of(ids[i], &plen);
            ((struct ObjectInstance *)row)->slots[1] = space_make_string_pub(path, plen);
            ((struct ObjectInstance *)row)->slots[2] = TAG_SMI((intptr_t)space_last_seq_of(ids[i]));
            ((struct ObjectInstance *)row)->slots[3] = space_value_of(ids[i]);
        }
        gc_temp_roots_count = saved;
        *out = rows;
        return 1;
    }
```

`space_make_string` is file-static in `space.c`. Rather than duplicating a String
constructor in `platform.c`, export it: in `space.c` drop the `static` and rename to
`space_make_string_pub`, and declare it in `space.h` beside the scan:

```c
/* The registry's own String constructor, exported for the spaceChangedSince:
   prim so the path String is built exactly as space_dequeue builds it (one
   allocation idiom, one place to review for GC hazards). */
oop space_make_string_pub(const char *bytes, size_t len);
```

Update `space_dequeue`'s call site to the new name in the same edit.

- [ ] **Step 6: Run the C gate to green**

```bash
cd ~/workspace/tuvm && tools/run_c_tests.sh
```
Expected: every line PASS. A `-1` sized array error means the parity guard was not
advanced in Step 4 — fix that, do not silence it.

- [ ] **Step 7: Commit**

```bash
cd ~/workspace/tuvm
git add src/prim_ids.h src/dispatch.c src/platform.c src/space.c src/space.h tests/test_platform.c
git commit -m "feat(prims): spaceSeq / spaceStamp / spaceChangedSince:under:max: (138-140, porta#25)"
```

---

### Task 3: `Channel>>changedSince:max:` and `Space>>seq` / `stamp` (nsl)

**Files:**
- Modify: `nsl-tuvm/lib/kernel.nsl` (`class Channel`, `class Space`)
- Create: `nsl-tests/22_spacesync/01_seq_and_scan.nsl`,
  `nsl-tests/22_spacesync/02_scan_confinement.nsl`

**Interfaces:**
- Consumes: prims 138–140.
- Produces:
  - `Channel>>changedSince: cursor max: n` → an `OrderedCollection` of 3-element
    algebra `List`s `{absolutePath, seq, value}`, ascending by seq, scoped to the
    capability's own root.
  - `Space>>seq` → the counter; `Space>>stamp` → the counter, bumped.

- [ ] **Step 1: Write the failing tests**

`nsl-tests/22_spacesync/01_seq_and_scan.nsl`:

```
(*
porta space-sync (plan 2) — the space's monotonic seq and the changed-since scan,
from nsl. The scan is the cell-replication query: it answers CELLS ONLY, ascending
by seq, cursor-exclusive, and each row carries the value so a caller never needs a
second peek. A stream's seq advances too (every write is stamped) but a stream can
never ride this scan — its value is killed on delivery.
expect:
seq advances 3
scan 2
first obs/health
ascending true
stream not scanned true
cursor exclusive 0
stamp strictly increasing true
*)
class SeqAndScan packageUsing: manifest = () (
  public main: platform args: args = (
    | space obs rows s0 s1 |
    space:: platform space.
    s0:: space seq.
    space put: 'obs/health' value: 11.
    space out: 'tel/log' value: 'hello'.
    space put: 'obs/name' value: 'bench'.
    s1:: space seq.
    ('seq advances ' , (s1 - s0) printString) out.

    obs:: space channel: 'obs'.
    rows:: obs changedSince: 0 max: 8.
    ('scan ' , rows size printString) out.
    ('first ' , ((rows at: 1) at: 1)) out.
    ('ascending ' , (((rows at: 1) at: 2) < ((rows at: 2) at: 2)) printString) out.

    (* the tel/log stream was written between the two cells, so a root-scoped scan
       would see it if streams rode the scan at all *)
    rows:: space changedSince: 0 max: 8.
    ('stream not scanned ' , (rows size = 2) printString) out.

    ('cursor exclusive ' , (space changedSince: space seq max: 8) size printString) out.
    ('stamp strictly increasing ' , (space stamp < space stamp) printString) out.
  )
) : (
)
```

`nsl-tests/22_spacesync/02_scan_confinement.nsl`:

```
(*
porta space-sync (plan 2) — the scan obeys A1 confinement, and it obeys it the same
way every other Channel verb does: by resolving against the capability's own root
before the prim is ever reached. A capability rooted at `obs` cannot learn that
`goal/name` was written, and — the segment-alignment rule `resolve:` already enforces
for paths, restated for prefixes — it does not match `obscure` either.
Rows carry ABSOLUTE paths: they are what goes on the wire, and a capability that can
already address a name is not told anything new by being told its full name.
expect:
obs sees 1
obs path obs/health
obscure not matched true
root sees 3
*)
class ScanConfinement packageUsing: manifest = () (
  public main: platform args: args = (
    | space obs rows |
    space:: platform space.
    space put: 'goal/name' value: 'bench'.
    space put: 'obs/health' value: 11.
    space put: 'obscure' value: 99.

    obs:: space channel: 'obs'.
    rows:: obs changedSince: 0 max: 8.
    ('obs sees ' , rows size printString) out.
    ('obs path ' , ((rows at: 1) at: 1)) out.
    ('obscure not matched ' , (rows size = 1) printString) out.

    rows:: space changedSince: 0 max: 8.
    ('root sees ' , rows size printString) out.
  )
) : (
)
```

- [ ] **Step 2: Run them and watch them fail**

```bash
cd ~/workspace/nsl-tuvm
python3 tools/run-one.py ../nsl-tests/22_spacesync/01_seq_and_scan.nsl B
python3 tools/run-one.py ../nsl-tests/22_spacesync/02_scan_confinement.nsl B
```
Expected: both FAIL with `Message 'changedSince:max:' not understood`.

- [ ] **Step 3: Add the verbs to `kernel.nsl`**

In `class Channel`, after `peek: rel ifAbsent:` and before `whenever:do:`:

```
    (* ---- porta space-sync (plan 2): the cell-replication query --------------
       Answer every CELL at or under this capability's root whose last write is
       newer than `cursor`, oldest first, at most `n` of them — each row a
       3-element algebra List {absolutePath, seq, value}.

       Confinement is enforced the way every other verb here enforces it: the
       scan prefix is `root` itself, resolved before the prim is reached, so a
       capability can only ever be told about names it can already address.
       Segment alignment comes from the C side (`under_prefix`), matching the
       rule `resolve:` obeys for whole paths — `obs` never matches `obscure`.

       CELLS ONLY. A stream's seq advances on every write, but its value is
       killed on delivery, so a stream row would carry a stale-or-nil value
       under a fresh seq. Streams reach the wire by reaction capture instead
       (`PortaBridge>>subscribeStream:`).

       ASCENDING is load-bearing: a caller sends a prefix of this list and moves
       its cursor to the last row it sent, so any other order strands whatever
       it skipped. `max` is how a caller keeps a batch inside the 512 B cap.

       Paths are ABSOLUTE (space-relative), not root-relative — they are what
       goes on the wire, and PROTOCOL-NSL D4.5 stores them node-relative with
       the node id beside them, never re-prefixed per capability. *)
    public changedSince: cursor max: n = (
      | raw rows i |
      raw:: platform spaceChangedSince: cursor under: root max: n.
      rows:: OrderedCollection new.
      i:: 1.
      [ i <= raw basicSize ] whileTrue: [
        rows add: (self rowOf: (raw basicAt: i)).
        i:: i + 1 ].
      ^ rows )
    (* The prim answers a bare `indexable_new` backing store (class-less vtable),
       exactly as `spaceDequeue` does, so it is consumed ONLY via the shape-gated
       basicAt:/basicSize and never escapes as itself — `Space>>drain`'s rule. *)
    rowOf: raw = (
      ^ List with: (raw basicAt: 1) with: (raw basicAt: 2) with: (raw basicAt: 3) )
```

In `class Space` (the root capability), after `drain`:

```
    (* porta space-sync (plan 2): the space's ONE monotonic counter (design D5).
       `seq` reads it; `stamp` bumps it and answers the new value. These are on
       Space and not on Channel because the counter is space-global — an
       attenuated capability has no narrower counter to read, so offering it one
       would be a lie about the scope.

       `stamp` exists for captured stream entries: a stream's value is not
       retained, so PortaBridge captures it in a `whenever:` reaction, and two
       captures with no intervening write would share a seq if it merely READ
       the counter. The gateway drops `seq <= hw` as a duplicate, so that would
       be silent loss, not a repeated delivery. *)
    public seq = ( ^ platform spaceSeq )
    public stamp = ( ^ platform spaceStamp )
```

- [ ] **Step 4: Run the tests to green**

```bash
cd ~/workspace/nsl-tuvm
python3 tools/run-one.py ../nsl-tests/22_spacesync/01_seq_and_scan.nsl B
python3 tools/run-one.py ../nsl-tests/22_spacesync/02_scan_confinement.nsl B
```
Expected: both PASS.

- [ ] **Step 5: Commit**

```bash
cd ~/workspace/nsl-tuvm && git add lib/kernel.nsl && \
  git commit -m "feat(channel): changedSince:max: and the space's seq/stamp (porta#25)"
cd ~/workspace/nsl-tests && git add 22_spacesync && \
  git commit -m "test(spacesync): the seq counter, the cell scan, and its confinement"
```

---

### Task 4: `class PortaBridge` — the exchange, in nsl

**Files:**
- Modify: `nsl-tuvm/lib/kernel.nsl` (new class + accessor)
- Create: `nsl-tests/22_spacesync/03_bridge_frames.nsl`,
  `nsl-tests/22_spacesync/04_bridge_authority.nsl`

**Interfaces:**
- Consumes: `Channel>>changedSince:max:`, `Space>>stamp`, `Cbor`, `Record`, `Map`,
  `List`, `OrderedCollection`, `Socket` (Task 3 and the existing kernel).
- Produces, on `platform PortaBridge` (the CLASS — the node holds one instance):
  - `socket: s space: sp id: idString peer: host port: p` (class-side ctor)
  - `subscribeStream: absolutePath` — capture that stream's writes for push
  - `pullFrame` → Bytes; `applyBatch: aBytes` → Boolean (applied?)
  - `pushFrame` → Bytes or nil (nil = nothing to push); `applyAck: aBytes` → Boolean
  - `checkIn` — the whole exchange over the socket, bounded, never wedging
  - `cursor`, `acked`, `rejected`, `misses` — observability, all SMIs

- [ ] **Step 1: Write the failing frame tests**

`nsl-tests/22_spacesync/03_bridge_frames.nsl`:

```
(*
porta space-sync (plan 2) — the bridge's frames, with no socket in sight. The
exchange is bytes-in/bytes-out, so every interesting property is testable on the
host without a network: a #pull carries the cursor the node has ALREADY applied,
a #batch is applied to the local space and moves the cursor to `hi`, a #push
carries obs cells and captured stream entries in ONE ascending seq sequence, and
an #ack drops exactly the pending entries the gateway accepted.

A decoded Symbol is NOT the image's interned Symbol (tuvm#75), so every tag test
in the bridge uses `=` and never `==` — asserted here by feeding it a batch built
through a real encode/decode round trip.
expect:
pull tag true
pull cursor 0
applied true
goal name bench
cursor 7
push entries 2
push ascending true
acked drops pending 0
err is not a batch false
*)
class BridgeFrames packageUsing: manifest = () (
  public main: platform args: args = (
    | space bridge pull batch push rows |
    space:: platform space.
    bridge:: platform PortaBridge socket: nil space: space id: 'n1' peer: '::1' port: 6969.

    (* --- the pull frame --- *)
    pull:: platform Cbor decode: bridge pullFrame ifInvalid: [ :m | nil ].
    (* `=` and not `printString`: printing a Symbol MASKS the identity question,
       and `=` is the comparison the bridge itself must get right (tuvm#75 — a
       decoded Symbol is not the image's interned one, so `==` never matches). *)
    ('pull tag ' , (pull tag = #pull) printString) out.
    ('pull cursor ' , (pull at: #cursor ifAbsent: [ -1 ]) printString) out.

    (* --- a batch, built exactly as porta builds it, applied --- *)
    batch:: platform Cbor encode:
      (platform Record tag: #batch
         with: #hi -> 7
         with: #entries -> (platform List with:
            (platform List with: 7 with: 'goal/name' with: 2 with: 'bench'))).
    ('applied ' , (bridge applyBatch: batch) printString) out.
    ('goal name ' , (space peek: 'goal/name')) out.
    ('cursor ' , bridge cursor printString) out.

    (* --- the push frame: one obs cell and one captured stream entry --- *)
    bridge subscribeStream: 'tel/log'.
    space put: 'obs/health' value: 11.
    space out: 'tel/log' value: 'up'.
    space drain.                          (* the capture is a reaction *)
    push:: platform Cbor decode: bridge pushFrame ifInvalid: [ :m | nil ].
    rows:: push at: #entries ifAbsent: [ nil ].
    ('push entries ' , rows size printString) out.
    ('push ascending ' , (((rows at: 1) at: 1) < ((rows at: 2) at: 1)) printString) out.

    (* --- the ack: pending stream entries at or below hw are done --- *)
    bridge applyAck: (platform Cbor encode:
      (platform Record tag: #ack with: #hw -> space seq)).
    ('acked drops pending ' , bridge pendingSize printString) out.

    (* --- an #err reply is not a batch and must not move the cursor --- *)
    ('err is not a batch ' , (bridge applyBatch: (platform Cbor encode:
       (platform Record tag: #err with: #msg -> 'malformed'))) printString) out.
  )
) : (
)
```

`nsl-tests/22_spacesync/04_bridge_authority.nsl`:

```
(*
porta space-sync (plan 2) — inbound prefix authority (N5). porta writes goal/** and
dbg/req; that is the whole of its authority, and the node is where it is enforced,
because the node is the thing that would be lied to. An entry at obs/** would forge
the convergence view — the node's own report of what it observes — so it is counted
and dropped, never applied. A malformed batch is dropped whole, not half-applied.
The node survives all of it: this is untrusted network input, the NetBridge rule.
expect:
goal applied bench
obs rejected true
obs untouched true
rejected 2
garbage survived false
still alive true
*)
class BridgeAuthority packageUsing: manifest = () (
  public main: platform args: args = (
    | space bridge bytes |
    space:: platform space.
    bridge:: platform PortaBridge socket: nil space: space id: 'n1' peer: '::1' port: 6969.

    bytes:: platform Cbor encode:
      (platform Record tag: #batch
         with: #hi -> 3
         with: #entries -> (platform List
            with: (platform List with: 1 with: 'goal/name' with: 2 with: 'bench')
            with: (platform List with: 2 with: 'obs/health' with: 2 with: 99)
            with: (platform List with: 3 with: 'sys/panic' with: 1 with: 'boo'))).
    bridge applyBatch: bytes.
    ('goal applied ' , (space peek: 'goal/name')) out.
    ('obs rejected ' , ((space peek: 'obs/health') == nil) printString) out.
    (* `peek:ifAbsent:` and not a kind probe: `spaceKind:` has no Kernel-instance
       forwarder, so an app cannot reach it — and the ifAbsent: branch is the
       stronger statement anyway, since it fires only on a name NOBODY has ever
       written, which is exactly the claim. *)
    ('obs untouched ' , ((space peek: 'obs/health' ifAbsent: [ #ABSENT ]) = #ABSENT) printString) out.
    ('rejected ' , bridge rejected printString) out.

    ('garbage survived ' , (bridge applyBatch: (platform ByteArray new: 1) freeze) printString) out.
    ('still alive ' , true printString) out.
  )
) : (
)
```

- [ ] **Step 2: Run them and watch them fail**

```bash
cd ~/workspace/nsl-tuvm
python3 tools/run-one.py ../nsl-tests/22_spacesync/03_bridge_frames.nsl B
python3 tools/run-one.py ../nsl-tests/22_spacesync/04_bridge_authority.nsl B
```
Expected: both FAIL — `Message 'PortaBridge' not understood`.

- [ ] **Step 3: Add `class PortaBridge` to `kernel.nsl`**

Place it immediately after `class NetBridge` (its sibling in kind: a reactive
channel↔UDP bridge), before the `serialOn:` / `netOn:` accessors.

```
  (* porta space-sync, plan 2 — the node half of the check-in exchange
     (porta/docs/design/2026-08-01-porta-space-sync-rewrite-design.md §5/§6).

     Two node-initiated CBOR datagrams over plain UDP, both idempotent:

       PULL { id, cursor, bootId, hw }  ->  BATCH { hi, entries }
       PUSH { id, bootId, entries }     ->  ACK   { hw }

     entries are [ seq, path, kind, value ], kind 1 = stream, 2 = cell.

     THERE IS NO RETRANSMIT LAYER, deliberately: the exchange is idempotent by
     cursor in both directions, so a lost pull re-pulls the same range next
     check-in and a lost push is re-sent and deduped on the gateway's node-seq
     high-water. Loss is absorbed by convergence.

     WHAT THIS HOLDS, and why each is here rather than elsewhere:
       `space`  — the ROOT capability. porta writes goal/** and dbg/req, the node
                  reports obs/**+tel/**+sys/**, and one object addressing both is
                  simpler than two capabilities until the grant machinery (G5)
                  can express the pair. Authority is enforced in `mayApply:`.
       `cursor` — porta's OUT-sequence, the last seq this node has APPLIED. RAM
                  only in this slice: cold boot = cursor 0 = full replay is the
                  designed behaviour (PROTOCOL-NSL §3), and NVS is a later
                  optimisation, not a prerequisite.
       `acked`  — the high-water the gateway has ACCEPTED from us, in the LOCAL
                  space's seq (design D5: one counter per space per side). Cells
                  newer than this are re-scanned every check-in; pending stream
                  entries at or below it are done.
       `pending`— captured stream entries not yet acked. A stream's value is
                  killed on delivery, so it cannot be re-read later the way a
                  cell can — capture is the only chance.

     A crash of this job is a CONTAINED fault: porta sees a late check-in and
     nothing worse. Nothing here may take the node down — every decode failure,
     every unexpected tag, every out-of-authority path is a counted drop. *)
  class PortaBridge = (
    | socket space nodeId peerHost peerPort
      cursor ::= 0. acked ::= 0. rejected ::= 0. misses ::= 0.
      pending ::= OrderedCollection new. |
  ) (
    setSocket: s space: sp id: anId peer: h port: p = (
      socket:: s. space:: sp. nodeId:: anId. peerHost:: h. peerPort:: p. ^ self )

    (* bootId rides the batch envelope, not every record (RECORDS §3). It is a
       source constant in this slice — persisting a boot counter is NVS work
       (G7) and porta#27 owns node identity. porta stores it and uses it to
       order observations across reboots; a constant makes that ordering
       degenerate, which is honest for a node that cannot yet count its boots. *)
    bootId = ( ^ 1 )
    (* RECORDS §2.2: a 512 B datagram, a 256 B value. Both are radio constraints
       (6LoWPAN fragments at ~102 B and a datagram survives only if every
       fragment does), not preferences, and porta rejects a push that breaks
       either — so the trimming below is what keeps the node from talking
       itself out of the fleet. *)
    maxBatchBytes = ( ^ 512 )
    maxValueBytes = ( ^ 256 )
    maxRows = ( ^ 8 )
    replyBudget = ( ^ 2000 )   (* ms to wait for one reply before scoring a miss *)

    public cursor = ( ^ cursor )
    public acked = ( ^ acked )
    public rejected = ( ^ rejected )
    public misses = ( ^ misses )
    public pendingSize = ( ^ pending size )

    (* ---- outbound stream capture (plan-2 N2) ------------------------------
       A cell can be re-read at check-in time; a stream entry cannot — the
       reactor kills the event on delivery. So each outbound stream path is
       subscribed here and its writes are captured into `pending`, stamped with
       `space stamp` so they share ONE ascending sequence with the cell writes.
       The block body is a single self-send: a direct cross-class call inside a
       block the reactor stores does not resolve its outer context on this VM
       (NetBridge>>encodeFor: documents the same workaround). *)
    public subscribeStream: absolutePath = (
      (space channel: absolutePath) whenever: '' do: [ :v | self capture: absolutePath value: v ].
      ^ self )
    capture: absolutePath value: v = (
      pending add: (List with: space stamp with: absolutePath with: 1 with: v).
      ^ self )

    (* ---- the pull half ---------------------------------------------------- *)
    public pullFrame = (
      ^ Cbor new encode: (Record tag: #pull
          with: #id -> nodeId
          with: #cursor -> cursor
          with: #bootId -> self bootId
          with: #hw -> acked) )

    (* Apply one #batch. Answers whether it WAS a batch — an #err reply, a
       malformed datagram, or anything else answers false and moves nothing.
       `=` and never `==` on the tag: a decoded Symbol is not the image's
       interned one (tuvm#75), and `==` would silently never match. *)
    public applyBatch: aBytes = (
      | bad r entries i hi |
      bad:: false.
      r:: Cbor new decode: aBytes ifInvalid: [ :m | bad:: true. nil ].
      bad ifTrue: [ ^ false ].
      (outer rankOf: r) = 9 ifFalse: [ ^ false ].
      (r tag = #batch) ifFalse: [ ^ false ].
      entries:: r at: #entries ifAbsent: [ nil ].
      (outer rankOf: entries) = 7 ifFalse: [ ^ false ].
      i:: 1.
      [ i <= entries size ] whileTrue: [
        self applyEntry: (entries at: i).
        i:: i + 1 ].
      hi:: r at: #hi ifAbsent: [ cursor ].
      (outer rankOf: hi) = 2 ifTrue: [ hi > cursor ifTrue: [ cursor:: hi ] ].
      ^ true )

    (* One inbound entry. Everything here is untrusted: a wrong shape, a wrong
       kind or an out-of-authority path is COUNTED AND DROPPED, never applied,
       and never raised — a node that crashes on a malformed datagram is a node
       an attacker can switch off. *)
    applyEntry: row = (
      | path kind value |
      (outer rankOf: row) = 7 ifFalse: [ rejected:: rejected + 1. ^ self ].
      row size = 4 ifFalse: [ rejected:: rejected + 1. ^ self ].
      path::  row at: 2.
      kind::  row at: 3.
      value:: row at: 4.
      (outer rankOf: path) = 4 ifFalse: [ rejected:: rejected + 1. ^ self ].
      (self mayApply: path) ifFalse: [ rejected:: rejected + 1. ^ self ].
      kind = 2 ifTrue: [ space put: path value: value. ^ self ].
      kind = 1 ifTrue: [ space out: path value: value. ^ self ].
      rejected:: rejected + 1.
      ^ self )

    (* porta's authority, enforced where it can be believed: `goal/**` and
       `dbg/req`, and nothing else. An accepted write to `obs/**` would forge the
       node's own report of what it observes, which is the one thing the
       convergence view rests on. *)
    mayApply: path = (
      (path = 'dbg/req') ifTrue: [ ^ true ].
      (path = 'goal') ifTrue: [ ^ true ].
      ^ self path: path startsWith: 'goal/' )

    (* No `startsWith:` exists in this dialect (grep-confirmed against kernel.nsl
       and dispatch.c's PRIM_NAMES) — the same finding `Space>>isSysPath:` and
       `SupervisedJob>>addresses:` already record. Byte compare over size/at:. *)
    path: p startsWith: prefix = (
      | n i |
      n:: prefix size.
      p size < n ifTrue: [ ^ false ].
      i:: 1.
      [ i <= n ] whileTrue: [
        (p at: i) = (prefix at: i) ifFalse: [ ^ false ].
        i:: i + 1 ].
      ^ true )

    (* ---- the push half ---------------------------------------------------- *)
    (* Answer the encoded #push, or nil when there is nothing to send. Cells come
       from the scan (obs/** newer than `acked`), stream entries from `pending`;
       both carry seqs from the ONE space counter, so merging them ascending is
       a sort and not a reconciliation. The result is then TRIMMED FROM THE TAIL
       until the datagram fits: entries dropped here are not lost, they ride the
       next check-in, whereas an oversize datagram is lost on the mesh. *)
    public pushFrame = (
      | rows bytes |
      rows:: self collectOutbound.
      rows isEmpty ifTrue: [ ^ nil ].
      bytes:: self encodePush: rows.
      [ bytes size > self maxBatchBytes and: [ rows size > 1 ] ] whileTrue: [
        rows removeLast.
        bytes:: self encodePush: rows ].
      bytes size > self maxBatchBytes ifTrue: [ ^ nil ].
      ^ bytes )

    encodePush: rows = (
      ^ Cbor new encode: (Record tag: #push
          with: #id -> nodeId
          with: #bootId -> self bootId
          with: #entries -> (List fromCollection: rows)) )

    collectOutbound = (
      | rows cells i r |
      rows:: OrderedCollection new.
      i:: 1.
      [ i <= pending size ] whileTrue: [ rows add: (pending at: i). i:: i + 1 ].
      cells:: (space channel: 'obs') changedSince: acked max: self maxRows.
      i:: 1.
      [ i <= cells size ] whileTrue: [
        r:: cells at: i.
        (self valueFits: (r at: 3))
          ifTrue: [ rows add: (List with: (r at: 2) with: (r at: 1) with: 2 with: (r at: 3)) ]
          ifFalse: [ rejected:: rejected + 1 ].
        i:: i + 1 ].
      ^ self sortedBySeq: rows )

    (* An oversize value is rejected by porta for the WHOLE push, so one bad cell
       would stall every good one behind it. Drop it here, count it, keep going —
       the tuvm#107 shape (a value whose size scales with a count) is exactly
       what this catches if it ever reaches a sync path. *)
    valueFits: v = ( ^ (Cbor new encode: v) size <= self maxValueBytes )

    sortedBySeq: rows = (
      | i j cur |
      i:: 2.
      [ i <= rows size ] whileTrue: [
        cur:: rows at: i.
        j:: i - 1.
        [ j >= 1 and: [ ((rows at: j) at: 1) > (cur at: 1) ] ] whileTrue: [
          rows at: j + 1 put: (rows at: j).
          j:: j - 1 ].
        rows at: j + 1 put: cur.
        i:: i + 1 ].
      ^ rows )

    (* Apply one #ack. `hw` is what the gateway ACCEPTED, so pending entries at
       or below it are done and cells at or below it need not be re-scanned. A
       trimmed push acks lower than we offered — that is correct and it is why
       the cursor is taken from the reply and never from what we sent. *)
    public applyAck: aBytes = (
      | bad r hw keep i row |
      bad:: false.
      r:: Cbor new decode: aBytes ifInvalid: [ :m | bad:: true. nil ].
      bad ifTrue: [ ^ false ].
      (outer rankOf: r) = 9 ifFalse: [ ^ false ].
      (r tag = #ack) ifFalse: [ ^ false ].
      hw:: r at: #hw ifAbsent: [ nil ].
      (outer rankOf: hw) = 2 ifFalse: [ ^ false ].
      hw > acked ifTrue: [ acked:: hw ].
      keep:: OrderedCollection new.
      i:: 1.
      [ i <= pending size ] whileTrue: [
        row:: pending at: i.
        (row at: 1) > acked ifTrue: [ keep add: row ].
        i:: i + 1 ].
      pending:: keep.
      ^ true )

    (* ---- one check-in ----------------------------------------------------- *)
    (* PULL, apply, PUSH, ack. Each leg waits a bounded time and then GIVES UP,
       counting a miss: a node that blocks forever on a silent gateway is a node
       that has stopped doing its actual job. The next check-in re-pulls the same
       range and re-pushes the same entries, so a miss costs latency and nothing
       else. *)
    public checkIn = (
      | reply |
      socket == nil ifTrue: [ ^ self ].
      socket sendTo: peerHost port: peerPort data: self pullFrame.
      reply:: self awaitReply.
      reply == nil
        ifTrue: [ misses:: misses + 1 ]
        ifFalse: [ self applyBatch: reply ].
      ^ self push )

    push = (
      | frame reply |
      frame:: self pushFrame.
      frame == nil ifTrue: [ ^ self ].
      socket sendTo: peerHost port: peerPort data: frame.
      reply:: self awaitReply.
      reply == nil
        ifTrue: [ misses:: misses + 1 ]
        ifFalse: [ self applyAck: reply ].
      ^ self )

    awaitReply = (
      | deadline dg |
      deadline:: platform monotonicMillis + self replyBudget.
      [ platform monotonicMillis < deadline ] whileTrue: [
        dg:: socket receive.
        dg == nil ifFalse: [ ^ dg data ] ].
      ^ nil )
  ) : (
    public socket: s space: sp id: anId peer: h port: p = (
      ^ self new setSocket: s space: sp id: anId peer: h port: p )
  )
```

Add the accessor beside `NetBridge`'s:

```
  (* porta space-sync (plan 2). Answers the CLASS, not an instance — `platform
     PortaBridge socket:space:id:peer:port:` reaches the class-side constructor,
     and the node must HOLD the one instance it gets: the cursor, the ack
     high-water and the pending capture queue all live on it, so re-fetching the
     accessor would silently start a second, empty bridge. (The `platform timers`
     lesson, which cost a 9h23m run.) *)
  public PortaBridge       = ( ^ PortaBridge )
```

- [ ] **Step 4: Run the tests to green**

```bash
cd ~/workspace/nsl-tuvm
python3 tools/run-one.py ../nsl-tests/22_spacesync/03_bridge_frames.nsl B
python3 tools/run-one.py ../nsl-tests/22_spacesync/04_bridge_authority.nsl B
```
Expected: both PASS. If `03` reports `push entries 1`, the capture reaction did not
fire — check that the test calls `space drain` after the `out:`, and that
`subscribeStream:` ran before the write.

- [ ] **Step 5: Run the whole nsl suite**

```bash
cd ~/workspace/nsl-tuvm && python3 tools/run_tests_oevm.py 2>&1 | tail -20
```
Expected: 0 failed **and** every aux gate line PASS — read the gate lines, not just
the count (tuvm#63: the suite prints "0 failed" while an aux gate is red).

- [ ] **Step 6: Commit**

```bash
cd ~/workspace/nsl-tuvm && git add lib/kernel.nsl && \
  git commit -m "feat(bridge): PortaBridge — the node half of the space-sync exchange (porta#25)"
cd ~/workspace/nsl-tests && git add 22_spacesync && \
  git commit -m "test(spacesync): bridge frames, and porta's inbound authority"
```

---

### Task 5: `PortaBridgeNode.nsl` — the node

**Files:**
- Create: `tuvm/zephyr/PortaBridgeNode.nsl`

**Interfaces:**
- Consumes: `platform PortaBridge`, `platform timers`, `platform space`,
  `platform heapStats`, `net udpBindPort:`.
- Produces: a node that binds a UDP socket, checks in on a fixed cadence, publishes
  `obs/health` + `obs/build`, echoes `goal/name` to `obs/name`, and writes a
  `tel/log` line per check-in. Constants: node id `bench-node`, gateway
  `127.0.0.1:16969`, cadence 2000 ms — all overridden per-board in plan 3.

A fresh minimal node, deliberately **not** the shipsBells rig — that one is entangled
with the DS3231, the strike driver and #105's boot-energise defect (design §6).

- [ ] **Step 1: Write the node**

```
(*
PortaBridgeNode — the space-sync check-in loop, minimal. Binds a UDP socket, holds
ONE PortaBridge, and on each tick: check in with the gateway, then publish what this
node observes. Nothing else is on this node on purpose — the gate is the protocol,
and a second concern would make a failure ambiguous.

The convergence demo is one line and it is the whole argument: `goal/name` arrives
from porta, `obs/name` is written back, and `porta get` shows the two agree. There is
no reconcile mechanism anywhere in this file — the diff IS the view.

Cadence is a fixed 2 s here. PROTOCOL-NSL §4.4's adaptive controller (freshness and
request-latency pull up, fleet load pulls down, liveness sets the floor) is node
POLICY and lands with the mode record; a constant is the honest placeholder.

Dialect: `[ ... ] whileTrue:`, never `repeat`; state in instance slots so every
reaction block is a one-line self-send (the LD_SLOT gotcha).
expect (console):
PORTA NODE READY
*)
class PortaBridgeNode usingPlatform: platform net: net = (
  | space bridge timers ticks ::= 0. |
) (
  nodeId    = ( ^ 'bench-node' )
  gwAddr    = ( ^ '127.0.0.1' )
  gwPort    = ( ^ 16969 )
  localPort = ( ^ 16970 )
  cadence   = ( ^ 2000 )   (* ms between check-ins *)
  (* RECORDS §4.2 / design D8: build provenance, so an agent can tell which code
     is on which node — tuvm#74, a labelled revenue-gate. `chip` is a source
     constant because it is a build fact; `sdk`, `image` and `builtAt` are
     OMITTED until tuvm#74 gives the node a way to name its own build, and
     absence means "the node cannot say", which is honest and is not a default. *)
  chip      = ( ^ #host )

  public main: platform2 args: args = (
    | sock tickChan |
    space:: platform space.
    sock:: net udpBindPort: self localPort.
    sock == nil ifTrue: [ ^ self fail: 'bind' ].
    bridge:: platform PortaBridge
      socket: sock space: space id: self nodeId peer: self gwAddr port: self gwPort.
    bridge subscribeStream: 'tel/log'.
    bridge subscribeStream: 'sys/reset'.
    (* NOT diag/spans/result: SpanCapture publishes ~1120 B against the 256 B cap
       (tuvm#107). It must not be synced until that value is split into sibling
       cells at diag/spans/<span>. *)

    (* porta's writes land as ordinary cell writes, so the echo is an ordinary
       reaction — there is no protocol handler anywhere in this node. *)
    (space channel: 'goal/name') whenever: '' do: [ :v | self nameChanged: v ].

    space put: 'obs/build' value: (platform Record tag: #Build with: #chip -> self chip).

    timers:: platform timers.     (* hold ONE instance — it owns the pending registry *)
    space whenever: 'sys/timer' do: [ :e | timers dispatch: e ].
    tickChan:: space channel: 'sys/tick'.
    tickChan whenever: '' do: [ :m | self tick ].
    timers every: self cadence post: #go on: tickChan.

    'PORTA NODE READY' println.
    [ true ] whileTrue: [
      platform pumpEvents.    (* blocks until the timer fires *)
      space drain ].
    ^ 0
  )

  (* One check-in, then one report. In that order: applying goal/** first means the
     health this node publishes describes the state porta just asked for, not the
     one before it. *)
  tick = (
    ticks:: ticks + 1.
    bridge checkIn.
    self publishHealth.
    space out: 'tel/log' value:
      (platform Record tag: #Log with: #level -> #info
                               with: #text -> ('tick ' , ticks printString)).
    ^ self
  )

  nameChanged: v = ( space put: 'obs/name' value: v. ^ self )

  (* RECORDS §4.2's Health. heapFree/heapLargest come from heapStats (prim 104):
     largestFreeBlock is the fragmentation signal — the number that says a
     years-long node is dying slowly, which heapFree alone will not.
     wakes/pollTimeouts are 0 until the mode record gives them meaning. *)
  publishHealth = (
    | h pairs |
    h:: platform heapStats.
    pairs:: platform OrderedCollection new.
    pairs add: #uptimeMs -> platform monotonicMillis.
    pairs add: #bootId -> 1.
    pairs add: #wakes -> ticks.
    pairs add: #pollTimeouts -> bridge misses.
    pairs add: #heapFree -> (h heapSize - h liveBytes).
    pairs add: #heapLargest -> h largestFreeBlock.
    pairs add: #overruns -> (platform spaceOverruns: '').
    space put: 'obs/health' value:
      (platform Record tag: #Health fields: (platform Map fromPairs: pairs)).
    ^ self
  )

  fail: msg = ( msg println. ^ 1 )
) : (
  public packageUsing: manifest = ( ^ self new )
)
```

- [ ] **Step 2: Compile it — a syntax and seam check before any gate**

```bash
cd ~/workspace/tuvm
gcc -std=c99 -Isrc tools/dump_layout.c -o /tmp/dl && /tmp/dl > /tmp/layout.json
python3 ~/workspace/nsl-tuvm/tools/ns_compile_oevm.py \
  zephyr/PortaBridgeNode.nsl /tmp/porta_node.oimg --format=oimg --layout=/tmp/layout.json
```
Expected: it writes `/tmp/porta_node.oimg` with no diagnostics. A `not understood`
here means an accessor is missing from `kernel.nsl` — an app's `platform` is the
KERNEL INSTANCE, so every class it names needs an explicit accessor.

- [ ] **Step 3: Commit**

```bash
cd ~/workspace/tuvm && git add zephyr/PortaBridgeNode.nsl && \
  git commit -m "feat(node): PortaBridgeNode — the minimal space-sync check-in node (porta#25)"
```

---

### Task 6: The host interop gate — this node against real `porta serve`

**Files:**
- Create: `tuvm/tests/porta_sync_host.sh`

**Interfaces:**
- Consumes: `zephyr/PortaBridgeNode.nsl`, the posix VM link set, and
  `~/workspace/porta` (plan 1's `porta serve` / `put` / `get` / `log`).
- Produces: the gate that closes plan 2 — two independent implementations of one
  protocol agreeing over a real socket, with no hardware involved.

This is a **cross-repo** gate and is deliberately NOT wired into
`run_tests_oevm.py`: the tuvm suite must not fail because a sibling repo is absent.
It skips with a stated reason instead, and plan 3 runs it as its host-side precursor.

- [ ] **Step 1: Write the gate**

```bash
#!/usr/bin/env bash
# porta space-sync plan 2, the host interop gate: PortaBridgeNode.nsl (nsl/tuvm)
# against a real `porta serve` (Go), over real UDP loopback. Two independently
# written implementations of one wire — which is the only kind of test that can
# catch a protocol misreading, and the reason it runs BEFORE hardware.
#
# What it witnesses (design §9, host-side legs):
#   1. `goal/name` written at porta appears in the node's space — proven by the
#      node echoing it back to `obs/name`, which porta then reads.
#   2. `obs/health` pushed by the node appears in porta's cell tree.
#   3. `tel/log` entries arrive EXACTLY ONCE — the count porta logged equals the
#      count the node wrote, so nothing was duplicated by a re-push.
# Leg 3 under a DELIBERATE datagram drop is plan 3's, on the mesh.
set -euo pipefail
cd "$(dirname "$0")/.."

PORTA_DIR="${PORTA_DIR:-$HOME/workspace/porta}"
NSL="$HOME/workspace/nsl-tuvm/tools/ns_compile_oevm.py"
TARGET="${OEVM_TARGET:-posix}"
DB=/tmp/porta_sync_gate.db
NODE=bench-node

if [ ! -d "$PORTA_DIR" ]; then
  echo "SKIP: no porta checkout at $PORTA_DIR (set PORTA_DIR)"; exit 0
fi
if ! command -v go >/dev/null 2>&1; then
  echo "SKIP: go is not on PATH — cannot build the porta counterparty"; exit 0
fi

echo "[1/7] build porta"
( cd "$PORTA_DIR" && go build -o /tmp/porta ./cmd/porta )

echo "[2/7] host-ABI layout descriptor"
gcc -std=c99 -Isrc tools/dump_layout.c -o /tmp/dl
/tmp/dl > /tmp/layout.json

echo "[3/7] compile PortaBridgeNode.nsl -> .oimg"
python3 "$NSL" zephyr/PortaBridgeNode.nsl /tmp/porta_node.oimg --format=oimg --layout=/tmp/layout.json

echo "[4/7] build the image VM"
gcc -Wall -std=c99 -Isrc \
    src/vm.c src/gc.c src/dispatch.c src/platform.c src/space.c src/handles.c \
    src/span.c \
    "src/backend/$TARGET/platform_$TARGET.c" src/serial.c \
    "src/backend/$TARGET/serial_$TARGET.c" src/udp.c "src/backend/$TARGET/udp_$TARGET.c" \
    src/http.c src/http_core.c "src/backend/$TARGET/http_$TARGET.c" \
    "src/backend/$TARGET/pulse_$TARGET.c" "src/backend/$TARGET/timer_$TARGET.c" \
    "src/backend/$TARGET/span_$TARGET.c" \
    "src/backend/$TARGET/sensor_$TARGET.c" \
    src/mirror.c \
    src/oimg_loader.c src/kernel_boot.c oevm_image_main.c -o /tmp/oevm_porta -lm

echo "[5/7] start porta serve on udp 127.0.0.1:16969"
rm -f "$DB"
/tmp/porta serve --db "$DB" --udp 127.0.0.1:16969 --http-port 0 >/tmp/porta_serve.log 2>&1 &
PORTA_PID=$!
trap 'kill $PORTA_PID $NODE_PID 2>/dev/null || true' EXIT
for _ in $(seq 1 50); do grep -q "serving the space" /tmp/porta_serve.log && break; sleep 0.1; done
grep -q "serving the space" /tmp/porta_serve.log || { echo "FAIL: porta did not start"; cat /tmp/porta_serve.log; exit 1; }

# The node's cadence runs on real time; the suite's default virtual clock never
# advances on its own, so the timer would never fire (the same knob the http gate
# needs, for the same reason).
echo "[6/7] start the node"
OEVM_TIMER_WALLCLOCK=1 /tmp/oevm_porta /tmp/porta_node.oimg >/tmp/porta_node.log 2>&1 &
NODE_PID=$!
for _ in $(seq 1 50); do grep -q "PORTA NODE READY" /tmp/porta_node.log && break; sleep 0.1; done
grep -q "PORTA NODE READY" /tmp/porta_node.log || { echo "FAIL: node did not start"; cat /tmp/porta_node.log; exit 1; }

echo "[7/7] drive it"
# Leg 2 first: it needs no operator write at all — an idle check-in IS the
# heartbeat, so obs/health arriving is also the liveness proof.
for _ in $(seq 1 100); do /tmp/porta get "$NODE" obs 2>/dev/null | grep -q "obs/health" && break; sleep 0.2; done
/tmp/porta get "$NODE" obs | grep -q "obs/health" || { echo "FAIL: obs/health never reached porta"; /tmp/porta get "$NODE"; exit 1; }
echo "  obs/health is in porta's cell tree"
/tmp/porta get "$NODE" obs | grep -q "obs/build" || { echo "FAIL: obs/build missing"; exit 1; }
echo "  obs/build carries the node's provenance"

# Leg 1: write the goal, wait for the node to echo it as obs.
/tmp/porta put "$NODE" goal/name bench >/dev/null
for _ in $(seq 1 100); do /tmp/porta get "$NODE" obs/name 2>/dev/null | grep -q "bench" && break; sleep 0.2; done
/tmp/porta get "$NODE" obs/name | grep -q "bench" || { echo "FAIL: goal/name never converged to obs/name"; /tmp/porta get "$NODE"; exit 1; }
echo "  goal/name converged to obs/name — the diff IS the view"

# Leg 3: exactly once. The node writes one tel/log per tick with a monotone
# counter, so a duplicated push shows up as a repeated 'tick N'.
sleep 3
LOGS=$(/tmp/porta log "$NODE" --limit 200 | grep -c "tel/log" || true)
UNIQ=$(/tmp/porta log "$NODE" --limit 200 | grep "tel/log" | sed 's/.*tick //' | sort -u | wc -l)
echo "  tel/log rows=$LOGS distinct ticks=$UNIQ"
test "$LOGS" -ge 2 || { echo "FAIL: fewer than two tel/log entries — the stream never rode"; exit 1; }
test "$LOGS" = "$UNIQ" || { echo "FAIL: a tel/log entry was delivered more than once"; /tmp/porta log "$NODE" --limit 200; exit 1; }
echo "  tel/log arrived exactly once per entry"

echo "PORTA SYNC OK"
```

`chmod +x tests/porta_sync_host.sh`.

- [ ] **Step 2: Run it and read the failure honestly**

```bash
cd ~/workspace/tuvm && tests/porta_sync_host.sh
```
Expected first run: it fails somewhere real. Work the failure with
`superpowers:systematic-debugging` — the two most likely, both worth naming in
advance:
- **`porta: rejecting datagram: malformed message`** in `/tmp/porta_serve.log` — the
  node's field names or record tags do not match `internal/wire/message.go`. Decode
  the node's actual bytes rather than guessing: add a temporary `println` of the
  frame, or read the rejection message, which names the missing field.
- **Nothing at all in porta's log** — the node never sent. Check that
  `OEVM_TIMER_WALLCLOCK=1` is set (without it the virtual clock never advances and
  the cadence timer never fires) and that the node bound its socket.

- [ ] **Step 3: Get it to green, then run it twice**

```bash
cd ~/workspace/tuvm && tests/porta_sync_host.sh && tests/porta_sync_host.sh
```
Expected: `PORTA SYNC OK` both times. Twice matters: the second run starts from a
fresh DB but the same node id, which is the cold-boot-at-cursor-0 path.

- [ ] **Step 4: Commit**

```bash
cd ~/workspace/tuvm && git add tests/porta_sync_host.sh && \
  git commit -m "test(spacesync): the host interop gate — nsl node vs real porta serve (porta#25)"
```

---

### Task 7: Promotion and the loose ends

**Files:**
- Modify: `protocol/PROTOCOL-NSL.md` (§4.1), `tuvm/review/tests/CLAIMS.md`,
  `tuvm/review/program-sequence.md`, `tuvm/docs/next-steps-overview.md`

The promotion rule: a rung is not closed until its durable knowledge is in a mainline
document of record. The dated design doc stays behind as the historical "why".

- [ ] **Step 1: Record what actually landed, in `PROTOCOL-NSL.md` §4.1**

Append to §4.1, after "The bridge is a *consumer* of that kind, not the owner of it.":

```markdown
**What landed on the node (2026-08-02, plan 2 of the space-sync rewrite).** The VM
gained one monotonic counter per space and a `last_seq` per channel (D5), reached by
prims 138–140 (`spaceSeq`, `spaceStamp`, `spaceChangedSince:under:max:`) and the nsl
verbs `Channel>>changedSince:max:` / `Space>>seq` / `Space>>stamp`. Three node-side
rules were decided in the writing and are part of the contract:

- **The changed-since scan answers cells only.** A stream's `seq` advances on every
  write, but its value is killed on delivery, so a stream row would carry a
  stale-or-nil value under a fresh seq. Streams reach the wire by reaction capture in
  the bridge, stamped with `spaceStamp` so both halves share ONE ascending sequence.
  Reading the counter rather than bumping it would let two captures collide on one
  seq, which the gateway drops as a duplicate — silent loss.
- **Inbound authority is enforced on the node.** porta may write `goal/**` and
  `dbg/req`; an entry at any other path is counted and dropped. An accepted write to
  `obs/**` would forge the node's own report of what it observes, which is the one
  thing the convergence view rests on.
- **`diag/spans/result` is not synced** until tuvm#107 splits its ~1120 B value into
  sibling cells at `diag/spans/<span>` (RECORDS §4.6).
```

- [ ] **Step 2: Add the CLAIMS row**

In `tuvm/review/tests/CLAIMS.md`, add a row in the same style as its neighbours:

```markdown
| — | **A node replicates its space with a gateway over a lossy link, with no retransmit layer** — cells by cursor, streams exactly once, and porta's writes confined to `goal/**` | `nsl-tests/22_spacesync/01_seq_and_scan.nsl` (one monotonic seq per space; the scan is cells-only, ascending, cursor-exclusive), `02_scan_confinement.nsl` (A1 confinement, segment-aligned: `obs` never matches `obscure`), `03_bridge_frames.nsl` (pull/batch/push/ack, and an `#err` reply moves nothing), `04_bridge_authority.nsl` (an inbound `obs/**` write is counted and dropped; a garbage datagram is survived); C seam: `tuvm/tests/test_space_seq.c` (the counter, `last_seq`, the prefix scan, and a shed write still stamped); host interop: `tuvm/tests/porta_sync_host.sh` — **THE GATE** (this node against a real `porta serve` over UDP loopback: `obs/health` reaches porta, `goal/name` converges to `obs/name`, `tel/log` arrives exactly once). Enforced by `tuvm/src/space.c` (`seq_counter`/`last_seq`/`space_changed_since`) + prims 138–140 + `nsl-tuvm/lib/kernel.nsl` `class PortaBridge`. Design: `porta/docs/design/2026-08-01-porta-space-sync-rewrite-design.md`; plan: `tuvm/docs/superpowers/plans/2026-08-02-porta-space-sync-node.md` | DONE (host); hardware gate is plan 3 |
```

- [ ] **Step 3: Update the ladder**

In `tuvm/review/program-sequence.md`, add the rung entry beside its neighbours, saying
plainly what is proven on the host and what is still owed to hardware. **The rung is
not closed here** — design §9's gate is witnessed on the C6 over a real Thread mesh,
and that is plan 3.

- [ ] **Step 4: Update `docs/next-steps-overview.md`**

In Part 2, under "1. porta → channels", replace "Plans 2 … and 3 … are unwritten" with
the current state: plan 2 written and executed, its host interop gate green, plan 3
(the C6 hardware gate) next and still the thing that closes the rung.

- [ ] **Step 5: File the deferred work as issues**

Each of these is named in the plan and deliberately not built. File in
`~/workspace/porta` unless it is VM work, which goes to `~/workspace/tuvm`:

```bash
cd ~/workspace/porta
gh issue create --title "space-sync: the node's cursor and bootId live in RAM — a reboot re-pulls everything" \
  --body "Plan 2 N6. Cold boot = cursor 0 = full replay is the designed behaviour (PROTOCOL-NSL §3), and the cell scan bounds a replay by TREE SIZE rather than history length, so this is survivable rather than urgent. What it costs: every reboot re-applies every goal cell, and bootId is a source constant (1), so porta cannot order observations across reboots — the (bootId, monoMs) session key of RECORDS §3 is degenerate. Needs NVS (G7). Relates porta#26 (replaying goal/do/* is a brick), porta#27 (node identity)."
gh issue create --title "space-sync: the bridge holds the ROOT space, not a goal-write + obs-read pair" \
  --body "Plan 2 N4. PROTOCOL-NSL §4.1 grants the bridge job write access to goal/** and dbg/req and read access to obs/**, tel/**, sys/**, dbg/resp. The implementation takes the root Space and enforces the inbound half itself in mayApply: — the Supervisor's shape, and the only one available until a capability can carry two roots. Authority IS enforced (04_bridge_authority.nsl), it is just enforced by the holder rather than by the capability. Tighten when the grant machinery (G5) lands."
gh issue create --title "space-sync: cadence is a constant — PROTOCOL-NSL §4.4's adaptive controller is unbuilt" \
  --body "Plan 2, PortaBridgeNode cadence = 2000 ms. §4.4 wants a small controller between a battery-set ceiling and a liveness-set floor, keyed off obs/mode plus application signals (observed change rate, a pending goal, an active debug session), with porta's cadenceHint advisory and unemitted. The hint field is already reserved on the wire so the shape does not move."
gh issue create --title "space-sync: a stream entry written while the bridge is down is lost" \
  --body "Plan 2 N2. Outbound stream entries are captured by whenever: reactions. A crash of the bridge job sweeps its reactions (spaceDropReactionsOwnedBy:), so entries written between the crash and the restart are dispatched to nobody and killed by the reactor. Cells are unaffected — they are re-scanned from the cursor, which is the whole point of a cell. Bounded and honest for tel/** (lossy by policy, RECORDS §4.3), NOT honest for sys/panic, which RECORDS marks must-deliver. Decide before sys/panic is subscribed."
```

- [ ] **Step 6: Commit the promotion**

```bash
cd ~/workspace/tuvm && git add review/tests/CLAIMS.md review/program-sequence.md docs/next-steps-overview.md && \
  git commit -m "docs(spacesync): promote plan 2 — the ladder, CLAIMS, and what plan 3 still owes"
cd ~/workspace/tuvm && git add protocol/PROTOCOL-NSL.md && \
  git commit -m "docs(protocol): §4.1 records the node side as built (porta#25)"
```

---

## Self-Review

**1. Spec coverage.** Design §6's three bullets: `space.c` counter + `last_seq` +
`changedSince:` prim → Tasks 1–3. `PortaBridge` as an ordinary nsl job → Task 4.
`PortaBridgeNode.nsl`, a fresh minimal node and not the shipsBells rig → Task 5.
"The cursor lives in RAM for this slice" → N6, and filed in Task 7. §5's wire shapes
→ Task 4, taken from the shipped Go code rather than the design's sketch (the
`hw` / `nodeSeqHW` divergence is called out in Global Constraints). D5's one-counter
ruling → Task 1. D8's `obs/build` → Task 5, with `sdk`/`image`/`builtAt` omitted and
tuvm#74 named. §9's gate: legs 1–3 land on the host in Task 6; the same three
witnessed on the C6 over a real mesh, including the deliberate drop, are plan 3's and
are stated as such in Tasks 6 and 7. Deliberately not covered, each with a home:
tuvm#107 (Task 5 excludes `diag/spans/result`), porta#24 (retention vs cursor — a
porta-side policy, untouched here), porta#26 (`goal/do/*` replay — filed, not built).

**2. Placeholders.** None: every step carries the code or the exact command, and the
one place a name is uncertain (`test_platform.c`'s helper names, Task 2 Step 1) says
to use that file's existing names rather than inventing new ones.

**3. Type consistency.** `changedSince:max:` answers `{path, seq, value}` in that
order in Task 3 and is consumed in that order in Task 4's `collectOutbound`
(`r at: 1` path, `r at: 2` seq, `r at: 3` value) — and re-ordered to the wire's
`[seq, path, kind, value]` there, which is the one place the two orders meet.
`kind` is 1/2 everywhere (C, nsl, Go). The prim is
`spaceChangedSince:under:max:` in `dispatch.c`, `platform.c`, `prim_ids.h` and
`kernel.nsl` alike. `PortaBridge` is constructed
`socket:space:id:peer:port:` in the class-side, the tests and the node. `space_stamp`
is C, `Space>>stamp` is nsl, `spaceStamp` is the prim — three spellings of one
counter, consistent within each layer.
